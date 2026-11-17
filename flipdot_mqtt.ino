// Basic code om een LAWO display van de RET te vullen.
//
// MQTT (ongewijzigd):
//   <topic>              tekst -> display (IBIS "aA"-telegram), regels met \n; na 2 regels \n\n, na 1 regel \n\n\n als er een pagina volgt
//   <debugtopic>         "hello world" bij (her)verbinden
// Ook:
//   <topic>/test         0 = zwart, 1 = geel, 2 = dambord, 3 = invers dambord (IBIS "aT")
//   <topic>/query        "status" of "version"
//   <topic>/raw          ruw telegram (zonder CR/checksum), bv. "aV1"
// Alleen met de ASFP-bitmap-firmware (github.com/sebastius/lawo-flipdot, firmware-patches/asfp-bitmap):
//   <topic>/pixel        "x,y,c" (c = 0 zwart / 1 geel), meerdere gescheiden door ; spatie of regeleinde
//   <topic>/bitmap       hex, 4 tekens per kolom (bit y = rij y van boven), optioneel "XXX:" startkolom
//   <topic>/refresh      alle dots opnieuw omzetten (aU)
// Terug:
//   <debugtopic>/ack     "ok" / "timeout" per telegram met antwoord (a0 van het display)
//   <debugtopic>/display "online" / "offline" (retained, periodieke statusvraag)
//   <debugtopic>/version versie van de displayfirmware, bv. "F00293" (retained)
//   <debugtopic>/config  huidige instellingen en IP-adres (retained)
//   <debugtopic>/raw     onbekende antwoorden en meldingen
//
// Telegrammen gaan via een wachtrij: het volgende telegram wordt pas verstuurd na het
// antwoord op het vorige (of na een korte time-out als de antwoordlijn niet is aangesloten).
// Dat is nodig voor de snelle ASFP-bitmap-firmwares (15625 / 31250 baud).
//
// Terugkoppeling werkt alleen als de TXD van de 80C31 via een spanningsdeler aan GPIO13
// (RX na Serial.swap) hangt; zie README.
//
// Instellingen (WiFi, MQTT-server/poort/topics, displayadres, baudrate) via WiFiManager:
// - geen WiFi: access point "Flipdot-XXXXXX", portal op 192.168.4.1
// - wel WiFi: dezelfde portal op het IP-adres van de ESP (menu "Setup")

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ---------- instellingen ----------
enum Pending { P_NONE, P_ACK, P_STATUS, P_VERSION };

struct Config {
  char mqttServer[64] = "mosquitto.space.revspace.nl";
  uint16_t mqttPort = 1883;
  char topic[64] = "revspace/flipdot";
  char debugTopic[64] = "revdebug/flipdot";
  uint8_t address = 1;              // IBIS-displayadres (1..15)
  uint32_t baud = 1200;             // 1200 = origineel/IBIS, 15625 / 31250 = ASFP-bitmap 6 / 12 MHz
};
Config cfg;

const char* CONFIG_FILE = "/config.json";
const size_t MAX_TEXT = 238;        // ASFP: telegram max. ~249 tekens, anders genegeerd
const size_t MAX_LONG = 499;        // ASFP-bitmap: aP/aB max. 500 tekens incl. CR
const unsigned long STATUS_INTERVAL_MS = 60000;

WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wm;

WiFiManagerParameter* pServer;
WiFiManagerParameter* pPort;
WiFiManagerParameter* pTopic;
WiFiManagerParameter* pDebug;
WiFiManagerParameter* pAddress;
WiFiManagerParameter* pBaud;

char apName[32];
bool configChanged = false;
bool baudChanged = false;
unsigned long lastMqttAttempt = 0;
unsigned long lastStatusPoll = 0;

String sub(const char* suffix) {
  return String(cfg.debugTopic) + "/" + suffix;
}

void note(const String& msg) {
  if (client.connected()) client.publish(sub("raw").c_str(), msg.c_str());
}

