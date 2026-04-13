#pragma once

/*
 * RBIO_RTC Client — Reference Implementation
 *
 * Drop this file into any ESP32 project to receive time from an
 * RBIO_RTC server via ESP-NOW beacons.
 *
 * Supports both v1 (unsigned) and v2 (HMAC-SHA256 signed) beacons.
 *
 * ── Quick Start ──────────────────────────────────────────────────
 *
 *   #include "rbio_rtc_client.h"
 *
 *   void app_main(void) {
 *       // ... init WiFi in any mode (AP, STA, or APSTA) ...
 *
 *       // Without authentication (accepts any v1 beacon):
 *       rbio_rtc_client_init(NULL, my_time_callback);
 *
 *       // With authentication (only accepts verified v2 beacons):
 *       uint8_t psk[32] = { ... };  // same PSK as the server
 *       rbio_rtc_client_init(psk, my_time_callback);
 *   }
 *
 *   void my_time_callback(const rbio_time_t *time) {
 *       printf("Time: %lu, verified: %d\n", time->epoch, time->verified);
 *       // Set your system clock, update your RTC, etc.
 *   }
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
    uint8_t  source;       /* 0=none, 1=DS3231, 2=NTP */
    uint32_t uptime;       /* Server uptime in seconds */
    uint32_t seq;          /* Sequence number (0 for v1) */
    bool     verified;     /* true if HMAC verified (v2 + PSK) */
    uint8_t  version;      /* 1 or 2 */
} rbio_time_t;

/**
 * Callback invoked when a valid time beacon is received.
 * Called from the ESP-NOW receive context — keep it short.
 */
typedef void (*rbio_time_cb_t)(const rbio_time_t *time);

/**
 * Initialize the RBIO_RTC client.
 *
 * @param psk   32-byte pre-shared key, or NULL for unsigned mode.
 *              If provided, only HMAC-verified v2 beacons are accepted.
 * @param cb    Callback for received time beacons.
 *
 * Prerequisites: WiFi must be initialized and started (any mode).
 *                ESP-NOW must NOT be initialized yet (this function does it).
 */
esp_err_t rbio_rtc_client_init(const uint8_t *psk, rbio_time_cb_t cb);

/**
 * Send a unicast time request to the server.
 * Use this if you want time immediately rather than waiting for a broadcast.
 *
 * @param server_mac  6-byte MAC of the RBIO_RTC server.
 *                    Pass NULL to use broadcast (any server will respond).
 */
esp_err_t rbio_rtc_client_request(const uint8_t *server_mac);
