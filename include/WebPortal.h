#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include "SettingsStore.h"
#include "BundleCrypto.h"
#include "TTLockClient.h"
#include "Models.h"

class WebPortal {
 public:
  WebPortal(SettingsStore& settings, BundleCrypto& bundleCrypto, TTLockClient& ttlock,
            QueueHandle_t unlockQueue, SemaphoreHandle_t bleMutex);
  void begin();
  void loop();
  void setIpProvider(std::function<String(void)> provider) { ipProvider_ = provider; }

 private:
  WebServer server_;
  SettingsStore& settings_;
  BundleCrypto& bundleCrypto_;
  TTLockClient& ttlock_;
  QueueHandle_t unlockQueue_;
  SemaphoreHandle_t bleMutex_;
  std::function<String(void)> ipProvider_;

  String sessionToken_;
  uint32_t sessionIssuedMs_{0};

  void sendJson(int status, JsonDocument& doc);
  void sendError(int status, const String& error);
  bool readJson(JsonDocument& doc);
  bool authenticated();
  bool webhookAuthenticated();
  String bearerToken();
  void issueSession();
  bool parseLock(JsonVariantConst root, LockConfig& out, String& error);
};
