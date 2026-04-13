/*
 * RBIO_RTC Client — Reference Implementation
 * See rbio_rtc_client.h for usage and protocol documentation.
 *
 * Features:
 *   - Autonomous channel scanning (finds server on any WiFi channel)
 *   - Active probing (sends request on each channel for fast discovery)
 *   - v1 unsigned + v2 HMAC-SHA256 signed beacon support
 *   - Replay protection via monotonic sequence numbers (v2)
 *   - Auto re-scan if beacons stop arriving
 */

#include "rbio_rtc_client.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "rbio_client";

/* ── Protocol constants ─────────────────────────────────────────── */

#define V1_MAGIC   0xBE
#define V2_VERSION 0x02
#define V1_LEN     13
#define V2_LEN     37
#define HMAC_LEN   20

/* ── Scanner configuration ──────────────────────────────────────── */

#define SCAN_DWELL_MS          150     /* ms to dwell on each channel */
#define SCAN_BEACON_TIMEOUT_S  30      /* seconds without beacon → re-scan */
#define SCAN_BACKOFF_INIT_MS   1000    /* initial backoff after failed scan */
#define SCAN_BACKOFF_MAX_MS    60000   /* max backoff (1 minute) */
#define SCAN_TASK_STACK        3072
#define SCAN_TASK_PRIO         5

#define EVT_BEACON_FOUND       BIT0

/* ── Scanner state ──────────────────────────────────────────────── */

typedef enum {
    SCAN_SCANNING,
    SCAN_LOCKED,
    SCAN_BACKOFF,
} scan_state_t;

static struct {
    scan_state_t        state;
    uint8_t             locked_channel;
    int64_t             last_beacon_us;
    uint32_t            backoff_ms;
    EventGroupHandle_t  events;
    TaskHandle_t        task_handle;
    uint8_t             chan_start;
    uint8_t             chan_count;
} s_scan;

/* ── Client state ───────────────────────────────────────────────── */

static uint8_t        s_psk[RBIO_PSK_LEN];
static bool           s_has_psk = false;
static rbio_time_cb_t s_callback = NULL;
static uint32_t       s_last_seq = 0;

/* ── Byte helpers ───────────────────────────────────────────────── */

static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t get_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

/* ── HMAC-SHA256 (mbedtls v4 compatible) ────────────────────────── */

#define SHA256_BLOCK_SIZE 64
#define SHA256_HASH_SIZE  32

static esp_err_t sha256(const uint8_t *data, size_t len, uint8_t *out)
{
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return ESP_FAIL;

    mbedtls_md_init(&ctx);
    int ret = mbedtls_md_setup(&ctx, info, 0);
    if (ret == 0) ret = mbedtls_md_starts(&ctx);
    if (ret == 0) ret = mbedtls_md_update(&ctx, data, len);
    if (ret == 0) ret = mbedtls_md_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t compute_hmac(const uint8_t *data, size_t data_len,
                              const uint8_t *key, size_t key_len,
                              uint8_t *out, size_t out_len)
{
    uint8_t k_prime[SHA256_BLOCK_SIZE];
    uint8_t inner_hash[SHA256_HASH_SIZE];
    uint8_t full_hmac[SHA256_HASH_SIZE];

    memset(k_prime, 0, SHA256_BLOCK_SIZE);
    if (key_len <= SHA256_BLOCK_SIZE) {
        memcpy(k_prime, key, key_len);
    } else {
        if (sha256(key, key_len, k_prime) != ESP_OK) return ESP_FAIL;
    }

    /* Inner hash: H((K' ^ ipad) || data) */
    uint8_t inner_buf[SHA256_BLOCK_SIZE + 250];
    if (data_len > 250) return ESP_FAIL;

    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        inner_buf[i] = k_prime[i] ^ 0x36;
    }
    memcpy(inner_buf + SHA256_BLOCK_SIZE, data, data_len);
    if (sha256(inner_buf, SHA256_BLOCK_SIZE + data_len, inner_hash) != ESP_OK)
        return ESP_FAIL;

    /* Outer hash: H((K' ^ opad) || inner_hash) */
    uint8_t outer_buf[SHA256_BLOCK_SIZE + SHA256_HASH_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        outer_buf[i] = k_prime[i] ^ 0x5c;
    }
    memcpy(outer_buf + SHA256_BLOCK_SIZE, inner_hash, SHA256_HASH_SIZE);
    if (sha256(outer_buf, sizeof(outer_buf), full_hmac) != ESP_OK)
        return ESP_FAIL;

    size_t copy_len = (out_len < SHA256_HASH_SIZE) ? out_len : SHA256_HASH_SIZE;
    memcpy(out, full_hmac, copy_len);
    return ESP_OK;
}

