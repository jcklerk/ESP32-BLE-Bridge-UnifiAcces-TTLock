# UniFi Access → TTLock local bridge (Arduino / ESP32)

A local ESP32 bridge intended for a PoE/Ethernet installation. UniFi Access remains the access-control authority. For each door, UniFi Alarm Manager/Webhooks stores an **encrypted TTLock context**. When the door webhook fires, the ESP decrypts that context in RAM, queues a BLE job, connects to the cylinder, performs the TTLock V3 unlock handshake, and immediately disconnects.

## Main properties

- Arduino framework / PlatformIO.
- Up to 10 queued unlock requests by default (`MAX_PENDING_UNLOCKS`).
- No permanent BLE connection.
- Runtime flow: `scan → connect → subscribe → CHECK_USER_TIME → UNLOCK → disconnect`.
- TTLock BLE V3 service `1910`, write `FFF2`, notify `FFF4`; Nordic-UART-style fallback is also included.
- AES-128-CBC TTLock command encryption with key-as-IV and PKCS#7 padding.
- CRC-8/MAXIM packet CRC.
- AES-256-GCM for the portable lock bundle stored in UniFi Access.
- Local web portal with admin password, site encryption key, webhook Bearer token, BLE scan, test unlock and UniFi context generation.
- Lock keys are not stored as an ESP-side lock database. The decrypted `LockConfig` exists only for the current request/job and is zeroed afterward.

## Important limitation

This project controls **already-paired/initialized TTLock V3 locks**. The web portal can discover a lock and import/test its AES key + unlock key, but it does not yet claim a factory-reset TTLock or generate its initial eKey. The current `roquerodrigo/ttlock-ble` client is likewise explicitly an already-paired-lock client. Factory provisioning should be added as a separate protocol module and tested on the exact cylinder model before use.

## Build

The provided `platformio.ini` defaults to an ESP32 with LAN8720/RMII Ethernet pins matching the common Espressif example:

- PHY: LAN8720
- PHY address: `0`
- MDC: GPIO23
- MDIO: GPIO18
- PHY power: `-1`
- reference clock: GPIO0 input

Change the `ETH_*` build flags for your PoE board. For example, some Olimex/custom boards use a different clock or PHY power pin.

Dependencies:

- Arduino-ESP32
- `h2zero/NimBLE-Arduino` 2.5.x
- ArduinoJson 7.x

Build/upload:

```bash
pio run
pio run -t upload
pio device monitor
```

## First setup

1. Connect the ESP to PoE/Ethernet.
2. Browse to `http://<esp-ip>/`.
3. Set an admin password (minimum 8 characters).
4. The ESP generates:
   - a random 256-bit **site key**;
   - a random **webhook Bearer token**.
5. Back up the site key somewhere secure. A replacement ESP must be configured with exactly the same site key to decrypt existing UniFi lock contexts.

The site key itself is stored in ESP Preferences for normal operation. For a hardened installation, enable ESP32 flash encryption + secure boot; otherwise physical flash extraction may reveal it.

## Add a lock

The portal supports BLE scanning to pre-fill the lock MAC/name. Enter:

```json
{
	"id": "front-door",
	"name": "Front Door",
	"mac": "AA:BB:CC:DD:EE:FF",
	"aes_key": "00112233445566778899aabbccddeeff",
	"unlock_key": "12345678",
	"admin_passcode": "",
	"protocol_type": 5,
	"protocol_version": 3,
	"scene": 2,
	"group_id": 1,
	"org_id": 1
}
```

Use **Test unlock** first. The test does not keep a BLE link open.

## Generate the UniFi context

Press **Generate UniFi context**. The ESP serializes the lock configuration and encrypts it with AES-256-GCM using the site key. Example shape:

```json
{
	"bridge": "ttlock-esp32",
	"v": 1,
	"lock": "front-door",
	"ttlock": {
		"v": 1,
		"nonce": "...base64...",
		"data": "...base64 ciphertext...",
		"tag": "...base64..."
	}
}
```

The AES key and unlock key are inside `data`; they are not plaintext in UniFi.

## UniFi Access Alarm Manager webhook

Configure a webhook for the authorized door-unlock event:

- Method: `POST`
- URL: `http://<esp-ip>/api/unlock`
- Authentication: Bearer
- Token: the ESP's webhook token
- Context: paste the generated encrypted JSON

The endpoint understands the encrypted object when it is:

- the request root itself;
- under `context`;
- under `customContext`.

This is useful because UniFi versions/templates can wrap custom context differently.

