#include "TTLockClient.h"
#include "AppConfig.h"
#include "Util.h"
#include <mbedtls/aes.h>
#include <algorithm>
#include <ctime>
#include <esp_system.h>

TTLockClient* TTLockClient::active_ = nullptr;

namespace {
constexpr uint8_t TTLOCK_DEFAULT_AES_KEY[16] = {
    0x98, 0x76, 0x23, 0xE8, 0xA9, 0x23, 0xA1, 0xBB,
    0x3D, 0x9E, 0x7D, 0x03, 0x78, 0x12, 0x45, 0x88};

void logProtocolEvent(const char* direction, uint8_t command, size_t frameLen, const char* detail = nullptr) {
  Serial.printf("[TTLock] %s cmd=0x%02X bytes=%u", direction, command,
                static_cast<unsigned>(frameLen));
  if (detail && *detail) Serial.printf(" %s", detail);
  Serial.println();
}

// V7.7 scanner: this intentionally mirrors the diagnostic firmware that
// reliably sees both 5A01 locks every ~900 ms.  The important details are
// onResult() + duplicate callbacks + active 100/100 scanning + maxResults(0).
// We do not retain NimBLEAdvertisedDevice pointers; only plain sighting data.
struct LockSighting {
  char mac[18]{};
  uint32_t seenAtMs{0};
  int rssi{-127};
  uint8_t addressType{BLE_ADDR_PUBLIC};
  uint32_t count{0};
  char name[40]{};
  uint8_t protocolType{0};
  uint8_t protocolVersion{0};
};

constexpr size_t MAX_SIGHTINGS = 16;
LockSighting gSightings[MAX_SIGHTINGS];
portMUX_TYPE gSightingsMux = portMUX_INITIALIZER_UNLOCKED;

static bool sameMac(const char* a, const char* b) {
  if (!a || !b) return false;
  return strcasecmp(a, b) == 0;
}

class GatewayScanCallbacks final : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    if (!device) return;
    NimBLEUUID ttService("00001910-0000-1000-8000-00805f9b34fb");
    if (!device->isAdvertisingService(ttService)) return;

    const std::string mac = device->getAddress().toString();
    const uint32_t now = millis();
    portENTER_CRITICAL(&gSightingsMux);
    size_t slot = MAX_SIGHTINGS;
    size_t oldest = 0;
    uint32_t oldestAt = UINT32_MAX;
    for (size_t i = 0; i < MAX_SIGHTINGS; ++i) {
      if (gSightings[i].mac[0] && sameMac(gSightings[i].mac, mac.c_str())) {
        slot = i;
        break;
      }
      if (!gSightings[i].mac[0] && slot == MAX_SIGHTINGS) slot = i;
      if (gSightings[i].seenAtMs < oldestAt) {
        oldestAt = gSightings[i].seenAtMs;
        oldest = i;
      }
    }
    if (slot == MAX_SIGHTINGS) slot = oldest;
    strlcpy(gSightings[slot].mac, mac.c_str(), sizeof(gSightings[slot].mac));
    gSightings[slot].seenAtMs = now;
    gSightings[slot].rssi = device->getRSSI();
    gSightings[slot].addressType = device->getAddress().getType();
    gSightings[slot].count++;
    if (device->haveName()) {
      strlcpy(gSightings[slot].name, device->getName().c_str(), sizeof(gSightings[slot].name));
    }
    if (device->haveManufacturerData()) {
      const std::string m = device->getManufacturerData();
      if (m.size() >= 2) {
        gSightings[slot].protocolType = static_cast<uint8_t>(m[0]);
        gSightings[slot].protocolVersion = static_cast<uint8_t>(m[1]);
      }
    }
    portEXIT_CRITICAL(&gSightingsMux);
  }
};

GatewayScanCallbacks gGatewayScanCallbacks;

static void startGatewayScanner() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) return;
  scan->setScanCallbacks(&gGatewayScanCallbacks, true); // duplicates are REQUIRED
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(100);
  scan->setMaxResults(0); // callback driven, same as diagnostic firmware
  scan->start(0, false, true);
}

static bool getSighting(const char* mac, LockSighting& out) {
  bool found = false;
  portENTER_CRITICAL(&gSightingsMux);
  for (const auto& s : gSightings) {
    if (s.mac[0] && sameMac(s.mac, mac)) {
      out = s;
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&gSightingsMux);
  return found;
}

static bool waitForFreshSighting(const char* mac, uint32_t timeoutMs, LockSighting& out) {
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (getSighting(mac, out) && millis() - out.seenAtMs <= AppConfig::BLE_SIGHTING_MAX_AGE_MS) return true;
    delay(5);
  }
  return getSighting(mac, out) && millis() - out.seenAtMs <= AppConfig::BLE_SIGHTING_MAX_AGE_MS;
}

}

TTLockProtocolFamily TTLockClient::protocolFamily(const LockConfig& lock) {
  if (lock.protocolType != 5) return TTLockProtocolFamily::Unknown;
  switch (lock.protocolVersion) {
    case 1: return TTLockProtocolFamily::V2Lock;
    case 3: return TTLockProtocolFamily::V3;
    case 4: return TTLockProtocolFamily::V2;
    default: return TTLockProtocolFamily::Unknown;
  }
}

