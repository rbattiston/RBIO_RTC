#pragma once

#include "esp_err.h"
#include <stdint.h>

/*
 * Status LED — optional WS2812 RGB LED for visual system status.
 *
 * Disabled by default. Configure the GPIO pin via web UI or
 * status_led_set_gpio(). Setting persists in NVS across reboots.
 *
 * When configured:
 *   RED solid         → No time set (fresh device, never synced)
 *   RED rapid flash   → Battery failure (DS3231 OSF flag set)
 *   AMBER pulse       → Time from DS3231 only, waiting for upstream
 *   GREEN heartbeat   → Synced (NTP or ESP-NOW upstream), healthy
 *   BLUE blinks (Nx)  → Device MAC identifier (every ~12 seconds)
 *   BLUE flash (3x)   → WiFi STA just connected
 *
 * When not configured (GPIO = 0xFF): no task, no GPIO touched.
 */

#define LED_GPIO_DISABLED  0xFF

/**
 * Initialize the status LED. Reads GPIO from NVS.
 * If no GPIO is configured, logs a message and returns ESP_OK
 * without creating any tasks or touching any pins.
 */
esp_err_t status_led_init(void);

/**
 * Set the WS2812 LED GPIO pin. Saved to NVS, takes effect on reboot.
 * Pass LED_GPIO_DISABLED to disable the LED entirely.
 */
esp_err_t status_led_set_gpio(uint8_t gpio);

/**
 * Get the currently configured GPIO pin.
 * Returns LED_GPIO_DISABLED if no LED is configured.
 */
uint8_t status_led_get_gpio(void);
