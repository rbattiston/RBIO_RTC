#pragma once

#include "esp_err.h"
#include <time.h>
#include <stdbool.h>

/*
 * Time Manager — single authority for facility time.
 *
 * The DS3231 can ONLY be written by a successful NTP sync.
 * No manual time set. No erase. Once NTP writes it, it stays
 * until NTP corrects it again.
 *
 * Boot sequence:
 *   1. Read DS3231 → set system clock (if DS3231 has valid time)
 *   2. When STA WiFi connects, auto-sync from upstream NTP
 *   3. On NTP success → update DS3231
 *   4. Periodic resync keeps system clock aligned with DS3231
 */

typedef enum {
    TIME_SRC_NONE,       /* no time set yet */
    TIME_SRC_DS3231,     /* read from RTC on boot */
    TIME_SRC_NTP,        /* synced from upstream NTP */
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
 * Has the DS3231 ever been set by NTP? (Persisted in NVS.)
 */
bool time_manager_rtc_is_set(void);

/**
 * Start automatic NTP sync. Tries standard servers in order.
 * Non-blocking — runs in background. On success, writes DS3231.
 * Call once STA WiFi is connected.
 */
esp_err_t time_manager_start_ntp(void);

/**
 * Stop NTP sync (e.g. if STA disconnects).
 */
void time_manager_stop_ntp(void);