// ---------- IBIS-telegrammen ----------
char addressChar() {
  return '0' + cfg.address;         // 10..15 -> ':'..'?' zoals de firmware verwacht
}

String pad(unsigned v, int width) {
  String s(v);
  while ((int)s.length() < width) s = "0" + s;
  return s;
}

// telegram (eindigend op '\r') + checksum direct naar de seriële poort
void writeTelegram(const String& body) {
  byte checksum = 0x7F;
  for (size_t i = 0; i < body.length(); i++) checksum ^= (byte)body[i];
  Serial.print(body);
  Serial.write(checksum);
}

// ---------- uitgaande wachtrij ----------
struct Out {
  String body;
  Pending expect;
};
const uint8_t QLEN = 16;
Out outq[QLEN];
uint8_t qHead = 0, qCount = 0;
bool busy = false;
Pending pending = P_NONE;
unsigned long busySince = 0, busyTimeout = 0;
String lastDisplayState = "";

unsigned long txMs(size_t chars) {
  return (unsigned long)((chars + 2) * 11UL * 1000UL / cfg.baud) + 1;
}

bool enqueue(const String& body, Pending expect) {
  if (qCount == QLEN) {
    note("wachtrij vol, telegram verworpen");
    return false;
  }
  outq[(qHead + qCount) % QLEN] = {body, expect};
  qCount++;
  return true;
}

void publishDisplayState(const char* state) {
  if (lastDisplayState != state && client.connected()) {
    client.publish(sub("display").c_str(), state, true);
    lastDisplayState = state;
  }
}

void pumpQueue() {
  if (busy) {
    if (millis() - busySince < busyTimeout) return;
    // geen antwoord gekomen: doorgaan
    if (pending == P_ACK && client.connected()) client.publish(sub("ack").c_str(), "timeout");
    if (pending != P_NONE) publishDisplayState("offline");
    busy = false;
    pending = P_NONE;
  }
  if (qCount == 0) return;
  Out o = outq[qHead];
  qHead = (qHead + 1) % QLEN;
  qCount--;
  writeTelegram(o.body);
  pending = o.expect;
  busy = true;
  busySince = millis();
  // verzendtijd + antwoord (4 tekens) + marge; zonder antwoord alleen een korte pauze
  busyTimeout = txMs(o.body.length()) + (o.expect == P_NONE ? 20 : txMs(4) + 150);
}

// ---------- antwoorden van het display ----------
void handleReply(const char* t, size_t len) {
  if (len == 2 && t[0] == 'a' && (t[1] == '0' || t[1] == '6')) {
    // a0 = ok, a6 = display meldt een bijzondere toestand
    if (pending == P_ACK && client.connected()) {
      client.publish(sub("ack").c_str(), t[1] == '0' ? "ok" : "a6");
    }
    publishDisplayState("online");
    if (pending == P_ACK || pending == P_STATUS) { pending = P_NONE; busy = false; }
    return;
  }
  if (len >= 3 && t[0] == 'a' && t[1] == 'V') {
    String v = String(t).substring(2);
    if (client.connected()) client.publish(sub("version").c_str(), v.c_str(), true);
    publishDisplayState("online");
    if (pending == P_VERSION) { pending = P_NONE; busy = false; }
    return;
  }
  note(t);
}

void pollSerial() {
  static char buf[40];
  static size_t len = 0;
  static bool waitChecksum = false;

  while (Serial.available()) {
    byte c = Serial.read() & 0x7F;
    if (waitChecksum) {
      waitChecksum = false;
      byte cs = 0x7F;
      for (size_t i = 0; i < len; i++) cs ^= (byte)buf[i];
      cs ^= '\r';
      buf[len] = 0;
      if ((cs & 0x7F) == c) handleReply(buf, len);
      else note(String("checksum error: ") + buf);
      len = 0;
      continue;
    }
    if (c == '\r') {
      if (len > 0) waitChecksum = true;
      continue;
    }
    if (len == 0 && (c < 'a' || c > 'z')) continue;   // wacht op begin telegram
    if (len < sizeof(buf) - 1) buf[len++] = c;
    else len = 0;
  }
}

