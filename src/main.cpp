#include <Arduino.h>
#include <time.h>
#include "AppConfig.h"
#include "Util.h"
#include "NetworkManager.h"
#include "SettingsStore.h"
#include "BundleCrypto.h"
#include "TTLockClient.h"
#include "WebPortal.h"

static NetworkManager network;
static SettingsStore settings;
static TTLockClient ttlock;
static QueueHandle_t unlockQueue;
static SemaphoreHandle_t bleMutex;
static BundleCrypto* bundleCrypto = nullptr;
static WebPortal* portal = nullptr;

static void unlockWorker(void*) {
  Serial.println("[BLE-WORKER] single radio owner ready (V7.9 reliable burst queue + clock preflight + multi-round recovery)");
  UnlockJob job;
  while (true) {
    if (xQueueReceive(unlockQueue, &job, portMAX_DELAY) != pdTRUE) continue;
    String error;
    const uint32_t workerStarted = millis();
    Serial.printf("[WEBHOOK][TIMING][%08lX] worker picked job queue_wait=%lu ms webhook_total=%lu ms\n",
                  (unsigned long)job.requestId,
                  (unsigned long)(workerStarted - job.enqueuedAtMs),
                  (unsigned long)(workerStarted - job.webhookStartedAtMs));

    const uint32_t mutexWaitStarted = millis();
    if (xSemaphoreTake(bleMutex, portMAX_DELAY) == pdTRUE) {
      const uint32_t bleStarted = millis();
      Serial.printf("[WEBHOOK][TIMING][%08lX] BLE mutex acquired wait=%lu ms webhook_total=%lu ms\n",
                    (unsigned long)job.requestId,
                    (unsigned long)(bleStarted - mutexWaitStarted),
                    (unsigned long)(bleStarted - job.webhookStartedAtMs));
      Serial.printf("Unlock request: %s (%s) [request=%08lX deferred=%u]\n",
                    job.lock.name, job.lock.mac, (unsigned long)job.requestId,
                    (unsigned)job.deferredRetries);

      const bool deferredAttempt = job.deferredRetries > 0;
      bool ok = deferredAttempt
                    ? ttlock.unlockRecoveryOnly(job.lock, error)
                    : ttlock.unlock(job.lock, error);
      const uint32_t unlockFinished = millis();

      // V7.9 reliability queue: any failed round may be deferred again up to
      // MAX_DEFERRED_UNLOCK_RETRIES. This includes a lock-level 0x47 rejection:
      // BLE worked, but the request was not accepted, so do not silently drop it.
      if (!ok && job.deferredRetries < AppConfig::MAX_DEFERRED_UNLOCK_RETRIES) {
        job.deferredRetries++;
        const UBaseType_t waiting = uxQueueMessagesWaiting(unlockQueue);
        const bool clockNotReady = error.indexOf("system clock") >= 0 ||
                                   error.indexOf("SNTP") >= 0;
        xSemaphoreGive(bleMutex);

        const uint32_t backoff = clockNotReady
            ? AppConfig::CLOCK_RETRY_BACKOFF_MS
            : (waiting == 0 ? AppConfig::DEFERRED_RETRY_IDLE_COOLDOWN_MS
                            : AppConfig::DEFERRED_RETRY_BACKOFF_MS);

        Serial.printf("[BLE-WORKER][%08lX][RELIABLE] attempt round failed (%u/%u): %s; "
                      "requeue at END after %lu ms; %u other job(s) waiting\n",
                      (unsigned long)job.requestId,
                      (unsigned)job.deferredRetries,
                      (unsigned)AppConfig::MAX_DEFERRED_UNLOCK_RETRIES,
                      error.c_str(), (unsigned long)backoff, (unsigned)waiting);

        // Give the lock/radio (or SNTP) time to settle. Other requests are
        // already queued ahead of this job and will retain FIFO order.
        if (backoff) delay(backoff);

        job.enqueuedAtMs = millis();
        if (xQueueSend(unlockQueue, &job, pdMS_TO_TICKS(250)) == pdTRUE) {
          Serial.printf("[BLE-WORKER][%08lX][RELIABLE] retry queued depth=%u\n",
                        (unsigned long)job.requestId,
                        (unsigned)uxQueueMessagesWaiting(unlockQueue));
          Util::secureZero(&job, sizeof(job));
          continue;
        }

        Serial.printf("[BLE-WORKER][%08lX][RELIABLE] retry requeue FAILED: queue full\n",
                      (unsigned long)job.requestId);
      } else {
        xSemaphoreGive(bleMutex);
      }

      // V7.8 burst handoff: a successful unlock intentionally leaves the
      // scanner paused. If another unlock is already queued, let the next job
      // take the radio immediately using its cached sighting/GATT state. Only
      // resume continuous scanning when the burst has drained.
      if (ok) {
        const UBaseType_t waitingNow = uxQueueMessagesWaiting(unlockQueue);
        if (waitingNow > 0) {
          Serial.printf("[BLE-WORKER][%08lX][BURST] %u queued job(s); direct handoff, scanner stays paused\n",
                        (unsigned long)job.requestId, (unsigned)waitingNow);
        } else {
          ttlock.resumeGatewayScanner();
        }
      }

      Serial.printf("Unlock %s: %s%s%s\n", job.lock.name, ok ? "SUCCESS" : "FAILED",
                    ok ? "" : " - ", ok ? "" : error.c_str());
      Serial.printf("[WEBHOOK][TIMING][%08lX] COMPLETE result=%s ble=%lu ms queue=%lu ms webhook_to_complete=%lu ms deferred=%u%s%s\n",
                    (unsigned long)job.requestId, ok ? "SUCCESS" : "FAILED",
                    (unsigned long)(unlockFinished - bleStarted),
                    (unsigned long)(workerStarted - job.enqueuedAtMs),
                    (unsigned long)(unlockFinished - job.webhookStartedAtMs),
                    (unsigned)job.deferredRetries,
                    ok ? "" : " error=", ok ? "" : error.c_str());
    }
    Util::secureZero(&job, sizeof(job));
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\nUniFi Access -> TTLock Arduino bridge");

  if (!settings.begin()) {
    Serial.println("Preferences initialization failed");
  }

  unlockQueue = xQueueCreate(AppConfig::MAX_PENDING_UNLOCKS, sizeof(UnlockJob));
  bleMutex = xSemaphoreCreateMutex();
  ttlock.begin();
  network.begin();

  bundleCrypto = new BundleCrypto(settings);
  portal = new WebPortal(settings, *bundleCrypto, ttlock, unlockQueue, bleMutex);
  portal->setIpProvider([]() { return network.ip(); });
  portal->begin();

  xTaskCreate(unlockWorker, "ttlock-worker", 8192, nullptr, 1, nullptr);

  // TTLock's UNLOCK payload includes current Unix epoch seconds. Use a local
  // NTP server here if the access-control VLAN has no Internet access.
  #ifndef NTP_SERVER_1
    #define NTP_SERVER_1 "pool.ntp.org"
  #endif
  #ifndef NTP_SERVER_2
    #define NTP_SERVER_2 "time.cloudflare.com"
  #endif
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
}

void loop() {
  if (portal) portal->loop();
  delay(2);
}
