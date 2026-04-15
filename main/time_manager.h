#pragma once

#include "esp_err.h"
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Time Manager — single authority for facility time.
 *
 * The DS3231 is written only by authoritative time sources:
 *   - NTP sync (root nodes with internet)
 *   - ESP-NOW beacon from an upstream with lower stratum (repeater nodes)
 *
 * Boot sequence:
 *   1. Read DS3231 → set system clock (if DS3231 has valid time)
 *   2a. Root: STA connects → NTP sync → write DS3231
 *   2b. Repeater: scanner finds upstream → ESP-NOW sync → write DS3231
 *   3. Periodic resync keeps system clock aligned with DS3231
 *
 * Mesh stratum:
 *   - Root: stratum 0
 *   - Repeater synced from stratum N: stratum N+1
 *   - Unsynced: stratum 7 (will not broadcast)
 */

typedef enum {
    TIME_SRC_NONE,       /* no time set yet */
    TIME_SRC_DS3231,     /* read from RTC on boot */
    TIME_SRC_NTP,        /* synced from upstream NTP (root) */
    TIME_SRC_ESPNOW,     /* synced from upstream ESP-NOW beacon (repeater) */
} time_source_t;

/**
 * Initialize: read DS3231, set system clock. Must call after ds3231_init().
 */
esp_err_t time_manager_init(void);

/**
 * Start the periodic resync task (DS3231 ↔ system clock drift correction).
 */
esp_err_t time_manager_start_sync_task(void);

/**
 * Get current facility time as epoch seconds.
 */
time_t time_manager_now(void);

/**
 * Get what source last set our time.
 */
time_source_t time_manager_get_source(void);

/**
 * Get our current stratum in the mesh.
 * 0 = root, 1+ = repeater with that many hops to root, 7 = unsynced.
 */
uint8_t time_manager_get_stratum(void);

/**
 * Explicitly set our stratum. Used by role config (root = 0) and
 * on parent timeout (reset to unsynced = 7).
 */
void time_manager_set_stratum(uint8_t stratum);

/**
 * Has the DS3231 ever been set by a trusted source? (Persisted in NVS.)
 */
bool time_manager_rtc_is_set(void);

/**
 * Start automatic NTP sync. Tries standard servers in order.
 * Non-blocking — runs in background. On success, writes DS3231.
 * Call once STA WiFi is connected. Root nodes only.
 */
esp_err_t time_manager_start_ntp(void);

/**
 * Stop NTP sync (e.g. if STA disconnects).
 */
void time_manager_stop_ntp(void);

/**
 * Sync from an ESP-NOW beacon (repeater path).
 *
 * @param epoch             Unix epoch seconds from the beacon
 * @param ms                Milliseconds fraction (0-999)
 * @param upstream_stratum  Stratum of the upstream node we synced from
 *
 * Applies the same drift-threshold logic as NTP sync. Sets our stratum
 * to upstream_stratum + 1. Returns ESP_ERR_INVALID_ARG if upstream
 * stratum is too high (would make us stratum >= 7).
 */
esp_err_t time_manager_espnow_sync(time_t epoch, uint16_t ms, uint8_t upstream_stratum);
