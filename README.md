# RBIO_RTC — Facility Time Server

A single ESP32 + DS3231 that becomes the time authority for your entire facility. Every device gets consistent, reliable time — no per-device RTC modules, no internet dependency after initial sync.

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
2. **Connect to the "RBIO_RTC" WiFi network** (password: `rbio_time`) from your phone or laptop.
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

Connect to the "RBIO_RTC" WiFi AP and query `192.168.4.1:123`. This works without a router but is limited to 8 simultaneous WiFi clients (with a 30-second auto-disconnect to free slots). Best for initial setup and devices that sync infrequently.

### Option 3: ESP-NOW (ESP32 devices only)

ESP-NOW beacons are broadcast every 5 seconds. ESP32 clients receive time without joining any WiFi network — no association, no password, no connection limit.

**Unsigned mode (zero-config):**
```c
#include "rbio_rtc_client.h"

void on_time(const rbio_time_t *t) {
    struct timeval tv = { .tv_sec = t->epoch, .tv_usec = t->ms * 1000 };
    settimeofday(&tv, NULL);
}

void app_main(void) {
    // init WiFi in any mode first...
    rbio_rtc_client_init(NULL, on_time);
}
```

**Signed mode (authenticated):**
```c
uint8_t psk[32] = { /* copy from server web UI */ };
rbio_rtc_client_init(psk, on_time);
```

The client reference implementation is in the `client_example/` directory. Copy `rbio_rtc_client.h` and `rbio_rtc_client.c` into your project.

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
| AP SSID | `RBIO_RTC` | `wifi_manager.c` |
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
