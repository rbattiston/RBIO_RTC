/*
 * RBIO_RTC Client — Minimal Example
 *
 * Receives time from an RBIO_RTC server via ESP-NOW and sets the
 * system clock. Works on any ESP32.
 *
 * Wiring: none required (ESP-NOW is wireless).
 * WiFi:   must be initialized in any mode before calling client_init.
 *
 * ── Usage ────────────────────────────────────────────────────────
 *
 *   Unsigned mode (trusts any RBIO_RTC server):
 *     rbio_rtc_client_init(NULL, on_time_received);
 *
 *   Signed mode (only trusts servers with matching PSK):
 *     uint8_t psk[32] = { 0xaa, 0xbb, ... };  // copy from server web UI
 *     rbio_rtc_client_init(psk, on_time_received);
 */

#include "rbio_rtc_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "example";

/*
 * Optional: set this to your 32-byte PSK (as shown in the server web UI).
 * Set to NULL to accept unsigned beacons from any server.
 */
static const uint8_t *MY_PSK = NULL;

/* Example with a real PSK (uncomment and fill in your key):
static const uint8_t MY_PSK_DATA[32] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
static const uint8_t *MY_PSK = MY_PSK_DATA;
*/

static void on_time_received(const rbio_time_t *t)
{
    ESP_LOGI(TAG, "Time received: epoch=%lu ms=%u src=%u seq=%lu verified=%d v%d",
             (unsigned long)t->epoch, t->ms, t->source,
             (unsigned long)t->seq, t->verified, t->version);

    /* Set system clock */
    struct timeval tv = {
        .tv_sec  = t->epoch,
        .tv_usec = t->ms * 1000,
    };
    settimeofday(&tv, NULL);

    /* Print human-readable time */
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    ESP_LOGI(TAG, "System clock set: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void init_wifi_sta_minimal(void)
{
    /* Minimal WiFi init — STA mode, not connected to any AP.
     * ESP-NOW works without being associated to an AP. */
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

void app_main(void)
{
    ESP_LOGI(TAG, "RBIO_RTC Client Example");

    /* 1. Init WiFi (required for ESP-NOW) */
    init_wifi_sta_minimal();

    /* 2. Init RBIO_RTC client */
    esp_err_t err = rbio_rtc_client_init(MY_PSK, on_time_received);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Client init failed: %s", esp_err_to_name(err));
        return;
    }

    /* 3. Optionally request time immediately instead of waiting
     *    for the next broadcast (pass NULL for broadcast request) */
    rbio_rtc_client_request(NULL);

    /* 4. That's it — the callback fires every time a beacon arrives.
     *    Your main loop can do whatever else it needs to. */
    ESP_LOGI(TAG, "Listening for RBIO_RTC beacons...");
}