const char* TTLockClient::protocolName(const LockConfig& lock) {
  switch (protocolFamily(lock)) {
    case TTLockProtocolFamily::V2Lock: return "TTLock V2 LOCK (5/1)";
    case TTLockProtocolFamily::V3: return "TTLock V3 (5/3)";
    case TTLockProtocolFamily::V2: return "TTLock V2 (5/4)";
    default: return "Unknown TTLock protocol";
  }
}

bool TTLockClient::supportsLocalUnlock(const LockConfig& lock) {
  // The local packet codec implemented here is the reverse-engineered V3 codec.
  // TTLock V2 (5/1 and 5/4) uses a different command/data codec and must not be
  // sent V3 frames just because the header fields happen to fit.
  return protocolFamily(lock) == TTLockProtocolFamily::V3;
}

bool TTLockClient::supportsFactoryProvisioning(const LockConfig& lock) {
  return protocolFamily(lock) == TTLockProtocolFamily::V3;
}

bool TTLockClient::validateProtocolForUnlock(const LockConfig& lock, String& error) const {
  if (supportsLocalUnlock(lock)) return true;
  error = String(protocolName(lock)) +
          " is recognized, but this firmware only has a verified local BLE command codec for V3 (5/3). "
          "V2 (5/1 and 5/4) needs a separate codec; refusing to send incompatible V3 commands.";
  return false;
}

bool TTLockClient::resolveGatt(NimBLEClient* client, GattEndpoint& endpoint) {
    // Reset endpoint explicitly.
    endpoint.service = nullptr;
    endpoint.write = nullptr;
    endpoint.notify = nullptr;
    endpoint.transportName = nullptr;

    // TTLock service
    auto* svc = client->getService(
        "00001910-0000-1000-8000-00805f9b34fb"
    );

    if (svc) {
        auto* write = svc->getCharacteristic(
            "0000fff2-0000-1000-8000-00805f9b34fb"
        );

        auto* notify = svc->getCharacteristic(
            "0000fff4-0000-1000-8000-00805f9b34fb"
        );

        if (write && notify) {
            endpoint.service = svc;
            endpoint.write = write;
            endpoint.notify = notify;
            endpoint.transportName = "TTLock 0x1910";

            return true;
        }
    }

    // Nordic UART compatible fallback
    svc = client->getService(
        "6e400001-b5a3-f393-e0a9-e50e24dcca1e"
    );

    if (svc) {
        auto* write = svc->getCharacteristic(
            "6e400002-b5a3-f393-e0a9-e50e24dcca1e"
        );

        auto* notify = svc->getCharacteristic(
            "6e400003-b5a3-f393-e0a9-e50e24dcca1e"
        );

        if (write && notify) {
            endpoint.service = svc;
            endpoint.write = write;
            endpoint.notify = notify;
            endpoint.transportName = "Nordic UART compatible";

            return true;
        }
    }

    return false;
}

void TTLockClient::begin() {
  if (!responseSem_) responseSem_ = xSemaphoreCreateBinary();
  NimBLEDevice::init("TTLock-Bridge");
  NimBLEDevice::setPower(AppConfig::BLE_TX_POWER_DBM);
  startGatewayScanner();
}

std::vector<BleScanItem> TTLockClient::scan(uint32_t durationMs) {
  // V7.6 uses the always-on callback scanner. Give it the requested observation
  // window, then return TTLock sightings from the cache rather than NimBLE's
  // retained result list (maxResults is intentionally zero).
  startGatewayScanner();
  const uint32_t started = millis();
  while (millis() - started < durationMs) delay(10);

  std::vector<BleScanItem> out;
  portENTER_CRITICAL(&gSightingsMux);
  for (const auto& s : gSightings) {
    if (!s.mac[0] || out.size() >= 20) continue;
    BleScanItem item;
    strlcpy(item.name, s.name[0] ? s.name : "TTLock", sizeof(item.name));
    String mac(s.mac); mac.toUpperCase();
    strlcpy(item.mac, mac.c_str(), sizeof(item.mac));
    item.rssi = s.rssi;
    item.ttlockService = true;
    item.uartService = false;
    if (s.protocolType == 5 &&
        (s.protocolVersion == 1 || s.protocolVersion == 3 || s.protocolVersion == 4)) {
      item.protocolType = s.protocolType;
      item.protocolVersion = s.protocolVersion;
      item.protocolDetected = true;
    }
    out.push_back(item);
  }
  portEXIT_CRITICAL(&gSightingsMux);
  return out;
}

const NimBLEAdvertisedDevice* TTLockClient::findDevice(const char* mac, NimBLEScanResults& results) {
  String wanted(mac); wanted.toLowerCase();
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice* d = results.getDevice(i);
    if (!d) continue;
    String got = d->getAddress().toString().c_str(); got.toLowerCase();
    if (got == wanted) return d;
  }
  return nullptr;
}

