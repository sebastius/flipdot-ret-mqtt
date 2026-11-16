// Basic code om een LAWO display van de RET te vullen.
//
// MQTT (ongewijzigd):
//   <topic>              tekst -> display (IBIS "aA"-telegram), regels met \n, pagina's met \n\n
//   <debugtopic>         "hello world" bij (her)verbinden
// Nieuw:
//   <topic>/test         0 = zwart, 1 = geel, 2 = dambord, 3 = invers dambord (IBIS "aT")
//   <topic>/query        "status" of "version"
//   <debugtopic>/ack     "ok" / "timeout" na elke tekst (antwoord "a0" van het display)
//   <debugtopic>/display "online" / "offline" (retained, periodieke statusvraag)
//   <debugtopic>/version versie van de displayfirmware, bv. "F00293" (retained)
//   <debugtopic>/raw     onbekende antwoorden van het display
//
// Terugkoppeling werkt alleen als de antwoordlijn van de controller (TXD) aan
// GPIO13 (RX na Serial.swap) hangt, via een niveau-aanpassing naar 3,3 V.
//
// Instellingen (WiFi, MQTT-server/poort/topics, displayadres) via WiFiManager:
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
};
Config cfg;

const char* CONFIG_FILE = "/config.json";
const size_t MAX_TEXT = 238;        // ASFP: telegram max. ~249 tekens, anders genegeerd
const unsigned long REPLY_TIMEOUT_MS = 1500;
const unsigned long STATUS_INTERVAL_MS = 60000;

WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wm;

WiFiManagerParameter* pServer;
WiFiManagerParameter* pPort;
WiFiManagerParameter* pTopic;
WiFiManagerParameter* pDebug;
WiFiManagerParameter* pAddress;

char apName[32];
bool configChanged = false;
unsigned long lastMqttAttempt = 0;
unsigned long lastStatusPoll = 0;

// ---------- IBIS-telegrammen ----------
char addressChar() {
  return '0' + cfg.address;         // 10..15 -> ':'..'?' zoals de firmware verwacht
}

void sendTelegram(const String& body) {
  // body eindigt op '\r'; checksum = 0x7F XOR alle tekens
  byte checksum = 0x7F;
  for (size_t i = 0; i < body.length(); i++) {
    checksum ^= (byte)body[i];
  }
  Serial.print(body);
  Serial.write(checksum);
}

void flipdot(String tekst) {
  String header = String("aA") + addressChar() + "1001\n";
  String footer = "\n\n\r";
  sendTelegram(header + tekst + footer);
}

// ---------- antwoorden van het display ----------
Pending pending = P_NONE;
unsigned long pendingSince = 0;
String lastDisplayState = "";

String sub(const char* suffix) {
  return String(cfg.debugTopic) + "/" + suffix;
}

void publishDisplayState(const char* state) {
  if (lastDisplayState != state && client.connected()) {
    client.publish(sub("display").c_str(), state, true);
    lastDisplayState = state;
  }
}

void expectReply(Pending p) {
  pending = p;
  pendingSince = millis();
}

void handleReply(const char* t, size_t len) {
  if (len == 2 && t[0] == 'a' && (t[1] == '0' || t[1] == '6')) {
    // statusantwoord: a0 = ok, a6 = display meldt een bijzondere toestand
    if (pending == P_ACK && client.connected()) {
      client.publish(sub("ack").c_str(), t[1] == '0' ? "ok" : "a6");
    }
    publishDisplayState("online");
    if (pending == P_ACK || pending == P_STATUS) pending = P_NONE;
    return;
  }
  if (len >= 3 && t[0] == 'a' && t[1] == 'V') {
    String v = String(t).substring(2);
    if (client.connected()) client.publish(sub("version").c_str(), v.c_str(), true);
    publishDisplayState("online");
    if (pending == P_VERSION) pending = P_NONE;
    return;
  }
  if (client.connected()) client.publish(sub("raw").c_str(), t);
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
      if ((cs & 0x7F) == c) {
        handleReply(buf, len);
      } else if (client.connected()) {
        client.publish(sub("raw").c_str(), (String("checksum error: ") + buf).c_str());
      }
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

  if (pending != P_NONE && millis() - pendingSince > REPLY_TIMEOUT_MS) {
    if (pending == P_ACK && client.connected()) {
      client.publish(sub("ack").c_str(), "timeout");
    }
    publishDisplayState("offline");
    pending = P_NONE;
  }
}

