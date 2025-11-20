/*
  IoT Smart Thermometer — Local + MQTT Cloud Connectivity
  Board : Arduino Uno WiFi Rev2 (NINA)
  Libs  : WiFiNINA, ArduinoMqttClient, Adafruit_MAX31856, Adafruit_GFX, Adafruit_ST7789, SPI

  Web UI: http://<device-ip>
  MQTT :
    Broker  : <your-mqtt-broker-address>:1883
    Topics  : smartthermo/<device-mac>/status
              smartthermo/<device-mac>/set_target_f
              smartthermo/<device-mac>/set_target_c
              smartthermo/<device-mac>/cmd
              smartthermo/<device-mac>/done  // one-shot when target first reached

  MAX31856 (Software SPI):
    D10 -> CS, D11 -> SDI (MOSI), D12 -> SDO (MISO), D13 -> SCK

  TFT ST7789 (Software SPI, Portrait):
    SCK=D9, MOSI=D8, CS=D7, DC=D6, RST=D4

  Green "Complete" LED:
    LED anode -> D5, LED cathode -> 220Ω -> GND (lights when tempF >= targetF)
*/

#include <SPI.h>
#include <WiFiNINA.h>
#include <ArduinoMqttClient.h>
#include <Adafruit_MAX31856.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "arduino_secrets.h"

// ---------- Unit helpers ----------
static inline float CtoF(float c){ return isnan(c) ? NAN : (c * 9.0f/5.0f + 32.0f); }
static inline float FtoC(float f){ return isnan(f) ? NAN : ((f - 32.0f) * 5.0f/9.0f); }

// ---------- Wi-Fi ----------
char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;
WiFiServer server(80);

// Non-blocking Wi-Fi reconnect
unsigned long lastWifiAttemptMs = 0;
const unsigned long WIFI_RETRY_MS = 10000;
bool webServerStarted = false;

// ---------- MQTT ----------
const char* BROKER = "<your-mqtt-broker-address>";
const int   PORT   = 1883;

WiFiClient  wifiClient;
MqttClient  mqttClient(wifiClient);
unsigned long lastMqttAttemptMs = 0;
const unsigned long MQTT_RETRY_MS = 10000;

char macHex[13] = {0};
String topicBase, topicStatus, topicSetTargetF, topicSetTargetC, topicCmd, topicDone;

// ---------- App state ----------
float targetF = 165.0;
String modeName = "Poultry";
unsigned long lastStatusMs = 0;
const unsigned long STATUS_EVERY_MS = 2000;

// One-shot DONE event state
bool   doneLatched = false;
const  float DONE_RESET_DELTA = 2.0;

// ---------- MAX31856 (Software SPI on 10/11/12/13) ----------
#define MAX_CS   10
#define MAX_DI   11
#define MAX_DO   12
#define MAX_CLK  13
Adafruit_MAX31856 maxthermo(MAX_CS, MAX_DI, MAX_DO, MAX_CLK);

// ---------- TFT ST7789 ----------
#define TFT_MOSI  8
#define TFT_SCLK  9
#define TFT_CS    7
#define TFT_DC    6
#define TFT_RST   4
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// ---------- LED ----------
#define LED_DONE 5

// ---- Display cadence ----
unsigned long lastDispMs = 0;
const unsigned long DISP_EVERY_MS = 250;

// ======= Temperature read (°C) =======
float readTempC() {
  maxthermo.setConversionMode(MAX31856_ONESHOT_NOWAIT);
  maxthermo.triggerOneShot();

  unsigned long t0 = millis();
  while (!maxthermo.conversionComplete()) {
    if (millis() - t0 > 400) return NAN;
    delay(5);
  }

  uint8_t f = maxthermo.readFault();
  if (f) return NAN;
  return (float)maxthermo.readThermocoupleTemperature();
}

// ======= Wi-Fi helpers =======
void printWifiStatus() {
  Serial.print("SSID: "); Serial.println(WiFi.SSID());
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: "); Serial.println(ip);
  Serial.print("signal strength (RSSI):");
  Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  Serial.print("Open browser: http://"); Serial.println(ip);
}

// Non-blocking reconnect
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!webServerStarted) {
      server.begin();
      webServerStarted = true;
      printWifiStatus();
    }
    return;
  }

  webServerStarted = false;
  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;

  Serial.print("WiFi reconnect attempt -> "); Serial.println(ssid);
  WiFi.begin(ssid, pass);
}

// ======= MQTT =======
void publishStatus(bool toSerial = false);

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = now;

  Serial.print("MQTT -> "); Serial.print(BROKER); Serial.print(":"); Serial.println(PORT);
  if (!mqttClient.connect(BROKER, PORT)) {
    Serial.print("MQTT fail err="); Serial.println(mqttClient.connectError());
    return;
  }
  Serial.println("MQTT OK");

  mqttClient.subscribe(topicSetTargetF);
  mqttClient.subscribe(topicSetTargetC);
  mqttClient.subscribe(topicCmd);

  Serial.print("Status topic: "); Serial.println(topicStatus);
  publishStatus(true);
  lastStatusMs = millis();
}