bool TTLockClient::unlock(const LockConfig& lock, String& error) {
  const uint32_t allAttemptsStarted = millis();
  for (uint8_t attempt = 1; attempt <= AppConfig::UNLOCK_ATTEMPTS; ++attempt) {
    const uint32_t attemptStarted = millis();
    Serial.printf("[TTLock][TIMING] attempt %u/%u start\n", attempt, AppConfig::UNLOCK_ATTEMPTS);
    String localError;
    if (unlockAttempt(lock, localError, attempt > 1)) {
      Serial.printf("[TTLock][TIMING] attempt %u/%u SUCCESS duration=%lu ms all_attempts=%lu ms\n",
                    attempt, AppConfig::UNLOCK_ATTEMPTS,
                    (unsigned long)(millis() - attemptStarted),
                    (unsigned long)(millis() - allAttemptsStarted));
      return true;
    }
    error = localError;
    Serial.printf("[TTLock][TIMING] attempt %u/%u FAILED duration=%lu ms error=%s\n",
                  attempt, AppConfig::UNLOCK_ATTEMPTS,
                  (unsigned long)(millis() - attemptStarted), localError.c_str());
    // A protocol-level UNLOCK rejection means BLE/GATT communication worked.
    // Reconnecting immediately is expensive and, as seen in the timing logs,
    // can turn a quick rejection into a multi-second 0x3e connection failure.
    // Keep transport failures retryable, but return protocol rejection promptly.
    const bool protocolRejected =
        localError == "UNLOCK rejected by lock" ||
        localError == "CHECK_USER_TIME rejected by lock";
    if (protocolRejected) {
      Serial.println("[TTLock][RETRY] protocol rejection; skipping immediate BLE reconnect");
      break;
    }
    if (attempt < AppConfig::UNLOCK_ATTEMPTS) {
      Serial.printf("[TTLock][TIMING] retry delay %lu ms\n", (unsigned long)AppConfig::RETRY_DELAY_MS);
      delay(AppConfig::RETRY_DELAY_MS);
    }
  }
  Serial.printf("[TTLock][TIMING] all attempts FAILED total=%lu ms\n",
                (unsigned long)(millis() - allAttemptsStarted));
  return false;
}

bool TTLockClient::unlockRecoveryOnly(const LockConfig& lock, String& error) {
  const uint32_t started = millis();
  Serial.println("[TTLock][V7.7] deferred queue retry");
  String localError;
  const bool ok = unlockAttempt(lock, localError, true);
  if (!ok) error = localError;
  Serial.printf("[TTLock][V7.7] deferred recovery %s duration=%lu ms%s%s\n",
                ok ? "SUCCESS" : "FAILED",
                (unsigned long)(millis() - started),
                ok ? "" : " error=", ok ? "" : localError.c_str());
  return ok;
}

bool TTLockClient::initializeNewLock(LockConfig& lock, String& error) {
  // Pairing is intentionally single-attempt: a partially initialized lock may need
  // a factory reset before retrying. The UI explains this to the installer.
  return initializeAttempt(lock, error);
}

