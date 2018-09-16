// Basic code om een LAWO display van de RET te vullen.
//

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

const char* ssid = "revspace-pub-2.4ghz";
const char* password = "";
const char* mqtt_server = "mosquitto.space.revspace.nl";

WiFiClient espClient;
PubSubClient client(espClient);
long lastMsg = 0;
char msg[512];
int value = 0;

void setup_wifi() {
  delay(1000);
  
  flipdot("Connecting to \n" + String(ssid));

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  randomSeed(micros());

  flipdot("WiFi connected\n" + WiFi.localIP());
  delay(4000);
  flipdot("");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String mededeling;
  for (unsigned int i = 0; i < length; i++) {
    mededeling = mededeling + ((char)payload[i]);
  }
  flipdot(mededeling);
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    // Create a random client ID
    String clientId = "Flipdot-";
    clientId += String(random(0xffff), HEX);
    // Attempt to connect
    if (client.connect(clientId.c_str())) {
      // Once connected, publish an announcement...
      client.publish("revdebug/flipdot", "hello world");
      // ... and resubscribe
      client.subscribe("revspace/flipdot");
    } else {
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void flipdot (String tekst) {
  String header = "aA11001\n";
  String footer = "\n\n\r";
  String bericht = header + tekst + footer;
  byte checksum = 0x7F;

  const char *p = bericht.c_str();

  for (size_t i = 0; i < strlen(p); i++) {
    checksum = checksum ^ p[i];
  }

  bericht = bericht + char(checksum);
  Serial.print(bericht);
}

void setup() {
  Serial.begin(1200, SERIAL_7E2);
  Serial.swap();
  delay(500);
  flipdot("Booting...");
  delay(500);
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();
}
