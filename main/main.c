/*
 * RBIO_RTC — Facility Time Server / Mesh Repeater
 *
 * ESP32 WROOM + DS3231 RTC with battery backup.
 *
 * Two runtime roles (configured via web UI, persisted in NVS):
 *
 *   ROOT:
 *     - Connects STA to facility router
 *     - Syncs from upstream NTP
 *     - Broadcasts ESP-NOW beacons at stratum 0
 *
 *   REPEATER:
 *     - STA unassociated (needs channel control for scanning)
 *     - Scans channels to find a parent (lower stratum)
 *     - Locks to best parent, broadcasts at stratum (parent+1)
 *
 * Both roles serve SNTP on UDP:123 and respond to ESP-NOW requests
 * from downstream clients. Clients see a unified time source.
 */

#include "ds3231.h"
#include "time_manager.h"
#include "wifi_manager.h"
#include "sntp_server.h"
#include "espnow_time.h"
#include "http_server.h"
#include "status_led.h"
#include "mesh_role.h"
#include "repeater.h"

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
    case TIME_SRC_ESPNOW: return "ESPNOW";
    default:              return "?";
    }
}

void app_main(void)
{
    esp_log_level_set("wifi", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " RBIO_RTC — Facility Time Server");
    ESP_LOGI(TAG, "========================================");

    /* Determine role from NVS */
    mesh_role_t role = mesh_role_get();
    ESP_LOGI(TAG, "Role: %s", mesh_role_str(role));

    /* 1. DS3231 → system clock */
    ESP_LOGI(TAG, "[1/5] Initializing DS3231...");
    esp_err_t err = ds3231_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3231 init failed — will rely on upstream when available");
    }

    status_led_init();

    ESP_LOGI(TAG, "[2/5] Loading time from RTC...");
    time_manager_init();

    /* Initial stratum based on role:
     * - Root with valid RTC time is effectively stratum 0 authority
     *   (NTP will re-confirm this and set the same value).
     * - Repeater starts unsynced regardless of RTC state; must find
     *   upstream before broadcasting. */
    if (role == MESH_ROLE_ROOT && time_manager_get_source() == TIME_SRC_DS3231) {
        time_manager_set_stratum(ESPNOW_STRATUM_ROOT);
    }

    /* 2. WiFi AP+STA */
    ESP_LOGI(TAG, "[3/5] Starting WiFi (AP + STA)...");
    bool repeater_mode = (role == MESH_ROLE_REPEATER);
    err = wifi_manager_init(repeater_mode);
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

    /* Role-specific: repeater starts scanner, root does nothing extra */
    if (role == MESH_ROLE_REPEATER) {
        ESP_LOGI(TAG, "Starting repeater scanner...");
        repeater_start();
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
        ESP_LOGI(TAG, "  Time:     %04d-%02d-%02d %02d:%02d:%02d UTC (source: %s, stratum: %u)",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec,
                 source_str(time_manager_get_source()),
                 time_manager_get_stratum());
    } else {
        ESP_LOGW(TAG, "  Time:     NOT SET — waiting for sync");
    }

    ESP_LOGI(TAG, "  Role:     %s", mesh_role_str(role));
    ESP_LOGI(TAG, "  RTC set:  %s", time_manager_rtc_is_set() ? "Yes" : "No");
    ESP_LOGI(TAG, "  AP:       %s (pass: rbio_time)", wifi_manager_get_ap_ssid());
    ESP_LOGI(TAG, "  Config:   http://%s", wifi_manager_get_ap_ip());
    ESP_LOGI(TAG, "  SNTP:     %s:123", wifi_manager_get_ap_ip());
    ESP_LOGI(TAG, "  ESP-NOW:  broadcast every %ds", ESPNOW_BROADCAST_INTERVAL_SEC);

    if (role == MESH_ROLE_ROOT) {
        if (wifi_manager_has_sta_creds()) {
            ESP_LOGI(TAG, "  STA:      '%s' (%s)",
                     wifi_manager_get_sta_ssid(),
                     wifi_manager_sta_connected() ? wifi_manager_get_sta_ip() : "connecting...");
        } else {
            ESP_LOGW(TAG, "  STA:      No credentials — configure at http://%s",
                     wifi_manager_get_ap_ip());
        }
    } else {
        ESP_LOGI(TAG, "  STA:      unassociated (repeater mode, scanning for parent)");
    }

    ESP_LOGI(TAG, "");
}
