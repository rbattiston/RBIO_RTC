#pragma once

/*
 * RBIO_RTC Client — Reference Implementation
 *
 * Drop this file into any ESP32 project to receive time from an
 * RBIO_RTC server via ESP-NOW beacons.
 *
 * Supports both v1 (unsigned) and v2 (HMAC-SHA256 signed) beacons.
 *
 * ── Intended Usage Pattern ───────────────────────────────────────
 *
 *   This is a SYNC-AND-DISCONNECT client, not a live time feed.
 *
 *   The beacon system exists so your device can check in with the
 *   facility time server occasionally — on boot and periodically
 *   afterward — NOT to maintain a constant radio connection.
 *
 *   Typical flow:
 *     1. On boot, call rbio_rtc_client_init() to get current time.
 *     2. Wait for the callback to fire (usually <1 second).
 *     3. Set your system clock from the received time.
 *     4. Call rbio_rtc_client_deinit() to free the radio.
 *     5. Do your application work using the system clock.
 *     6. Every 6-24 hours (as drift tolerance dictates), repeat.
 *
 *   Why not leave the client running?
 *     - The radio stays active at ~80mA, killing battery life.
 *     - The radio is tied up — can't be used for WiFi, BLE, etc.
 *     - Listening continuously provides no value — facility time
 *       doesn't change faster than your crystal drifts.
 *
 *   The ESP32's internal RTC (accurate to ~20ppm) drifts ~1.7 seconds
 *   per day. If your application needs minute-level accuracy, sync
 *   once every 24 hours. For second-level accuracy, sync every 1-6
 *   hours. Set up a FreeRTOS timer or RTC alarm to trigger re-sync.
 *
 * ── Quick Start (recommended sync-once pattern) ──────────────────
 *
 *   #include "rbio_rtc_client.h"
 *   #include <sys/time.h>
 *
 *   static SemaphoreHandle_t s_sync_done;
 *
 *   static void on_time(const rbio_time_t *t) {
 *       struct timeval tv = { .tv_sec = t->epoch, .tv_usec = t->ms*1000 };
 *       settimeofday(&tv, NULL);
 *       xSemaphoreGive(s_sync_done);
 *   }
 *
 *   void sync_time_from_rbio(void) {
 *       s_sync_done = xSemaphoreCreateBinary();
 *       rbio_rtc_client_init(NULL, on_time);
 *       xSemaphoreTake(s_sync_done, pdMS_TO_TICKS(10000));  // 10s timeout
 *       rbio_rtc_client_deinit();
 *       vSemaphoreDelete(s_sync_done);
 *   }
 *
 *   void app_main(void) {
 *       // ... init WiFi in STA mode (don't connect to any AP) ...
 *       sync_time_from_rbio();
 *       // Radio is now free for other uses. System clock is set.
 *       // Schedule periodic re-sync as your drift tolerance requires.
 *   }
 *
 * ── Continuous-Listening Pattern (if you really need it) ─────────
 *
 *   If your device has no power constraints and never needs the
 *   radio for anything else, you can just leave the client running:
 *
 *     rbio_rtc_client_init(NULL, on_time);
 *     // Callback fires every 5 seconds forever.
 *
 *   Most devices should prefer the sync-once pattern above.
 *
 * ── Channel Scanning ────────────────────────────────────────────
 *
 *   The client autonomously discovers the server's WiFi channel:
 *
 *   1. On init, a background task scans all valid channels (respecting
 *      the device's country regulatory domain).
 *   2. On each channel, it sends an ESP-NOW request and listens for
 *      a response (~150ms dwell per channel).
 *   3. Once a beacon is received, the client locks to that channel
 *      and receives beacons passively.
 *   4. If beacons stop arriving for 30 seconds, the client re-scans.
 *   5. If no server is found, retries with exponential backoff
 *      (1s, 2s, 4s, ... up to 60s).
 *
 *   First beacon typically arrives within 2-6 seconds of init().
 *   No pre-configuration of the server's channel is needed.
 *
 * ── Constraint: STA Must Be Unassociated ────────────────────────
 *
 *   Channel scanning requires that the WiFi STA is NOT connected to
 *   an AP. When connected, the AP dictates the channel and
 *   esp_wifi_set_channel() will fail. Initialize WiFi in STA mode
 *   and start it, but do NOT call esp_wifi_connect().
 *
 *   If your device also needs WiFi connectivity, connect to the same
 *   network as the RBIO_RTC server (same channel) and channel
 *   scanning is unnecessary — beacons arrive on the shared channel.
 *
 * ── Beacon Protocol ──────────────────────────────────────────────
 *
 *   v1 (13 bytes, unsigned):
 *     [0]      0xBE magic
 *     [1..4]   epoch seconds (big-endian)
 *     [5..6]   milliseconds (big-endian)
 *     [7]      time source
 *     [8..11]  uptime seconds (big-endian)
 *     [12]     XOR checksum
 *
 *   v2 (37 bytes, signed):
 *     [0]       0x02 version
 *     [1..4]    epoch seconds (big-endian)
 *     [5..6]    milliseconds (big-endian)
 *     [7]       time source
 *     [8..11]   uptime seconds (big-endian)
 *     [12..15]  sequence number (big-endian, monotonic)
 *     [16..35]  HMAC-SHA256 truncated to 20 bytes
 *     [36]      reserved
 *
 * ── Security ─────────────────────────────────────────────────────
 *
 *   When a PSK is provided:
 *     - Only v2 beacons with valid HMAC are accepted
 *     - Sequence numbers must be monotonically increasing (replay protection)
 *     - v1 beacons are silently ignored
 *
 *   When no PSK is provided:
 *     - v1 beacons are accepted (XOR checksum verified)
 *     - v2 beacons are accepted but HMAC is not verified
 *     - No replay protection
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define RBIO_PSK_LEN  32

typedef struct {
    uint32_t epoch;        /* Unix epoch seconds */
    uint16_t ms;           /* Milliseconds within second */
    uint8_t  source;       /* 0=none, 1=DS3231, 2=NTP, 3=ESPNOW */
    uint32_t uptime;       /* Server uptime in seconds */
    uint32_t seq;          /* Sequence number (0 for v1) */
    uint8_t  stratum;      /* Mesh hop count (0=root, 1+=repeater) */
    bool     verified;     /* true if HMAC verified (v2 + PSK) */
    uint8_t  version;      /* 1 or 2 */
} rbio_time_t;

