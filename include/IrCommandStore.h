#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace ir_store {

constexpr const char *kLightOnCommand = "light_on";
constexpr const char *kNightLightCommand = "night_light";
constexpr size_t kMaxRawTimings = 512;
constexpr uint16_t kDefaultFrequencyKhz = 38;

struct LearnedIrCommand {
  bool present = false;
  uint16_t frequencyKhz = kDefaultFrequencyKhz;
  size_t rawLength = 0;
  uint16_t raw[kMaxRawTimings] = {};
};

class IrCommandStore {
 public:
  explicit IrCommandStore(const char *path = "/ir_commands.json");

  bool begin();
  bool isValidCommandId(const char *commandId) const;
  bool hasCommand(const char *commandId);
  bool loadCommand(const char *commandId, LearnedIrCommand &command);
  bool saveCommand(const char *commandId, const uint16_t *raw, size_t rawLength,
                   uint16_t frequencyKhz = kDefaultFrequencyKhz);
  bool deleteCommand(const char *commandId);
  bool clear();
  void printStatus(Stream &stream);

 private:
  const char *path_;

  bool loadDocument(JsonDocument &doc);
  bool saveDocument(JsonDocument &doc);
};

}  // namespace ir_store