bool TTLockClient::initializeAttempt(LockConfig& lock, String& error) {
  if (strlen(lock.mac) != 17) {
    error = "Select a discovered lock first";
    return false;
  }
  if (!supportsFactoryProvisioning(lock)) {
    error = String(protocolName(lock)) +
            " is recognized, but factory provisioning is only implemented for V3 (5/3). "
            "Do not initialize V2 locks with the V3 factory sequence.";
    return false;
  }

  NimBLEScan* scanner = NimBLEDevice::getScan();
  if (scanner->isScanning()) scanner->stop();
  scanner->setScanCallbacks(nullptr, false);
  scanner->setMaxResults(40);
  scanner->setActiveScan(true);
  scanner->setInterval(100);
  scanner->setWindow(100);
  NimBLEScanResults results = scanner->getResults(AppConfig::BLE_SCAN_TIMEOUT_MS, false);
  const NimBLEAdvertisedDevice* device = findDevice(lock.mac, results);
  if (!device) {
    scanner->clearResults();
    startGatewayScanner();
    error = "Factory-reset lock not found. Wake/touch it immediately before initialization.";
    return false;
  }

  NimBLEClient* client = NimBLEDevice::createClient();
  if (!client) {
    scanner->clearResults();
    startGatewayScanner();
    error = "Could not create BLE client";
    return false;
  }
  client->setConnectTimeout(AppConfig::BLE_RECOVERY_CONNECT_TIMEOUT_MS);
  bool connected = client->connect(device);
  scanner->clearResults();
  if (!connected) {
    NimBLEDevice::deleteClient(client);
    startGatewayScanner();
    error = "BLE connect failed";
    return false;
  }

  bool success = false;
  do {
    NimBLERemoteService* svc = client->getService("00001910-0000-1000-8000-00805f9b34fb");
    if (!svc) {
      error = "TTLock V3 service 0x1910 not found";
      break;
    }
    NimBLERemoteCharacteristic* writeChr = svc->getCharacteristic("0000fff2-0000-1000-8000-00805f9b34fb");
    NimBLERemoteCharacteristic* notifyChr = svc->getCharacteristic("0000fff4-0000-1000-8000-00805f9b34fb");
    if (!writeChr || !notifyChr) {
      error = "TTLock FFF2/FFF4 characteristics not found";
      break;
    }

    active_ = this;
    rxBuffer_.clear();
    responseFrame_.clear();
    while (xSemaphoreTake(responseSem_, 0) == pdTRUE) {}
    if (!notifyChr->subscribe(true, notifyCallback)) {
      error = "Could not subscribe to TTLock notifications";
      active_ = nullptr;
      break;
    }
    delay(AppConfig::BLE_POST_NOTIFY_SETTLE_MS);

    LockConfig initCfg = lock;
    memcpy(initCfg.aesKey, TTLOCK_DEFAULT_AES_KEY, sizeof(initCfg.aesKey));

    // 1) COMM_INITIALIZATION (0x45), no payload, default factory AES key.
    std::vector<uint8_t> plain;
    auto frame = buildFrame(initCfg, 0x45, nullptr, 0);
    if (frame.empty() || !exchange(writeChr, frame, TTLOCK_DEFAULT_AES_KEY, 0x45, plain, error)) {
      if (error.length() == 0) error = "Initialization command failed";
      break;
    }

    // 2) COMM_GET_AES_KEY (0x19). Request payload is literal "SCIENER".
    static const uint8_t sciener[] = {'S','C','I','E','N','E','R'};
    frame = buildFrame(initCfg, 0x19, sciener, sizeof(sciener));
    plain.clear();
    if (!exchange(writeChr, frame, TTLOCK_DEFAULT_AES_KEY, 0x19, plain, error)) break;
    if (plain.size() < 18 || plain[1] != 0x01) {
      error = "Lock did not return a valid AES key";
      break;
    }
    memcpy(lock.aesKey, plain.data() + 2, 16);

    // 3) COMM_ADD_ADMIN (0x56). The controller creates the permanent admin
    // secret and unlock key. They are stored only in the encrypted UniFi bundle.
    uint32_t adminPs = esp_random() % 100000000UL;
    uint32_t unlockKey = esp_random() % 100000000UL;
    if (adminPs == 0) adminPs = 1;
    if (unlockKey == 0) unlockKey = 1;
    lock.adminPs = adminPs;
    snprintf(lock.unlockKey, sizeof(lock.unlockKey), "%lu", (unsigned long)unlockKey);

    auto adminPayload = buildAddAdminPayload(adminPs, unlockKey);
    frame = buildFrame(lock, 0x56, adminPayload.data(), adminPayload.size());
    plain.clear();
    if (!exchange(writeChr, frame, lock.aesKey, 0x56, plain, error)) break;
    if (plain.size() < 2 || plain[1] != 0x01) {
      error = "ADD_ADMIN rejected by lock";
      break;
    }

    // 4) Calibrate lock time. Some firmware rejects this; the reference SDK
    // treats that as non-fatal, so we do the same.
    if (time(nullptr) >= 1577836800) {
      auto timePayload = buildCalibrationTimePayload();
      frame = buildFrame(lock, 0x43, timePayload.data(), timePayload.size());
      plain.clear();
      String timeError;
      (void)exchange(writeChr, frame, lock.aesKey, 0x43, plain, timeError);
    }

    // 5) Query features. This mirrors the reference init flow and is useful as
    // a second check that communication with the newly issued AES key works.
    frame = buildFrame(lock, 0x01, nullptr, 0);
    plain.clear();
    String featureError;
    (void)exchange(writeChr, frame, lock.aesKey, 0x01, plain, featureError);

    // 6) OPERATE_FINISHED (0x57), no payload.
    frame = buildFrame(lock, 0x57, nullptr, 0);
    plain.clear();
    if (!exchange(writeChr, frame, lock.aesKey, 0x57, plain, error)) break;
    if (plain.size() < 2 || plain[1] != 0x01) {
      error = "OPERATE_FINISHED rejected by lock";
      break;
    }

    notifyChr->unsubscribe();
    active_ = nullptr;
    success = true;
  } while (false);

  active_ = nullptr;
  NimBLEDevice::deleteClient(client); // always disconnect after provisioning
  if (!success) {
    // Do not leave generated secrets around after a failed init attempt. The
    // lock may require a factory reset before the installer retries.
    Util::secureZero(lock.aesKey, sizeof(lock.aesKey));
    Util::secureZero(lock.unlockKey, sizeof(lock.unlockKey));
    lock.adminPs = 0;
  }
  startGatewayScanner();
  return success;
}

bool TTLockClient::unlockAttempt(const LockConfig& lock, String& error, bool recoveryScan) {
  if (!validateProtocolForUnlock(lock, error)) return false;
  return unlockV3Attempt(lock, error, recoveryScan);
}

