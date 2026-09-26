# TTLock protocol support

The bridge now treats the TTLock protocol version as an explicit protocol family instead of assuming every lock is V3.

| protocolType | protocolVersion | TTLock generation | Detection/config | Local unlock | Factory init |
|---|---:|---|---|---|---|
| 5 | 1 | V2 `LOCK` | Yes | Not yet | No |
| 5 | 4 | V2 | Yes | Not yet | No |
| 5 | 3 | V3 | Yes | Yes | Yes |

Why V2 is not silently sent through the V3 codec: TTLock's SDK identifies 5/1 and 5/4 as V2 and 5/3 as V3. The public reverse-engineered implementations used by this project document the V3 frame/command codec only. Sending provisioning or unlock commands using the wrong codec can leave a lock in an unknown state.

The code is now split so a V2 codec can be added without changing the BLE transport, scanner, web API, storage or UniFi integration. Add the V2 implementation behind `unlockAttempt()` and keep its packet builder separate from `buildFrame()`.