static bool verify_hmac(const uint8_t *data, size_t data_len,
                        const uint8_t *expected, size_t expected_len)
{
    uint8_t computed[HMAC_LEN];
    if (compute_hmac(data, data_len, s_psk, RBIO_PSK_LEN,
                     computed, expected_len) != ESP_OK) {
        return false;
    }

    /* Constant-time comparison */
    uint8_t diff = 0;
    for (size_t i = 0; i < expected_len; i++) {
        diff |= computed[i] ^ expected[i];
    }
    return diff == 0;
}

/* ── v1 beacon parsing ──────────────────────────────────────────── */

static bool parse_v1(const uint8_t *data, int len, rbio_time_t *out)
{
    if (len < V1_LEN) return false;
    if (data[0] != V1_MAGIC) return false;

    uint8_t cs = 0;
    for (int i = 0; i < 12; i++) cs ^= data[i];
    if (cs != data[12]) {
        ESP_LOGW(TAG, "v1 checksum mismatch");
        return false;
    }

    out->version  = 1;
    out->epoch    = get_u32_be(&data[1]);
    out->ms       = get_u16_be(&data[5]);
    out->source   = data[7];
    out->uptime   = get_u32_be(&data[8]);
    out->seq      = 0;
    out->verified = false;
    return true;
}

/* ── v2 beacon parsing ──────────────────────────────────────────── */

static bool parse_v2(const uint8_t *data, int len, rbio_time_t *out)
{
    if (len < V2_LEN) return false;
    if (data[0] != V2_VERSION) return false;

    out->version  = 2;
    out->epoch    = get_u32_be(&data[1]);
    out->ms       = get_u16_be(&data[5]);
    out->source   = data[7];
    out->uptime   = get_u32_be(&data[8]);
    out->seq      = get_u32_be(&data[12]);
    out->verified = false;

    if (s_has_psk) {
        if (!verify_hmac(data, 16, &data[16], HMAC_LEN)) {
            ESP_LOGW(TAG, "v2 HMAC verification FAILED — rejected");
            return false;
        }
        out->verified = true;

        if (out->seq <= s_last_seq) {
            ESP_LOGW(TAG, "v2 replay: seq %lu <= last %lu — rejected",
                     (unsigned long)out->seq, (unsigned long)s_last_seq);
            return false;
        }
        s_last_seq = out->seq;
    }

    return true;
}

/* ── ESP-NOW receive callback ───────────────────────────────────── */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!s_callback || len < 1) return;

    rbio_time_t t;
    bool valid = false;

    if (s_has_psk) {
        if (data[0] == V2_VERSION && parse_v2(data, len, &t)) {
            valid = true;
        }
    } else {
        if (data[0] == V2_VERSION && parse_v2(data, len, &t)) {
            valid = true;
        } else if (data[0] == V1_MAGIC && parse_v1(data, len, &t)) {
            valid = true;
        }
    }

    if (valid) {
        /* Signal scanner before user callback */
        s_scan.last_beacon_us = esp_timer_get_time();
        if (s_scan.events) {
            xEventGroupSetBits(s_scan.events, EVT_BEACON_FOUND);
        }
        s_callback(&t);
    }
}

/* ── Channel scanner task ───────────────────────────────────────── */

