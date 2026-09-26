#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <vector>
#include "Models.h"

class TTLockClient {
 public:
  void begin();
  bool unlock(const LockConfig& lock, String& error);
  // V7.6 queue retry: run one recovery-mode attempt only.
  bool unlockRecoveryOnly(const LockConfig& lock, String& error);
  // Factory-reset V3 lock provisioning. The returned LockConfig contains the
  // lock-generated AES key and locally generated unlock/admin secrets.
  bool initializeNewLock(LockConfig& lock, String& error);
  std::vector<BleScanItem> scan(uint32_t durationMs = 4000);
  static TTLockProtocolFamily protocolFamily(const LockConfig& lock);
  static const char* protocolName(const LockConfig& lock);
  static bool supportsLocalUnlock(const LockConfig& lock);
  static bool supportsFactoryProvisioning(const LockConfig& lock);

 private:
  static TTLockClient* active_;
  static void notifyCallback(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);

  SemaphoreHandle_t responseSem_{nullptr};
  std::vector<uint8_t> rxBuffer_;
  std::vector<uint8_t> responseFrame_;

  enum class ExchangeFailure : uint8_t {
    None,
    Transport,
    Timeout,
    InvalidFrame,
    Crc,
    Truncated,
    Decrypt,
    Opcode
  };
  ExchangeFailure lastExchangeFailure_{ExchangeFailure::None};

  struct GattEndpoint {
    NimBLERemoteService* service{nullptr};
    NimBLERemoteCharacteristic* write{nullptr};
    NimBLERemoteCharacteristic* notify{nullptr};
    const char* transportName{nullptr};
  };

  bool unlockAttempt(const LockConfig& lock, String& error, bool recoveryScan = false);
  bool unlockV3Attempt(const LockConfig& lock, String& error, bool recoveryScan = false);
  bool resolveGatt(NimBLEClient* client, GattEndpoint& endpoint);
  bool validateProtocolForUnlock(const LockConfig& lock, String& error) const;
  bool initializeAttempt(LockConfig& lock, String& error);
  const NimBLEAdvertisedDevice* findDevice(const char* mac, NimBLEScanResults& results);
  bool exchange(NimBLERemoteCharacteristic* writeChr, const std::vector<uint8_t>& frame,
                const uint8_t aesKey[16], uint8_t expectedEcho,
                std::vector<uint8_t>& plaintext, String& error);
  void onNotify(const uint8_t* data, size_t len);

  static uint8_t crc8Maxim(const uint8_t* data, size_t len);
  static bool aesCbcEncrypt(const uint8_t key[16], const uint8_t* plain, size_t plainLen, std::vector<uint8_t>& out);
  static bool aesCbcDecrypt(const uint8_t key[16], const uint8_t* cipher, size_t cipherLen, std::vector<uint8_t>& out);
  static std::vector<uint8_t> buildFrame(const LockConfig& lock, uint8_t command,
                                         const uint8_t* plain, size_t plainLen);
  static std::vector<uint8_t> buildCheckUserTimePayload();
  static std::vector<uint8_t> buildUnlockPayload(uint32_t psFromLock, const char* unlockKey);
  static std::vector<uint8_t> buildAddAdminPayload(uint32_t adminPs, uint32_t unlockKey);
  static std::vector<uint8_t> buildCalibrationTimePayload();
  static void appendBe32(std::vector<uint8_t>& out, uint32_t value);
  static uint8_t bcdByte(char hi, char lo);
};
