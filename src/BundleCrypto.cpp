#include "BundleCrypto.h"
#include "AppConfig.h"
#include "Util.h"
#include <esp_system.h>
#include <mbedtls/gcm.h>

bool BundleCrypto::lockToPlainJson(const LockConfig& lock, String& out) {
  JsonDocument doc;
  doc["v"] = 1;
  doc["id"] = lock.id;
  doc["name"] = lock.name;
  doc["mac"] = lock.mac;
  doc["aes_key"] = Util::bytesToHex(lock.aesKey, sizeof(lock.aesKey));
  doc["unlock_key"] = lock.unlockKey;
  if (lock.adminPs) doc["admin_ps"] = lock.adminPs;
  if (strlen(lock.adminPasscode)) doc["admin_passcode"] = lock.adminPasscode;
  doc["protocol_type"] = lock.protocolType;
  doc["protocol_version"] = lock.protocolVersion;
  doc["scene"] = lock.scene;
  doc["group_id"] = lock.groupId;
  doc["org_id"] = lock.orgId;
  serializeJson(doc, out);
  return true;
}

bool BundleCrypto::plainJsonToLock(const uint8_t* data, size_t len, LockConfig& out, String& error) {
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, data, len);
  if (de) {
    error = "Decrypted lock JSON is invalid";
    return false;
  }
  memset(&out, 0, sizeof(out));
  strlcpy(out.id, doc["id"] | "lock", sizeof(out.id));
  strlcpy(out.name, doc["name"] | "TTLock", sizeof(out.name));
  strlcpy(out.mac, doc["mac"] | "", sizeof(out.mac));
  String aes = doc["aes_key"] | "";
  if (!Util::hexToBytes(aes, out.aesKey, sizeof(out.aesKey))) {
    error = "Invalid AES key in bundle";
    return false;
  }
  strlcpy(out.unlockKey, doc["unlock_key"] | "", sizeof(out.unlockKey));
  out.adminPs = doc["admin_ps"] | 0;
  strlcpy(out.adminPasscode, doc["admin_passcode"] | "", sizeof(out.adminPasscode));
  out.protocolType = doc["protocol_type"] | 5;
  out.protocolVersion = doc["protocol_version"] | 3;
  out.scene = doc["scene"] | 2;
  out.groupId = doc["group_id"] | 1;
  out.orgId = doc["org_id"] | 1;
  if (strlen(out.mac) != 17 || strlen(out.unlockKey) == 0) {
    error = "Bundle is missing MAC or unlock key";
    return false;
  }
  return true;
}

bool BundleCrypto::encryptLock(const LockConfig& lock, JsonDocument& out, String& error) {
  uint8_t key[32];
  if (!settings_.getSiteKey(key)) {
    error = "Site key not configured";
    return false;
  }
  String plaintext;
  lockToPlainJson(lock, plaintext);

  uint8_t nonce[12];
  uint8_t tag[16];
  esp_fill_random(nonce, sizeof(nonce));
  std::vector<uint8_t> cipher(plaintext.length());

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(
      &ctx, MBEDTLS_GCM_ENCRYPT, plaintext.length(), nonce, sizeof(nonce),
      reinterpret_cast<const uint8_t*>(AppConfig::BUNDLE_AAD), strlen(AppConfig::BUNDLE_AAD),
      reinterpret_cast<const uint8_t*>(plaintext.c_str()), cipher.data(), sizeof(tag), tag);
  }
  mbedtls_gcm_free(&ctx);
  Util::secureZero(key, sizeof(key));
  if (rc != 0) {
    error = "AES-GCM encryption failed";
    return false;
  }

  out.clear();
  out["bridge"] = "ttlock-esp32";
  out["v"] = 1;
  out["lock"] = lock.id;
  JsonObject b = out["ttlock"].to<JsonObject>();
  b["v"] = 1;
  b["nonce"] = Util::base64Encode(nonce, sizeof(nonce));
  b["data"] = Util::base64Encode(cipher.data(), cipher.size());
  b["tag"] = Util::base64Encode(tag, sizeof(tag));
  Util::secureZero(nonce, sizeof(nonce));
  Util::secureZero(tag, sizeof(tag));
  if (!cipher.empty()) Util::secureZero(cipher.data(), cipher.size());
  return true;
}

bool BundleCrypto::decryptLock(JsonVariantConst input, LockConfig& out, String& error) {
  JsonVariantConst root = input;
  if (root["context"].is<JsonObjectConst>()) root = root["context"];
  if (root["customContext"].is<JsonObjectConst>()) root = root["customContext"];
  JsonVariantConst b = root["ttlock"];
  if (!b.is<JsonObjectConst>()) {
    error = "No ttlock bundle in webhook JSON";
    return false;
  }

  std::vector<uint8_t> nonce, cipher, tag;
  if (!Util::base64Decode(String(b["nonce"] | ""), nonce) || nonce.size() != 12 ||
      !Util::base64Decode(String(b["data"] | ""), cipher) ||
      !Util::base64Decode(String(b["tag"] | ""), tag) || tag.size() != 16) {
    error = "Malformed encrypted bundle";
    return false;
  }

  uint8_t key[32];
  if (!settings_.getSiteKey(key)) {
    error = "Site key not configured";
    return false;
  }

  std::vector<uint8_t> plain(cipher.size());
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(
      &ctx, cipher.size(), nonce.data(), nonce.size(),
      reinterpret_cast<const uint8_t*>(AppConfig::BUNDLE_AAD), strlen(AppConfig::BUNDLE_AAD),
      tag.data(), tag.size(), cipher.data(), plain.data());
  }
  mbedtls_gcm_free(&ctx);
  Util::secureZero(key, sizeof(key));
  if (rc != 0) {
    error = "Bundle authentication failed: wrong site key or modified data";
    return false;
  }

  bool ok = plainJsonToLock(plain.data(), plain.size(), out, error);
  if (!plain.empty()) Util::secureZero(plain.data(), plain.size());
  return ok;
}
