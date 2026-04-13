#pragma once

#include "esp_err.h"
#include <stdbool.h>

/*
 * WiFi Manager — AP+STA concurrent mode.
 *
 * AP side:  Always on. Serves SNTP, ESP-NOW, and the config web UI.
 * STA side: Connects to user's router for upstream NTP access.
 *           Credentials stored in NVS. Auto-reconnects.
 *
 * When STA connects → time_manager_start_ntp() is called automatically.
 * When STA disconnects → time_manager_stop_ntp() is called.
 */

/**
 * Initialize WiFi in AP+STA mode. Starts AP immediately.
 * If STA credentials exist in NVS, begins connection attempts.
 */
esp_err_t wifi_manager_init(void);

/**
 * Save STA credentials to NVS and begin connecting.
 * ssid and password are null-terminated strings.
 */
esp_err_t wifi_manager_set_sta_creds(const char *ssid, const char *password);

/**
 * Check if STA credentials are configured.
 */
bool wifi_manager_has_sta_creds(void);

/**
 * Check if STA is currently connected to the router.
 */
bool wifi_manager_sta_connected(void);

/**
 * Get the AP's IP address string.
 */
const char *wifi_manager_get_ap_ip(void);

/**
 * Get the STA's IP address string (empty if not connected).
 */
const char *wifi_manager_get_sta_ip(void);

/**
 * Get the configured STA SSID (empty if none).
 */
const char *wifi_manager_get_sta_ssid(void);
