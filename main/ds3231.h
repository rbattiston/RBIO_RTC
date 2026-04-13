#pragma once

#include "esp_err.h"
#include <time.h>

/*
 * DS3231 RTC driver — minimal, robust.
 *
 * Talks I2C to DS3231 at 0x68. Reads/writes calendar time as struct tm.
 * Also reads battery health (OSF flag) and on-die temperature.
 */

#include <stdbool.h>

#define DS3231_I2C_ADDR     0x68

/* Which I2C port and pins to use — override in sdkconfig or here */
#ifndef DS3231_I2C_PORT
#define DS3231_I2C_PORT     0
#endif
#ifndef DS3231_SDA_PIN
#define DS3231_SDA_PIN      21
#endif
#ifndef DS3231_SCL_PIN
#define DS3231_SCL_PIN      22
#endif

/**
 * Initialize I2C bus and verify DS3231 is responding.
 * Safe to call multiple times — will skip if already initialized.
 */
esp_err_t ds3231_init(void);

/**
 * Read current time from DS3231 into a struct tm.
 * Returns ESP_OK on success, ESP_FAIL / ESP_ERR_TIMEOUT on bus error.
 */
esp_err_t ds3231_get_time(struct tm *time_out);

/**
 * Write time to DS3231 from a struct tm.
 * Returns ESP_OK on success.
 */
esp_err_t ds3231_set_time(const struct tm *time_in);

/**
 * Read the Oscillator Stop Flag (OSF) from the status register.
 * OSF=true means the oscillator stopped at some point — battery likely
 * dead or was removed. Time data may be invalid.
 *
 * After a successful NTP sync + time write, call ds3231_clear_osf() to
 * reset the flag so future checks are meaningful.
 */
esp_err_t ds3231_get_osf(bool *osf_out);

/**
 * Clear the OSF flag. Call after a known-good time has been written.
 */
esp_err_t ds3231_clear_osf(void);

/**
 * Read the DS3231 on-die temperature in degrees Celsius.
 * Resolution: 0.25C. Useful for monitoring the RTC's environment.
 */
esp_err_t ds3231_get_temperature(float *temp_out);
