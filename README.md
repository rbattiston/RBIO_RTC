# RBIO_RTC — Facility Time Server

A single ESP32 + DS3231 that becomes the time authority for your entire facility. Every device gets consistent, reliable time — no per-device RTC modules, no internet dependency after initial sync.

---

## Migration Notice: Client Updates (2026-04-13)

**If you implemented a client against the original codebase, please read this section.** There have been two client updates.

### What changed

**1. Channel discovery is now automatic** (commit `b871f5f`). The client scans all WiFi channels on init and locks to whichever one the server is broadcasting on. Previously, the client listened on whatever channel the WiFi stack defaulted to (usually channel 1), which silently failed if the server's router was on a different channel.

**2. `rbio_rtc_client_deinit()` added + sync-once usage pattern clarified.** The beacon system is designed for periodic check-ins (boot + every 6-24 hours), not continuous listening. The new `rbio_rtc_client_deinit()` API lets you cleanly tear down the client after getting time, freeing the radio for WiFi/BLE/deep sleep. If you've been leaving the client running indefinitely, switch to the sync-once pattern — see [example_main.c](client_example/example_main.c) and the "Client Usage Philosophy" section below.

**Secondary fix:** The client's HMAC verification used `mbedtls_md_hmac()`, which is a private API in ESP-IDF v6.0's mbedtls 4.x and will fail to compile. The new client uses a manual HMAC-SHA256 implementation instead.

### What you need to do

**For most integrations — nothing in your code.** The public API is unchanged:

```c
rbio_rtc_client_init(psk, callback);  // same signature
rbio_rtc_client_request(mac);          // same signature
```

Just replace the two files in [client_example/](client_example/):
- `rbio_rtc_client.h`
- `rbio_rtc_client.c`

### One behavioral constraint to verify

The new client requires that your WiFi STA is **not connected to any AP** when calling `rbio_rtc_client_init()`. Channel scanning needs control of the radio, which it can't have if the STA is associated.

If your code does this, you're fine (this is the standard pattern):
```c
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();
rbio_rtc_client_init(NULL, on_time);  // OK
```

If your code does this, you need to restructure:
```c
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_start();
esp_wifi_connect();                    // ← this breaks channel scanning
rbio_rtc_client_init(NULL, on_time);   // ← will not find server
```

Three resolution options:
1. **Get time first, connect to WiFi after.** Call `rbio_rtc_client_init()` before `esp_wifi_connect()`. Once the callback fires with valid time, you can connect to your AP normally.
2. **Connect to the same router as the RBIO_RTC server.** You'll be on the same channel, so no scanning is needed — beacons arrive on your associated channel.
3. **Skip ESP-NOW, use SNTP.** If your device will be connected to the LAN anyway, point a standard NTP client at the server's STA IP (visible on the server's web UI).

### Observable behavioral changes

- `rbio_rtc_client_init()` now spawns a background scan task (stack: 3072 bytes, priority: 5). Returns immediately as before.
- First callback now fires within ~300ms to 6 seconds after init (depending on which channel the server is on). Previously it fired immediately if you were lucky with channels, never if unlucky.
- A new harmless log line may appear: `W ESPNOW: Peer exists` when `rbio_rtc_client_request()` is called after the scanner has already added the broadcast peer. Ignorable.
- New log lines during scanning: `Scanning channels 1-13...`, `Beacon found on channel N, locked`. Set the `rbio_client` log tag to `ESP_LOG_WARN` to suppress if desired.

### What did not change

- Beacon protocol (v1 and v2 wire formats are identical)
- PSK format (32 bytes)
- Callback signature and `rbio_time_t` struct fields
- HMAC algorithm (still HMAC-SHA256 truncated to 20 bytes)
- Replay protection semantics (monotonic sequence numbers)
- Server-side code (server broadcasts are unchanged; it answers requests as before)

### Quick compatibility check

