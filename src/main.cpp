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
constexpr uint8_t kIrTimeoutMs = 15;
constexpr uint32_t kLearningTimeoutMs = 15000;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kShortPressMaxMs = 1000;
constexpr uint32_t kIrClearPressMs = 7000;
constexpr uint16_t kIrSendRepeats = 3;
constexpr uint16_t kIrRepeatGapMs = 80;
constexpr uint8_t kRequiredStableCaptures = 2;
constexpr uint16_t kRawTimingToleranceUsec = 250;
constexpr uint8_t kRawTimingTolerancePercent = 25;
constexpr uint32_t kAutoSetupApDelayMs = 30000;

IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kIrTimeoutMs, true);
decode_results results;
ir_store::IrCommandStore irCommandStore;

uint32_t lastBlinkAt = 0;
uint32_t wifiDisconnectedSince = 0;
bool ledState = false;
bool autoSetupApStarted = false;
const char *learningCommandId = nullptr;
uint32_t learningStartedAt = 0;
ir_store::LearnedIrCommand learningCandidate;
uint8_t stableCaptureCount = 0;
bool buttonWasPressed = false;
uint32_t buttonPressedAt = 0;
bool buttonLongHandled = false;

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

void resetLearningCandidate() {
  learningCandidate = ir_store::LearnedIrCommand{};
  stableCaptureCount = 0;
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
  resetLearningCandidate();
  Serial.printf("Learning %s. Press the matching remote button %u times within %lu seconds.\n",
                learningCommandId, kRequiredStableCaptures,
                kLearningTimeoutMs / 1000);
}

