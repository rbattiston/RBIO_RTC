/*
 * RBIO_RTC Client — Sync-Once Example
 *
 * Demonstrates the RECOMMENDED usage pattern: sync time once on boot,
 * then tear down the client to free the radio. Schedule periodic
 * re-sync every 6-24 hours based on your drift tolerance.
 *
 * The beacon system is for periodic check-ins, NOT continuous polling.
 * Leaving the radio active to listen for beacons wastes power and
 * prevents using WiFi/BLE for your actual application.
 *
 * Wiring: none (ESP-NOW is wireless).
 * WiFi:   STA mode, NOT connected to any AP (scanning needs the radio).
 *
 * ── Flow ─────────────────────────────────────────────────────────
 *
 *   1. Init WiFi (STA, unassociated)
 *   2. Init RBIO_RTC client — background scanner starts
 *   3. Wait for callback (usually <1 second)
 *   4. Set system clock from received time
 *   5. Deinit client — radio is now free
 *   6. Do your application work
 *   7. Periodically (every 6-24h), goto step 2
 */

#include "rbio_rtc_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "example";

/* How long to wait for the first beacon before giving up.
 * 10 seconds is generous — typical discovery is under 1 second. */
#define SYNC_TIMEOUT_MS  10000

/* Re-sync interval.  Trade off between drift tolerance and radio usage.
 * ESP32 crystal drifts ~1.7 sec/day, so 24h = ~1.7s accuracy. */
#define RESYNC_INTERVAL_MS  (6 * 60 * 60 * 1000UL)  /* 6 hours */

/*
 * Optional: 32-byte PSK (from server web UI). NULL = accept unsigned beacons.
 */
static const uint8_t *MY_PSK = NULL;

static SemaphoreHandle_t s_sync_done;

static void on_time_received(const rbio_time_t *t)
{
    /* Set system clock */
    struct timeval tv = {
        .tv_sec  = t->epoch,
        .tv_usec = t->ms * 1000,
    };
    settimeofday(&tv, NULL);

    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    ESP_LOGI(TAG, "Synced: %04d-%02d-%02d %02d:%02d:%02d UTC (v%d verified=%d)",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             t->version, t->verified);

    /* Signal the main task that we got a sync */
    xSemaphoreGive(s_sync_done);
}

static void init_wifi_sta_minimal(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

/* Perform one sync cycle: init client, wait for time, tear down. */
static bool sync_time_from_rbio(void)
{
    ESP_LOGI(TAG, "Starting time sync...");

    esp_err_t err = rbio_rtc_client_init(MY_PSK, on_time_received);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Client init failed: %s", esp_err_to_name(err));
        return false;
    }

    bool got_time = (xSemaphoreTake(s_sync_done, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)) == pdTRUE);

    rbio_rtc_client_deinit();

    if (!got_time) {
        ESP_LOGW(TAG, "Sync timed out after %dms", SYNC_TIMEOUT_MS);
    }
    return got_time;
}

void app_main(void)
{
    ESP_LOGI(TAG, "RBIO_RTC Client Example (sync-once pattern)");

    s_sync_done = xSemaphoreCreateBinary();

    /* Init WiFi once — stays up across sync cycles */
    init_wifi_sta_minimal();

    /* Initial sync on boot */
    sync_time_from_rbio();

    /* ── At this point, the radio is free. ──
     * Do your actual application work here. Connect to WiFi,
     * transmit data, enter deep sleep, whatever your device needs. */

    /* Periodic re-sync loop (optional — illustrates the pattern) */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RESYNC_INTERVAL_MS));
        sync_time_from_rbio();
    }
}