bool TTLockClient::unlockV3Attempt(const LockConfig& lock, String& error, bool recoveryScan) {
  const uint32_t unlockStarted = millis();
  uint32_t timingMark = unlockStarted;
  auto timing = [&](const char* step) {
    const uint32_t now = millis();
    Serial.printf("[TTLock][TIMING] %-26s step=%lu ms total=%lu ms\n", step,
                  (unsigned long)(now - timingMark),
                  (unsigned long)(now - unlockStarted));
    timingMark = now;
  };
  Serial.printf("[TTLock][TIMING] Unlock start %s (%s)\n", lock.name, lock.mac);

  // No scan: connect directly to the configured MAC.
  timingMark = millis();
  NimBLEAddress address(std::string(lock.mac), BLE_ADDR_PUBLIC);
  timing("build BLE address");

  // NimBLE keeps the remote GATT objects on the client object. Reuse one client
  // per MAC so subsequent unlocks can reuse the discovered 1910/FFF2/FFF4
  // attributes instead of discovering them again.
  timingMark = millis();
  NimBLEClient* client = NimBLEDevice::getClientByPeerAddress(address);
  bool gattCacheHit = client != nullptr;
  if (!client) {
    client = NimBLEDevice::createClient(address);

    // The ESP32-C3 should keep NimBLE's normal connection limit. If all client
    // slots are occupied by disconnected GATT caches, evict one old cache and
    // reuse the slot. This lets the bridge support many configured locks while
    // only retaining as many live GATT caches as the controller safely allows.
    if (!client) {
      NimBLEClient* evict = NimBLEDevice::getDisconnectedClient();
      if (evict) {
        Serial.println("[TTLock][CACHE] Client cache full; evicting one disconnected GATT cache");
        NimBLEDevice::deleteClient(evict);
        client = NimBLEDevice::createClient(address);
      }
    }
  }
  timing(gattCacheHit ? "get cached BLE client" : "create cached BLE client");
  if (!client) {
    error = "Could not create BLE client/GATT cache slot";
    return false;
  }
  // v7: NimBLE-Arduino defaults to 2 automatic retries after HCI 0x3e. Those
  // retries can hold the single BLE worker for >6 seconds. Disable them and
  // let our explicit attempt-2 retry policy handle failure.
  client->setConnectRetries(0);
  client->setConnectTimeout(recoveryScan
                                ? AppConfig::BLE_RECOVERY_CONNECT_TIMEOUT_MS
                                : AppConfig::BLE_FAST_CONNECT_TIMEOUT_MS);
  Serial.printf("[TTLock][V7.7] connect policy: retries=0 timeout=%lu ms mode=%s\n",
                (unsigned long)(recoveryScan
                                    ? AppConfig::BLE_RECOVERY_CONNECT_TIMEOUT_MS
                                    : AppConfig::BLE_FAST_CONNECT_TIMEOUT_MS),
                recoveryScan ? "recovery" : "fast");
  // Ask for a fast 15 ms initial connection. This does not make a sleeping
  // lock advertise more often, but once the controller catches an advertisement
  // it minimizes the GAP/GATT command latency.
  client->setConnectionParams(AppConfig::BLE_CONN_INTERVAL_MIN,
                              AppConfig::BLE_CONN_INTERVAL_MAX,
                              AppConfig::BLE_CONN_LATENCY,
                              AppConfig::BLE_CONN_SUPERVISION_TIMEOUT);

  Serial.printf("[TTLock][CACHE] %s for %s; shared profile=TTLock-V3-1910/FFF2/FFF4\n",
                gattCacheHit ? "GATT cache HIT" : "GATT cache MISS", lock.mac);

  // V7.7: synchronize the connection with the scanner configuration that was
  // proven to see these locks.  The diagnostic showed both configured 5A01
  // locks (public address type 0) roughly every 900 ms.  Accept a sighting no
  // older than 10 s. If no usable sighting exists, wait briefly for the next onResult().
  NimBLEScan* gatewayScanner = NimBLEDevice::getScan();
  startGatewayScanner();
  timingMark = millis();
  LockSighting sighting;
  const bool seen = waitForFreshSighting(lock.mac, AppConfig::BLE_SIGHTING_WAIT_MS, sighting);
  timing(seen ? "fresh 1910 sighting" : "1910 sighting TIMEOUT");
  if (!seen) {
    if (!gattCacheHit) NimBLEDevice::deleteClient(client);
    error = "TTLock 0x1910 advertisement not seen";
    return false;
  }
  Serial.printf("[TTLock][V7.7][ADV] %s seen age=%lu ms RSSI=%d addrType=%u count=%lu\n",
                lock.mac, (unsigned long)(millis() - sighting.seenAtMs), sighting.rssi,
                (unsigned)sighting.addressType, (unsigned long)sighting.count);

  // Stop the same scanner cleanly before GAP connection.  We deliberately do
  // not retain a NimBLEAdvertisedDevice pointer across this transition.
  timingMark = millis();
  if (gatewayScanner->isScanning()) gatewayScanner->stop();
  timing("stop continuous scanner");

  timingMark = millis();
  bool connected = client->connect(address, false, false, true);
  timing("BLE connect after sighting");

  if (!connected) {
    // Keep a previously discovered GATT cache across ordinary GAP connection
    // failures. There was no GATT transaction, so the cached service objects are
    // not proven stale. If this was a brand-new client there is no useful GATT
    // cache yet; delete it so the recovery attempt starts from a clean client.
    if (!gattCacheHit) {
      NimBLEDevice::deleteClient(client);
      Serial.println("[TTLock][CACHE] connect failed before discovery; removed empty client");
    } else {
      Serial.println("[TTLock][CACHE] connect failed; preserving existing GATT cache for retry");
    }
    error = "BLE connect failed after fresh 0x1910 sighting";
    startGatewayScanner();
    return false;
  }

  bool success = false;
  bool invalidateGattCache = false;
  do {
    timingMark = millis();
    GattEndpoint endpoint;
    if (!resolveGatt(client, endpoint)) {
      error = "TTLock GATT service/characteristics not found";
      invalidateGattCache = true;
      timing("GATT resolve FAILED");
      break;
    }
    timing(gattCacheHit ? "GATT resolve (cached)" : "GATT resolve (discover)");
    NimBLERemoteCharacteristic* writeChr = endpoint.write;
    NimBLERemoteCharacteristic* notifyChr = endpoint.notify;

    timingMark = millis();
    active_ = this;
    rxBuffer_.clear();
    responseFrame_.clear();
    while (xSemaphoreTake(responseSem_, 0) == pdTRUE) {}
    timing("prepare notify state");

    timingMark = millis();
    if (!notifyChr->subscribe(true, notifyCallback)) {
      error = "Could not subscribe to TTLock notifications";
      invalidateGattCache = true;
      active_ = nullptr;
      timing("subscribe FFF4 FAILED");
      break;
    }
    timing("subscribe FFF4");

    timingMark = millis();
    delay(AppConfig::BLE_POST_NOTIFY_SETTLE_MS);
    timing("post-notify settle");

    // Battery reads are intentionally not part of the unlock path.

    timingMark = millis();
    std::vector<uint8_t> checkPayload = buildCheckUserTimePayload();
    std::vector<uint8_t> checkFrame = buildFrame(lock, 0x55, checkPayload.data(), checkPayload.size());
    timing("build 0x55 frame");

    timingMark = millis();
    std::vector<uint8_t> checkPlain;
    if (!exchange(writeChr, checkFrame, lock.aesKey, 0x55, checkPlain, error)) {
      if (lastExchangeFailure_ == ExchangeFailure::Transport ||
          lastExchangeFailure_ == ExchangeFailure::Timeout) {
        invalidateGattCache = true;
      }
      timing("0x55 exchange FAILED");
      break;
    }
    timing("0x55 exchange");
    if (checkPlain.size() < 6 || checkPlain[1] != 0x01) {
      error = "CHECK_USER_TIME rejected by lock";
      break;
    }

    uint32_t ps = (uint32_t(checkPlain[2]) << 24) | (uint32_t(checkPlain[3]) << 16) |
                  (uint32_t(checkPlain[4]) << 8) | uint32_t(checkPlain[5]);

    if (time(nullptr) < 1577836800) {
      error = "ESP system clock is not set; configure SNTP/time before unlocking";
      break;
    }

    timingMark = millis();
    std::vector<uint8_t> unlockPayload = buildUnlockPayload(ps, lock.unlockKey);
    if (unlockPayload.empty()) {
      error = "Invalid numeric unlock key";
      break;
    }
    std::vector<uint8_t> unlockFrame = buildFrame(lock, 0x47, unlockPayload.data(), unlockPayload.size());
    timing("build 0x47 frame");

    timingMark = millis();
    std::vector<uint8_t> unlockPlain;
    if (!exchange(writeChr, unlockFrame, lock.aesKey, 0x47, unlockPlain, error)) {
      if (lastExchangeFailure_ == ExchangeFailure::Transport ||
          lastExchangeFailure_ == ExchangeFailure::Timeout) {
        invalidateGattCache = true;
      }
      timing("0x47 exchange FAILED");
      break;
    }
    timing("0x47 exchange");
    if (unlockPlain.size() < 2 || unlockPlain[1] != 0x01) {
      error = "UNLOCK rejected by lock";
      break;
    }
    success = true;

    // CCCD subscription belongs to the current BLE connection, so unsubscribe
    // before disconnecting. The discovered GATT objects themselves remain cached.
    notifyChr->unsubscribe();
    active_ = nullptr;
  } while (false);

  active_ = nullptr;
  timingMark = millis();
  client->disconnect();
  timing("disconnect (keep cache)");

  if (!success && invalidateGattCache) {
    // Only transport/GATT failures invalidate the cache. Protocol failures such
    // as CRC/decrypt/opcode errors prove that the characteristic path worked,
    // so throwing away a valid 1+ second GATT discovery cache only makes the
    // retry slower.
    NimBLEDevice::deleteClient(client);
    Serial.printf("[TTLock][CACHE] Cleared GATT cache for %s after GATT/transport failure\n", lock.mac);
  } else {
    Serial.printf("[TTLock][CACHE] Kept GATT cache for %s%s\n", lock.mac,
                  success ? "" : " after protocol failure");
  }

  Serial.printf("[TTLock][TIMING] Unlock %s total=%lu ms%s%s\n",
                success ? "SUCCESS" : "FAILED",
                (unsigned long)(millis() - unlockStarted),
                success ? "" : " error=",
                success ? "" : error.c_str());
  startGatewayScanner();
  Serial.println("[TTLock][V7.7][SCAN] continuous scanner resumed");
  return success;
}