void cancelLearning() {
  if (learningCommandId == nullptr) {
    Serial.println("No active learning session.");
    return;
  }

  Serial.printf("Canceled learning %s.\n", learningCommandId);
  learningCommandId = nullptr;
  resetLearningCandidate();
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

void clearLearnedIrCommands() {
  if (learningCommandId != nullptr) {
    learningCommandId = nullptr;
    resetLearningCandidate();
    Serial.println("Canceled active learning before clearing stored IR commands.");
  }

  if (irCommandStore.clear() && irCommandStore.begin()) {
    Serial.println("Cleared learned IR commands.");
  } else {
    Serial.println("Failed to clear learned IR commands.");
  }
  irCommandStore.printStatus(Serial);
}

void commandClearLearnedIr(const char *) {
  clearLearnedIrCommands();
}

void printRawPreview(const uint16_t *raw, size_t rawLength) {
  const size_t previewLength = rawLength < 16 ? rawLength : 16;
  Serial.print("  raw preview:");
  for (size_t i = 0; i < previewLength; i++) {
    Serial.printf(" %u", raw[i]);
  }
  if (rawLength > previewLength) {
    Serial.print(" ...");
  }
  Serial.println();
}

bool rawTimingsMatch(const uint16_t *expected, const uint16_t *actual,
                     size_t rawLength) {
  for (size_t i = 0; i < rawLength; i++) {
    const uint16_t a = expected[i];
    const uint16_t b = actual[i];
    const uint16_t diff = a > b ? a - b : b - a;
    const uint16_t percentLimit =
        static_cast<uint32_t>(a) * kRawTimingTolerancePercent / 100;
    const uint16_t limit = percentLimit > kRawTimingToleranceUsec
                               ? percentLimit
                               : kRawTimingToleranceUsec;
    if (diff > limit) {
      return false;
    }
  }
  return true;
}

void rememberLearningCandidate(const uint16_t *raw, size_t rawLength) {
  learningCandidate = ir_store::LearnedIrCommand{};
  learningCandidate.present = true;
  learningCandidate.frequencyKhz = ir_store::kDefaultFrequencyKhz;
  learningCandidate.rawLength = rawLength;
  for (size_t i = 0; i < rawLength; i++) {
    learningCandidate.raw[i] = raw[i];
  }
  stableCaptureCount = 1;
}

void averageLearningCandidate(const uint16_t *raw, size_t rawLength) {
  for (size_t i = 0; i < rawLength; i++) {
    learningCandidate.raw[i] =
        (static_cast<uint32_t>(learningCandidate.raw[i]) + raw[i]) / 2;
  }
}

void printStoredCommandDetails(const char *commandId) {
  ir_store::LearnedIrCommand command;
  if (!irCommandStore.loadCommand(commandId, command)) {
    Serial.printf("  %s: missing\n", commandId);
    return;
  }

  Serial.printf("  %s: stored, rawLength=%u, frequency=%u kHz\n", commandId,
                command.rawLength, command.frequencyKhz);
  printRawPreview(command.raw, command.rawLength);
}

void commandPrintIrDetails(const char *) {
  irCommandStore.printStatus(Serial);
  printStoredCommandDetails(ir_store::kLightOnCommand);
  printStoredCommandDetails(ir_store::kNightLightCommand);
}

bool sendStoredRawCommand(const char *commandId, uint16_t repeatCount = 1) {
  ir_store::LearnedIrCommand command;
  if (!irCommandStore.loadCommand(commandId, command)) {
    Serial.printf("IR command %s is not learned yet.\n", commandId);
    return false;
  }

  Serial.printf("Sending %s with %u RAW timings at %u kHz, repeats=%u.\n",
                commandId, command.rawLength, command.frequencyKhz,
                repeatCount);
  irrecv.disableIRIn();
  for (uint16_t i = 0; i < repeatCount; i++) {
    irsend.sendRaw(command.raw, command.rawLength, command.frequencyKhz);
    if (i + 1 < repeatCount) {
      delay(kIrRepeatGapMs);
    }
  }
  irrecv.enableIRIn();
  return true;
}

void commandSendLightOn(const char *) {
  sendStoredRawCommand(ir_store::kLightOnCommand, kIrSendRepeats);
}

void commandSendNightLight(const char *) {
  sendStoredRawCommand(ir_store::kNightLightCommand, kIrSendRepeats);
}

void handleAutoSetupAp() {
  if (autoSetupApStarted || WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedSince = 0;
    return;
  }

  const uint32_t now = millis();
  if (wifiDisconnectedSince == 0) {
    wifiDisconnectedSince = now;
    return;
  }

  if (now - wifiDisconnectedSince < kAutoSetupApDelayMs) {
    return;
  }

  autoSetupApStarted = true;
  Serial.printf("Wi-Fi not connected for %lu seconds. Starting setup AP %s.\n",
                kAutoSetupApDelayMs / 1000, SETUP_AP_SSID);
  homeSpan.processSerialCommand("A");
}

struct SmartRemoteLight : Service::LightBulb {
  SpanCharacteristic *power;

  SmartRemoteLight() : Service::LightBulb() {
    power = new Characteristic::On();
  }

  boolean update() override {
    const bool requestedOn = power->getNewVal();
    return sendStoredRawCommand(requestedOn ? ir_store::kLightOnCommand
                                           : ir_store::kNightLightCommand,
                                 kIrSendRepeats);
  }
};

void handleConfigButton() {
  const bool pressed = digitalRead(CONFIG_BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  if (pressed && !buttonWasPressed) {
    buttonWasPressed = true;
    buttonPressedAt = now;
    buttonLongHandled = false;
    return;
  }

  if (pressed && buttonWasPressed && !buttonLongHandled &&
      now - buttonPressedAt >= kIrClearPressMs) {
    buttonLongHandled = true;
    clearLearnedIrCommands();
    return;
  }

  if (!pressed && buttonWasPressed) {
    buttonWasPressed = false;
    const uint32_t pressMs = now - buttonPressedAt;
    if (!buttonLongHandled && pressMs >= kButtonDebounceMs &&
        pressMs <= kShortPressMaxMs) {
      if (learningCommandId == nullptr) {
        startNextButtonLearning();
      } else {
        cancelLearning();
      }
    }
    buttonLongHandled = false;
  }
}

void updateLastDecoded() {
  if (!irrecv.decode(&results)) {
    if (learningCommandId != nullptr &&
        millis() - learningStartedAt > kLearningTimeoutMs) {
      Serial.printf("Learning %s timed out. Existing command was not changed.\n",
                    learningCommandId);
      learningCommandId = nullptr;
      resetLearningCandidate();
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
      if (raw == nullptr) {
        Serial.printf("Failed to store %s. Existing command was not changed.\n",
                      learningCommandId);
      } else {
        Serial.printf("Captured %s: protocol=%s value=%s bits=%u rawLength=%u\n",
                      learningCommandId,
                      typeToString(results.decode_type).c_str(),
                      uint64ToString(results.value, 16).c_str(), results.bits,
                      rawLength);
        printRawPreview(raw, rawLength);

        if (!learningCandidate.present ||
            learningCandidate.rawLength != rawLength ||
            !rawTimingsMatch(learningCandidate.raw, raw, rawLength)) {
          rememberLearningCandidate(raw, rawLength);
          Serial.printf("Captured candidate 1/%u. Press the same button again.\n",
                        kRequiredStableCaptures);
        } else {
          averageLearningCandidate(raw, rawLength);
          stableCaptureCount++;
          if (stableCaptureCount < kRequiredStableCaptures) {
            Serial.printf("Captured candidate %u/%u. Press the same button again.\n",
                          stableCaptureCount, kRequiredStableCaptures);
          } else if (irCommandStore.saveCommand(
                         learningCommandId, learningCandidate.raw,
                         learningCandidate.rawLength,
                         learningCandidate.frequencyKhz)) {
            Serial.printf("Stored %s with stable RAW length %u.\n",
                          learningCommandId, learningCandidate.rawLength);
            printRawPreview(learningCandidate.raw,
                            learningCandidate.rawLength);
            irCommandStore.printStatus(Serial);
            learningCommandId = nullptr;
            resetLearningCandidate();
          } else {
            Serial.printf("Failed to store %s. Existing command was not changed.\n",
                          learningCommandId);
          }
        }
      }
      delete[] raw;
    }

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
  homeSpan.enableAutoStartAP();
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
  new SpanUserCommand('p', " - show learned IR command details",
                      commandPrintIrDetails);
  new SpanUserCommand('O', " - send light_on IR command 3 times",
                      commandSendLightOn);
  new SpanUserCommand('N', " - send night_light IR command 3 times",
                      commandSendNightLight);
  new SpanUserCommand('k', " - cancel active IR learning",
                      commandCancelLearning);
  new SpanUserCommand('y', " - clear learned IR commands",
                      commandClearLearnedIr);
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
  handleAutoSetupAp();
  handleConfigButton();
  updateLastDecoded();
  blinkStatus(WiFi.status() == WL_CONNECTED ? 1000 : 150);
}
