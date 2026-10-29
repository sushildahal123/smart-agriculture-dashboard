#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>

// ================= WIFI =================
#define WLAN_SSID "CJWiFi_A19B" //"Sushil"
#define WLAN_PASS "1505008332"////"123456789"

// ================= HIVEMQ =================
#define MQTT_HOST   "39d75f9fc43d497a9f30f8049a65f429.s1.eu.hivemq.cloud"
#define MQTT_PORT   8883
#define MQTT_USER   "SmartAgriculture"
#define MQTT_PASS   "Agriculture123"

// ================= PINS =================
#define soil    35
#define PIR     13
#define buzzer  27
#define relay   14
#define DHTPIN  25
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

// ================= MQTT CLIENT =================
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

// ================= TIMERS =================
unsigned long lastSoilTime  = 0;
unsigned long lastCloudTime = 0;
unsigned long lastDHTTime   = 0;

// ================= STATE =================
int moisture = 0;
bool pumpState = false;
bool lastMotionState = false;
float lastTemp = -999;
float lastHumidity = -999;

// ================= BUZZER =================
bool buzzerActive = false;
int beepCount  = 0;
bool beepState = false;
unsigned long beepTimer = 0;
unsigned long lastHeartbeat = 0;


// ================================================
//  MQTT CALLBACK — remote pump control
// ================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == "esp32/pump/control") {
    pumpState = (msg == "1");
    digitalWrite(relay, pumpState ? LOW : HIGH);
    Serial.print(">>> Remote pump command: ");
    Serial.println(pumpState ? "ON" : "OFF");
  }
}

// ================================================
//  SETUP
// ================================================
void setup() {
  Serial.begin(115200);

  pinMode(soil,   INPUT);
  pinMode(PIR,    INPUT_PULLDOWN);
  pinMode(buzzer, OUTPUT);
  pinMode(relay,  OUTPUT);
  digitalWrite(relay, HIGH);

  dht.begin();

  WiFi.begin(WLAN_SSID, WLAN_PASS);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());

  secureClient.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(60);
  mqtt.setBufferSize(512);
  mqtt.setCallback(mqttCallback);
}

// ================================================
//  LOOP
// ================================================
void loop() {

  if (!mqtt.connected()) MQTT_connect();
  mqtt.loop();
  if (millis() - lastHeartbeat > 3000) {
    mqtt.publish("esp32/status", "online");
    lastHeartbeat = millis();
  }
  


  readSoilAndControlPump();
  readPIR();
  handleBuzzer();
  sendCloudData();
  sendDHTData();
}

  

// ================================================
//  SOIL MOISTURE + AUTO PUMP CONTROL
//  Reads every 500ms
// ================================================
void readSoilAndControlPump() {
  if (millis() - lastSoilTime < 500) return;

  int raw = analogRead(soil);
  moisture = map(raw, 4095, 1880, 0, 100);
  moisture = constrain(moisture, 0, 100);

  if (moisture < 30) pumpState = true;
  else if (moisture > 60) pumpState = false;

  digitalWrite(relay, pumpState ? LOW : HIGH);

  Serial.print("Soil: ");       Serial.print(moisture);
  Serial.print("%  Motion: ");  Serial.print(lastMotionState ? "Detected" : "No Motion");
  Serial.print("  Pump: ");     Serial.print(pumpState ? "ON" : "OFF");
  Serial.print("  Humidity: "); Serial.print(lastHumidity);
  Serial.print("%  Temp: ");    Serial.print(lastTemp);
  Serial.println(" C");

  lastSoilTime = millis();
}

// ================================================
//  PIR — publishes instantly on change
// ================================================
void readPIR() {
  bool motion = digitalRead(PIR);

  if (motion != lastMotionState) {
    if (motion) {
      buzzerActive = true;
      beepCount    = 0;
      beepState    = false;
      beepTimer    = millis();
    }
    mqtt.publish("esp32/motion", motion ? "1" : "0", true); // retained = true
    lastMotionState = motion;
  }
}

// ================================================
//  BUZZER
// ================================================
void handleBuzzer() {
  if (!buzzerActive) return;
  unsigned long now = millis();

  if (!beepState) {
    if (now - beepTimer >= 200) {
      beepState = true;
      tone(buzzer, 4000);
      beepTimer = now;
    }
  } else {
    if (now - beepTimer >= 500) {
      noTone(buzzer);
      beepState = false;
      beepTimer = now;
      beepCount++;
      if (beepCount >= 5) { buzzerActive = false; noTone(buzzer); }
    }
  }
}

// ================================================
//  PUBLISH SOIL + PUMP + MOTION
//  All 3 sent together every 500ms — instant updates
// ================================================
void sendCloudData() {
  mqtt.publish("esp32/status", "1", true);
  if (millis() - lastCloudTime < 500) return;

  char buf[10];

  sprintf(buf, "%d", moisture);
  mqtt.publish("esp32/soil", buf, true);

  mqtt.publish("esp32/pump", pumpState ? "1" : "0", true);

  mqtt.publish("esp32/motion", lastMotionState ? "1" : "0", true);

  lastCloudTime = millis();
}

// ================================================
//  PUBLISH HUMIDITY every 2 seconds
// ================================================
void sendDHTData() {

  if (millis() - lastDHTTime < 2000) return;

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (!isnan(h) && !isnan(t)) {

    char buf[10];

    // ===== TEMPERATURE =====
    // publish only if changed by >= 0.5°C
    if (abs(t - lastTemp) >= 0.5) {

      sprintf(buf, "%.1f", t);
      mqtt.publish("esp32/temperature", buf, true);

      lastTemp = t;
    }

    // ===== HUMIDITY =====
    // publish only if changed by >= 1%
    if (abs(h - lastHumidity) >= 1.0) {

      sprintf(buf, "%.1f", h);
      mqtt.publish("esp32/humidity", buf, true);

      lastHumidity = h;
    }
  }

  lastDHTTime = millis();
}

// ================================================
//  MQTT CONNECT + RECONNECT
// ================================================
void MQTT_connect() {
  Serial.print("Connecting MQTT... ");

  while (!mqtt.connected()) {
    if (mqtt.connect("ESP32SmartFarm", MQTT_USER, MQTT_PASS)) {
      Serial.println("MQTT Connected!");
      mqtt.subscribe("esp32/pump/control");
    } else {
      Serial.print("Failed, rc=");
      Serial.print(mqtt.state());
      Serial.println("  retrying in 3s...");
      delay(3000);
    }
  }
}