The webhook returns HTTP `202` as soon as the request is authenticated, decrypted and queued. BLE work is performed by a FreeRTOS worker so the HTTP request does not remain open while the lock connects.

## Replacement ESP

If a bridge dies:

1. Flash this project to a replacement.
2. Set a new local admin password.
3. Paste the **same site key** in the portal.
4. Set the webhook token to the old value if you add that option, or regenerate the token and update the UniFi webhook auth setting.
5. Existing encrypted lock contexts do not need to change as long as the site key is unchanged.

Currently the UI exposes generation of a new webhook token; if you want a zero-change replacement, add/manual-set the same token in `SettingsStore`, or update the UniFi webhook's Bearer token once.

## Clock / local-only network

The unlock payload contains Unix epoch seconds. `main.cpp` calls SNTP via `configTime()`. If the Access VLAN has no Internet access, define a reachable local NTP server, for example in `platformio.ini`:

```ini
build_flags =
    ...
    -D NTP_SERVER_1=\"192.168.10.1\"
    -D NTP_SERVER_2=\"192.168.10.2\"
```

The bridge refuses to unlock if its clock is still obviously unset.

## Security model

- Keep the ESP and UniFi Access controller on an access-control VLAN.
- Firewall `/api/unlock` so only the UniFi controller can reach it if possible.
- Use the Bearer token even on the isolated VLAN.
- The portal is plain HTTP in this Arduino build. Do not expose it to untrusted networks. For higher assurance, terminate HTTPS on a local reverse proxy or add an HTTPS-capable embedded server.
- Enable Secure Boot and Flash Encryption before using production door credentials.
- Store the site key in a proper secrets/password manager as the disaster-recovery key.

## Source references used for the TTLock implementation

The BLE behavior mirrors current public reverse-engineered implementations rather than inventing a new protocol: `roquerodrigo/ttlock-ble` for the current V3 frame/handshake and `h2zero/NimBLE-Arduino` for the Arduino BLE client API. TTLock BLE is unofficial and firmware variants can differ, so validate on a test cylinder before deployment.

## Factory-new TTLock provisioning

The web portal now has **Initialize New Lock** for factory-reset TTLock V3 devices.

Workflow:

1. Factory-reset the TTLock.
2. Wake/touch its keypad. Factory-reset locks are typically discoverable for only a short window.
3. In the bridge portal, click **Scan nearby TTLocks** and select the lock.
4. Confirm protocol values (currently provisioning supports protocol type `5`, version `3`; defaults are scene `2`, group `1`, organisation `1`).
5. Click **Initialize New Lock**.
6. The ESP performs the local BLE initialization sequence, disconnects, and returns an encrypted UniFi context bundle.
7. Store the generated bundle as the custom context of the corresponding UniFi Access Alarm Manager webhook.

The implemented V3 provisioning sequence follows the public `kind3r/ttlock-sdk-js` implementation:

- `COMM_INITIALIZATION` (`0x45`) using the factory/default AES key.
- `COMM_GET_AES_KEY` (`0x19`) using the literal `SCIENER` request marker; the lock returns its permanent 16-byte AES key.
- `COMM_ADD_ADMIN` (`0x56`) using a locally generated admin secret and unlock key.
- time calibration when the ESP clock is valid (non-fatal if unsupported).
- device-feature query (non-fatal).
- `OPERATE_FINISHED` (`0x57`).
- disconnect immediately.

The generated `admin_ps` is preserved inside the encrypted UniFi bundle for possible future administrative operations. It is different from the optional keypad/admin passcode.

### HTTP API

Authenticated local portal endpoint:

```http
POST /api/locks/init
Content-Type: application/json
```

Example request:

```json
{
	"id": "front-door",
	"name": "Front Door",
	"mac": "AA:BB:CC:DD:EE:FF",
	"protocol_type": 5,
	"protocol_version": 3,
	"scene": 2,
	"group_id": 1,
	"org_id": 1
}
```

On success the response contains `lock_data` for the authenticated installer and `bundle`, which is the AES-256-GCM encrypted object intended for UniFi Access. Normal webhook unlocks still decrypt the lock data only in RAM and disconnect BLE after each operation.

### Important testing note

TTLock is an OEM ecosystem and firmware variants exist. The factory provisioning path is based on a known working V3 implementation but has not been validated against every cylinder model. Test new firmware on a lock that is not installed on a critical door first. If initialization fails after beginning, factory-reset the lock before trying again.

## Unlock reliability fixes