void TTLockClient::notifyCallback(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (active_) active_->onNotify(data, len);
}

void TTLockClient::onNotify(const uint8_t* data, size_t len) {
  rxBuffer_.insert(rxBuffer_.end(), data, data + len);

  // TTLock payloads are encrypted binary. Never use CR/LF as the primary frame
  // boundary: the ciphertext can legitimately contain 0D 0A. Instead, parse
  // the protocol header and byte 11 payload length:
  //   12-byte header + encrypted payload + 1-byte CRC.
  // CR/LF is only transport framing and is discarded after a complete frame.
  while (true) {
    // Resynchronize to 7F 5A if noise/stale bytes are present.
    size_t start = 0;
    while (start + 1 < rxBuffer_.size() &&
           !(rxBuffer_[start] == 0x7F && rxBuffer_[start + 1] == 0x5A)) {
      ++start;
    }
    if (start > 0) {
      rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + start);
    }

    if (rxBuffer_.size() < 12) return;
    if (rxBuffer_[0] != 0x7F || rxBuffer_[1] != 0x5A) return;

    const size_t payloadLen = rxBuffer_[11];
    const size_t frameLen = 12 + payloadLen + 1;  // header + payload + CRC
    if (rxBuffer_.size() < frameLen) return;

    responseFrame_.assign(rxBuffer_.begin(), rxBuffer_.begin() + frameLen);
    rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + frameLen);

    // Consume the optional TTLock CR/LF transport terminator only after the
    // binary frame has been extracted by its declared length.
    if (rxBuffer_.size() >= 2 && rxBuffer_[0] == 0x0D && rxBuffer_[1] == 0x0A) {
      rxBuffer_.erase(rxBuffer_.begin(), rxBuffer_.begin() + 2);
    }

    xSemaphoreGive(responseSem_);
    return;
  }
}

