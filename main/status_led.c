#include "status_led.h"
#include "time_manager.h"
#include "wifi_manager.h"
#include "ds3231.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "status_led";

#define NVS_NAMESPACE   "rbio_led"
#define NVS_KEY_GPIO    "gpio"

/* WS2812 timing at 10MHz (100ns per tick) */
#define LED_RMT_RES_HZ  10000000
#define T0H  3
#define T0L  9
#define T1H  9
#define T1L  3

static rmt_channel_handle_t s_rmt_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static uint8_t s_led_gpio = LED_GPIO_DISABLED;
static uint8_t s_mac_blink_count = 0;

/* ── NVS-backed GPIO configuration ─────────────────────────────── */

static uint8_t load_gpio_from_nvs(void)
{
    nvs_handle_t h;
    uint8_t val = LED_GPIO_DISABLED;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_GPIO, &val);
        nvs_close(h);
    }
    return val;
}

esp_err_t status_led_set_gpio(uint8_t gpio)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8(h, NVS_KEY_GPIO, gpio);
    nvs_commit(h);
    nvs_close(h);

    if (gpio == LED_GPIO_DISABLED) {
        ESP_LOGI(TAG, "LED disabled (takes effect on reboot)");
    } else {
        ESP_LOGI(TAG, "LED GPIO set to %u (takes effect on reboot)", gpio);
    }
    return ESP_OK;
}

uint8_t status_led_get_gpio(void)
{
    return s_led_gpio;
}

/* ── WS2812 driver ──────────────────────────────────────────────── */

static esp_err_t ws2812_init(uint8_t gpio)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_rmt_channel);
    if (err != ESP_OK) return err;

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L },
        .flags.msb_first = true,
    };
    err = rmt_new_bytes_encoder(&enc_cfg, &s_encoder);
    if (err != ESP_OK) return err;

    return rmt_enable(s_rmt_channel);
}

static void ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    rmt_transmit(s_rmt_channel, s_encoder, grb, sizeof(grb), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_channel, pdMS_TO_TICKS(100));
}

static void led_off(void)     { ws2812_set(0, 0, 0); }
static void led_red(void)     { ws2812_set(40, 0, 0); }
static void led_green(void)   { ws2812_set(0, 40, 0); }
static void led_amber(void)   { ws2812_set(40, 20, 0); }
static void led_blue(void)    { ws2812_set(0, 0, 60); }

/* ── MAC-based device identifier ────────────────────────────────── */

static uint8_t compute_mac_blink_count(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    char hex[13];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    for (int i = 11; i >= 0; i--) {
        if (hex[i] >= '0' && hex[i] <= '9') {
            return (uint8_t)(hex[i] - '0');
        }
    }
    return 0;
}

/* ── Status state machine ───────────────────────────────────────── */

typedef enum {
    STATUS_NO_TIME,
    STATUS_DS3231_ONLY,
    STATUS_NTP_HEALTHY,
    STATUS_BATTERY_BAD,
} status_state_t;

static status_state_t get_status(void)
{
    bool osf = false;
    if (ds3231_get_osf(&osf) == ESP_OK && osf) {
        return STATUS_BATTERY_BAD;
    }

    time_source_t src = time_manager_get_source();
    if (src == TIME_SRC_NTP || src == TIME_SRC_ESPNOW) return STATUS_NTP_HEALTHY;
    if (src == TIME_SRC_DS3231) return STATUS_DS3231_ONLY;
    return STATUS_NO_TIME;
}

static void do_mac_identifier_blinks(void)
{
    uint8_t n = s_mac_blink_count;
    if (n == 0) {
        led_blue();
        vTaskDelay(pdMS_TO_TICKS(500));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(500));
        return;
    }

    for (uint8_t i = 0; i < n; i++) {
        led_blue();
        vTaskDelay(pdMS_TO_TICKS(150));
        led_off();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void status_led_task(void *arg)
{
    int tick = 0;
    bool sta_was_connected = false;

    do_mac_identifier_blinks();

    for (;;) {
        status_state_t state = get_status();
        bool sta_now = wifi_manager_sta_connected();

        if (sta_now && !sta_was_connected) {
            for (int i = 0; i < 3; i++) {
                led_blue();
                vTaskDelay(pdMS_TO_TICKS(100));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            sta_was_connected = true;
            tick = 0;
            continue;
        }
        sta_was_connected = sta_now;

        switch (state) {
        case STATUS_NO_TIME:
            led_red();
            break;
        case STATUS_DS3231_ONLY:
            if ((tick % 4) < 3) led_amber();
            else led_off();
            break;
        case STATUS_NTP_HEALTHY:
            if ((tick % 6) == 0) led_green();
            else led_off();
            break;
        case STATUS_BATTERY_BAD:
            if (tick % 2) led_red();
            else led_off();
            break;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(500));

        if ((tick % 24) == 0) {
            do_mac_identifier_blinks();
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t status_led_init(void)
{
    s_led_gpio = load_gpio_from_nvs();

    /* No LED configured — skip everything, touch no pins */
    if (s_led_gpio == LED_GPIO_DISABLED) {
        ESP_LOGI(TAG, "No LED configured (set via web UI, takes effect on reboot)");
        return ESP_OK;
    }

#ifdef CONFIG_SPIRAM
    /* Guard against PSRAM conflict on WROVER modules:
     * GPIO 16 and 17 are used for PSRAM data lines. */
    if (s_led_gpio == 16 || s_led_gpio == 17) {
        ESP_LOGW(TAG, "GPIO %u conflicts with PSRAM — LED disabled", s_led_gpio);
        s_led_gpio = LED_GPIO_DISABLED;
        return ESP_OK;
    }
#endif

    esp_err_t err = ws2812_init(s_led_gpio);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init on GPIO %u failed: %s", s_led_gpio, esp_err_to_name(err));
        s_led_gpio = LED_GPIO_DISABLED;
        return ESP_OK;  /* non-fatal — device works without LED */
    }

    s_mac_blink_count = compute_mac_blink_count();
    ESP_LOGI(TAG, "LED on GPIO %u — ID blink count: %u", s_led_gpio, s_mac_blink_count);

    led_red();

    BaseType_t ret = xTaskCreate(status_led_task, "status_led", 2048, NULL, 1, NULL);
    if (ret != pdPASS) return ESP_FAIL;

    return ESP_OK;
}
