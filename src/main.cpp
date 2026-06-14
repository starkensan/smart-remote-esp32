#include <Arduino.h>
#include <ArduinoJson.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HomeSpan.h>

#include "config.h"
#include "IrCommandStore.h"

namespace {

constexpr size_t kCaptureBufferSize = 1024;
constexpr uint8_t kIrTimeoutMs = 50;
constexpr uint32_t kLearningTimeoutMs = 15000;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kShortPressMaxMs = 1000;

IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kIrTimeoutMs, true);
decode_results results;
ir_store::IrCommandStore irCommandStore;

uint32_t lastBlinkAt = 0;
bool ledState = false;
const char *learningCommandId = nullptr;
uint32_t learningStartedAt = 0;
bool buttonWasPressed = false;
uint32_t buttonPressedAt = 0;

void blinkStatus(uint32_t intervalMs) {
  if (STATUS_LED_PIN < 0) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastBlinkAt < intervalMs) {
    return;
  }

  lastBlinkAt = now;
  ledState = !ledState;
  digitalWrite(STATUS_LED_PIN, ledState ? HIGH : LOW);
}

void printHelp() {
  Serial.println("IR learning commands in HomeSpan CLI:");
  Serial.println("  o - learn light_on");
  Serial.println("  n - learn night_light");
  Serial.println("  q - show IR command status");
  Serial.println("  k - cancel active learning");
}

void startLearning(const char *commandId) {
  if (!irCommandStore.isValidCommandId(commandId)) {
    Serial.println("Unknown IR command id.");
    return;
  }

  learningCommandId = commandId;
  learningStartedAt = millis();
  Serial.printf("Learning %s. Press the matching remote button within %lu seconds.\n",
                learningCommandId, kLearningTimeoutMs / 1000);
}

void cancelLearning() {
  if (learningCommandId == nullptr) {
    Serial.println("No active learning session.");
    return;
  }

  Serial.printf("Canceled learning %s.\n", learningCommandId);
  learningCommandId = nullptr;
}

void startNextButtonLearning() {
  if (!irCommandStore.hasCommand(ir_store::kLightOnCommand)) {
    startLearning(ir_store::kLightOnCommand);
    return;
  }

  if (!irCommandStore.hasCommand(ir_store::kNightLightCommand)) {
    startLearning(ir_store::kNightLightCommand);
    return;
  }

  static bool learnLightOnNext = true;
  startLearning(learnLightOnNext ? ir_store::kLightOnCommand
                                 : ir_store::kNightLightCommand);
  learnLightOnNext = !learnLightOnNext;
}

void commandLearnLightOn(const char *) {
  startLearning(ir_store::kLightOnCommand);
}

void commandLearnNightLight(const char *) {
  startLearning(ir_store::kNightLightCommand);
}

void commandPrintIrStatus(const char *) {
  irCommandStore.printStatus(Serial);
}

void commandCancelLearning(const char *) {
  cancelLearning();
}

bool sendStoredRawCommand(const char *commandId) {
  ir_store::LearnedIrCommand command;
  if (!irCommandStore.loadCommand(commandId, command)) {
    Serial.printf("IR command %s is not learned yet.\n", commandId);
    return false;
  }

  Serial.printf("Sending %s with %u RAW timings at %u kHz.\n", commandId,
                command.rawLength, command.frequencyKhz);
  irsend.sendRaw(command.raw, command.rawLength, command.frequencyKhz);
  return true;
}

struct SmartRemoteLight : Service::LightBulb {
  SpanCharacteristic *power;

  SmartRemoteLight() : Service::LightBulb() {
    power = new Characteristic::On();
  }

  boolean update() override {
    const bool requestedOn = power->getNewVal();
    return sendStoredRawCommand(requestedOn ? ir_store::kLightOnCommand
                                           : ir_store::kNightLightCommand);
  }
};

void handleConfigButton() {
  const bool pressed = digitalRead(CONFIG_BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  if (pressed && !buttonWasPressed) {
    buttonWasPressed = true;
    buttonPressedAt = now;
    return;
  }

  if (!pressed && buttonWasPressed) {
    buttonWasPressed = false;
    const uint32_t pressMs = now - buttonPressedAt;
    if (pressMs >= kButtonDebounceMs && pressMs <= kShortPressMaxMs) {
      if (learningCommandId == nullptr) {
        startNextButtonLearning();
      } else {
        cancelLearning();
      }
    }
  }
}

void updateLastDecoded() {
  if (!irrecv.decode(&results)) {
    if (learningCommandId != nullptr &&
        millis() - learningStartedAt > kLearningTimeoutMs) {
      Serial.printf("Learning %s timed out. Existing command was not changed.\n",
                    learningCommandId);
      learningCommandId = nullptr;
    }
    return;
  }

  if (learningCommandId != nullptr) {
    const uint16_t rawLength = getCorrectedRawLength(&results);
    if (rawLength == 0 || rawLength > ir_store::kMaxRawTimings) {
      Serial.printf("Rejected %s capture. RAW length %u exceeds limit %u.\n",
                    learningCommandId, rawLength, ir_store::kMaxRawTimings);
    } else {
      uint16_t *raw = resultToRawArray(&results);
      if (raw != nullptr &&
          irCommandStore.saveCommand(learningCommandId, raw, rawLength)) {
        Serial.printf("Stored %s with %u RAW timings.\n", learningCommandId,
                      rawLength);
        irCommandStore.printStatus(Serial);
      } else {
        Serial.printf("Failed to store %s. Existing command was not changed.\n",
                      learningCommandId);
      }
      delete[] raw;
    }

    learningCommandId = nullptr;
    irrecv.resume();
    return;
  }

  Serial.printf("Received IR: protocol=%s value=%s bits=%u rawLength=%u\n",
                typeToString(results.decode_type).c_str(),
                uint64ToString(results.value, 16).c_str(), results.bits,
                results.rawlen);
  irrecv.resume();
}

void setupHomeSpan() {
  homeSpan.setLogLevel(1);
  homeSpan.setControlPin(CONFIG_BUTTON_PIN);
  homeSpan.setApSSID(SETUP_AP_SSID);
  homeSpan.setApPassword(SETUP_AP_PASSWORD);
  homeSpan.setApTimeout(300);
  homeSpan.begin(Category::Lighting, DEVICE_NAME);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Smart Remote Light");
      new Characteristic::Manufacturer("starkensan");
      new Characteristic::Model("XIAO ESP32S3 IR Remote");
      new Characteristic::FirmwareRevision("0.1.0");
    new SmartRemoteLight();

  new SpanUserCommand('o', " - learn light_on IR command", commandLearnLightOn);
  new SpanUserCommand('n', " - learn night_light IR command",
                      commandLearnNightLight);
  new SpanUserCommand('q', " - show learned IR command status",
                      commandPrintIrStatus);
  new SpanUserCommand('k', " - cancel active IR learning",
                      commandCancelLearning);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
  }
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  }
  if (!irCommandStore.begin()) {
    Serial.println("IR command store initialization failed.");
  }
  irCommandStore.printStatus(Serial);

  irsend.begin();
  irrecv.enableIRIn();

  setupHomeSpan();
  printHelp();
}

void loop() {
  homeSpan.poll();
  handleConfigButton();
  updateLastDecoded();
  blinkStatus(WiFi.status() == WL_CONNECTED ? 1000 : 150);
}
