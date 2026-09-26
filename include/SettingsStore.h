#pragma once

#include <Arduino.h>
#include <Preferences.h>

class SettingsStore {
 public:
  bool begin();
  bool hasAdminPassword() const;
  bool setAdminPassword(const String& password);
  bool verifyAdminPassword(const String& password) const;

  bool hasSiteKey() const;
  bool setSiteKeyHex(const String& hex);
  bool getSiteKey(uint8_t out[32]) const;
  String getSiteKeyHex() const;
  String generateSiteKey();

  String getWebhookToken() const;
  String regenerateWebhookToken();
  bool verifyWebhookToken(const String& token) const;

 private:
  mutable Preferences prefs_;
  void derivePasswordHash(const String& password, const uint8_t salt[16], uint8_t out[32]) const;
};
