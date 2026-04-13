/*
 * RBIO_RTC — Facility Time Server
 *
 * ESP32 WROOM + DS3231 RTC with battery backup.
 * Fully automatic time management — no manual set.
 *
 * Time is set ONLY by NTP. Users configure WiFi via web UI
 * at http://192.168.4.1 (connect to "RBIO_RTC" AP first).
 * Once NTP syncs, DS3231 is written and persists across power cycles.
 *
 * Time distribution:
 *   - SNTP server on UDP:123 (WiFi AP clients)
 *   - ESP-NOW broadcast beacons + unicast replies
 *
 * Boot sequence:
 *   1. Init DS3231, load time into system clock (if previously set by NTP)
 *   2. Start WiFi AP+STA (AP always on, STA auto-connects if creds saved)
 *   3. Start SNTP server, ESP-NOW beacons, HTTP config UI
 *   4. When STA connects → NTP sync starts automatically
 *   5. NTP success → DS3231 written (only write path)
 */

#include "ds3231.h"
#include "time_manager.h"
#include "wifi_manager.h"
#include "sntp_server.h"
#include "espnow_time.h"
#include "http_server.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "rbio_rtc";

static const char *source_str(time_source_t src)
{
    switch (src) {
    case TIME_SRC_NONE:   return "NONE";
    case TIME_SRC_DS3231: return "DS3231";
    case TIME_SRC_NTP:    return "NTP";
    default:              return "?";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " RBIO_RTC — Facility Time Server");
    ESP_LOGI(TAG, "========================================");

    /* 1. DS3231 → system clock */
    ESP_LOGI(TAG, "[1/5] Initializing DS3231...");
    esp_err_t err = ds3231_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 init failed — will rely on NTP when available");
    }

    ESP_LOGI(TAG, "[2/5] Loading time from RTC...");
    time_manager_init();

    /* 2. WiFi AP+STA */
    ESP_LOGI(TAG, "[3/5] Starting WiFi (AP + STA)...");
    err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed!");
    }

    /* 3. Services */
    ESP_LOGI(TAG, "[4/5] Starting time services...");
    sntp_server_start();
    http_server_start();

    if (espnow_time_init() == ESP_OK) {
        espnow_time_start_broadcast();
    }

    /* 4. Background resync */
    time_manager_start_sync_task();

    /* 5. Print status */
    ESP_LOGI(TAG, "[5/5] Startup complete.");
    ESP_LOGI(TAG, "");

    time_t now = time_manager_now();
    struct tm t;
    gmtime_r(&now, &t);

    if (time_manager_get_source() != TIME_SRC_NONE) {
        ESP_LOGI(TAG, "  Time:     %04d-%02d-%02d %02d:%02d:%02d UTC (source: %s)",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec,
                 source_str(time_manager_get_source()));
    } else {
        ESP_LOGW(TAG, "  Time:     NOT SET — waiting for NTP");
    }

    ESP_LOGI(TAG, "  RTC set:  %s", time_manager_rtc_is_set() ? "Yes (by NTP)" : "No");
    ESP_LOGI(TAG, "  AP:       RBIO_RTC (pass: rbio_time)");
    ESP_LOGI(TAG, "  Config:   http://%s", wifi_manager_get_ap_ip());
    ESP_LOGI(TAG, "  SNTP:     %s:123", wifi_manager_get_ap_ip());
    ESP_LOGI(TAG, "  ESP-NOW:  broadcast every %ds", ESPNOW_BROADCAST_INTERVAL_SEC);

    if (wifi_manager_has_sta_creds()) {
        ESP_LOGI(TAG, "  STA:      '%s' (%s)",
                 wifi_manager_get_sta_ssid(),
                 wifi_manager_sta_connected() ? wifi_manager_get_sta_ip() : "connecting...");
    } else {
        ESP_LOGW(TAG, "  STA:      No credentials — configure at http://%s",
                 wifi_manager_get_ap_ip());
    }

    ESP_LOGI(TAG, "");
}