void publishDoneEvent(float tempF) {
  if (!mqttClient.connected()) return;
  mqttClient.beginMessage(topicDone);
  mqttClient.print('{');
  mqttClient.print("\"event\":\"done\"");
  mqttClient.print(",\"temp_f\":"); mqttClient.print(tempF, 1);
  mqttClient.print(",\"target_f\":"); mqttClient.print(targetF, 1);
  mqttClient.print(",\"mode\":\""); mqttClient.print(modeName); mqttClient.print('"');
  mqttClient.print('}');
  mqttClient.endMessage();
  Serial.println("PUB done event");
}

void publishStatus(bool toSerial) {
  float tC = readTempC();
  float tF = CtoF(tC);
  float tgtF = targetF;
  float tgtC = FtoC(targetF);

  if (mqttClient.connected()) {
    mqttClient.beginMessage(topicStatus);
    mqttClient.print('{');
    mqttClient.print("\"temp_f\":");  if (isnan(tF)) mqttClient.print("null"); else mqttClient.print(tF, 1);
    mqttClient.print(",\"temp_c\":"); if (isnan(tC)) mqttClient.print("null"); else mqttClient.print(tC, 2);
    mqttClient.print(",\"target_f\":"); mqttClient.print(tgtF, 1);
    mqttClient.print(",\"target_c\":"); mqttClient.print(tgtC, 2);
    mqttClient.print(",\"mode\":\""); mqttClient.print(modeName); mqttClient.print('"');
    if (WiFi.status() == WL_CONNECTED) {
      mqttClient.print(",\"ip\":\""); mqttClient.print(WiFi.localIP()); mqttClient.print('"');
      mqttClient.print(",\"rssi\":"); mqttClient.print(WiFi.RSSI());
    }
    mqttClient.print('}');
    mqttClient.endMessage();
  }

  if (toSerial) {
    Serial.print("PUB "); Serial.print(topicStatus);
    Serial.print("  tempF="); if (isnan(tF)) Serial.print("null"); else Serial.print(tF, 1);
    Serial.print("  targetF="); Serial.print(tgtF, 1);
    Serial.print("  mode="); Serial.print(modeName);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("  rssi="); Serial.println(WiFi.RSSI());
    } else {
      Serial.println("  (offline)");
    }
  }
}

static float extractJsonNumber(const String& payload, const char* key) {
  int i = payload.indexOf(key); if (i < 0) return NAN;
  int c = payload.indexOf(':', i); if (c < 0) return NAN;
  return payload.substring(c + 1).toFloat();
}

void onMqttMessage(int size) {
  String topic = mqttClient.messageTopic();
  String payload; payload.reserve(size);
  while (mqttClient.available()) payload += (char)mqttClient.read();

  float v = NAN;
  if (topic == topicSetTargetF || topic == topicCmd) {
    v = payload.toFloat();
    if ((v == 0.0f) && payload != "0") v = extractJsonNumber(payload, "target_f");
    if (isnan(v) && topic == topicCmd) {
      float vc = payload.toFloat();
      if ((vc == 0.0f) && payload != "0") vc = extractJsonNumber(payload, "target_c");
      if (!isnan(vc)) v = CtoF(vc);
    }
  } else if (topic == topicSetTargetC) {
    float vc = payload.toFloat();
    if ((vc == 0.0f) && payload != "0") vc = extractJsonNumber(payload, "target_c");
    if (!isnan(vc)) v = CtoF(vc);
  }

  if (!isnan(v) && v > -40 && v < 572) {
    targetF = v;
    modeName = "Custom";
    Serial.print("Target set via MQTT -> "); Serial.print(targetF); Serial.println(" °F");
    doneLatched = false;
  }
}

// ---- presets ----
void setPreset(const String& which) {
  if (which == "beef")        { targetF = 145.0; modeName = "Beef";    }
  else if (which == "poultry"){ targetF = 165.0; modeName = "Poultry"; }
  else if (which == "fish")   { targetF = 140.0; modeName = "Fish";    }
  else { modeName = "Custom"; }
  doneLatched = false;
}

