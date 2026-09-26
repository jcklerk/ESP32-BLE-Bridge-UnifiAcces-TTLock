#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "Models.h"
#include "SettingsStore.h"

class BundleCrypto {
 public:
  explicit BundleCrypto(SettingsStore& settings) : settings_(settings) {}
  bool encryptLock(const LockConfig& lock, JsonDocument& out, String& error);
  bool decryptLock(JsonVariantConst input, LockConfig& out, String& error);

 private:
  SettingsStore& settings_;
  bool lockToPlainJson(const LockConfig& lock, String& out);
  bool plainJsonToLock(const uint8_t* data, size_t len, LockConfig& out, String& error);
};
