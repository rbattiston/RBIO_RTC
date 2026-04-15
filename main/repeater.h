#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Repeater Mode
 *
 * Runs only when mesh_role_get() == MESH_ROLE_REPEATER.
 *
 * Behavior:
 *   1. Scan WiFi channels looking for RBIO_RTC beacons (v1 or v2 signed)
 *   2. Lock to the parent with the lowest stratum (tiebreaker: strongest RSSI)
 *   3. On each valid parent beacon, call time_manager_espnow_sync()
 *   4. Hysteresis: don't switch parents unless new candidate is clearly better
 *   5. If parent beacons stop for 30s, mark self unsynced and re-scan
 *
 * The repeater reuses the server's own broadcast module (espnow_time.c)
 * to relay beacons downstream. The broadcast module checks our stratum
 * and only broadcasts when we're synced.
 */

/**
 * Initialize and start the repeater scanner/parent-tracker task.
 * Call after WiFi is started in AP+STA mode (STA unassociated).
 */
esp_err_t repeater_start(void);

/**
 * Called by espnow_time.c's recv callback when an incoming packet is
 * NOT a client request (i.e., it's a beacon from an upstream peer).
 * No-op if the repeater isn't running. Safe to call unconditionally.
 */
struct esp_now_recv_info;
void repeater_on_beacon(const struct esp_now_recv_info *info,
                        const uint8_t *data, int len);

/**
 * Status getters for the web UI / JSON status endpoint.
 */
bool repeater_has_parent(void);
uint8_t repeater_get_parent_channel(void);
const uint8_t *repeater_get_parent_mac(void);   /* 6 bytes, or NULL if no parent */
int8_t  repeater_get_parent_rssi(void);
