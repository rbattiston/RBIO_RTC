#pragma once

#include "esp_err.h"
#include <stdint.h>

/*
 * SNTP Server — listens on UDP port 123, responds to NTP queries.
 *
 * Rate limited: each client IP gets at most 1 response per 2 seconds,
 * with a burst allowance of 3 rapid queries for initial sync.
 * Excess queries are silently dropped.
 */

esp_err_t sntp_server_start(void);

/* Stats for monitoring */
uint32_t sntp_server_get_served(void);
uint32_t sntp_server_get_dropped(void);
