#include "IrCommandStore.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace ir_store {
namespace {

constexpr const char *kVersionKey = "version";
constexpr const char *kCommandsKey = "commands";
constexpr const char *kFrequencyKey = "frequencyKhz";
constexpr const char *kRawKey = "raw";
constexpr int kSchemaVersion = 1;

}  // namespace

IrCommandStore::IrCommandStore(const char *path) : path_(path) {}

bool IrCommandStore::begin() {
  if (LittleFS.exists(path_)) {
    return true;
  }

  JsonDocument doc;
  doc[kVersionKey] = kSchemaVersion;
  doc[kCommandsKey].to<JsonObject>();
  return saveDocument(doc);
}

bool IrCommandStore::isValidCommandId(const char *commandId) const {
  return commandId != nullptr &&
         (strcmp(commandId, kLightOnCommand) == 0 ||
          strcmp(commandId, kNightLightCommand) == 0);
}

bool IrCommandStore::hasCommand(const char *commandId) {
  LearnedIrCommand command;
  return loadCommand(commandId, command);
}

bool IrCommandStore::loadCommand(const char *commandId, LearnedIrCommand &command) {
  command = LearnedIrCommand{};
  if (!isValidCommandId(commandId)) {
    return false;
  }

  JsonDocument doc;
  if (!loadDocument(doc)) {
    return false;
  }

  JsonObject commands = doc[kCommandsKey].as<JsonObject>();
  JsonObject stored = commands[commandId].as<JsonObject>();
  JsonArray raw = stored[kRawKey].as<JsonArray>();
  if (stored.isNull() || raw.isNull() || raw.size() == 0 ||
      raw.size() > kMaxRawTimings) {
    return false;
  }

  command.frequencyKhz = stored[kFrequencyKey] | kDefaultFrequencyKhz;
  command.rawLength = 0;
  for (JsonVariant item : raw) {
    command.raw[command.rawLength++] = item.as<uint16_t>();
  }
  command.present = true;
  return true;
}

bool IrCommandStore::saveCommand(const char *commandId, const uint16_t *raw,
                                 size_t rawLength, uint16_t frequencyKhz) {
  if (!isValidCommandId(commandId) || raw == nullptr || rawLength == 0 ||
      rawLength > kMaxRawTimings || frequencyKhz == 0) {
    return false;
  }

  JsonDocument doc;
  if (!loadDocument(doc)) {
    doc.clear();
    doc[kVersionKey] = kSchemaVersion;
    doc[kCommandsKey].to<JsonObject>();
  }

  doc[kVersionKey] = kSchemaVersion;
  JsonObject commands = doc[kCommandsKey].as<JsonObject>();
  if (commands.isNull()) {
    commands = doc[kCommandsKey].to<JsonObject>();
  }
  JsonObject stored = commands[commandId].to<JsonObject>();
  stored[kFrequencyKey] = frequencyKhz;
  JsonArray timings = stored[kRawKey].to<JsonArray>();
  timings.clear();
  for (size_t i = 0; i < rawLength; i++) {
    timings.add(raw[i]);
  }

  return saveDocument(doc);
}

bool IrCommandStore::deleteCommand(const char *commandId) {
  if (!isValidCommandId(commandId)) {
    return false;
  }

  JsonDocument doc;
  if (!loadDocument(doc)) {
    return false;
  }

  JsonObject commands = doc[kCommandsKey].as<JsonObject>();
  if (commands.isNull() || !commands[commandId]) {
    return true;
  }

  commands.remove(commandId);
  return saveDocument(doc);
}

bool IrCommandStore::clear() {
  if (LittleFS.exists(path_)) {
    return LittleFS.remove(path_);
  }
  return true;
}

void IrCommandStore::printStatus(Stream &stream) {
  stream.println("IR command storage:");
  stream.printf("  %s: %s\n", kLightOnCommand,
                hasCommand(kLightOnCommand) ? "stored" : "missing");
  stream.printf("  %s: %s\n", kNightLightCommand,
                hasCommand(kNightLightCommand) ? "stored" : "missing");
}

bool IrCommandStore::loadDocument(JsonDocument &doc) {
  if (!LittleFS.exists(path_)) {
    return false;
  }

  File file = LittleFS.open(path_, "r");
  if (!file) {
    return false;
  }

  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  return !error;
}

bool IrCommandStore::saveDocument(JsonDocument &doc) {
  File file = LittleFS.open(path_, "w");
  if (!file) {
    return false;
  }

  const size_t written = serializeJson(doc, file);
  file.close();
  return written > 0;
}

}  // namespace ir_store
