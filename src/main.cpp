#include <Arduino.h>
#include <ArduinoJson.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"

namespace {

constexpr size_t kCaptureBufferSize = 1024;
constexpr uint8_t kIrTimeoutMs = 50;

IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kIrTimeoutMs, true);
decode_results results;
WebServer server(HTTP_PORT);

String lastDecodedJson = "{}";
uint32_t lastBlinkAt = 0;
bool ledState = false;

void blinkStatus(uint32_t intervalMs) {
  const uint32_t now = millis();
  if (now - lastBlinkAt < intervalMs) {
    return;
  }

  lastBlinkAt = now;
  ledState = !ledState;
  digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_NAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);
  for (uint8_t i = 0; i < 60; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Connected. IP address: %s\n", WiFi.localIP().toString().c_str());
      return true;
    }
    blinkStatus(250);
    delay(250);
  }

  Serial.println("Wi-Fi connection failed.");
  return false;
}

String contentTypeFor(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  return "text/plain";
}

bool serveFile(const String &path) {
  String filePath = path == "/" ? "/index.html" : path;
  if (!LittleFS.exists(filePath)) {
    return false;
  }

  File file = LittleFS.open(filePath, "r");
  server.streamFile(file, contentTypeFor(filePath));
  file.close();
  return true;
}

void sendJson(int status, const JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  server.send(status, "application/json", body);
}

void handleStatus() {
  JsonDocument doc;
  doc["device"] = DEVICE_NAME;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["irSendPin"] = IR_SEND_PIN;
  doc["irRecvPin"] = IR_RECV_PIN;
  doc["lastDecoded"] = serialized(lastDecodedJson);
  sendJson(200, doc);
}

void handleSend() {
  JsonDocument req;
  const DeserializationError error = deserializeJson(req, server.arg("plain"));
  if (error) {
    JsonDocument doc;
    doc["error"] = "invalid_json";
    sendJson(400, doc);
    return;
  }

  const char *protocol = req["protocol"] | "NEC";
  const uint64_t value = strtoull((req["value"] | "0"), nullptr, 0);
  const uint16_t bits = req["bits"] | 32;
  const uint16_t repeat = req["repeat"] | 0;

  if (strcasecmp(protocol, "NEC") == 0) {
    irsend.sendNEC(value, bits, repeat);
  } else if (strcasecmp(protocol, "SONY") == 0) {
    irsend.sendSony(value, bits, repeat);
  } else if (strcasecmp(protocol, "PANASONIC") == 0) {
    const uint16_t address = req["address"] | 0x4004;
    irsend.sendPanasonic(address, value);
  } else if (strcasecmp(protocol, "RAW") == 0) {
    JsonArray raw = req["raw"].as<JsonArray>();
    if (raw.isNull() || raw.size() == 0 || raw.size() > 512) {
      JsonDocument doc;
      doc["error"] = "raw_array_required";
      sendJson(400, doc);
      return;
    }

    uint16_t timings[512];
    size_t count = 0;
    for (JsonVariant item : raw) {
      timings[count++] = item.as<uint16_t>();
    }
    const uint16_t frequency = req["frequency"] | 38;
    irsend.sendRaw(timings, count, frequency);
  } else {
    JsonDocument doc;
    doc["error"] = "unsupported_protocol";
    sendJson(400, doc);
    return;
  }

  JsonDocument doc;
  doc["ok"] = true;
  doc["protocol"] = protocol;
  sendJson(200, doc);
}

void handleNotFound() {
  if (serveFile(server.uri())) {
    return;
  }

  JsonDocument doc;
  doc["error"] = "not_found";
  doc["path"] = server.uri();
  sendJson(404, doc);
}

void updateLastDecoded() {
  if (!irrecv.decode(&results)) {
    return;
  }

  JsonDocument doc;
  doc["protocol"] = typeToString(results.decode_type);
  doc["value"] = uint64ToString(results.value, 16);
  doc["bits"] = results.bits;
  doc["rawLength"] = results.rawlen;
  doc["timestampMs"] = millis();

  String json;
  serializeJson(doc, json);
  lastDecodedJson = json;
  Serial.println(json);
  irrecv.resume();
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() { serveFile("/index.html"); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/send", HTTP_POST, handleSend);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("HTTP server started on port %d\n", HTTP_PORT);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  }

  irsend.begin();
  irrecv.enableIRIn();

  connectWiFi();
  setupRoutes();
}

void loop() {
  server.handleClient();
  updateLastDecoded();
  blinkStatus(WiFi.status() == WL_CONNECTED ? 1000 : 150);
}
