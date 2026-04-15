#pragma once

#include "esp_err.h"

/*
 * Mesh Role Configuration
 *
 * Every RBIO_RTC device runs the same firmware. What it does depends
 * on its configured role, persisted in NVS and settable via web UI.
 *
 * ROOT:
 *   - Connects STA to facility router
 *   - Syncs from upstream NTP (pool.ntp.org, etc.)
 *   - Broadcasts ESP-NOW beacons at stratum 0
 *   - Serves SNTP on :123 (AP + LAN)
 *
 * REPEATER:
 *   - Does NOT connect to any router (STA stays unassociated)
 *   - Scans WiFi channels for upstream ESP-NOW beacons
 *   - Locks to a parent with lower stratum
 *   - Broadcasts ESP-NOW beacons at stratum (parent+1)
 *   - Serves SNTP on :123 (AP only)
 *
 * Default role: ROOT (matches single-device deployment).
 */

typedef enum {
    MESH_ROLE_ROOT     = 0,
    MESH_ROLE_REPEATER = 1,
} mesh_role_t;

/**
 * Load role from NVS. Returns MESH_ROLE_ROOT if not configured.
 */
mesh_role_t mesh_role_get(void);

/**
 * Persist role to NVS. Takes effect on next reboot.
 */
esp_err_t mesh_role_set(mesh_role_t role);

/**
 * String form for logging/UI.
 */
const char *mesh_role_str(mesh_role_t role);