// wachtrij afwerken (voor gebruik buiten loop(), bv. tijdens het opstarten)
void flushQueue(unsigned long maxMs) {
  unsigned long t0 = millis();
  while ((qCount > 0 || busy) && millis() - t0 < maxMs) {
    pollSerial();
    pumpQueue();
    delay(1);
  }
}

// ---------- telegrammen opbouwen ----------
void flipdot(String tekst) {
  String header = String("aA") + addressChar() + "1001\n";
  String footer = "\n\n\r";
  enqueue(header + tekst + footer, P_ACK);
}

void queryStatus() {
  enqueue(String("a") + addressChar() + "\r", P_STATUS);
}

void queryVersion() {
  enqueue(String("aV") + addressChar() + "\r", P_VERSION);
}

void testPattern(int z) {
  if (z < 0 || z > 3) return;
  enqueue(String("aT") + addressChar() + char('0' + z) + "\r", P_NONE);
}

// "x,y,c" groepen -> een of meer aP-telegrammen
void sendPixels(const String& payload) {
  String head = String("aP") + addressChar();
  String body = head;
  int count = 0, bad = 0;
  int i = 0, n = payload.length();
  while (i < n) {
    while (i < n && (payload[i] == ';' || payload[i] == ' ' || payload[i] == '\n' || payload[i] == '\r')) i++;
    if (i >= n) break;
    int j = i;
    while (j < n && payload[j] != ';' && payload[j] != ' ' && payload[j] != '\n' && payload[j] != '\r') j++;
    String item = payload.substring(i, j);
    i = j;
    int c1 = item.indexOf(','), c2 = item.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) { bad++; continue; }
    long x = item.substring(0, c1).toInt(), y = item.substring(c1 + 1, c2).toInt(), c = item.substring(c2 + 1).toInt();
    if (x < 0 || x > 255 || y < 0 || y > 99 || (c != 0 && c != 1)) { bad++; continue; }
    if (body.length() + 6 + 1 > MAX_LONG) { enqueue(body + "\r", P_ACK); body = head; }
    body += pad(x, 3) + pad(y, 2) + String(c);
    count++;
  }
  if (body.length() > head.length()) enqueue(body + "\r", P_ACK);
  if (bad) note(String(bad) + " ongeldige pixel(s) overgeslagen");
}

// hex (4 tekens per kolom), optioneel "XXX:" startkolom -> een of meer aB-telegrammen
void sendBitmap(const String& payload) {
  String p = payload;
  int start = 0;
  int colon = p.indexOf(':');
  if (colon > 0) { start = p.substring(0, colon).toInt(); p = p.substring(colon + 1); }
  String hex;
  hex.reserve(p.length());
  for (size_t i = 0; i < p.length(); i++) {
    char ch = p[i];
    if (isxdigit((unsigned char)ch)) hex += ch;
    else if (ch != ' ' && ch != '\n' && ch != '\r') { note("ongeldig teken in bitmap"); return; }
  }
  size_t words = hex.length() / 4;
  if (hex.length() % 4) note("bitmap: laatste onvolledige kolom genegeerd");
  const size_t perTele = (MAX_LONG - 6 - 1) / 4;       // "aB" + adres + XXX + ... + CR
  for (size_t w = 0; w < words; w += perTele) {
    size_t cnt = min(perTele, words - w);
    enqueue(String("aB") + addressChar() + pad(start + w, 3) + hex.substring(w * 4, (w + cnt) * 4) + "\r", P_ACK);
  }
}

// ---------- configuratie opslaan ----------
void copyStr(char* dst, size_t n, const char* src) {
  if (src && *src) strlcpy(dst, src, n);
}

