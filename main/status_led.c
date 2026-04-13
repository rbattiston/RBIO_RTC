#include "status_led.h"
#include "time_manager.h"
#include "wifi_manager.h"
#include "ds3231.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "status_led";

/* WS2812 on Freenove devkit */
#define LED_GPIO        16
#define LED_RMT_RES_HZ  10000000  /* 10MHz = 100ns per tick */

/* WS2812 timing (in 100ns ticks) */
#define T0H  3   /* 0.3us */
#define T0L  9   /* 0.9us */
#define T1H  9   /* 0.9us */
#define T1L  3   /* 0.3us */

static rmt_channel_handle_t s_rmt_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

/* ── WS2812 driver ──────────────────────────────────────────────── */

static esp_err_t ws2812_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = LED_GPIO,
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

/* Set LED color — GRB byte order (WS2812 native) */
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
static void led_blue(void)    { ws2812_set(0, 0, 40); }

/* ── Status state machine ───────────────────────────────────────── */

typedef enum {
    STATUS_NO_TIME,      /* red solid */
    STATUS_DS3231_ONLY,  /* amber pulse */
    STATUS_NTP_HEALTHY,  /* green heartbeat */
    STATUS_BATTERY_BAD,  /* red rapid flash */
} status_state_t;

static status_state_t get_status(void)
{
    /* Check battery first — highest priority alarm */
    bool osf = false;
    if (ds3231_get_osf(&osf) == ESP_OK && osf) {
        return STATUS_BATTERY_BAD;
    }

    time_source_t src = time_manager_get_source();
    if (src == TIME_SRC_NTP) return STATUS_NTP_HEALTHY;
    if (src == TIME_SRC_DS3231) return STATUS_DS3231_ONLY;
    return STATUS_NO_TIME;
}

static void status_led_task(void *arg)
{
    int tick = 0;
    bool sta_was_connected = false;

    for (;;) {
        status_state_t state = get_status();
        bool sta_now = wifi_manager_sta_connected();

        /* Brief blue flash on STA connect event */
        if (sta_now && !sta_was_connected) {
            for (int i = 0; i < 6; i++) {
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
            /* Red solid */
            led_red();
            break;

        case STATUS_DS3231_ONLY:
            /* Amber slow pulse (on 1.5s, off 0.5s) */
            if ((tick % 4) < 3) {
                led_amber();
            } else {
                led_off();
            }
            break;

        case STATUS_NTP_HEALTHY:
            /* Green heartbeat: brief blink every 3s */
            if ((tick % 6) == 0) {
                led_green();
            } else {
                led_off();
            }
            break;

        case STATUS_BATTERY_BAD:
            /* Red rapid flash */
            if (tick % 2) {
                led_red();
            } else {
                led_off();
            }
            break;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t status_led_init(void)
{
    esp_err_t err = ws2812_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Start with red to indicate "no time" */
    led_red();

    BaseType_t ret = xTaskCreate(status_led_task, "status_led", 2048, NULL, 1, NULL);
    if (ret != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "Status LED on GPIO %d: RED=no time, AMBER=DS3231, GREEN=NTP synced", LED_GPIO);
    return ESP_OK;
}
