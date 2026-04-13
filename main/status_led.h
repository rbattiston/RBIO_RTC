#pragma once

#include "esp_err.h"

/*
 * Status LED — WS2812 RGB LED on GPIO 16 (Freenove devkit).
 *
 * Visual system status at a glance:
 *
 *   RED solid         → No time set (fresh device, never synced)
 *   RED rapid flash   → Battery failure (DS3231 OSF flag set)
 *   AMBER pulse       → Time from DS3231 only, waiting for NTP
 *   GREEN heartbeat   → NTP synced, healthy, serving time
 *   BLUE flash        → WiFi STA connected event
 *
 * The LED task reads system state every 500ms and updates accordingly.
 */

/**
 * Initialize the WS2812 on the configured GPIO and start the
 * background status task.
 */
esp_err_t status_led_init(void);