bool validBaud(long b) {
  return b >= 300 && b <= 250000;
}

void loadConfig() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    copyStr(cfg.mqttServer, sizeof(cfg.mqttServer), doc["mqtt_server"] | "");
    cfg.mqttPort = doc["mqtt_port"] | cfg.mqttPort;
    copyStr(cfg.topic, sizeof(cfg.topic), doc["topic"] | "");
    copyStr(cfg.debugTopic, sizeof(cfg.debugTopic), doc["debug_topic"] | "");
    cfg.address = doc["address"] | cfg.address;
    long b = doc["baud"] | (long)cfg.baud;
    if (validBaud(b)) cfg.baud = b;
  }
  f.close();
}

void saveConfig() {
  JsonDocument doc;
  doc["mqtt_server"] = cfg.mqttServer;
  doc["mqtt_port"] = cfg.mqttPort;
  doc["topic"] = cfg.topic;
  doc["debug_topic"] = cfg.debugTopic;
  doc["address"] = cfg.address;
  doc["baud"] = cfg.baud;
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}

void saveParamsCallback() {
  copyStr(cfg.mqttServer, sizeof(cfg.mqttServer), pServer->getValue());
  int port = atoi(pPort->getValue());
  if (port > 0 && port < 65536) cfg.mqttPort = port;
  copyStr(cfg.topic, sizeof(cfg.topic), pTopic->getValue());
  copyStr(cfg.debugTopic, sizeof(cfg.debugTopic), pDebug->getValue());
  int addr = atoi(pAddress->getValue());
  if (addr >= 1 && addr <= 15) cfg.address = addr;
  long b = atol(pBaud->getValue());
  if (validBaud(b) && (uint32_t)b != cfg.baud) { cfg.baud = b; baudChanged = true; }
  saveConfig();
  configChanged = true;             // MQTT opnieuw verbinden met nieuwe instellingen
}

void apCallback(WiFiManager* m) {
  flipdot("WiFi instellen\n" + String(apName));
  flushQueue(2000);
}

void publishConfig() {
  if (!client.connected()) return;
  String s = String("{\"baud\":") + cfg.baud + ",\"address\":" + cfg.address + ",\"topic\":\"" + cfg.topic +
             "\",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
  client.publish(sub("config").c_str(), s.c_str(), true);
}

// ---------- WiFi / MQTT ----------
void setup_wifi() {
  delay(1000);

  static char portStr[6], addrStr[3], baudStr[8];
  snprintf(portStr, sizeof(portStr), "%u", cfg.mqttPort);
  snprintf(addrStr, sizeof(addrStr), "%u", cfg.address);
  snprintf(baudStr, sizeof(baudStr), "%lu", (unsigned long)cfg.baud);
  pServer = new WiFiManagerParameter("server", "MQTT server", cfg.mqttServer, 63);
  pPort = new WiFiManagerParameter("port", "MQTT poort", portStr, 5);
  pTopic = new WiFiManagerParameter("topic", "Luistertopic (tekst)", cfg.topic, 63);
  pDebug = new WiFiManagerParameter("debug", "Debug/statustopic", cfg.debugTopic, 63);
  pAddress = new WiFiManagerParameter("address", "IBIS-displayadres (1-15)", addrStr, 2);
  pBaud = new WiFiManagerParameter("baud",
      "Baudrate: 1200 = originele firmware, 15625 = ASFP-bitmap 6 MHz, 31250 = ASFP-bitmap 12 MHz",
      baudStr, 7);
  wm.addParameter(pServer);
  wm.addParameter(pPort);
  wm.addParameter(pTopic);
  wm.addParameter(pDebug);
  wm.addParameter(pAddress);
  wm.addParameter(pBaud);

  snprintf(apName, sizeof(apName), "Flipdot-%06X", ESP.getChipId());
  WiFi.hostname("flipdot");
  wm.setSaveConfigCallback(saveParamsCallback);   // portal bij eerste setup
  wm.setSaveParamsCallback(saveParamsCallback);   // "Setup"-pagina later
  wm.setAPCallback(apCallback);
  wm.setConfigPortalTimeout(300);                  // na 5 min opnieuw proberen
  std::vector<const char*> menu = {"wifi", "param", "info", "restart", "exit"};
  wm.setMenu(menu);

  if (WiFi.SSID().length() > 0) {
    flipdot("Connecting to \n" + WiFi.SSID());
    flushQueue(2000);
  }
  if (!wm.autoConnect(apName)) {
    flipdot("Geen WiFi\nherstart...");
    flushQueue(2000);
    delay(3000);
    ESP.restart();
  }
  randomSeed(micros());

  flipdot("WiFi connected\n" + WiFi.localIP().toString());
  flushQueue(2000);
  delay(4000);
  flipdot("");
  flushQueue(2000);

  wm.startWebPortal();              // instellingen blijven bereikbaar op het IP-adres
}