The fast unlock path uses direct MAC connections and retains NimBLE's per-peer GATT cache.
TTLock notification frames are parsed from the binary protocol header/payload length rather than
searching encrypted ciphertext for CR/LF. This prevents an encrypted `0D 0A` sequence from being
misinterpreted as the end of a frame. Protocol-level errors (CRC/decrypt/opcode) no longer discard
a valid GATT cache; only GATT/transport failures do. CRC failures log the received ciphertext frame
and calculated/received CRC bytes for diagnosis without logging decrypted credentials.

### RX CRC compatibility fix

Some tested 5A01 protocol 5/version 3 locks intermittently return a response CRC byte that does not match CRC-8/MAXIM while the encrypted response still decrypts correctly. RX CRC mismatch is therefore no longer immediately fatal. The bridge logs `CRC-WARN`, then requires successful AES-CBC decryption with valid PKCS#7 padding and the expected command echo before accepting the response. The caller still validates the TTLock status byte. If decryption or opcode validation also fails, the exchange fails normally. Outgoing frame CRC generation is unchanged.

## BLE unlock connection optimization

The unlock path uses a 700 ms passive, 100%-duty targeted scan before connecting. If the configured lock is observed, the client connects from the fresh advertisement instead of blindly waiting for the MAC to advertise. If it is not observed, the bridge falls back to direct-MAC connection. Unlock clients use a 2.5 s connection timeout, 15 ms initial connection interval, zero latency, and 9 dBm TX power. Timing logs report `fast target scan HIT/MISS` and either `BLE connect from ADV` or `direct BLE fallback`, making A/B performance easy to measure.

## Connection optimization v2

Unlock fast path changes:

- Targeted passive scan is capped at 350 ms and stops immediately when the configured lock MAC is discovered.
- Logs the exact time-to-advertisement and RSSI on an early hit.
- Falls back immediately to direct-MAC connection when the target is not seen.
- Keeps the existing per-lock GATT cache and tuned connection parameters.
- Explicit TTLock protocol rejections are not followed by an expensive immediate BLE reconnect; transport/connect failures remain retryable.

## Safe multi-lock webhook handling (v4)

Each `GET /api/unlock` request is authenticated, decrypted and enqueued independently and receives its own immediate HTTP 202 response/request ID. The ESP32 NimBLE host has one BLE radio owner: unlock jobs are executed serially by a single worker. This is intentional; concurrent GAP connection attempts caused `Already attempting to connect`, `Unable to scan - connection in progress`, connection failures, and eventually a Load access fault. Multiple locks from one UniFi reader are therefore safe: both webhooks are accepted immediately, then lock A and lock B are unlocked in queue order. The v2 early-stop 350 ms target scan, direct-MAC fallback, per-MAC GATT cache, CRC validation, and protocol-rejection handling are retained.

## v6 unlock latency optimization

The normal unlock path now uses **direct-MAC connect first**. It no longer spends ~350 ms on a foreground scan before every attempt. If the direct connection fails, attempt 2 performs the short targeted scan as a recovery mechanism and then reconnects. Existing discovered GATT attributes are preserved across ordinary connection-establishment failures; empty/new client objects are discarded before retry. The bridge intentionally keeps a single BLE radio owner because overlapping ESP32-C3/NimBLE connection procedures proved unsafe. Separate webhook requests are still accepted and queued independently.

Expected serial markers:

- `[TTLock][V6] fast path: skip foreground scan; direct-MAC connect`
- `[TTLock][V6] recovery attempt: short targeted scan before reconnect`
- `[TTLock][CACHE] connect failed; preserving existing GATT cache for retry`

## v7 unlock latency tuning

- Normal webhook unlock attempt 1 connects directly by MAC with no foreground scan.
- NimBLE automatic retries after connection-establishment error `0x3e` are disabled (`setConnectRetries(0)`).
- Fast attempt timeout: 1800 ms.
- Recovery attempt: 350 ms targeted scan, then connect with a 2500 ms timeout.
- Existing per-lock GATT cache is preserved across ordinary GAP connection failures.
- The single BLE worker remains in place; separate webhook GET requests are acknowledged and queued independently.

## V7.1 queue retry

V7.1 keeps the V7 BLE fast/recovery policy unchanged. If both normal V7 attempts fail, the unlock job is moved to the end of the worker queue once. Other waiting locks are serviced first. When the failed job returns, it performs one recovery-only attempt (targeted scan + recovery connect), then succeeds or fails permanently. If no other lock is waiting, a 500 ms cooldown is used before the final recovery attempt.
