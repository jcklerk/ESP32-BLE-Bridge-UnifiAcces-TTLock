# UniFi Access → TTLock ESP32 BLE Bridge

A local ESP32 bridge that allows **UniFi Access** to unlock
**TTLock-compatible BLE smart cylinders**.

The bridge listens for webhook calls from UniFi Access. When an
authorized UniFi Access reader unlock event occurs, UniFi Alarm Manager
calls the ESP32. The ESP32 decrypts the selected lock configuration,
connects to that TTLock over Bluetooth Low Energy, sends the unlock
command, and disconnects again.

This makes it possible to use a **UniFi Access Reader as the
credential/access-control system** while using one or more **TTLock BLE
cylinders as the physical locks**.

> \[!IMPORTANT\] This project uses an unofficial, reverse-engineered
> TTLock BLE protocol. Test the complete installation before using it on
> an important door. The ESP32 bridge is an integration layer; it is not
> a replacement for required mechanical, fire-safety, emergency-egress,
> or certified access-control hardware.
> This gateway was tested on the TTLock V3 / `5A01` style locks used during development.

---

## Features

- UniFi Access → TTLock integration using **Alarm Manager webhooks**
- Local operation; normal unlocking does not require the TTLock cloud
- ESP32-C3 + NimBLE
- Supports TTLock V3 / `5A01` style locks used during development
- TTLock BLE service `0x1910`
- Cached BLE/GATT information for faster unlocks
- Continuous background discovery of TTLock advertisements
- Direct BLE connection to configured lock MAC addresses
- Disconnects after each unlock instead of maintaining permanent BLE
  connections
- Multiple TTLock locks can be controlled by one ESP32 bridge
- Multiple webhook actions can be attached to one UniFi Access reader
- Multiple TTLock locks can therefore unlock from one UniFi reader
  event
- FreeRTOS unlock queue
- Burst handling for multiple nearly simultaneous unlock requests
- Failed jobs are moved to the end of the queue and retried
- Up to 5 deferred recovery rounds
- BLE connection retry/recovery
- Protocol rejection retry/recovery
- SNTP/system-time preflight before an unlock is attempted
- Encrypted lock configuration in webhook headers
- Local web portal for setup, BLE scanning, provisioning, testing and
  webhook generation
- Factory-new TTLock V3 initialization support
- Site encryption key and webhook Bearer authentication
- Wi-Fi development configuration and optional Ethernet/PoE support

---

## How it works

The basic data flow is:

```text
User presents credential
        │
        ▼
UniFi Access Reader
        │
        ▼
UniFi Access authorizes access
        │
        ▼
Door Unlocked event
        │
        ▼
UniFi Alarm Manager
        │
        ├── Webhook → TTLock Front Door
        │
        ├── Webhook → TTLock Inner Door
        │
        └── Webhook → other TTLock(s)
                        │
                        ▼
                  ESP32 Bridge
                        │
                 decrypt lock data
                        │
                  queue unlock job
                        │
                 BLE / NimBLE
                        │
                        ▼
                    TTLock
```

UniFi Access remains responsible for deciding **who is allowed to
enter**. The ESP32 only reacts to the resulting webhook and unlocks the
TTLock associated with that webhook.

### TTLock unlock flow

For a normal TTLock V3 unlock the bridge performs approximately:

```text
Receive webhook
    ↓
Authenticate Bearer token
    ↓
Decrypt lock configuration
    ↓
Queue UnlockJob
    ↓
Use cached TTLock advertisement
    ↓
Connect over BLE
    ↓
Reuse cached GATT attributes when available
    ↓
Subscribe to FFF4 notifications
    ↓
Send CHECK_USER_TIME (0x55)
    ↓
Receive lock response
    ↓
Send UNLOCK (0x47)
    ↓
Receive success response
    ↓
Disconnect
```

The BLE connection is not kept permanently open. This keeps the design
simple and avoids maintaining an unnecessary connection to
battery-powered cylinders.

---

## Multiple locks on one reader

One UniFi Access reader can trigger **multiple TTLock cylinders**.

For example:

```text
Reader: Main Entrance

Door Unlocked
    │
    ├── Webhook 1 → Front Door TTLock
    ├── Webhook 2 → Hall TTLock
    └── Webhook 3 → Equipment Room TTLock
```