static void scan_task(void *arg)
{
    /* Add broadcast peer for active probing */
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_peer_info_t peer = {
        .channel = 0,
        .encrypt = false,
        .ifidx   = WIFI_IF_STA,
    };
    memcpy(peer.peer_addr, broadcast, 6);
    esp_now_add_peer(&peer);

    uint8_t req_byte = s_has_psk ? 0x02 : 0x01;

    for (;;) {
        switch (s_scan.state) {

        case SCAN_SCANNING: {
            bool found = false;
            ESP_LOGI(TAG, "Scanning channels %d-%d...",
                     s_scan.chan_start,
                     s_scan.chan_start + s_scan.chan_count - 1);

            for (uint8_t i = 0; i < s_scan.chan_count && !found; i++) {
                uint8_t ch = s_scan.chan_start + i;
                esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);

                /* Active probe: request a beacon on this channel */
                esp_now_send(broadcast, &req_byte, 1);

                EventBits_t bits = xEventGroupWaitBits(
                    s_scan.events, EVT_BEACON_FOUND,
                    pdTRUE, pdFALSE,
                    pdMS_TO_TICKS(SCAN_DWELL_MS));

                if (bits & EVT_BEACON_FOUND) {
                    s_scan.locked_channel = ch;
                    s_scan.state = SCAN_LOCKED;
                    s_scan.backoff_ms = SCAN_BACKOFF_INIT_MS;
                    found = true;
                    ESP_LOGI(TAG, "Beacon found on channel %d, locked", ch);
                }
            }

            if (!found) {
                s_scan.state = SCAN_BACKOFF;
                ESP_LOGW(TAG, "No beacon found, backoff %lums",
                         (unsigned long)s_scan.backoff_ms);
            }
            break;
        }

        case SCAN_LOCKED: {
            EventBits_t bits = xEventGroupWaitBits(
                s_scan.events, EVT_BEACON_FOUND,
                pdTRUE, pdFALSE,
                pdMS_TO_TICKS(1000));
            (void)bits;

            int64_t elapsed_us = esp_timer_get_time() - s_scan.last_beacon_us;
            if (elapsed_us > (int64_t)SCAN_BEACON_TIMEOUT_S * 1000000) {
                ESP_LOGW(TAG, "Beacon timeout (%ds), re-scanning",
                         SCAN_BEACON_TIMEOUT_S);
                s_scan.state = SCAN_SCANNING;
                s_scan.backoff_ms = SCAN_BACKOFF_INIT_MS;
            }
            break;
        }

        case SCAN_BACKOFF:
            vTaskDelay(pdMS_TO_TICKS(s_scan.backoff_ms));
            if (s_scan.backoff_ms < SCAN_BACKOFF_MAX_MS) {
                s_scan.backoff_ms *= 2;
                if (s_scan.backoff_ms > SCAN_BACKOFF_MAX_MS) {
                    s_scan.backoff_ms = SCAN_BACKOFF_MAX_MS;
                }
            }
            s_scan.state = SCAN_SCANNING;
            break;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t rbio_rtc_client_init(const uint8_t *psk, rbio_time_cb_t cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;

    s_callback = cb;

    if (psk) {
        memcpy(s_psk, psk, RBIO_PSK_LEN);
        s_has_psk = true;
        ESP_LOGI(TAG, "Initialized in SECURE mode (v2 only, HMAC verified)");
    } else {
        memset(s_psk, 0, RBIO_PSK_LEN);
        s_has_psk = false;
        ESP_LOGI(TAG, "Initialized in OPEN mode (v1 + v2 accepted)");
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_now_register_recv_cb(on_recv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register recv callback: %s", esp_err_to_name(err));
        return err;
    }

    /* Query regulatory domain for valid channel range */
    wifi_country_t country;
    if (esp_wifi_get_country(&country) == ESP_OK) {
        s_scan.chan_start = country.schan;
        s_scan.chan_count = country.nchan;
    } else {
        s_scan.chan_start = 1;
        s_scan.chan_count = 13;
    }

    s_scan.state = SCAN_SCANNING;
    s_scan.locked_channel = 0;
    s_scan.last_beacon_us = 0;
    s_scan.backoff_ms = SCAN_BACKOFF_INIT_MS;

    s_scan.events = xEventGroupCreate();
    if (!s_scan.events) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(scan_task, "rbio_scan", SCAN_TASK_STACK,
                                 NULL, SCAN_TASK_PRIO, &s_scan.task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scan task");
        vEventGroupDelete(s_scan.events);
        s_scan.events = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Channel scan started (ch %d-%d, dwell %dms)",
             s_scan.chan_start,
             s_scan.chan_start + s_scan.chan_count - 1,
             SCAN_DWELL_MS);

    return ESP_OK;
}

esp_err_t rbio_rtc_client_request(const uint8_t *server_mac)
{
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    const uint8_t *target = server_mac ? server_mac : broadcast;

    esp_now_peer_info_t peer = {
        .channel = 0,
        .encrypt = false,
        .ifidx   = WIFI_IF_STA,
    };
    memcpy(peer.peer_addr, target, 6);
    esp_now_add_peer(&peer);

    uint8_t req = s_has_psk ? 0x02 : 0x01;
    return esp_now_send(target, &req, 1);
}
