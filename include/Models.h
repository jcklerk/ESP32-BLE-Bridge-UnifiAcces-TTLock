#pragma once

#include <Arduino.h>

enum class TTLockProtocolFamily : uint8_t {
  Unknown = 0,
  V2Lock = 1,      // protocolType=5, protocolVersion=1
  V3 = 3,          // protocolType=5, protocolVersion=3
  V2 = 4           // protocolType=5, protocolVersion=4
};

struct LockConfig {
  char id[40]{};
  char name[64]{};
  char mac[18]{};
  uint8_t aesKey[16]{};
  char unlockKey[24]{};
  uint32_t adminPs{0}; // generated during factory provisioning; kept inside encrypted bundle
  char adminPasscode[24]{};
  uint8_t protocolType{5};
  uint8_t protocolVersion{3};
  uint8_t scene{2};
  uint16_t groupId{1};
  uint16_t orgId{1};
};

struct UnlockJob {
  LockConfig lock{};
  uint32_t requestId{0};
  uint32_t webhookStartedAtMs{0};
  uint32_t enqueuedAtMs{0};
  uint8_t deferredRetries{0};
};

struct BleScanItem {
  char name[48]{};
  char mac[18]{};
  int rssi{0};
  uint8_t protocolType{0};
  uint8_t protocolVersion{0};
  uint8_t scene{0};
  bool protocolDetected{false};
  bool ttlockService{false};
  bool uartService{false};
};