Each webhook contains the encrypted configuration for a specific TTLock.

When these webhooks arrive close together, the ESP32 places them in its
unlock queue and processes them using one BLE radio.

The current v7.9 firmware includes burst handling so the next queued
lock can be processed immediately after the previous lock finishes.

---

## Reliability and retry system

BLE is a radio protocol and an individual connection can occasionally
fail.

The bridge therefore does not immediately discard an unlock request.

The current reliability strategy is:

```text
Normal unlock
    │
    ├── Success → done
    │
    └── Failure
          ↓
       recovery attempt
          │
          ├── Success → done
          │
          └── Failure / lock rejects command
                ↓
          move request to END of queue
                ↓
          allow other locks to run
                ↓
          retry failed lock later
```

v7.9 allows up to **5 deferred recovery rounds**.

This is particularly useful when several locks are connected to one
reader. A temporary problem with one lock should not prevent the other
locks from being processed.

### System clock

TTLock's unlock protocol uses the current time.

Before starting BLE, v7.9 verifies that the ESP32 has a valid system
clock. If SNTP has not synchronized yet, the request is requeued instead
of wasting BLE attempts.

For isolated networks, configure a reachable local NTP server.

---

# Hardware

The current PlatformIO target is:

```ini
board = seeed_xiao_esp32c3
framework = arduino
```

The project was developed with a **Seeed Studio XIAO ESP32-C3**.

The bridge can run using:

- Wi-Fi, or
- Ethernet/PoE hardware supported by the project configuration.

For a permanent access-control installation, Ethernet/PoE is generally
preferable because the bridge can be powered and networked from the same
infrastructure.

---

# Building and flashing

## Requirements

Install:

- Visual Studio Code
- PlatformIO
- USB data cable for the ESP32-C3

Or install PlatformIO Core and use it from the terminal.

The important project dependencies are installed automatically by
PlatformIO:

```ini
h2zero/NimBLE-Arduino@^2.5.1
bblanchon/ArduinoJson@^7.4.2
```

---

## Configure networking

Open:

```text
platformio.ini
```

### Wi-Fi

For Wi-Fi operation:

```ini
-D USE_ETHERNET=0
'-D WIFI_SSID="YOUR_WIFI_SSID"'
'-D WIFI_PASSWORD="YOUR_WIFI_PASSWORD"'
```

Do **not** commit real Wi-Fi credentials to a public Git repository.

### Ethernet / PoE

Enable Ethernet:

```ini
-D USE_ETHERNET=1
```

Then configure the PHY for your ESP32 Ethernet/PoE board.

Example LAN8720 settings:

```ini
-D ETH_PHY_TYPE=ETH_PHY_LAN8720
-D ETH_PHY_ADDR=0
-D ETH_PHY_MDC=23
-D ETH_PHY_MDIO=18
-D ETH_PHY_POWER=-1
-D ETH_CLK_MODE=ETH_CLOCK_GPIO0_IN
```

The correct pins depend on the board.

---

## Optional NTP configuration

The TTLock unlock packet requires valid time.

If the bridge cannot reach public NTP servers, configure local NTP
servers:

```ini
-D NTP_SERVER_1=\"192.168.1.1\"
-D NTP_SERVER_2=\"192.168.1.2\"
```

Use addresses that are reachable from the ESP32's VLAN.

---

## Flash with PlatformIO

Connect the ESP32 over USB.

Build:

```bash
pio run
```

Upload:

```bash
pio run -t upload
```

Open the serial monitor:

```bash
pio device monitor
```

The configured serial speed is:

```text
115200 baud
```

After boot you should see messages similar to:

```text
UniFi Access -> TTLock Arduino bridge
Web portal listening on port 80
[BLE-WORKER] single radio owner ready ...
```

---

# First-time setup

After the ESP32 is connected to the network, determine its IP address
from:

- the serial console, or
- your DHCP/UniFi client list.

Open:

```text
http://ESP32-IP/
```

For example:

```text
http://192.168.1.50/
```

On first setup:

1.  Create the local administrator password.
2.  The bridge generates a **Site Key**.
3.  The bridge generates a **Webhook Token**.
4.  Store the Site Key somewhere safe.

The Site Key is important because it encrypts the TTLock configuration
used by the UniFi webhooks.