bool TTLockClient::exchange(NimBLERemoteCharacteristic* writeChr, const std::vector<uint8_t>& frame,
                            const uint8_t aesKey[16], uint8_t expectedEcho,
                            std::vector<uint8_t>& plaintext, String& error) {
  lastExchangeFailure_ = ExchangeFailure::None;
  while (xSemaphoreTake(responseSem_, 0) == pdTRUE) {}
  responseFrame_.clear();

  // Log only protocol metadata. Do not print plaintext, AES keys, admin secrets,
  // unlock keys, or decrypted payloads.
  logProtocolEvent("TX", expectedEcho, frame.size());

  std::vector<uint8_t> wire = frame;
  wire.push_back(0x0D); wire.push_back(0x0A);
  for (size_t i = 0; i < wire.size(); i += 20) {
    size_t n = std::min<size_t>(20, wire.size() - i);
    if (!writeChr->writeValue(wire.data() + i, n, false)) {
      lastExchangeFailure_ = ExchangeFailure::Transport;
      error = "BLE write failed";
      return false;
    }
  }

  if (xSemaphoreTake(responseSem_, pdMS_TO_TICKS(AppConfig::BLE_RESPONSE_TIMEOUT_MS)) != pdTRUE) {
    logProtocolEvent("TIMEOUT", expectedEcho, 0);
    lastExchangeFailure_ = ExchangeFailure::Timeout;
    error = "Timed out waiting for TTLock response";
    return false;
  }
  logProtocolEvent("RX", expectedEcho, responseFrame_.size());
  if (responseFrame_.size() < 13 || responseFrame_[0] != 0x7F || responseFrame_[1] != 0x5A) {
    lastExchangeFailure_ = ExchangeFailure::InvalidFrame;
    error = "Invalid TTLock response frame";
    return false;
  }
  const uint8_t receivedCrc = responseFrame_.back();
  const uint8_t calculatedCrc = crc8Maxim(responseFrame_.data(), responseFrame_.size() - 1);
  const bool crcValid = calculatedCrc == receivedCrc;
  if (!crcValid) {
    // Some 5A01/V3 locks intermittently return a CRC byte that does not match
    // CRC-8/MAXIM even though the encrypted response is otherwise valid. Do not
    // accept it blindly: continue only so AES-CBC/PKCS#7 and the expected opcode
    // can authenticate/validate the response below.
    Serial.printf("[TTLock][CRC-WARN] cmd=0x%02X len=%u calculated=%02X received=%02X; validating decrypted response RAW: ",
                  expectedEcho, static_cast<unsigned>(responseFrame_.size()),
                  calculatedCrc, receivedCrc);
    // Ciphertext only; credentials/plaintext are never logged.
    for (size_t i = 0; i < responseFrame_.size(); ++i) Serial.printf("%02X ", responseFrame_[i]);
    Serial.println();
  }
  size_t payloadLen = responseFrame_[11];
  if (12 + payloadLen + 1 > responseFrame_.size()) {
    lastExchangeFailure_ = ExchangeFailure::Truncated;
    error = "Truncated TTLock response";
    return false;
  }
  // The reference SDK only checks that COMM_INITIALIZATION got an envelope
  // response; some factory firmware returns no encrypted command payload here.
  if (payloadLen == 0 && expectedEcho == 0x45) {
    plaintext.clear();
    return true;
  }
  if (!aesCbcDecrypt(aesKey, responseFrame_.data() + 12, payloadLen, plaintext)) {
    lastExchangeFailure_ = ExchangeFailure::Decrypt;
    error = crcValid ? "Could not decrypt TTLock response"
                     : "TTLock response CRC mismatch and decrypt failed";
    return false;
  }
  if (plaintext.size() < 2 || plaintext[0] != expectedEcho) {
    logProtocolEvent("RX-ERROR", expectedEcho, responseFrame_.size(), "opcode mismatch");
    lastExchangeFailure_ = ExchangeFailure::Opcode;
    error = crcValid ? "TTLock response opcode does not match request"
                     : "TTLock response CRC mismatch and opcode validation failed";
    return false;
  }

  if (!crcValid) {
    // The encrypted payload decrypted with valid PKCS#7 padding and echoed the
    // exact command we are waiting for. Treat the bad RX CRC as a firmware quirk
    // instead of forcing a disconnect/retry. The command status is still checked
    // by the caller (e.g. 0x01 for CHECK_USER_TIME / UNLOCK success).
    Serial.printf("[TTLock][CRC-WARN] cmd=0x%02X accepted after AES/padding/opcode validation\n",
                  expectedEcho);
  }

  Serial.printf("[TTLock] RX-OK cmd=0x%02X status=0x%02X plaintext_bytes=%u (payload redacted)%s\n",
                expectedEcho, plaintext[1], static_cast<unsigned>(plaintext.size()),
                crcValid ? "" : " CRC-WARN");
  return true;
}

uint8_t TTLockClient::crc8Maxim(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; ++b) crc = (crc & 1) ? ((crc >> 1) ^ 0x8C) : (crc >> 1);
  }
  return crc;
}

bool TTLockClient::aesCbcEncrypt(const uint8_t key[16], const uint8_t* plain, size_t plainLen, std::vector<uint8_t>& out) {
  size_t pad = 16 - (plainLen % 16);
  std::vector<uint8_t> padded(plainLen + pad);
  memcpy(padded.data(), plain, plainLen);
  memset(padded.data() + plainLen, pad, pad);
  out.resize(padded.size());
  uint8_t iv[16]; memcpy(iv, key, 16);
  mbedtls_aes_context ctx; mbedtls_aes_init(&ctx);
  int rc = mbedtls_aes_setkey_enc(&ctx, key, 128);
  if (rc == 0) rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, padded.size(), iv, padded.data(), out.data());
  mbedtls_aes_free(&ctx);
  Util::secureZero(iv, sizeof(iv));
  if (!padded.empty()) Util::secureZero(padded.data(), padded.size());
  return rc == 0;
}

