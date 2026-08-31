#include <Arduino.h>
#include <ArduinoJson.h>
#include <IRac.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <HomeSpan.h>
#include <esp_system.h>
#include <ir_Sharp.h>

#include "config.h"
#include "IrCommandStore.h"

namespace {

constexpr size_t kCaptureBufferSize = 1024;
constexpr uint8_t kIrTimeoutMs = 15;
constexpr uint32_t kLearningTimeoutMs = 15000;
constexpr uint32_t kButtonDebounceMs = 30;
constexpr uint32_t kShortPressMaxMs = 1000;
constexpr uint32_t kHomeKitUnpairPressMs = 7000;
constexpr uint16_t kIrSendRepeats = 3;
constexpr uint16_t kIrRepeatGapMs = 80;
constexpr uint8_t kRequiredStableCaptures = 2;
constexpr uint16_t kRawTimingToleranceUsec = 250;
constexpr uint8_t kRawTimingTolerancePercent = 25;
constexpr uint32_t kAutoSetupApDelayMs = 30000;
constexpr uint8_t kAcDisplayMinTempC = 17;
constexpr uint8_t kAcDisplayMaxTempC = 32;
constexpr uint8_t kAcDisplayTempOffsetC = 2;
constexpr uint16_t kAcSwingFrameGapMs = 80;
constexpr uint16_t kApiServerPort = 8080;
constexpr size_t kApiTokenBytes = 24;
constexpr size_t kSetupApPasswordLength = 16;
constexpr char kAuthorizationHeader[] = "Authorization";

// A907 baseline captured at cool 25C, fan auto, fixed direction 2.
constexpr uint8_t kSharpAcStateTemplate[kSharpAcStateLength] = {
    0xAA, 0x5A, 0xCF, 0x10, 0x08, 0x31, 0x22,
    0x00, 0x0A, 0xA0, 0x00, 0xE4, 0xC1,
};

enum class AcMode : uint8_t {
  Cool,
  Heat,
  Dry,
};

enum class AcChange : uint8_t {
  PowerOrMode,
  Temperature,
  Fan,
  Direction,
};

struct AcState {
  bool power = false;
  AcMode mode = AcMode::Cool;
  uint8_t coolTempC = 25;
  uint8_t heatTempC = 25;
  uint8_t fanLevel = 0;   // 0=auto, 1..4=remote fan levels.
  uint8_t direction = 0;  // 0=auto, 1..5=fixed top-to-bottom.
  bool fullSwing = false;
};

constexpr uint8_t kAcFanCodes[] = {2, 3, 5, 7, 6};

IRsend irsend(IR_SEND_PIN);
IRSharpAc sharpAc(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN, kCaptureBufferSize, kIrTimeoutMs, true);
decode_results results;
ir_store::IrCommandStore irCommandStore;
Preferences acPreferences;
Preferences credentialPreferences;
WebServer apiServer(kApiServerPort);
AcState acState;
bool acPreferencesReady = false;
bool apiServerStarted = false;
bool lightPowerState = false;
String apiBearerToken;
String setupApPassword;
String homeKitPairingCode;

SpanCharacteristic *lightPowerCharacteristic = nullptr;
SpanCharacteristic *acActiveCharacteristic = nullptr;
SpanCharacteristic *acCurrentTemperatureCharacteristic = nullptr;
SpanCharacteristic *acCurrentStateCharacteristic = nullptr;
SpanCharacteristic *acTargetStateCharacteristic = nullptr;
SpanCharacteristic *acCoolingTemperatureCharacteristic = nullptr;
SpanCharacteristic *acHeatingTemperatureCharacteristic = nullptr;
SpanCharacteristic *acFanCharacteristic = nullptr;
SpanCharacteristic *acDryCharacteristic = nullptr;
SpanCharacteristic *acSlatStateCharacteristic = nullptr;
SpanCharacteristic *acSwingCharacteristic = nullptr;
SpanCharacteristic *acCurrentTiltCharacteristic = nullptr;
SpanCharacteristic *acTargetTiltCharacteristic = nullptr;

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

String randomString(const char *alphabet, size_t length) {
  String value;
  value.reserve(length);
  const size_t alphabetLength = strlen(alphabet);
  for (size_t i = 0; i < length; i++) {
    value += alphabet[esp_random() % alphabetLength];
  }
  return value;
}

bool isAllowedHomeKitCode(const String &code) {
  static const char *const disallowed[] = {
      "00000000", "11111111", "22222222", "33333333",
      "44444444", "55555555", "66666666", "77777777",
      "88888888", "99999999", "12345678", "87654321",
  };
  for (const char *value : disallowed) {
    if (code == value) {
      return false;
    }
  }
  return true;
}

String generateHomeKitCode() {
  String code;
  do {
    code = randomString("0123456789", 8);
  } while (!isAllowedHomeKitCode(code));
  return code;
}

void loadDeviceCredentials() {
  if (!credentialPreferences.begin("device-auth", false)) {
    Serial.println("Failed to open device credential storage; using temporary credentials.");
    apiBearerToken = randomString("0123456789abcdef", kApiTokenBytes * 2);
    setupApPassword = randomString(
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789",
        kSetupApPasswordLength);
    homeKitPairingCode = generateHomeKitCode();
    return;
  }

  apiBearerToken = credentialPreferences.getString("apiToken", "");
  if (apiBearerToken.length() != kApiTokenBytes * 2) {
    apiBearerToken = randomString("0123456789abcdef", kApiTokenBytes * 2);
    credentialPreferences.putString("apiToken", apiBearerToken);
  }

  setupApPassword = credentialPreferences.getString("apPassword", "");
  if (setupApPassword.length() != kSetupApPasswordLength) {
    setupApPassword = randomString(
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789",
        kSetupApPasswordLength);
    credentialPreferences.putString("apPassword", setupApPassword);
  }

  homeKitPairingCode = credentialPreferences.getString("homeKitCode", "");
  if (homeKitPairingCode.length() != 8 ||
      !isAllowedHomeKitCode(homeKitPairingCode)) {
    homeKitPairingCode = generateHomeKitCode();
    credentialPreferences.putString("homeKitCode", homeKitPairingCode);
  }
}

void printDeviceCredentials() {
  Serial.println("Local device credentials (do not publish):");
  Serial.printf("  Setup AP password: %s\n", setupApPassword.c_str());
  Serial.printf("  HomeKit pairing code: %.3s-%.2s-%.3s\n",
                homeKitPairingCode.c_str(), homeKitPairingCode.c_str() + 3,
                homeKitPairingCode.c_str() + 5);
  Serial.printf("  API bearer token: %s\n", apiBearerToken.c_str());
}

uint8_t clampAcTemperature(uint8_t temperature) {
  return constrain(temperature, kAcDisplayMinTempC, kAcDisplayMaxTempC);
}

uint8_t acModeCode(AcMode mode) {
  switch (mode) {
    case AcMode::Heat:
      return kSharpAcHeat;
    case AcMode::Dry:
      return kSharpAcDry;
    case AcMode::Cool:
    default:
      return kSharpAcCool;
  }
}

uint8_t selectedAcTemperature() {
  return acState.mode == AcMode::Heat ? acState.heatTempC
                                      : acState.coolTempC;
}

uint8_t acSpecialCode(AcChange change) {
  switch (change) {
    case AcChange::Temperature:
      return kSharpAcSpecialTempEcono;
    case AcChange::Fan:
      return kSharpAcSpecialFan;
    case AcChange::Direction:
      return kSharpAcSpecialSwing;
    case AcChange::PowerOrMode:
    default:
      return kSharpAcSpecialPower;
  }
}

void buildSharpAcState(uint8_t output[kSharpAcStateLength], AcChange change,
                       uint8_t directionCode) {
  memcpy(output, kSharpAcStateTemplate, kSharpAcStateLength);

  output[4] = acState.mode == AcMode::Dry
                  ? 0
                  : clampAcTemperature(selectedAcTemperature()) -
                        kAcDisplayTempOffsetC - kSharpAcMinTemp;
  output[5] = acState.power ? 0x31 : 0x21;

  const uint8_t fanCode =
      acState.mode == AcMode::Dry
          ? kSharpAcFanAuto
          : kAcFanCodes[constrain(acState.fanLevel, 0, 4)];
  output[6] = static_cast<uint8_t>((fanCode << 4) | acModeCode(acState.mode));
  output[8] = static_cast<uint8_t>(0x08 | (directionCode & 0x07));
  output[10] = acSpecialCode(change);
  // The low nibble is a fixed protocol marker. IRSharpAc::getRaw() replaces
  // only the high checksum nibble, so preserve the captured 0x1 marker.
  output[12] = 0x01;

  sharpAc.setRaw(output);
  memcpy(output, sharpAc.getRaw(), kSharpAcStateLength);
}

void printSentAcState(const uint8_t state[kSharpAcStateLength]) {
  Serial.print("  state[13] = {");
  for (uint8_t i = 0; i < kSharpAcStateLength; i++) {
    Serial.printf("%s0x%02X", i == 0 ? "" : ", ", state[i]);
  }
  Serial.println("};");
}

bool sendSharpAcFrame(AcChange change, uint8_t directionCode) {
  uint8_t state[kSharpAcStateLength];
  buildSharpAcState(state, change, directionCode);
  sharpAc.setRaw(state);
  printSentAcState(state);
  sharpAc.send();
  return true;
}

bool sendSharpAcState(AcChange change) {
  Serial.printf(
      "Sending SHARP_AC: power=%s mode=%u temp=%uC fan=%u direction=%s%u.\n",
      acState.power ? "on" : "off", static_cast<uint8_t>(acState.mode),
      selectedAcTemperature(), acState.fanLevel,
      acState.fullSwing ? "swing/" : "", acState.direction);

  irrecv.disableIRIn();
  bool sent = true;
  if (change == AcChange::Direction && acState.fullSwing) {
    sent = sendSharpAcFrame(change, 5);
    delay(kAcSwingFrameGapMs);
    sent = sendSharpAcFrame(change, kSharpAcSwingVToggle) && sent;
  } else {
    const uint8_t directionCode =
        acState.fullSwing ? kSharpAcSwingVToggle : acState.direction;
    sent = sendSharpAcFrame(change, directionCode);
  }
  irrecv.enableIRIn();
  return sent;
}

void saveAcState() {
  if (!acPreferencesReady) {
    return;
  }
  acPreferences.putBool("power", acState.power);
  acPreferences.putUChar("mode", static_cast<uint8_t>(acState.mode));
  acPreferences.putUChar("coolTemp", acState.coolTempC);
  acPreferences.putUChar("heatTemp", acState.heatTempC);
  acPreferences.putUChar("fan", acState.fanLevel);
  acPreferences.putUChar("direction", acState.direction);
  acPreferences.putBool("swing", acState.fullSwing);
}

void loadAcState() {
  acPreferencesReady = acPreferences.begin("sharp-ac", false);
  if (!acPreferencesReady) {
    Serial.println("Failed to open SHARP_AC preferences.");
    return;
  }

  acState.power = acPreferences.getBool("power", false);
  const uint8_t storedMode = acPreferences.getUChar("mode", 0);
  acState.mode = storedMode <= static_cast<uint8_t>(AcMode::Dry)
                     ? static_cast<AcMode>(storedMode)
                     : AcMode::Cool;
  acState.coolTempC = clampAcTemperature(
      acPreferences.getUChar("coolTemp", acState.coolTempC));
  acState.heatTempC = clampAcTemperature(
      acPreferences.getUChar("heatTemp", acState.heatTempC));
  acState.fanLevel = constrain(acPreferences.getUChar("fan", 0), 0, 4);
  acState.direction =
      constrain(acPreferences.getUChar("direction", 0), 0, 5);
  acState.fullSwing = acPreferences.getBool("swing", false);
}

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
  Serial.println("  @o - learn light_on");
  Serial.println("  @n - learn night_light");
  Serial.println("  @a - send current SHARP_AC state");
  Serial.println("  @q - show IR command status");
  Serial.println("  @p - show IR command details");
  Serial.println("  @O - send light_on 3 times");
  Serial.println("  @N - send night_light 3 times");
  Serial.println("  @k - cancel active learning");
  Serial.println("  @y - clear learned IR commands");
  Serial.println("  @z - show local device credentials");
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

void unpairHomeKit() {
  if (learningCommandId != nullptr) {
    learningCommandId = nullptr;
    resetLearningCandidate();
  }
  Serial.println(
      "Deleting HomeKit pairing data. Wi-Fi, IR learning, and AC settings "
      "will be preserved.");
  homeSpan.processSerialCommand("U");
  delay(500);
  homeSpan.processSerialCommand("R");
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

void printAcStateDetails(const decode_results &decoded) {
  if (!hasACState(decoded.decode_type) || decoded.bits == 0) {
    return;
  }

  const uint16_t stateLength = (decoded.bits + 7) / 8;
  Serial.printf("  state[%u] = {", stateLength);
  for (uint16_t i = 0; i < stateLength; i++) {
    Serial.printf("%s0x%02X", i == 0 ? "" : ", ", decoded.state[i]);
  }
  Serial.println("};");

  const String description = IRAcUtils::resultAcToString(&decoded);
  if (!description.isEmpty()) {
    Serial.printf("  description: %s\n", description.c_str());
  }
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

void commandSendSharpAcState(const char *) {
  sendSharpAcState(AcChange::PowerOrMode);
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
    lightPowerCharacteristic = power;
    lightPowerState = power->getVal();
  }

  boolean update() override {
    const bool requestedOn = power->getNewVal();
    const bool sent =
        sendStoredRawCommand(requestedOn ? ir_store::kLightOnCommand
                                         : ir_store::kNightLightCommand,
                             kIrSendRepeats);
    if (sent) {
      lightPowerState = requestedOn;
    }
    return sent;
  }
};

int acDirectionToTilt(uint8_t direction) {
  constexpr int kDirectionTilts[] = {-90, -60, -30, 0, 30, 60};
  return kDirectionTilts[constrain(direction, 0, 5)];
}

uint8_t acTiltToDirection(int tilt) {
  if (tilt < -75) return 0;
  if (tilt < -45) return 1;
  if (tilt < -15) return 2;
  if (tilt < 15) return 3;
  if (tilt < 45) return 4;
  return 5;
}

uint8_t currentHeaterCoolerState() {
  if (!acState.power) {
    return 0;  // INACTIVE
  }
  if (acState.mode == AcMode::Heat) {
    return 2;  // HEATING
  }
  if (acState.mode == AcMode::Cool) {
    return 3;  // COOLING
  }
  return 1;  // IDLE (HomeKit has no dry state).
}

void syncAcCharacteristics() {
  if (acActiveCharacteristic != nullptr &&
      !acActiveCharacteristic->updated()) {
    acActiveCharacteristic->setVal(acState.power);
  }
  if (acCurrentTemperatureCharacteristic != nullptr &&
      !acCurrentTemperatureCharacteristic->updated()) {
    // No room sensor is installed, so mirror the selected setpoint.
    acCurrentTemperatureCharacteristic->setVal(selectedAcTemperature());
  }
  if (acCurrentStateCharacteristic != nullptr &&
      !acCurrentStateCharacteristic->updated()) {
    acCurrentStateCharacteristic->setVal(currentHeaterCoolerState());
  }
  if (acTargetStateCharacteristic != nullptr &&
      !acTargetStateCharacteristic->updated() && acState.mode != AcMode::Dry) {
    acTargetStateCharacteristic->setVal(acState.mode == AcMode::Heat ? 1 : 2);
  }
  if (acCoolingTemperatureCharacteristic != nullptr &&
      !acCoolingTemperatureCharacteristic->updated()) {
    acCoolingTemperatureCharacteristic->setVal(acState.coolTempC);
  }
  if (acHeatingTemperatureCharacteristic != nullptr &&
      !acHeatingTemperatureCharacteristic->updated()) {
    acHeatingTemperatureCharacteristic->setVal(acState.heatTempC);
  }
  if (acFanCharacteristic != nullptr && !acFanCharacteristic->updated()) {
    acFanCharacteristic->setVal(acState.fanLevel * 25);
  }
  if (acDryCharacteristic != nullptr && !acDryCharacteristic->updated()) {
    acDryCharacteristic->setVal(acState.power && acState.mode == AcMode::Dry);
  }
  if (acSlatStateCharacteristic != nullptr &&
      !acSlatStateCharacteristic->updated()) {
    acSlatStateCharacteristic->setVal(acState.fullSwing ? 2 : 0);
  }
  if (acSwingCharacteristic != nullptr && !acSwingCharacteristic->updated()) {
    acSwingCharacteristic->setVal(acState.fullSwing);
  }
  const int tilt = acDirectionToTilt(acState.direction);
  if (acCurrentTiltCharacteristic != nullptr &&
      !acCurrentTiltCharacteristic->updated()) {
    acCurrentTiltCharacteristic->setVal(tilt);
  }
  if (acTargetTiltCharacteristic != nullptr &&
      !acTargetTiltCharacteristic->updated()) {
    acTargetTiltCharacteristic->setVal(tilt);
  }
}

bool commitAcState(AcChange change, bool transmit) {
  const bool sent = !transmit || sendSharpAcState(change);
  if (sent) {
    saveAcState();
    syncAcCharacteristics();
  }
  return sent;
}

struct AirConditionerHeaterCooler : Service::HeaterCooler {
  SpanCharacteristic *active;
  SpanCharacteristic *currentTemperature;
  SpanCharacteristic *currentState;
  SpanCharacteristic *targetState;
  SpanCharacteristic *coolingTemperature;
  SpanCharacteristic *heatingTemperature;
  SpanCharacteristic *fan;

  AirConditionerHeaterCooler() : Service::HeaterCooler() {
    new Characteristic::Name("Air Conditioner");
    active = new Characteristic::Active(acState.power);
    currentTemperature =
        new Characteristic::CurrentTemperature(selectedAcTemperature());
    currentState =
        new Characteristic::CurrentHeaterCoolerState(currentHeaterCoolerState());
    targetState = new Characteristic::TargetHeaterCoolerState(
        acState.mode == AcMode::Heat ? 1 : 2);
    targetState->setValidValues(2, 1, 2);
    coolingTemperature =
        new Characteristic::CoolingThresholdTemperature(acState.coolTempC);
    coolingTemperature->setRange(kAcDisplayMinTempC, kAcDisplayMaxTempC, 1);
    heatingTemperature =
        new Characteristic::HeatingThresholdTemperature(acState.heatTempC);
    heatingTemperature->setRange(kAcDisplayMinTempC, kAcDisplayMaxTempC, 1);
    fan = new Characteristic::RotationSpeed(acState.fanLevel * 25);
    fan->setRange(0, 100, 25);
    new Characteristic::TemperatureDisplayUnits(0);

    acActiveCharacteristic = active;
    acCurrentTemperatureCharacteristic = currentTemperature;
    acCurrentStateCharacteristic = currentState;
    acTargetStateCharacteristic = targetState;
    acCoolingTemperatureCharacteristic = coolingTemperature;
    acHeatingTemperatureCharacteristic = heatingTemperature;
    acFanCharacteristic = fan;
  }

  boolean update() override {
    bool shouldTransmit = false;
    bool powerOrModeChanged = false;
    bool fanChanged = false;

    if (targetState->updated()) {
      acState.mode = targetState->getNewVal() == 1 ? AcMode::Heat : AcMode::Cool;
      shouldTransmit = acState.power;
      powerOrModeChanged = true;
    }
    if (coolingTemperature->updated()) {
      acState.coolTempC = clampAcTemperature(
          static_cast<uint8_t>(coolingTemperature->getNewVal()));
      if (acState.power && acState.mode == AcMode::Cool) {
        shouldTransmit = true;
      }
    }
    if (heatingTemperature->updated()) {
      acState.heatTempC = clampAcTemperature(
          static_cast<uint8_t>(heatingTemperature->getNewVal()));
      if (acState.power && acState.mode == AcMode::Heat) {
        shouldTransmit = true;
      }
    }
    if (fan->updated()) {
      const float requestedSpeed = fan->getNewVal();
      acState.fanLevel = constrain(
          static_cast<int>((requestedSpeed + 12.5f) / 25.0f), 0, 4);
      if (acState.power && acState.mode != AcMode::Dry) {
        shouldTransmit = true;
        fanChanged = true;
      }
    }
    if (active->updated()) {
      acState.power = active->getNewVal();
      if (acState.power && acState.mode == AcMode::Dry) {
        acState.mode = targetState->getVal() == 1 ? AcMode::Heat : AcMode::Cool;
      }
      shouldTransmit = true;
      powerOrModeChanged = true;
    }

    const AcChange change =
        powerOrModeChanged
            ? AcChange::PowerOrMode
            : fanChanged ? AcChange::Fan : AcChange::Temperature;
    return commitAcState(change, shouldTransmit);
  }
};

struct AirConditionerDrySwitch : Service::Switch {
  SpanCharacteristic *dry;

  AirConditionerDrySwitch() : Service::Switch() {
    new Characteristic::Name("Air Conditioner Dry");
    dry =
        new Characteristic::On(acState.power && acState.mode == AcMode::Dry);
    acDryCharacteristic = dry;
  }

  boolean update() override {
    const bool requestedDry = dry->getNewVal();
    if (requestedDry) {
      acState.mode = AcMode::Dry;
      acState.power = true;
      return commitAcState(AcChange::PowerOrMode, true);
    }
    if (acState.mode == AcMode::Dry) {
      acState.power = false;
      return commitAcState(AcChange::PowerOrMode, true);
    }
    syncAcCharacteristics();
    return true;
  }
};

struct AirConditionerSlat : Service::Slat {
  SpanCharacteristic *currentState;
  SpanCharacteristic *swing;
  SpanCharacteristic *currentTilt;
  SpanCharacteristic *targetTilt;

  AirConditionerSlat() : Service::Slat() {
    new Characteristic::Name("Air Conditioner Direction");
    currentState =
        new Characteristic::CurrentSlatState(acState.fullSwing ? 2 : 0);
    new Characteristic::SlatType(0);
    swing = new Characteristic::SwingMode(acState.fullSwing);
    currentTilt =
        new Characteristic::CurrentTiltAngle(acDirectionToTilt(acState.direction));
    targetTilt =
        new Characteristic::TargetTiltAngle(acDirectionToTilt(acState.direction));
    targetTilt->setRange(-90, 60, 30);

    acSlatStateCharacteristic = currentState;
    acSwingCharacteristic = swing;
    acCurrentTiltCharacteristic = currentTilt;
    acTargetTiltCharacteristic = targetTilt;
  }

  boolean update() override {
    bool changed = false;
    if (targetTilt->updated()) {
      acState.direction = acTiltToDirection(targetTilt->getNewVal());
      acState.fullSwing = false;
      changed = true;
    }
    if (swing->updated()) {
      acState.fullSwing = swing->getNewVal();
      changed = true;
    }
    return commitAcState(AcChange::Direction, changed && acState.power);
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
      now - buttonPressedAt >= kHomeKitUnpairPressMs) {
    buttonLongHandled = true;
    unpairHomeKit();
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
  printAcStateDetails(results);
  irrecv.resume();
}

const char *acModeName(AcMode mode) {
  switch (mode) {
    case AcMode::Heat:
      return "heat";
    case AcMode::Dry:
      return "dry";
    case AcMode::Cool:
    default:
      return "cool";
  }
}

const char *acDirectionName() {
  if (acState.fullSwing) {
    return "swing";
  }
  if (acState.direction == 0) {
    return "auto";
  }
  return "fixed";
}

bool homeKitIsPaired() {
  return homeSpan.controllerListBegin() != homeSpan.controllerListEnd();
}

void populateApiStatus(JsonDocument &document) {
  document["ok"] = true;

  JsonObject ac = document["air_conditioner"].to<JsonObject>();
  ac["power"] = acState.power;
  ac["mode"] = acModeName(acState.mode);
  ac["temperature_c"] = selectedAcTemperature();
  ac["cool_temperature_c"] = acState.coolTempC;
  ac["heat_temperature_c"] = acState.heatTempC;
  ac["fan_level"] = acState.fanLevel;
  ac["direction_type"] = acDirectionName();
  ac["direction"] = acState.direction;
  ac["full_swing"] = acState.fullSwing;

  JsonObject light = document["light"].to<JsonObject>();
  light["on"] = lightPowerState;
  light["off_command"] = "night_light";
  light["on_learned"] =
      irCommandStore.hasCommand(ir_store::kLightOnCommand);
  light["off_learned"] =
      irCommandStore.hasCommand(ir_store::kNightLightCommand);

  JsonObject homeKit = document["homespan"].to<JsonObject>();
  homeKit["paired"] = homeKitIsPaired();
  homeKit["state_linked"] = true;

  JsonObject network = document["network"].to<JsonObject>();
  network["wifi_connected"] = WiFi.status() == WL_CONNECTED;
  network["ip"] = WiFi.localIP().toString();
  network["api_port"] = kApiServerPort;
}

void sendApiJson(int statusCode, JsonDocument &document) {
  String response;
  serializeJson(document, response);
  apiServer.sendHeader("Cache-Control", "no-store");
  apiServer.sendHeader("X-Content-Type-Options", "nosniff");
  apiServer.send(statusCode, "application/json; charset=utf-8", response);
}

void sendApiStatus(int statusCode = 200) {
  JsonDocument document;
  populateApiStatus(document);
  sendApiJson(statusCode, document);
}

void sendApiError(int statusCode, const char *error) {
  JsonDocument document;
  document["ok"] = false;
  document["error"] = error;
  sendApiJson(statusCode, document);
}

bool secureEquals(const String &left, const String &right) {
  const size_t maximumLength = max(left.length(), right.length());
  uint8_t difference = static_cast<uint8_t>(left.length() ^ right.length());
  for (size_t index = 0; index < maximumLength; ++index) {
    const char leftCharacter = index < left.length() ? left[index] : 0;
    const char rightCharacter = index < right.length() ? right[index] : 0;
    difference |= static_cast<uint8_t>(leftCharacter ^ rightCharacter);
  }
  return difference == 0;
}

bool requireApiAuthorization() {
  const String expectedAuthorization = "Bearer " + apiBearerToken;
  if (secureEquals(apiServer.header(kAuthorizationHeader),
                   expectedAuthorization)) {
    return true;
  }

  apiServer.sendHeader("WWW-Authenticate", "Bearer");
  sendApiError(401, "unauthorized");
  return false;
}

bool parseOptionalApiInteger(const char *name, int minimum, int maximum,
                             int &output) {
  if (!apiServer.hasArg(name)) {
    return true;
  }

  const String value = apiServer.arg(name);
  char *end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (value.isEmpty() || end == nullptr || *end != '\0' || parsed < minimum ||
      parsed > maximum) {
    String message = String(name) + " must be between " + minimum + " and " +
                     maximum;
    sendApiError(400, message.c_str());
    return false;
  }
  output = static_cast<int>(parsed);
  return true;
}

void handleApiAcOn(AcMode mode) {
  int temperature =
      mode == AcMode::Heat ? acState.heatTempC : acState.coolTempC;
  int fanLevel = acState.fanLevel;
  if (!parseOptionalApiInteger("temperature", kAcDisplayMinTempC,
                               kAcDisplayMaxTempC, temperature) ||
      !parseOptionalApiInteger("fan", 0, 4, fanLevel)) {
    return;
  }

  acState.mode = mode;
  acState.power = true;
  acState.fanLevel = fanLevel;
  if (mode == AcMode::Heat) {
    acState.heatTempC = temperature;
  } else {
    acState.coolTempC = temperature;
  }

  if (!commitAcState(AcChange::PowerOrMode, true)) {
    sendApiError(500, "failed to send SHARP_AC state");
    return;
  }
  sendApiStatus();
}

void handleApiAcOff() {
  acState.power = false;
  if (!commitAcState(AcChange::PowerOrMode, true)) {
    sendApiError(500, "failed to send SHARP_AC off state");
    return;
  }
  sendApiStatus();
}

void handleApiLight(bool requestedOn) {
  const char *commandId = requestedOn ? ir_store::kLightOnCommand
                                      : ir_store::kNightLightCommand;
  if (!sendStoredRawCommand(commandId, kIrSendRepeats)) {
    sendApiError(409, requestedOn ? "light_on is not learned"
                                  : "night_light is not learned");
    return;
  }

  lightPowerState = requestedOn;
  if (lightPowerCharacteristic != nullptr) {
    lightPowerCharacteristic->setVal(requestedOn);
  }
  sendApiStatus();
}

void setupApiServer() {
  static const char *collectedHeaders[] = {kAuthorizationHeader};
  apiServer.collectHeaders(collectedHeaders, 1);
  apiServer.on("/", HTTP_GET, []() {
    if (requireApiAuthorization()) sendApiStatus();
  });
  apiServer.on("/api/status", HTTP_GET, []() {
    if (requireApiAuthorization()) sendApiStatus();
  });
  apiServer.on("/api/ac", HTTP_GET, []() {
    if (requireApiAuthorization()) sendApiStatus();
  });
  apiServer.on("/api/light", HTTP_GET, []() {
    if (requireApiAuthorization()) sendApiStatus();
  });
  apiServer.on("/api/ac/off", HTTP_POST, []() {
    if (requireApiAuthorization()) handleApiAcOff();
  });
  apiServer.on("/api/ac/cool", HTTP_POST,
               []() {
                 if (requireApiAuthorization()) handleApiAcOn(AcMode::Cool);
               });
  apiServer.on("/api/ac/heat", HTTP_POST,
               []() {
                 if (requireApiAuthorization()) handleApiAcOn(AcMode::Heat);
               });
  apiServer.on("/api/light/on", HTTP_POST,
               []() {
                 if (requireApiAuthorization()) handleApiLight(true);
               });
  apiServer.on("/api/light/off", HTTP_POST,
               []() {
                 if (requireApiAuthorization()) handleApiLight(false);
               });
  apiServer.onNotFound([]() {
    if (requireApiAuthorization()) {
      sendApiError(404, "API endpoint not found");
    }
  });
}

void handleApiServer() {
  if (WiFi.status() != WL_CONNECTED) {
    if (apiServerStarted) {
      apiServer.stop();
      apiServerStarted = false;
    }
    return;
  }

  if (!apiServerStarted) {
    apiServer.begin();
    apiServerStarted = true;
    Serial.printf("HTTP API ready at http://%s:%u/api/status\n",
                  WiFi.localIP().toString().c_str(), kApiServerPort);
  }
  apiServer.handleClient();
}

void setupHomeSpan() {
  homeSpan.setLogLevel(1);
  homeSpan.setApSSID(SETUP_AP_SSID);
  homeSpan.setApPassword(setupApPassword.c_str());
  homeSpan.setPairingCode(homeKitPairingCode.c_str());
  homeSpan.setApTimeout(300);
  homeSpan.enableAutoStartAP();
  homeSpan.begin(Category::Lighting, DEVICE_NAME);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Smart Remote Light");
      new Characteristic::Manufacturer("starkensan");
      new Characteristic::Model("XIAO ESP32S3 IR Remote");
      new Characteristic::FirmwareRevision("0.2.0");
    new SmartRemoteLight();
    new AirConditionerHeaterCooler();
    new AirConditionerDrySwitch();
    new AirConditionerSlat();

  syncAcCharacteristics();

  new SpanUserCommand('o', " - learn light_on IR command", commandLearnLightOn);
  new SpanUserCommand('n', " - learn night_light IR command",
                      commandLearnNightLight);
  new SpanUserCommand('a', " - send current SHARP_AC state",
                      commandSendSharpAcState);
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
  new SpanUserCommand('z', " - show local device credentials",
                      [](const char *) { printDeviceCredentials(); });
}

}  // namespace

void setup() {
  Serial.begin(115200);
  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
  }
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  loadDeviceCredentials();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed.");
  }
  if (!irCommandStore.begin()) {
    Serial.println("IR command store initialization failed.");
  }
  irCommandStore.printStatus(Serial);
  loadAcState();

  irsend.begin();
  sharpAc.begin();
  irrecv.enableIRIn();

  setupHomeSpan();
  setupApiServer();
  printHelp();
}

void loop() {
  homeSpan.poll();
  handleApiServer();
  handleAutoSetupAp();
  handleConfigButton();
  updateLastDecoded();
  blinkStatus(WiFi.status() == WL_CONNECTED ? 1000 : 150);
}
