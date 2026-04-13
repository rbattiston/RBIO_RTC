#pragma once

#include "esp_err.h"

/*
 * HTTP Config Server — runs on the AP interface (192.168.4.1:80).
 *
 * Provides:
 *   GET  /        → status page + WiFi config form
 *   POST /wifi    → save STA credentials, trigger connection
 *   GET  /status  → JSON status (for programmatic access)
 */

/**
 * Start the HTTP server. Call after wifi_manager_init().
 */
esp_err_t http_server_start(void);