/**
 * Callback invoked when a valid time beacon is received.
 * Called from the ESP-NOW receive context — keep it short.
 */
typedef void (*rbio_time_cb_t)(const rbio_time_t *time);

/**
 * Initialize the RBIO_RTC client and start channel scanning.
 *
 * Returns immediately — channel scanning runs in a background task.
 * The callback will fire once a beacon is found (typically 2-6 seconds).
 *
 * @param psk   32-byte pre-shared key, or NULL for unsigned mode.
 *              If provided, only HMAC-verified v2 beacons are accepted.
 * @param cb    Callback for received time beacons.
 *
 * Prerequisites:
 *   - WiFi must be initialized and started (any mode).
 *   - WiFi STA must NOT be connected to an AP (channel scanning
 *     requires control of the radio channel).
 *   - ESP-NOW must NOT be initialized yet (this function does it).
 */
esp_err_t rbio_rtc_client_init(const uint8_t *psk, rbio_time_cb_t cb);

/**
 * Send a unicast time request to the server.
 *
 * Not usually needed — the background scanner sends requests
 * automatically during channel discovery. Use this for on-demand
 * time refresh after the scanner has locked to a channel.
 *
 * @param server_mac  6-byte MAC of the RBIO_RTC server.
 *                    Pass NULL to use broadcast (any server will respond).
 */
esp_err_t rbio_rtc_client_request(const uint8_t *server_mac);

/**
 * Shut down the client cleanly: stop the scan task, deinit ESP-NOW,
 * free resources. After this call, the radio is available for other
 * uses (WiFi connect, BLE, deep sleep, etc.).
 *
 * Safe to call rbio_rtc_client_init() again later to re-sync.
 *
 * Returns ESP_ERR_INVALID_STATE if the client was not initialized.
 */
esp_err_t rbio_rtc_client_deinit(void);