void queryStatus() {
  sendTelegram(String("a") + addressChar() + "\r");
  expectReply(P_STATUS);
}

void queryVersion() {
  sendTelegram(String("aV") + addressChar() + "\r");
  expectReply(P_VERSION);
}

void testPattern(int z) {
  if (z < 0 || z > 3) return;
  sendTelegram(String("aT") + addressChar() + char('0' + z) + "\r");
}

// ---------- configuratie opslaan ----------
void copyStr(char* dst, size_t n, const char* src) {
  if (src && *src) strlcpy(dst, src, n);
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
  saveConfig();
  configChanged = true;             // MQTT opnieuw verbinden met nieuwe instellingen
}

void apCallback(WiFiManager* m) {
  flipdot("WiFi instellen\n" + String(apName));
}

// ---------- WiFi / MQTT ----------
void setup_wifi() {
  delay(1000);

  static char portStr[6], addrStr[3];
  snprintf(portStr, sizeof(portStr), "%u", cfg.mqttPort);
  snprintf(addrStr, sizeof(addrStr), "%u", cfg.address);
  pServer = new WiFiManagerParameter("server", "MQTT server", cfg.mqttServer, 63);
  pPort = new WiFiManagerParameter("port", "MQTT poort", portStr, 5);
  pTopic = new WiFiManagerParameter("topic", "Luistertopic (tekst)", cfg.topic, 63);
  pDebug = new WiFiManagerParameter("debug", "Debug/statustopic", cfg.debugTopic, 63);
  pAddress = new WiFiManagerParameter("address", "IBIS-displayadres (1-15)", addrStr, 2);
  wm.addParameter(pServer);
  wm.addParameter(pPort);
  wm.addParameter(pTopic);
  wm.addParameter(pDebug);
  wm.addParameter(pAddress);

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
  }
  if (!wm.autoConnect(apName)) {
    flipdot("Geen WiFi\nherstart...");
    delay(3000);
    ESP.restart();
  }
  randomSeed(micros());

  flipdot("WiFi connected\n" + WiFi.localIP().toString());
  delay(4000);
  flipdot("");

  wm.startWebPortal();              // instellingen blijven bereikbaar op het IP-adres
}

void callback(char* topic, byte* payload, unsigned int length) {
  String mededeling;
  for (unsigned int i = 0; i < length; i++) {
    mededeling = mededeling + ((char)payload[i]);
  }
  String t(topic);

  if (t == cfg.topic) {
    if (mededeling.length() > MAX_TEXT) {
      client.publish(sub("raw").c_str(), "tekst te lang, afgekapt");
      mededeling = mededeling.substring(0, MAX_TEXT);
    }
    flipdot(mededeling);
    expectReply(P_ACK);
  } else if (t == String(cfg.topic) + "/test") {
    testPattern(mededeling.toInt());
  } else if (t == String(cfg.topic) + "/query") {
    if (mededeling == "version") queryVersion();
    else queryStatus();
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
    client.subscribe(cfg.topic);
    client.subscribe((String(cfg.topic) + "/test").c_str());
    client.subscribe((String(cfg.topic) + "/query").c_str());
    lastDisplayState = "";
    queryVersion();
  }
}

void setup() {
  Serial.begin(1200, SERIAL_7E2);
  Serial.swap();
  delay(500);
  flipdot("Booting...");
  delay(500);
  loadConfig();
  setup_wifi();
  client.setBufferSize(512);
  client.setServer(cfg.mqttServer, cfg.mqttPort);
  client.setCallback(callback);
}

void loop() {
  wm.process();

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

  if (pending == P_NONE && millis() - lastStatusPoll > STATUS_INTERVAL_MS) {
    lastStatusPoll = millis();
    queryStatus();
  }
}
