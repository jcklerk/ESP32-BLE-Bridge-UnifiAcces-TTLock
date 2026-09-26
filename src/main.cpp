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
  Serial.println("[BLE-WORKER] single radio owner ready (V7.7 quiet filtered 1910 scanner + end-of-queue retry)");
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

      // V7.6: if the normal two-attempt sequence failed, release the radio
      // and put this lock at the END of the queue once. This lets another lock
      // run immediately and naturally gives the failed lock time to settle.
      if (!ok && !deferredAttempt &&
          job.deferredRetries < AppConfig::MAX_DEFERRED_UNLOCK_RETRIES) {
        job.deferredRetries++;
        const UBaseType_t waiting = uxQueueMessagesWaiting(unlockQueue);
        xSemaphoreGive(bleMutex);

        if (waiting == 0) {
          Serial.printf("[BLE-WORKER][%08lX] no other lock waiting; cooldown %lu ms before final retry\n",
                        (unsigned long)job.requestId,
                        (unsigned long)AppConfig::DEFERRED_RETRY_IDLE_COOLDOWN_MS);
          delay(AppConfig::DEFERRED_RETRY_IDLE_COOLDOWN_MS);
        } else {
          Serial.printf("[BLE-WORKER][%08lX] unlock failed; moving to END of queue behind %u waiting job(s)\n",
                        (unsigned long)job.requestId, (unsigned)waiting);
        }

        job.enqueuedAtMs = millis();
        if (xQueueSend(unlockQueue, &job, 0) == pdTRUE) {
          Serial.printf("[BLE-WORKER][%08lX] deferred retry queued depth=%u\n",
                        (unsigned long)job.requestId,
                        (unsigned)uxQueueMessagesWaiting(unlockQueue));
          Util::secureZero(&job, sizeof(job));
          continue;
        }

        // Queue may have filled while this BLE operation was running. Do not
        // spin or retry forever; report the original unlock as failed.
        Serial.printf("[BLE-WORKER][%08lX] deferred requeue FAILED: queue full\n",
                      (unsigned long)job.requestId);
      } else {
        xSemaphoreGive(bleMutex);
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
