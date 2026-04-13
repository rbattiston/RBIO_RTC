#include "time_manager.h"
#include "ds3231.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>
#include <math.h>

static const char *TAG = "time_mgr";

static time_source_t s_source = TIME_SRC_NONE;
static bool s_ntp_running = false;

/* How often to re-read DS3231 and correct system clock drift (seconds).
 * Every 10 minutes is plenty for minute-level accuracy. */
#define RESYNC_INTERVAL_SEC  600

/* Only write DS3231 from NTP if drift exceeds this threshold.
 * DS3231 drifts ~2ppm (~0.17s/day), so sub-second drift is normal. */
#define NTP_RTC_DRIFT_THRESHOLD_SEC  2

/* NTP polling interval: 6 hours. DS3231 drift at 6h is ~0.04s — negligible. */
#define NTP_POLL_INTERVAL_MS  (6 * 60 * 60 * 1000)

/* NVS key to persist "RTC has been set by NTP at least once" */
#define NVS_NAMESPACE  "rbio_rtc"
#define NVS_KEY_RTC_SET "rtc_set"

/* Standard NTP servers, tried in order */
static const char *NTP_SERVERS[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
    "time.nist.gov",
};
#define NTP_SERVER_COUNT (sizeof(NTP_SERVERS) / sizeof(NTP_SERVERS[0]))

static void mark_rtc_set(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_RTC_SET, 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool time_manager_rtc_is_set(void)
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_RTC_SET, &val);
        nvs_close(h);
    }
    return val != 0;
}

esp_err_t time_manager_init(void)
{
    struct tm rtc_time;
    esp_err_t err = ds3231_get_time(&rtc_time);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read DS3231 — time unset until NTP sync");
        return ESP_OK;  /* non-fatal */
    }

    time_t epoch = mktime(&rtc_time);

    /* Sanity: if DS3231 returns something before 2024, it's never been set */
    if (epoch < 1704067200) {  /* 2024-01-01 00:00:00 UTC */
        ESP_LOGW(TAG, "DS3231 time looks uninitialized (epoch=%lld) — waiting for NTP",
                 (long long)epoch);
        return ESP_OK;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_source = TIME_SRC_DS3231;

    ESP_LOGI(TAG, "System clock set from DS3231: %04d-%02d-%02d %02d:%02d:%02d",
             rtc_time.tm_year + 1900, rtc_time.tm_mon + 1, rtc_time.tm_mday,
             rtc_time.tm_hour, rtc_time.tm_min, rtc_time.tm_sec);

    return ESP_OK;
}

static void resync_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RESYNC_INTERVAL_SEC * 1000));

        struct tm rtc_time;
        if (ds3231_get_time(&rtc_time) != ESP_OK) {
            ESP_LOGW(TAG, "Resync: DS3231 read failed, skipping");
            continue;
        }

        time_t rtc_epoch = mktime(&rtc_time);
        time_t sys_epoch = time(NULL);
        int drift = (int)(sys_epoch - rtc_epoch);

        if (abs(drift) > 2) {
            if (s_source == TIME_SRC_NTP) {
                /* NTP is more accurate — push system time to DS3231 */
                struct tm sys_tm;
                time_t now = time(NULL);
                gmtime_r(&now, &sys_tm);
                ds3231_set_time(&sys_tm);
                ESP_LOGI(TAG, "Pushed NTP-derived time to DS3231 (drift was %ds)", drift);
            } else {
                /* DS3231 is our only source — correct system clock */
                struct timeval tv = { .tv_sec = rtc_epoch, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "Corrected system clock drift of %ds from DS3231", drift);
            }
        }
    }
}

esp_err_t time_manager_start_sync_task(void)
{
    BaseType_t ret = xTaskCreate(resync_task, "time_resync", 2048, NULL, 3, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

time_t time_manager_now(void)
{
    return time(NULL);
}

time_source_t time_manager_get_source(void)
{
    return s_source;
}

/* Track whether we've already marked RTC as set in NVS (avoid repeat writes) */
static bool s_rtc_marked = false;
static bool s_rtc_marked_checked = false;

/* Callback for ESP-IDF SNTP sync notification */
static void ntp_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "NTP sync received: epoch=%lld", (long long)tv->tv_sec);
    s_source = TIME_SRC_NTP;

    /* Check cached NVS state on first call */
    if (!s_rtc_marked_checked) {
        s_rtc_marked = time_manager_rtc_is_set();
        s_rtc_marked_checked = true;
    }

    /* Read DS3231 to check drift before deciding to write */
    struct tm rtc_time;
    esp_err_t err = ds3231_get_time(&rtc_time);
    if (err == ESP_OK) {
        time_t rtc_epoch = mktime(&rtc_time);
        int drift = abs((int)(tv->tv_sec - rtc_epoch));

        if (drift > NTP_RTC_DRIFT_THRESHOLD_SEC) {
            /* Drift exceeds threshold — write NTP time to DS3231 */
            struct tm ntp_tm;
            gmtime_r(&tv->tv_sec, &ntp_tm);
            err = ds3231_set_time(&ntp_tm);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "DS3231 updated from NTP (drift was %ds)", drift);
                if (!s_rtc_marked) {
                    mark_rtc_set();
                    s_rtc_marked = true;
                }
            } else {
                ESP_LOGE(TAG, "Failed to write DS3231: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "DS3231 drift %ds within threshold, skipping write", drift);
        }
    } else {
        /* Can't read DS3231 — write unconditionally as fallback */
        ESP_LOGW(TAG, "DS3231 read failed, writing unconditionally");
        struct tm ntp_tm;
        gmtime_r(&tv->tv_sec, &ntp_tm);
        ds3231_set_time(&ntp_tm);
        if (!s_rtc_marked) {
            mark_rtc_set();
            s_rtc_marked = true;
        }
    }

    /* Only clear OSF if it's actually set */
    bool osf = false;
    if (ds3231_get_osf(&osf) == ESP_OK && osf) {
        ds3231_clear_osf();
        ESP_LOGI(TAG, "Cleared stale OSF flag");
    }
}

esp_err_t time_manager_start_ntp(void)
{
    if (s_ntp_running) {
        ESP_LOGW(TAG, "NTP already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting NTP sync...");

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    /* Set servers in priority order */
    for (int i = 0; i < NTP_SERVER_COUNT && i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++) {
        esp_sntp_setservername(i, NTP_SERVERS[i]);
        ESP_LOGI(TAG, "  NTP server %d: %s", i, NTP_SERVERS[i]);
    }

    sntp_set_time_sync_notification_cb(ntp_sync_cb);
    sntp_set_sync_interval(NTP_POLL_INTERVAL_MS);
    esp_sntp_init();
    s_ntp_running = true;

    ESP_LOGI(TAG, "NTP client started (poll every %luh)",
             (unsigned long)(NTP_POLL_INTERVAL_MS / 3600000));
    return ESP_OK;
}

void time_manager_stop_ntp(void)
{
    if (s_ntp_running) {
        esp_sntp_stop();
        s_ntp_running = false;
        ESP_LOGI(TAG, "NTP client stopped");
    }
}
