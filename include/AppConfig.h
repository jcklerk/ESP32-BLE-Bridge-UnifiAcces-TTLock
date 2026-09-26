#pragma once

#include <Arduino.h>

#ifndef USE_ETHERNET
#define USE_ETHERNET 1
#endif

namespace AppConfig {
constexpr const char* HOSTNAME = "ttlock-bridge";
constexpr uint16_t HTTP_PORT = 80;
constexpr size_t MAX_PENDING_UNLOCKS = 10;
constexpr uint32_t BLE_SCAN_TIMEOUT_MS = 4000;
// V7.7: configured 5A01 locks use fixed public BLE addresses. A sighting remains
// useful across another lock's ~1.3 s transaction, so do not force a new ADV.
constexpr uint32_t BLE_SIGHTING_MAX_AGE_MS = 10000;
constexpr uint32_t BLE_SIGHTING_WAIT_MS = 1200;
// v7: fail the first direct connection quickly. The recovery attempt gets a
// longer timeout after the short targeted scan. NimBLE's own 0x3e retries are
// disabled in TTLockClient so our recovery strategy owns the retry policy.
constexpr uint32_t BLE_FAST_CONNECT_TIMEOUT_MS = 1800;
constexpr uint32_t BLE_RECOVERY_CONNECT_TIMEOUT_MS = 2500;
constexpr uint16_t BLE_CONN_INTERVAL_MIN = 12; // 15 ms (1.25 ms units)
constexpr uint16_t BLE_CONN_INTERVAL_MAX = 12; // 15 ms
constexpr uint16_t BLE_CONN_LATENCY = 0;
constexpr uint16_t BLE_CONN_SUPERVISION_TIMEOUT = 150; // 1500 ms (10 ms units)
constexpr int8_t BLE_TX_POWER_DBM = 9;
constexpr uint32_t BLE_RESPONSE_TIMEOUT_MS = 6500;
constexpr uint32_t BLE_POST_NOTIFY_SETTLE_MS = 100;
constexpr uint32_t RETRY_DELAY_MS = 250;
constexpr uint8_t UNLOCK_ATTEMPTS = 2;
// V7.6: after both V7 attempts fail, move the job to the back of the queue once.
constexpr uint8_t MAX_DEFERRED_UNLOCK_RETRIES = 1;
constexpr uint32_t DEFERRED_RETRY_IDLE_COOLDOWN_MS = 500;
constexpr uint32_t SESSION_TTL_MS = 12UL * 60UL * 60UL * 1000UL;
constexpr size_t MAX_HTTP_BODY = 8192;
constexpr char BUNDLE_AAD[] = "ttlock-bridge:v1";
}
