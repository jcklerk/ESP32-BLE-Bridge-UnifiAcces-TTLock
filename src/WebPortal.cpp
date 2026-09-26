#include "WebPortal.h"
#include "AppConfig.h"
#include "WebUi.h"
#include "Util.h"
#include <esp_system.h>

WebPortal::WebPortal(SettingsStore& settings, BundleCrypto& bundleCrypto, TTLockClient& ttlock,
                     QueueHandle_t unlockQueue, SemaphoreHandle_t bleMutex)
    : server_(AppConfig::HTTP_PORT), settings_(settings), bundleCrypto_(bundleCrypto), ttlock_(ttlock),
      unlockQueue_(unlockQueue), bleMutex_(bleMutex) {}

void WebPortal::sendJson(int status, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  server_.send(status, "application/json", body);
}

void WebPortal::sendError(int status, const String& error) {
  JsonDocument doc;
  doc["error"] = error;
  sendJson(status, doc);
}

bool WebPortal::readJson(JsonDocument& doc) {
  if (!server_.hasArg("plain")) {
    sendError(400, "Missing JSON body");
    return false;
  }
  String body = server_.arg("plain");
  if (body.length() > AppConfig::MAX_HTTP_BODY) {
    sendError(413, "Request too large");
    return false;
  }
  auto err = deserializeJson(doc, body);
  if (err) {
    sendError(400, "Invalid JSON");
    return false;
  }
  return true;
}

String WebPortal::bearerToken() {
  String auth = server_.header("Authorization");
  if (!auth.startsWith("Bearer ")) return "";
  auth.remove(0, 7);
  auth.trim();
  return auth;
}

bool WebPortal::webhookAuthenticated() {
  return settings_.verifyWebhookToken(bearerToken());
}

bool WebPortal::authenticated() {
  if (sessionToken_.isEmpty()) return false;
  if ((uint32_t)(millis() - sessionIssuedMs_) > AppConfig::SESSION_TTL_MS) return false;
  String cookie = server_.header("Cookie");
  int pos = cookie.indexOf("SID=");
  if (pos < 0) return false;
  String sid = cookie.substring(pos + 4);
  int semi = sid.indexOf(';');
  if (semi >= 0) sid = sid.substring(0, semi);
  if (sid.length() != sessionToken_.length()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < sid.length(); ++i) diff |= sid[i] ^ sessionToken_[i];
  return diff == 0;
}

void WebPortal::issueSession() {
  uint8_t random[32];
  esp_fill_random(random, sizeof(random));
  sessionToken_ = Util::bytesToHex(random, sizeof(random));
  Util::secureZero(random, sizeof(random));
  sessionIssuedMs_ = millis();
  server_.sendHeader("Set-Cookie", "SID=" + sessionToken_ + "; HttpOnly; SameSite=Strict; Path=/");
}

bool WebPortal::parseLock(JsonVariantConst root, LockConfig& out, String& error) {
  memset(&out, 0, sizeof(out));
  strlcpy(out.id, root["id"] | "lock", sizeof(out.id));
  strlcpy(out.name, root["name"] | "TTLock", sizeof(out.name));
  strlcpy(out.mac, root["mac"] | "", sizeof(out.mac));
  String aes = root["aes_key"] | "";
  if (!Util::hexToBytes(aes, out.aesKey, sizeof(out.aesKey))) {
    error = "AES key must be exactly 16 bytes (32 hex characters)";
    return false;
  }
  strlcpy(out.unlockKey, root["unlock_key"] | "", sizeof(out.unlockKey));
  out.adminPs = root["admin_ps"] | 0;
  strlcpy(out.adminPasscode, root["admin_passcode"] | "", sizeof(out.adminPasscode));
  out.protocolType = root["protocol_type"] | 5;
  out.protocolVersion = root["protocol_version"] | 3;
  out.scene = root["scene"] | 2;
  out.groupId = root["group_id"] | 1;
  out.orgId = root["org_id"] | 1;
  if (strlen(out.mac) != 17) { error = "MAC address must be AA:BB:CC:DD:EE:FF"; return false; }
  if (strlen(out.unlockKey) == 0) { error = "Unlock key is required"; return false; }
  for (size_t i = 0; i < strlen(out.unlockKey); ++i) {
    if (!isdigit((unsigned char)out.unlockKey[i])) { error = "Unlock key must contain digits only"; return false; }
  }
  return true;
}