bool TTLockClient::aesCbcDecrypt(const uint8_t key[16], const uint8_t* cipher, size_t cipherLen, std::vector<uint8_t>& out) {
  if (cipherLen == 0 || cipherLen % 16 != 0) return false;
  out.resize(cipherLen);
  uint8_t iv[16]; memcpy(iv, key, 16);
  mbedtls_aes_context ctx; mbedtls_aes_init(&ctx);
  int rc = mbedtls_aes_setkey_dec(&ctx, key, 128);
  if (rc == 0) rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, cipherLen, iv, cipher, out.data());
  mbedtls_aes_free(&ctx);
  Util::secureZero(iv, sizeof(iv));
  if (rc != 0 || out.empty()) return false;
  uint8_t pad = out.back();
  if (pad == 0 || pad > 16 || pad > out.size()) return false;
  for (size_t i = out.size() - pad; i < out.size(); ++i) if (out[i] != pad) return false;
  out.resize(out.size() - pad);
  return true;
}

std::vector<uint8_t> TTLockClient::buildFrame(const LockConfig& lock, uint8_t command,
                                               const uint8_t* plain, size_t plainLen) {
  std::vector<uint8_t> encrypted;
  // TTLock does not encrypt an empty command payload; its length is exactly 0.
  if (plainLen > 0) {
    if (!plain || !aesCbcEncrypt(lock.aesKey, plain, plainLen, encrypted)) return {};
  }
  std::vector<uint8_t> frame;
  frame.reserve(13 + encrypted.size());
  frame.push_back(0x7F); frame.push_back(0x5A);
  frame.push_back(lock.protocolType);
  frame.push_back(lock.protocolVersion);
  frame.push_back(lock.scene);
  frame.push_back((lock.groupId >> 8) & 0xFF); frame.push_back(lock.groupId & 0xFF);
  frame.push_back((lock.orgId >> 8) & 0xFF); frame.push_back(lock.orgId & 0xFF);
  frame.push_back(command);
  frame.push_back(0xAA); // current TTLock V3 app-command marker
  frame.push_back(static_cast<uint8_t>(encrypted.size()));
  frame.insert(frame.end(), encrypted.begin(), encrypted.end());
  frame.push_back(crc8Maxim(frame.data(), frame.size()));
  if (!encrypted.empty()) Util::secureZero(encrypted.data(), encrypted.size());
  return frame;
}

uint8_t TTLockClient::bcdByte(char hi, char lo) {
  return uint8_t(((hi - '0') << 4) | (lo - '0'));
}

std::vector<uint8_t> TTLockClient::buildCheckUserTimePayload() {
  const char* start = "0001311400";
  const char* end = "9911301400";
  std::vector<uint8_t> out(17, 0);
  for (int i = 0; i < 5; ++i) out[i] = bcdByte(start[i * 2], start[i * 2 + 1]);
  for (int i = 0; i < 5; ++i) out[5 + i] = bcdByte(end[i * 2], end[i * 2 + 1]);
  // lockFlagPos = 0 occupies bytes 9..12. This intentionally overwrites byte 9,
  // matching the official SDK layout. uid = 0 occupies bytes 13..16.
  memset(out.data() + 9, 0, 8);
  return out;
}

std::vector<uint8_t> TTLockClient::buildAddAdminPayload(uint32_t adminPs, uint32_t unlockKey) {
  std::vector<uint8_t> out;
  out.reserve(15);
  appendBe32(out, adminPs);
  appendBe32(out, unlockKey);
  static const uint8_t marker[] = {'S','C','I','E','N','E','R'};
  out.insert(out.end(), marker, marker + sizeof(marker));
  return out;
}

std::vector<uint8_t> TTLockClient::buildCalibrationTimePayload() {
  // Reference SDK sends YY MM DD HH mm ss as six numeric bytes (not BCD).
  time_t now = time(nullptr);
  struct tm tmv{};
  localtime_r(&now, &tmv);
  return {
    static_cast<uint8_t>((tmv.tm_year + 1900) % 100),
    static_cast<uint8_t>(tmv.tm_mon + 1),
    static_cast<uint8_t>(tmv.tm_mday),
    static_cast<uint8_t>(tmv.tm_hour),
    static_cast<uint8_t>(tmv.tm_min),
    static_cast<uint8_t>(tmv.tm_sec)
  };
}

void TTLockClient::appendBe32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back((value >> 24) & 0xFF); out.push_back((value >> 16) & 0xFF);
  out.push_back((value >> 8) & 0xFF); out.push_back(value & 0xFF);
}

std::vector<uint8_t> TTLockClient::buildUnlockPayload(uint32_t psFromLock, const char* unlockKey) {
  if (!unlockKey || !*unlockKey) return {};
  char* end = nullptr;
  unsigned long key = strtoul(unlockKey, &end, 10);
  if (!end || *end != '\0') return {};
  uint32_t sum = psFromLock + static_cast<uint32_t>(key);
  std::vector<uint8_t> out;
  out.reserve(8);
  appendBe32(out, sum);
  appendBe32(out, static_cast<uint32_t>(time(nullptr)));
  return out;
}