If the ESP32 has to be replaced, using the same Site Key allows the
existing encrypted TTLock data to remain usable.

---

# Adding an existing TTLock

Open the ESP32 web portal.

Use **Scan nearby TTLocks**.

The bridge scans for compatible TTLock BLE devices and displays
information such as:

```text
5A01_6cea64
21:BF:37:64:EA:6C
-50 dBm
```

Select the required lock.

For an already initialized TTLock, configure the required lock
information, including:

- Lock ID
- Lock name
- BLE MAC address
- AES key
- Unlock key
- Protocol type
- Protocol version
- Scene
- Group ID
- Organisation ID

Typical protocol values for the tested V3 locks are:

```text
Protocol type:    5
Protocol version: 3
Scene:            2
Group ID:         1
Organisation ID:  1
```

Use **Test Unlock** before configuring UniFi.

The lock should physically unlock and the serial console should finish
with something similar to:

```text
Unlock Front Door: SUCCESS
```

---

# Initializing a factory-new TTLock

The web portal also contains **Initialize New Lock** for supported
factory-reset TTLock V3 devices.

Typical workflow:

1.  Factory-reset the TTLock.
2.  Wake the lock.
3.  Open the ESP32 web portal.
4.  Select **Scan nearby TTLocks**.
5.  Select the factory-reset lock.
6.  Verify the protocol information.
7.  Select **Initialize New Lock**.
8.  Wait for initialization to complete.
9.  Test the lock.
10. Generate the UniFi webhook information.

Factory provisioning currently targets the tested V3 protocol family.
TTLock is an OEM ecosystem and firmware differences exist, so test this
with your exact cylinder model.

---

# Generate the UniFi webhook

After the lock has been configured and tested, select:

**Generate UniFi context**

The portal generates something similar to:

```text
Authorization: Bearer YOUR_WEBHOOK_TOKEN

X-TTLock-Nonce: ENCRYPTED_NONCE
X-TTLock-Data: ENCRYPTED_LOCK_DATA
X-TTLock-Tag: AUTHENTICATION_TAG

Method: GET or POST
URL: http://ESP32-IP/api/unlock
Body: none
```

Each lock gets different encrypted TTLock headers.

The lock's AES key and unlock key are encrypted inside `X-TTLock-Data`.

Do not manually copy the headers from one lock to another.

---

# Configure UniFi Access

UniFi Alarm Manager is used to connect an authorized Access unlock event
to the ESP32.

Ubiquiti documents Alarm Manager as consisting of:

1.  **Trigger**
2.  **Scope**
3.  **Action**

For this project:

```text
Trigger = Door unlocked
Scope   = Specific Access reader / door
Action  = Webhook to ESP32
```

---

## 1. Open Alarm Manager

Open your UniFi Console and go to:

```text
UniFi Access
    ↓
Alarm Manager
```

Select:

**Create Alarm**

Give it a useful name, for example:

```text
Front Entrance → TTLock
```

---

## 2. Select the unlock trigger

Select an Access unlock event as the trigger.

The intended configuration is:

```text
Category: Unlocks
Event:    Door Unlocked
```

The exact wording can vary slightly between UniFi Access versions.

The important point is that the alarm should run when UniFi Access has
accepted the access event and the selected door/reader is unlocked.

---

## 3. Select the specific reader / door

Set the alarm **Scope** to the Access device or door that should control
the TTLock.

For example:

```text
Trigger:
    Door Unlocked

Scope:
    Main Entrance Reader
```

Do not use every reader/site-wide scope unless that is intentionally
required.

A specific scope prevents another Access reader from accidentally
triggering the TTLock webhook.

---

# 4. Add the webhook

Under the alarm's actions, add a **Custom Webhook**.

Use the URL generated by the ESP32:

```text
http://ESP32-IP/api/unlock
```

The bridge accepts:

```text
GET
```

or:

```text
POST
```

No request body is required.

---

## 5. Add authentication

Add this custom HTTP header:

```text
Authorization: Bearer YOUR_WEBHOOK_TOKEN
```

The token must match the **Webhook Token** shown in the ESP32 web
portal.

Requests with an invalid token are rejected.

---

## 6. Add the encrypted TTLock headers

Copy the three headers generated by the ESP32 for this particular lock:

```text
X-TTLock-Nonce: ...
X-TTLock-Data: ...
X-TTLock-Tag: ...
```

A complete webhook therefore looks conceptually like:

```http
GET /api/unlock HTTP/1.1
Host: 192.168.1.50
Authorization: Bearer YOUR_WEBHOOK_TOKEN
X-TTLock-Nonce: ...
X-TTLock-Data: ...
X-TTLock-Tag: ...
```

There is no request body.

When the request is accepted, the ESP32 normally returns:

```text
HTTP 202 Accepted
```

`202` means the request has been authenticated, decrypted and placed
into the BLE unlock queue. The physical BLE unlock continues
asynchronously.

---

# Multiple TTLocks for one UniFi reader

This is supported.

For example, suppose the **Main Entrance Reader** should unlock two
TTLock cylinders:

```text
Main Entrance Reader
       │
       └── Door Unlocked
              │
              ├── Webhook → Front Door
              └── Webhook → Inner Door
```

Create/add a separate webhook action for each TTLock.

### Webhook 1

```text
URL:
http://192.168.1.50/api/unlock

Authorization:
Bearer <same bridge token>

X-TTLock-Nonce:
<Front Door nonce>

X-TTLock-Data:
<Front Door encrypted data>

X-TTLock-Tag:
<Front Door tag>
```

### Webhook 2

```text
URL:
http://192.168.1.50/api/unlock

Authorization:
Bearer <same bridge token>

X-TTLock-Nonce:
<Inner Door nonce>

X-TTLock-Data:
<Inner Door encrypted data>

X-TTLock-Tag:
<Inner Door tag>
```

Both webhooks point to the same ESP32 endpoint.

The encrypted headers tell the bridge **which TTLock should be
unlocked**.

When UniFi sends both webhooks, the ESP32 queues both requests and
processes them one after another.

---

# Example installation

```text
                         ┌──────────────────────────┐
                         │      UniFi Console       │
                         │      UniFi Access        │
                         └────────────┬─────────────┘
                                      │
                               Alarm Manager
                                      │
                           Door Unlocked event
                                      │
                   ┌──────────────────┴──────────────────┐
                   │                                     │
             Webhook Lock A                        Webhook Lock B
                   │                                     │
                   └──────────────────┬──────────────────┘
                                      │
                                      ▼
                         ┌──────────────────────────┐
                         │       ESP32 Bridge       │
                         │                          │
                         │ HTTP webhook endpoint    │
                         │ encrypted lock config    │
                         │ FreeRTOS queue           │
                         │ NimBLE                   │
                         └────────────┬─────────────┘
                                      │
                           Bluetooth Low Energy
                          ┌───────────┴───────────┐
                          │                       │
                          ▼                       ▼
                    TTLock A                  TTLock B
```

---

# Webhook security

The unlock endpoint is:

```text
/api/unlock
```

It requires a valid:

```text
Authorization: Bearer ...
```

and valid encrypted TTLock headers.

The lock configuration is protected using the bridge's Site Key.

Recommended production setup:

- Put UniFi Access and the ESP32 bridge on a trusted/isolated VLAN.
- Give the ESP32 a static DHCP reservation.
- Firewall access to the ESP32.
- Ideally only allow the UniFi Console/controller to access
  `/api/unlock`.
- Do not expose the ESP32 web portal to the Internet.
- Keep the Webhook Token private.
- Keep the Site Key backed up securely.
- Do not commit Wi-Fi credentials to Git.
- Consider ESP32 Secure Boot and Flash Encryption for production
  installations.

The current embedded web portal uses HTTP, so it should only be
reachable from a trusted management network.

---

# Important backup information

Back up at least:

```text
Site Key
Webhook Token
Lock configuration / generated webhook headers
```

The **Site Key is especially important**.

If you replace the ESP32 and restore the same Site Key, existing
encrypted lock webhook data can continue to be decrypted.

If you generate a new Site Key, regenerate the webhook headers for every
lock.

If you generate a new Webhook Token, update the `Authorization` header
in UniFi Alarm Manager.

---

# Troubleshooting

## Webhook returns unauthorized

Check:

```text
Authorization: Bearer <token>
```

Make sure the token exactly matches the ESP32 portal.

---

## Webhook is accepted but lock does not immediately unlock