void WebPortal::begin() {
  const char* headers[] = {
    "Authorization", "Cookie",
    "X-TTLock-Nonce", "X-TTLock-Data", "X-TTLock-Tag"
  };
  server_.collectHeaders(headers, 5);

  server_.on("/", HTTP_GET, [this]() {
    server_.send_P(200, "text/html", WEB_UI);
  });

  server_.on("/api/status", HTTP_GET, [this]() {
    JsonDocument doc;
    doc["configured"] = settings_.hasAdminPassword() && settings_.hasSiteKey();
    doc["ip"] = ipProvider_ ? ipProvider_() : "";
    doc["site_key_configured"] = settings_.hasSiteKey();
    sendJson(200, doc);
  });

  server_.on("/api/setup", HTTP_POST, [this]() {
    if (settings_.hasAdminPassword()) { sendError(403, "Already configured"); return; }
    JsonDocument doc;
    if (!readJson(doc)) return;
    String password = doc["password"] | "";
    if (!settings_.setAdminPassword(password)) { sendError(400, "Password must be at least 8 characters"); return; }
    settings_.generateSiteKey();
    settings_.regenerateWebhookToken();
    issueSession();
    JsonDocument out; out["ok"] = true;
    sendJson(200, out);
  });

  server_.on("/api/login", HTTP_POST, [this]() {
    JsonDocument doc;
    if (!readJson(doc)) return;
    if (!settings_.verifyAdminPassword(doc["password"] | "")) { sendError(401, "Invalid password"); return; }
    issueSession();
    JsonDocument out; out["ok"] = true;
    sendJson(200, out);
  });

  server_.on("/api/secrets", HTTP_GET, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    JsonDocument out;
    out["site_key"] = settings_.getSiteKeyHex();
    out["webhook_token"] = settings_.getWebhookToken();
    sendJson(200, out);
  });

  server_.on("/api/settings/site-key", HTTP_POST, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    JsonDocument doc; if (!readJson(doc)) return;
    if (!settings_.setSiteKeyHex(doc["site_key"] | "")) { sendError(400, "Site key must be 32 bytes / 64 hex characters"); return; }
    JsonDocument out; out["ok"] = true; sendJson(200, out);
  });

  server_.on("/api/settings/generate-site-key", HTTP_POST, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    JsonDocument out; out["site_key"] = settings_.generateSiteKey(); sendJson(200, out);
  });

  server_.on("/api/settings/regenerate-token", HTTP_POST, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    JsonDocument out; out["webhook_token"] = settings_.regenerateWebhookToken(); sendJson(200, out);
  });

  server_.on("/api/ble/scan", HTTP_GET, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    if (xSemaphoreTake(bleMutex_, pdMS_TO_TICKS(100)) != pdTRUE) { sendError(409, "BLE busy"); return; }
    auto devices = ttlock_.scan();
    xSemaphoreGive(bleMutex_);
    JsonDocument out;
    JsonArray arr = out["devices"].to<JsonArray>();
    for (const auto& d : devices) {
      JsonObject o = arr.add<JsonObject>();
      o["name"] = d.name; o["mac"] = d.mac; o["rssi"] = d.rssi;
      o["ttlock_service"] = d.ttlockService;
      o["uart_service"] = d.uartService;
      o["protocol_detected"] = d.protocolDetected;
      if (d.protocolDetected) {
        o["protocol_type"] = d.protocolType;
        o["protocol_version"] = d.protocolVersion;
        LockConfig detected{};
        detected.protocolType = d.protocolType;
        detected.protocolVersion = d.protocolVersion;
        o["protocol_name"] = TTLockClient::protocolName(detected);
        o["local_unlock_supported"] = TTLockClient::supportsLocalUnlock(detected);
        o["factory_provisioning_supported"] = TTLockClient::supportsFactoryProvisioning(detected);
      }
    }
    sendJson(200, out);
  });

  server_.on("/api/locks/init", HTTP_POST, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    JsonDocument doc; if (!readJson(doc)) return;

    LockConfig lock{};
    strlcpy(lock.id, doc["id"] | "lock", sizeof(lock.id));
    strlcpy(lock.name, doc["name"] | "TTLock", sizeof(lock.name));
    strlcpy(lock.mac, doc["mac"] | "", sizeof(lock.mac));
    lock.protocolType = doc["protocol_type"] | 5;
    lock.protocolVersion = doc["protocol_version"] | 3;
    lock.scene = doc["scene"] | 2;
    lock.groupId = doc["group_id"] | 1;
    lock.orgId = doc["org_id"] | 1;

    if (strlen(lock.mac) != 17) { sendError(400, "Select a lock / provide a valid MAC address"); return; }
    if (xSemaphoreTake(bleMutex_, pdMS_TO_TICKS(100)) != pdTRUE) {
      sendError(409, "BLE busy");
      return;
    }

    String error;
    bool ok = ttlock_.initializeNewLock(lock, error);
    xSemaphoreGive(bleMutex_);
    if (!ok) {
      Util::secureZero(&lock, sizeof(lock));
      sendError(502, error + ". If the lock partially initialized, factory-reset it before retrying.");
      return;
    }

    JsonDocument bundle;
    if (!bundleCrypto_.encryptLock(lock, bundle, error)) {
      Util::secureZero(&lock, sizeof(lock));
      sendError(500, error);
      return;
    }

    JsonDocument out;
    out["ok"] = true;
    out["message"] = "Factory provisioning succeeded; BLE disconnected. Save the encrypted context in UniFi Access.";
    JsonObject info = out["lock_data"].to<JsonObject>();
    info["id"] = lock.id;
    info["name"] = lock.name;
    info["mac"] = lock.mac;
    info["aes_key"] = Util::bytesToHex(lock.aesKey, sizeof(lock.aesKey));
    info["unlock_key"] = lock.unlockKey;
    info["admin_ps"] = lock.adminPs;
    info["protocol_type"] = lock.protocolType;
    info["protocol_version"] = lock.protocolVersion;
    info["scene"] = lock.scene;
    info["group_id"] = lock.groupId;
    info["org_id"] = lock.orgId;
    out["bundle"] = bundle.as<JsonVariantConst>();
    sendJson(200, out);
    Util::secureZero(&lock, sizeof(lock));
  });

  server_.on("/api/locks/test", HTTP_POST, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    JsonDocument doc; if (!readJson(doc)) return;
    LockConfig lock; String error;
    if (!parseLock(doc.as<JsonVariantConst>(), lock, error)) { sendError(400, error); return; }
    if (xSemaphoreTake(bleMutex_, pdMS_TO_TICKS(100)) != pdTRUE) { Util::secureZero(&lock, sizeof(lock)); sendError(409, "BLE busy"); return; }
    bool ok = ttlock_.unlock(lock, error);
    xSemaphoreGive(bleMutex_);
    Util::secureZero(&lock, sizeof(lock));
    if (!ok) { sendError(502, error); return; }
    JsonDocument out; out["ok"] = true; out["message"] = "Unlock succeeded and BLE disconnected"; sendJson(200, out);
  });

  server_.on("/api/locks/bundle", HTTP_POST, [this]() {
    if (!authenticated()) { sendError(401, "Login required"); return; }
    JsonDocument doc; if (!readJson(doc)) return;
    LockConfig lock; String error;
    if (!parseLock(doc.as<JsonVariantConst>(), lock, error)) { sendError(400, error); return; }
    JsonDocument bundle;
    bool ok = bundleCrypto_.encryptLock(lock, bundle, error);
    Util::secureZero(&lock, sizeof(lock));
    if (!ok) { sendError(500, error); return; }
    sendJson(200, bundle);
  });

  // UniFi Access webhook endpoint. UniFi can provide a Bearer token and custom
  // headers but does not need to send a JSON body. The encrypted lock bundle is
  // split over three headers so credentials remain AES-GCM protected:
  //   X-TTLock-Nonce, X-TTLock-Data, X-TTLock-Tag
  // Both GET and POST are accepted because UniFi supports either method.
  auto unlockFromHeaders = [this]() {
    const uint32_t webhookStarted = millis();
    const uint32_t requestId = esp_random();
    uint32_t timingMark = webhookStarted;
    auto webhookTiming = [&](const char* step) {
      const uint32_t now = millis();
      Serial.printf("[WEBHOOK][TIMING][%08lX] %-24s step=%lu ms total=%lu ms\n",
                    (unsigned long)requestId, step,
                    (unsigned long)(now - timingMark),
                    (unsigned long)(now - webhookStarted));
      timingMark = now;
    };
    Serial.printf("[WEBHOOK][TIMING][%08lX] request start method=%s uri=%s\n",
                  (unsigned long)requestId, server_.method() == HTTP_POST ? "POST" : "GET",
                  server_.uri().c_str());

    if (!webhookAuthenticated()) {
      webhookTiming("authenticate FAILED");
      sendError(401, "Invalid Bearer token");
      webhookTiming("HTTP 401 sent");
      return;
    }
    webhookTiming("authenticate");

    String nonce = server_.header("X-TTLock-Nonce");
    String data  = server_.header("X-TTLock-Data");
    String tag   = server_.header("X-TTLock-Tag");
    nonce.trim(); data.trim(); tag.trim();
    webhookTiming("read/trim headers");

    if (nonce.isEmpty() || data.isEmpty() || tag.isEmpty()) {
      sendError(400, "Missing X-TTLock-Nonce, X-TTLock-Data or X-TTLock-Tag header");
      webhookTiming("HTTP 400 sent");
      return;
    }

    JsonDocument encrypted;
    JsonObject bundle = encrypted["ttlock"].to<JsonObject>();
    bundle["v"] = 1;
    bundle["nonce"] = nonce;
    bundle["data"] = data;
    bundle["tag"] = tag;

    LockConfig lock;
    String error;
    if (!bundleCrypto_.decryptLock(encrypted.as<JsonVariantConst>(), lock, error)) {
      webhookTiming("decrypt bundle FAILED");
      Util::secureZero(&lock, sizeof(lock));
      sendError(400, error);
      webhookTiming("HTTP 400 sent");
      return;
    }
    webhookTiming("decrypt bundle");

    UnlockJob job;
    job.lock = lock;
    job.requestId = requestId;
    job.webhookStartedAtMs = webhookStarted;
    job.enqueuedAtMs = millis();
    Util::secureZero(&lock, sizeof(lock));

    if (xQueueSend(unlockQueue_, &job, 0) != pdTRUE) {
      webhookTiming("enqueue FAILED");
      Util::secureZero(&job, sizeof(job));
      sendError(503, "Unlock queue full");
      webhookTiming("HTTP 503 sent");
      return;
    }
    webhookTiming("enqueue unlock");
    Serial.printf("[WEBHOOK][TIMING][%08lX] queue depth=%u\n",
                  (unsigned long)requestId, (unsigned)uxQueueMessagesWaiting(unlockQueue_));

    // Return immediately; BLE discovery/unlock runs on the worker task.
    JsonDocument out;
    out["accepted"] = true;
    out["request_id"] = String(requestId, HEX);
    out["lock"] = job.lock.id;
    sendJson(202, out);
    webhookTiming("HTTP 202 sent");
    Util::secureZero(&job, sizeof(job));
  };

  server_.on("/api/unlock", HTTP_POST, unlockFromHeaders);
  server_.on("/api/unlock", HTTP_GET, unlockFromHeaders);

  server_.onNotFound([this]() { sendError(404, "Not found"); });
  server_.begin();
  Serial.printf("Web portal listening on port %u\n", AppConfig::HTTP_PORT);
}

void WebPortal::loop() {
  server_.handleClient();
}