void callback(char* topic, byte* payload, unsigned int length) {
  String mededeling;
  mededeling.reserve(length);
  for (unsigned int i = 0; i < length; i++) mededeling += (char)payload[i];
  String t(topic);
  String base(cfg.topic);

  if (t == base) {
    if (mededeling.length() > MAX_TEXT) {
      note("tekst te lang, afgekapt");
      mededeling = mededeling.substring(0, MAX_TEXT);
    }
    flipdot(mededeling);
  } else if (t == base + "/test") {
    testPattern(mededeling.toInt());
  } else if (t == base + "/query") {
    if (mededeling == "version") queryVersion();
    else queryStatus();
  } else if (t == base + "/pixel") {
    sendPixels(mededeling);
  } else if (t == base + "/bitmap") {
    sendBitmap(mededeling);
  } else if (t == base + "/refresh") {
    enqueue(String("aU") + addressChar() + "\r", P_ACK);
  } else if (t == base + "/raw") {
    enqueue(mededeling + "\r", P_ACK);
  }
}

void reconnect() {
  // niet-blokkerend: elke 5 s een poging, zodat de webportal blijft werken
  if (millis() - lastMqttAttempt < 5000) return;
  lastMqttAttempt = millis();

  String clientId = "Flipdot-";
  clientId += String(random(0xffff), HEX);
  if (client.connect(clientId.c_str())) {
    client.publish(cfg.debugTopic, "hello world");
    String base(cfg.topic);
    client.subscribe(cfg.topic);
    for (const char* s : {"/test", "/query", "/pixel", "/bitmap", "/refresh", "/raw"}) {
      client.subscribe((base + s).c_str());
    }
    lastDisplayState = "";
    publishConfig();
    queryVersion();
  }
}

void setup() {
  loadConfig();                     // eerst: baudrate nodig voor de eerste melding
  Serial.begin(cfg.baud, SERIAL_7E2);
  Serial.swap();
  delay(500);
  flipdot("Booting...");
  flushQueue(2000);
  delay(500);
  setup_wifi();
  client.setBufferSize(2048);       // bitmap van 112 kolommen = 448 hex-tekens
  client.setServer(cfg.mqttServer, cfg.mqttPort);
  client.setCallback(callback);
}

void loop() {
  wm.process();

  if (baudChanged) {
    baudChanged = false;
    flushQueue(1000);
    Serial.flush();
    Serial.updateBaudRate(cfg.baud);
  }

  if (configChanged) {
    configChanged = false;
    client.disconnect();
    client.setServer(cfg.mqttServer, cfg.mqttPort);
    lastMqttAttempt = 0;
  }

  if (!client.connected()) {
    reconnect();
  } else {
    client.loop();
  }

  pollSerial();
  pumpQueue();

  if (!busy && qCount == 0 && millis() - lastStatusPoll > STATUS_INTERVAL_MS) {
    lastStatusPoll = millis();
    queryStatus();
  }
}