Check the serial monitor:

```bash
pio device monitor
```

Look for:

```text
Unlock request:
BLE connect
0x55 exchange
0x47 exchange
Unlock ...: SUCCESS
```

The HTTP request can return `202` before BLE processing is finished.
This is intentional.

---

## `UNLOCK rejected by lock`

The lock was successfully reached over BLE, but the TTLock rejected that
unlock command.

v7.9 automatically moves the request to the end of the queue and retries
it.

Look for:

```text
[RELIABLE] attempt round failed
[RELIABLE] retry queued
```

followed later by:

```text
Unlock ...: SUCCESS
```

---

## BLE connection fails

The bridge automatically performs recovery attempts.

An occasional BLE connection failure can happen because the TTLock is a
battery-powered advertising device.

The continuous scanner and GATT cache are used to reduce connection
time.

---

## Clock / SNTP problem

If the serial console reports that the system clock is not ready, make
sure the ESP32 can reach an NTP server.

For isolated VLANs, configure local NTP servers in `platformio.ini`.

v7.9 keeps the unlock request queued while waiting for valid time rather
than immediately losing the request.

---

## One reader controls several locks

Add **multiple webhook actions** to the same UniFi Access alarm.

Generate the headers separately for each TTLock.

Do not use the encrypted `X-TTLock-*` headers from Lock A for Lock B.

---

# Current v7.9 architecture

```text
WebPortal
   │
   ├── setup/login
   ├── BLE scan
   ├── initialize lock
   ├── test lock
   ├── generate encrypted webhook headers
   └── /api/unlock
            │
            ▼
       Unlock Queue
            │
            ▼
       BLE Worker
            │
            ▼
      TTLockClient
            │
            ├── background sightings
            ├── per-lock GATT cache
            ├── fast connection
            ├── recovery connection
            ├── CHECK_USER_TIME
            ├── UNLOCK
            └── disconnect
```

Only one BLE operation owns the ESP32-C3 radio at a time.

Incoming HTTP webhook requests can still be received while another lock
is being processed because they are placed into the FreeRTOS queue.

---

# API overview

The project currently exposes endpoints including:

```text
GET  /api/status
POST /api/setup
POST /api/login

GET  /api/secrets

POST /api/settings/site-key
POST /api/settings/generate-site-key
POST /api/settings/regenerate-token

GET  /api/ble/scan

POST /api/locks/init
POST /api/locks/test
POST /api/locks/bundle

GET  /api/unlock
POST /api/unlock
```

The setup/configuration endpoints are intended for the local web portal.

The UniFi integration uses:

```text
GET/POST /api/unlock
```

---

# UniFi requirements

UniFi Alarm Manager is available in UniFi Access on supported/current
UniFi OS and Access versions.

The UniFi documentation describes Access alarm triggers including **door
unlocked** events and allows the scope to be restricted to individual
Access devices or door locations.

Official documentation:

UniFi Alarm Manager -- Customize Alerts, Integrations and Automations
Across UniFi

https://help.ui.com/hc/en-us/articles/27721287753239-UniFi-Alarm-Manager-Customize-Alerts-Integrations-and-Automations-Across-UniFi

---

# Project status

The current development baseline is **v7.9**.

The focus of v7.9 is:

- reliable multi-lock operation;
- fast cached BLE connections;
- queueing simultaneous UniFi webhook requests;
- automatic recovery instead of silently skipping a lock;
- retaining the fast v7.8 burst behavior.

Example successful flow:

```text
Webhook Lock A
    ↓
Lock A SUCCESS
    ↓
direct queue handoff
    ↓
Webhook Lock B
    ↓
Lock B SUCCESS
```

Example recovery flow:

```text
Lock A
    ↓
0x47 rejected
    ↓
requeue at end
    ↓
other jobs processed
    ↓
Lock A recovery
    ↓
SUCCESS
```

---

# Disclaimer

This project is not affiliated with or endorsed by Ubiquiti, UniFi,
TTLock, Sciener, or their respective manufacturers.

TTLock BLE behavior is based on publicly available reverse-engineering
work and testing with the target locks. Different TTLock/OEM firmware
versions may behave differently.

Always retain a safe mechanical/emergency method of opening doors and
comply with applicable building, fire-safety, insurance and
access-control requirements.