If your existing client currently works reliably, one of these is true:
1. You happen to be on the same channel as the server (likely because your client's WiFi STA connects to the same router). You're fine either way — the update won't break you, and gives you resilience if the server later moves to a different router.
2. You compile against ESP-IDF 5.x where `mbedtls_md_hmac()` was still public. The update is still recommended for ESP-IDF 6.0 readiness.

If your existing client intermittently fails or fails in some deployments but not others, that's the channel mismatch problem and this update fixes it.

---

## The Problem

You're building multiple devices that need to know the time. Adding a DS3231 to each one is expensive, tedious, and creates a synchronization nightmare — every RTC drifts independently. Connecting each device to the internet for NTP isn't always possible in workshops, labs, or industrial environments.

## The Solution

One ESP32 with a battery-backed DS3231 serves time to everything:

```
                    ┌──────────────────────────────────┐
                    │   RBIO_RTC (ESP32 + DS3231)      │
                    │                                  │
  Internet ─────── │ STA ──► NTP sync ──► DS3231      │
  (one-time or     │                        │         │
   periodic)       │              ┌─────────┘         │
                    │              ▼                    │
                    │   System Clock (runtime authority)│
                    │      │              │            │
                    │      ▼              ▼            │
                    │  SNTP :123    ESP-NOW Beacons    │
                    │  (WiFi AP     (broadcast every   │
                    │   + LAN)       5 seconds)        │
                    └──────────────────────────────────┘
                         │                    │
              ┌──────────┘                    └──────────┐
              ▼                                          ▼
     Any NTP client                              Any ESP32
     (Pi, Arduino,                            (no WiFi association
      laptop, etc.)                            needed, just listens)
```

## Design Philosophy

**Robustness over precision.** This is a minute-level time source, not a nanosecond one. The DS3231 drifts ~1 minute per year. That's fine. What matters is that every device in the facility agrees on what time it is.

**Facility-internal consistency over world-time accuracy.** If the RTC is 3 seconds off from UTC, every device is 3 seconds off together. That's a feature — your logs correlate, your schedules fire in sync.

**Fully automatic operation.** No manual time setting. No serial commands. Plug it in, configure WiFi once through the web UI, and forget about it. NTP sets the DS3231, and the DS3231 keeps time across power cycles.

**The DS3231 is the persistent truth.** The system clock is the runtime authority (what serves time to clients), but the DS3231 is the persistent store. If the ESP32 reboots, it reads the DS3231 immediately. If NTP corrects the system clock, it writes back to the DS3231. There is exactly one code path that writes the RTC: a successful NTP sync.

**Defense in depth for ESP-NOW.** Unsigned v1 beacons for zero-config compatibility. Signed v2 beacons (HMAC-SHA256 + monotonic sequence numbers) for environments that need authentication. Clients choose their security level.

## Hardware

| Component | Purpose | Notes |
|-----------|---------|-------|
| ESP32 WROOM DevKit | Main MCU | Any ESP32 dev board works |
| DS3231 module | Battery-backed RTC | Must be DS3231, not DS1307 (temperature-compensated, ~2ppm drift) |

### Wiring

| DS3231 Pin | ESP32 Pin |
|------------|-----------|
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| VCC | 3.3V |
| GND | GND |

The DS3231 module includes a CR2032 battery holder. With the battery installed, the RTC keeps time through power loss indefinitely.

## Building

Requires [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/).

```bash
idf.py set-target esp32
idf.py build
idf.py -p COMx flash    # replace COMx with your port
idf.py -p COMx monitor  # optional: watch boot log
```

## First-Time Setup

1. **Flash the firmware** and power up the device.
2. **Connect to the "RBIO_RTC_XXXXX" WiFi network** (password: `rbio_time`) from your phone or laptop. The `XXXXX` is the last 5 hex digits of the device's MAC address, so each physical device has a unique SSID. The device also blinks its identifier count in blue on the LED every ~12 seconds to help you match a specific device to its SSID.
3. **Open `http://192.168.4.1`** in a browser.
4. **Enter your router's SSID and password** and click Connect.
5. The device connects to your router, reaches NTP servers, sets the DS3231, and starts serving time. Done.

After this, the device is fully autonomous. It will:
- Reconnect to your router automatically on reboot
- Re-sync from NTP periodically
- Serve time immediately from the DS3231 even before NTP syncs (on subsequent boots)
- Keep time through power loss via the DS3231's battery backup

## How Clients Get Time

### Option 1: Standard NTP (any device on the LAN)

Once the RBIO_RTC is connected to your router (STA mode), any device on the same network can use it as an NTP server. Point your NTP client at the RBIO_RTC's STA IP address (shown on the web UI).

**ESP-IDF:**
```c
esp_sntp_setservername(0, "192.168.1.50");  // use actual STA IP
```

**Arduino:**
```c
configTime(0, 0, "192.168.1.50");
```

**Raspberry Pi (Linux):**
```bash
# /etc/systemd/timesyncd.conf
[Time]
NTP=192.168.1.50
```

**Any device with NTP support** works. No special client code needed. This path supports unlimited clients — there is no connection limit when querying via the LAN.

### Option 2: NTP via the AP (direct connection)

Connect to the "RBIO_RTC_XXXXX" WiFi AP (where XXXXX is the last 5 hex digits of the device's MAC) and query `192.168.4.1:123`. This works without a router but is limited to 8 simultaneous WiFi clients (with a 30-second auto-disconnect to free slots). Best for initial setup and devices that sync infrequently.

### Option 3: ESP-NOW (ESP32 devices only)

ESP-NOW beacons are broadcast every 5 seconds. ESP32 clients receive time without joining any WiFi network — no association, no password, no connection limit.

**Recommended pattern — sync once, then free the radio:**
```c
#include "rbio_rtc_client.h"

static SemaphoreHandle_t s_sync_done;

static void on_time(const rbio_time_t *t) {
    struct timeval tv = { .tv_sec = t->epoch, .tv_usec = t->ms * 1000 };
    settimeofday(&tv, NULL);
    xSemaphoreGive(s_sync_done);
}

void sync_time_from_rbio(void) {
    s_sync_done = xSemaphoreCreateBinary();
    rbio_rtc_client_init(NULL, on_time);
    xSemaphoreTake(s_sync_done, pdMS_TO_TICKS(10000));  // 10s timeout
    rbio_rtc_client_deinit();
    vSemaphoreDelete(s_sync_done);
}

void app_main(void) {
    // init WiFi (STA, unassociated) ...
    sync_time_from_rbio();
    // Radio is free now. Do your work.
    // Re-sync every 6-24 hours as drift tolerance requires.
}
```

**Signed mode (authenticated):**
```c
uint8_t psk[32] = { /* copy from server web UI */ };
rbio_rtc_client_init(psk, on_time);
```

The client reference implementation is in the `client_example/` directory. Copy `rbio_rtc_client.h` and `rbio_rtc_client.c` into your project. See [example_main.c](client_example/example_main.c) for a complete working example with periodic re-sync.

### Client Usage Philosophy: Sync-and-Disconnect, Not Live Listening

The beacon system is designed for **periodic check-ins** — not a constant radio connection. Think of it like NTP: you sync on boot and occasionally afterward, not continuously.

**When to sync:**
- On boot (internal RTC is lost on power cycle)
- Periodically, based on your drift tolerance:
  - **Minute-level accuracy:** every 24 hours (ESP32 drifts ~1.7s/day)
  - **Second-level accuracy:** every 1-6 hours
  - **Sub-second accuracy:** not really this device's purpose — use NTP on a wired server

**Why not leave the client running?**
- The radio draws ~80mA active — kills battery life on anything portable
- The radio is tied up — can't use WiFi, BLE, or ESP-NOW for other peers
- Listening continuously provides no value — facility time doesn't change faster than your crystal drifts

**The recommended flow:**
1. Power on → `rbio_rtc_client_init()` → wait for callback (~1 second) → `rbio_rtc_client_deinit()`
2. Do your application work using the system clock
3. Set a timer or RTC alarm to repeat step 1 every 6-24 hours

## Which Method Should I Use?

| Your device | Recommended method | Why |
|-------------|-------------------|-----|
| ESP32 (any variant) | ESP-NOW | No WiFi association needed, works without a router, unlimited clients |
| Raspberry Pi | NTP via LAN | Native `timedatectl` / `systemd-timesyncd` support |
| Arduino with WiFi | NTP via LAN | Standard `configTime()` |
| Non-networked device | Wire a DS3231 to it | This server can't help if there's no radio |

## Web Interface

Accessible at `http://192.168.4.1` when connected to the RBIO_RTC AP.

The status page shows:
- **Current time** and source (NTP, DS3231, or none)
- **Battery status** — reads the DS3231's Oscillator Stop Flag. "GOOD" means the backup battery is healthy. "BAD" means the oscillator stopped at some point (replace the CR2032).
- **RTC temperature** — the DS3231's on-die temperature sensor (useful for monitoring the device's environment)
- **Network status** — STA connection, IP address
- **ESP-NOW security** — whether signed v2 beacons are active, PSK fingerprint

### API Endpoints

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/` | GET | Status page + WiFi config form |
| `/wifi` | POST | Save STA credentials |
| `/psk` | POST | Save ESP-NOW pre-shared key (64 hex chars) |
| `/psk/generate` | GET | Generate a random 256-bit PSK (returns hex string) |
| `/status` | GET | JSON status for programmatic access |

### JSON Status Example

```json
{
  "time": "2026-04-12T18:30:00Z",
  "source": "NTP",
  "rtc_set": true,
  "battery_ok": true,
  "rtc_temp_c": 24.5,
  "sta_connected": true,
  "sta_ssid": "MyRouter",
  "sta_ip": "192.168.1.50",
  "espnow_v2_active": true,
  "espnow_psk_fingerprint": "a1b2c3d4...",
  "sntp_served": 142,
  "sntp_dropped": 0
}
```

## Security Model

### Time cannot be set manually

There is no serial command, no API endpoint, no code path to manually set the time. The DS3231 is writable only by a successful NTP sync. This is intentional — it prevents accidental misconfiguration and ensures the time source is always traceable.

### WiFi AP access control

The AP uses WPA2-PSK. Only devices with the password can connect and query the SNTP server or access the web UI. The AP auto-disconnects clients after 30 seconds to prevent slot exhaustion.

### SNTP rate limiting

Per-IP rate limiting on the NTP server: 1 query per 2 seconds sustained, with a 3-query burst allowance for initial sync. Excess queries are silently dropped. This prevents a single misbehaving client from monopolizing the ESP32's CPU.

### ESP-NOW beacon authentication

Two beacon versions are broadcast simultaneously:

**v1 (13 bytes, unsigned):** Compatible with any ESP32. XOR checksum catches transmission errors but provides no authentication. Use this if you control the physical space and trust the RF environment.

**v2 (37 bytes, HMAC-SHA256 signed):** Requires a pre-shared key (PSK) configured on both the server and client. Provides:

| Protection | Mechanism |
|------------|-----------|
| Anti-spoofing | HMAC-SHA256 over beacon data — can't forge without the key |
| Anti-replay | Monotonic sequence number — clients reject stale/replayed beacons |
| Anti-evil-twin | HMAC verification — a cloned MAC can't produce valid signatures |

The PSK is a 256-bit key stored in NVS. Generate it from the web UI and flash it into your client devices at build time.

### What this does NOT protect against

- **RF jamming** — an attacker with a 2.4GHz jammer can prevent all communication. This is a physical-layer attack with no software mitigation.
- **NTP response spoofing on the LAN** — standard NTP has no authentication. If an attacker is on your router's network, they could race the server's responses. This affects every NTP server in existence. If you need cryptographic time authentication over IP, you need NTS (RFC 8915) and a server that supports it — that's not this device's purpose.
- **Physical access to the ESP32** — if someone can touch the device, they can reflash it. Secure the physical hardware.

## Architecture

```
main/
├── main.c           — boot sequence, task orchestration
├── ds3231.h/.c      — I2C driver for DS3231 (time read/write, OSF, temperature)
├── time_manager.h/.c — system clock authority, NTP-only RTC writes, drift correction
├── wifi_manager.h/.c — AP+STA concurrent mode, NVS credential storage, client auto-kick
├── sntp_server.h/.c  — UDP:123 NTP responder with per-IP rate limiting
├── espnow_time.h/.c  — v1/v2 beacon broadcast, HMAC-SHA256 signing, sequence persistence
├── http_server.h/.c  — web UI, WiFi config, PSK config, JSON status API

client_example/
├── rbio_rtc_client.h — reference client header with full protocol documentation
├── rbio_rtc_client.c — reference client implementation (drop into any ESP-IDF project)
├── example_main.c    — minimal working example
```

## Configuration Defaults

| Parameter | Value | Where to change |
|-----------|-------|-----------------|
| AP SSID | `RBIO_RTC_<last5hexMAC>` | `wifi_manager.c` |
| AP password | `rbio_time` | `wifi_manager.c` |
| AP max clients | 8 | `wifi_manager.c` |
| AP client timeout | 30 seconds | `wifi_manager.c` |
| ESP-NOW beacon interval | 5 seconds | `espnow_time.h` |
| NTP rate limit | 1 query/2s per IP | `sntp_server.c` |
| DS3231 I2C pins | SDA=21, SCL=22 | `ds3231.h` |
| NTP servers | pool.ntp.org, time.google.com, time.cloudflare.com, time.nist.gov | `time_manager.c` |
| RTC resync interval | 10 minutes | `time_manager.c` |

## License

MIT
