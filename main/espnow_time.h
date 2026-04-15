#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * ESP-NOW Time Distribution — Signed Beacon Protocol
 *
 * Two beacon versions are broadcast simultaneously:
 *
 * ── v1 (unsigned, 14 bytes) ──────────────────────────────────────
 *   For clients that don't need authentication.
 *   [0]      magic: 0xBE
 *   [1..4]   epoch seconds (big-endian uint32)
 *   [5..6]   milliseconds (big-endian uint16)
 *   [7]      time source (0=NONE, 1=DS3231, 2=NTP, 3=ESPNOW)
 *   [8..11]  uptime seconds (big-endian uint32)
 *   [12]     stratum (0=root/NTP, 1+=hops from root, 7=unsynced)
 *   [13]     XOR checksum of bytes 0..12
 *
 * ── v2 (signed, 37 bytes) ────────────────────────────────────────
 *   HMAC-SHA256 authenticated. Clients with the PSK can verify
 *   authenticity. Includes monotonic sequence number for replay
 *   protection.
 *
 *   [0]       version: 0x02
 *   [1..4]    epoch seconds (big-endian uint32)
 *   [5..6]    milliseconds (big-endian uint16)
 *   [7]       time source (0=NONE, 1=DS3231, 2=NTP, 3=ESPNOW)
 *   [8..11]   uptime seconds (big-endian uint32)
 *   [12..15]  sequence number (big-endian uint32, monotonic)
 *   [16..35]  HMAC-SHA256 truncated to 20 bytes (over bytes 0..15)
 *   [36]      stratum (0-7)
 *
 *   HMAC key: 32-byte PSK stored in NVS. If no PSK is configured,
 *   v2 beacons are not sent (only v1).
 *
 * ── Request/Reply ────────────────────────────────────────────────
 *   Client sends 0x01 → gets v1 reply
 *   Client sends 0x02 → gets v2 signed reply (if PSK configured)
 *
 * ── Stratum semantics (mesh support) ─────────────────────────────
 *   0 = root node with direct NTP or manual authority
 *   1 = repeater one hop from root
 *   2..6 = further hops
 *   7 = unsynchronized (node does not yet have valid time — will not broadcast)
 *
 *   Nodes only accept beacons with STRICTLY LOWER stratum than their own.
 *   Maximum useful stratum in a broadcast is 6 (child would be 7 = unsynced).
 */

#define ESPNOW_TIME_MAGIC       0xBE
#define ESPNOW_V2_VERSION       0x02
#define ESPNOW_TIME_REQ_V1      0x01
#define ESPNOW_TIME_REQ_V2      0x02

#define ESPNOW_V1_LEN           14
#define ESPNOW_V2_LEN           37
#define ESPNOW_HMAC_LEN         20   /* truncated SHA256 */
#define ESPNOW_PSK_LEN          32   /* 256-bit pre-shared key */

#define ESPNOW_STRATUM_ROOT     0
#define ESPNOW_STRATUM_UNSYNCED 7
#define ESPNOW_STRATUM_MAX      6    /* max stratum we will broadcast */

/* Broadcast interval in seconds */
#define ESPNOW_BROADCAST_INTERVAL_SEC  5

/**
 * Initialize ESP-NOW and register callbacks. Call after WiFi is started.
 */
esp_err_t espnow_time_init(void);

/**
 * Start the periodic broadcast task. The task only actually broadcasts
 * when time_manager reports a valid sync (stratum <= ESPNOW_STRATUM_MAX).
 */
esp_err_t espnow_time_start_broadcast(void);

/**
 * Set the pre-shared key for v2 signed beacons.
 * key must be exactly ESPNOW_PSK_LEN bytes. Saved to NVS.
 * Pass NULL to disable v2 signing.
 */
esp_err_t espnow_time_set_psk(const uint8_t *key);

/**
 * Check if a PSK is configured.
 */
bool espnow_time_has_psk(void);

/**
 * Get the current PSK (for display — first 4 bytes only, rest masked).
 * Returns false if no PSK set.
 */
bool espnow_time_get_psk_fingerprint(char *buf, size_t buf_len);
