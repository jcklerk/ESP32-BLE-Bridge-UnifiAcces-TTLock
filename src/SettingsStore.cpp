#include "SettingsStore.h"
#include "Util.h"
#include <esp_system.h>
#include <mbedtls/sha256.h>

bool SettingsStore::begin() {
  return prefs_.begin("ttlock", false);
}

bool SettingsStore::hasAdminPassword() const {
  return prefs_.getBytesLength("pwd_hash") == 32 && prefs_.getBytesLength("pwd_salt") == 16;
}

void SettingsStore::derivePasswordHash(const String& password, const uint8_t salt[16], uint8_t out[32]) const {
  std::vector<uint8_t> first(16 + password.length());
  memcpy(first.data(), salt, 16);
  memcpy(first.data() + 16, password.c_str(), password.length());

  uint8_t digest[32];
  mbedtls_sha256(first.data(), first.size(), digest, 0);

  std::vector<uint8_t> round(32 + 16 + password.length());
  memcpy(round.data() + 32, salt, 16);
  memcpy(round.data() + 48, password.c_str(), password.length());
  for (int i = 0; i < 20000; ++i) {
    memcpy(round.data(), digest, 32);
    mbedtls_sha256(round.data(), round.size(), digest, 0);
  }
  memcpy(out, digest, 32);
  Util::secureZero(digest, sizeof(digest));
  if (!first.empty()) Util::secureZero(first.data(), first.size());
  if (!round.empty()) Util::secureZero(round.data(), round.size());
}

bool SettingsStore::setAdminPassword(const String& password) {
  if (password.length() < 8) return false;
  uint8_t salt[16];
  esp_fill_random(salt, sizeof(salt));
  uint8_t hash[32];
  derivePasswordHash(password, salt, hash);
  bool ok = prefs_.putBytes("pwd_salt", salt, sizeof(salt)) == sizeof(salt) &&
            prefs_.putBytes("pwd_hash", hash, sizeof(hash)) == sizeof(hash);
  Util::secureZero(hash, sizeof(hash));
  Util::secureZero(salt, sizeof(salt));
  return ok;
}

bool SettingsStore::verifyAdminPassword(const String& password) const {
  if (!hasAdminPassword()) return false;
  uint8_t salt[16], stored[32], actual[32];
  prefs_.getBytes("pwd_salt", salt, sizeof(salt));
  prefs_.getBytes("pwd_hash", stored, sizeof(stored));
  derivePasswordHash(password, salt, actual);
  bool ok = Util::constantTimeEquals(stored, actual, sizeof(stored));
  Util::secureZero(salt, sizeof(salt));
  Util::secureZero(stored, sizeof(stored));
  Util::secureZero(actual, sizeof(actual));
  return ok;
}

bool SettingsStore::hasSiteKey() const {
  return prefs_.getBytesLength("site_key") == 32;
}

bool SettingsStore::setSiteKeyHex(const String& hex) {
  uint8_t key[32];
  if (!Util::hexToBytes(hex, key, sizeof(key))) return false;
  bool ok = prefs_.putBytes("site_key", key, sizeof(key)) == sizeof(key);
  Util::secureZero(key, sizeof(key));
  return ok;
}

bool SettingsStore::getSiteKey(uint8_t out[32]) const {
  if (!hasSiteKey()) return false;
  return prefs_.getBytes("site_key", out, 32) == 32;
}

String SettingsStore::getSiteKeyHex() const {
  uint8_t key[32];
  if (!getSiteKey(key)) return "";
  String out = Util::bytesToHex(key, sizeof(key));
  Util::secureZero(key, sizeof(key));
  return out;
}

String SettingsStore::generateSiteKey() {
  uint8_t key[32];
  esp_fill_random(key, sizeof(key));
  prefs_.putBytes("site_key", key, sizeof(key));
  String out = Util::bytesToHex(key, sizeof(key));
  Util::secureZero(key, sizeof(key));
  return out;
}

String SettingsStore::getWebhookToken() const {
  return prefs_.getString("hook_token", "");
}

String SettingsStore::regenerateWebhookToken() {
  uint8_t token[32];
  esp_fill_random(token, sizeof(token));
  String hex = Util::bytesToHex(token, sizeof(token));
  prefs_.putString("hook_token", hex);
  Util::secureZero(token, sizeof(token));
  return hex;
}

bool SettingsStore::verifyWebhookToken(const String& token) const {
  String expected = getWebhookToken();
  if (expected.length() == 0 || expected.length() != token.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < expected.length(); ++i) diff |= expected[i] ^ token[i];
  return diff == 0;
}