// ======= HTTP =======
void handleHttp() {
  WiFiClient client = server.available();
  if (!client) return;

  String currentLine = "", firstLine = "";
  bool gotFirstLine = false;

  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      if (!gotFirstLine) { if (c == '\n') gotFirstLine = true; else firstLine += c; }

      if (c == '\n') {
        if (currentLine.length() == 0) {
          String path = "/";
          int sp = firstLine.indexOf(' ');
          if (sp >= 0) { int sp2 = firstLine.indexOf(' ', sp + 1); if (sp2 > sp) path = firstLine.substring(sp + 1, sp2); }

          if (path.startsWith("/set?")) {
            int ix = path.indexOf("target=");
            if (ix >= 0) {
              float v = path.substring(ix + 7).toFloat();
              if (!isnan(v) && v > -40 && v < 572) { targetF = v; modeName = "Custom"; doneLatched = false; }
            }
          }

          if (path.startsWith("/preset?")) {
            int ix = path.indexOf("mode=");
            if (ix >= 0) {
              String m = path.substring(ix + 5);
              int amp = m.indexOf('&'); if (amp > 0) m = m.substring(0, amp);
              m.toLowerCase(); setPreset(m);
            }
          }

          float tC = readTempC(), tF = CtoF(tC);

          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: text/html; charset=utf-8");
          client.println("Connection: close");
          client.println();
          client.println("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Thermo Controller</title></head><body style='font-family:system-ui,Arial'>");
          client.println("<h2>Smart Thermometer Controller</h2>");
          client.print("<p><b>Mode:</b> "); client.print(modeName); client.println("</p>");
          client.print("<p><b>Current Temp (°F):</b> "); if (isnan(tF)) client.print("&mdash;"); else client.print(tF, 1);
          client.print("<br><small>(°C: "); if (isnan(tC)) client.print("&mdash;"); else client.print(tC, 1); client.println(")</small>");
          client.print("<p><b>Target (°F):</b> "); client.print(targetF, 1);
          client.print("<br><small>(°C: "); client.print(FtoC(targetF), 1); client.println(")</small></p>");
          client.println("<form action='/set' method='get'>Set target °F: <input name='target' type='number' step='0.1'> <button>Apply</button></form>");
          client.println("<p>Presets: <a href='/preset?mode=beef'>Beef</a> | <a href='/preset?mode=poultry'>Poultry</a> | <a href='/preset?mode=fish'>Fish</a></p>");
          client.println("</body></html>");
          break;
        } else currentLine = "";
      } else if (c != '\r') currentLine += c;
    }
  }
  delay(1);
  client.stop();
}

// ======= TFT =======
int textPixelWidth(const String& s, uint8_t textSize) {
  return s.length() * 6 * textSize;
}

void drawCenteredText(int16_t cx, int16_t y, const String& s, uint8_t textSize, uint16_t color) {
  int w = textPixelWidth(s, textSize);
  int16_t x = cx - (w / 2);
  tft.setCursor(x, y);
  tft.setTextSize(textSize);
  tft.setTextColor(color);
  tft.print(s);
}

void tftInit() {
  tft.init(135, 240);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  tft.println("Smart");
  tft.setCursor(6, 24);
  tft.println("Thermo");
  tft.setTextSize(1);
  tft.setCursor(6, 62);    tft.print("Mode:");
  tft.setCursor(6, 196);   tft.print("Target F:");
}

void tftUpdate(float tempF, float tempC) {
  const int W = 135;
  const int centerX = W / 2;
  tft.fillRect(6, 74, 123, 16, ST77XX_BLACK);
  tft.setCursor(6, 76);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.print(modeName);
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 96, 135, 60, ST77XX_BLACK);
  String tempFStr = isnan(tempF) ? String("--.- F") : String(String(tempF, 1) + " F");
  drawCenteredText(centerX, 102, tempFStr, 3, ST77XX_YELLOW);
  tft.fillRect(0, 160, 135, 16, ST77XX_BLACK);
  String tempCStr = isnan(tempC) ? String("C: --.-") : String("C: " + String(tempC, 1));
  drawCenteredText(centerX, 160, tempCStr, 1, ST77XX_WHITE);
  tft.fillRect(78, 208, 150, 16, ST77XX_BLACK);
  tft.setCursor(78, 210);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.print(targetF, 1);
}

// ======= SETUP / LOOP =======
void setup() {
  Serial.begin(9600);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LED_DONE, OUTPUT);
  digitalWrite(LED_DONE, LOW);

  byte mac[6]; WiFi.macAddress(mac);
  sprintf(macHex, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  topicBase       = String("smartthermo/") + macHex;
  topicStatus     = topicBase + "/status";
  topicSetTargetF = topicBase + "/set_target_f";
  topicSetTargetC = topicBase + "/set_target_c";
  topicCmd        = topicBase + "/cmd";
  topicDone       = topicBase + "/done";

  if (maxthermo.begin()) maxthermo.setThermocoupleType(MAX31856_TCTYPE_K);
  else Serial.println("MAX31856 init failed (check wiring).");

  ensureWifi();
  mqttClient.onMessage(onMqttMessage);
  ensureMqtt();
  tftInit();
}

void loop() {
  ensureWifi();
  ensureMqtt();
  if (mqttClient.connected()) mqttClient.poll();

  if (mqttClient.connected() && (millis() - lastStatusMs >= STATUS_EVERY_MS)) {
    lastStatusMs = millis();
    publishStatus(true);
  }

  handleHttp();

  if (millis() - lastDispMs >= DISP_EVERY_MS) {
    lastDispMs = millis();
    float c = readTempC();
    float f = CtoF(c);
    tftUpdate(f, c);
    bool reached = (!isnan(f) && f >= targetF);
    if (reached && !doneLatched) {
      doneLatched = true;
      digitalWrite(LED_DONE, HIGH);
      publishDoneEvent(f);
    } else if (!isnan(f) && f <= (targetF - DONE_RESET_DELTA)) {
      doneLatched = false;
      digitalWrite(LED_DONE, LOW);
    } else {
      digitalWrite(LED_DONE, doneLatched ? HIGH : LOW);
    }
  }
}
