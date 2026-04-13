/*
 * RBIO_RTC Client — Reference Implementation
 * See rbio_rtc_client.h for usage and protocol documentation.
 */

#include "rbio_rtc_client.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include <string.h>

static const char *TAG = "rbio_client";

/* Protocol constants */
#define V1_MAGIC   0xBE
#define V2_VERSION 0x02
#define V1_LEN     13
#define V2_LEN     37
#define HMAC_LEN   20

/* State */
static uint8_t       s_psk[RBIO_PSK_LEN];
static bool          s_has_psk = false;
static rbio_time_cb_t s_callback = NULL;
static uint32_t      s_last_seq = 0;

/* ── Helpers ────────────────────────────────────────────────────── */

static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint16_t get_u16_be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static bool verify_hmac(const uint8_t *data, size_t data_len,
                        const uint8_t *expected, size_t expected_len)
{
    uint8_t full[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;

    if (mbedtls_md_hmac(info, s_psk, RBIO_PSK_LEN, data, data_len, full) != 0) {
        return false;
    }

    /* Constant-time comparison to prevent timing attacks */
    uint8_t diff = 0;
    for (size_t i = 0; i < expected_len && i < 32; i++) {
        diff |= full[i] ^ expected[i];
    }
    return diff == 0;
}

/* ── v1 beacon parsing ──────────────────────────────────────────── */

static bool parse_v1(const uint8_t *data, int len, rbio_time_t *out)
{
    if (len < V1_LEN) return false;
    if (data[0] != V1_MAGIC) return false;

    /* Verify XOR checksum */
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
    out->verified = false;  /* v1 is never cryptographically verified */
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
        /* Verify HMAC over bytes 0..15 */
        if (!verify_hmac(data, 16, &data[16], HMAC_LEN)) {
            ESP_LOGW(TAG, "v2 HMAC verification FAILED — beacon rejected");
            return false;
        }
        out->verified = true;

        /* Replay protection: sequence must be strictly increasing */
        if (out->seq <= s_last_seq) {
            ESP_LOGW(TAG, "v2 replay detected: seq %lu <= last %lu — rejected",
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

    if (s_has_psk) {
        /* Secure mode: only accept verified v2 beacons */
        if (data[0] == V2_VERSION && parse_v2(data, len, &t)) {
            s_callback(&t);
        }
        /* v1 beacons silently ignored in secure mode */
    } else {
        /* Open mode: accept either version */
        if (data[0] == V2_VERSION && parse_v2(data, len, &t)) {
            s_callback(&t);
        } else if (data[0] == V1_MAGIC && parse_v1(data, len, &t)) {
            s_callback(&t);
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

    return esp_now_register_recv_cb(on_recv);
}

esp_err_t rbio_rtc_client_request(const uint8_t *server_mac)
{
    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    const uint8_t *target = server_mac ? server_mac : broadcast;

    /* Add peer if needed */
    esp_now_peer_info_t peer = {
        .channel = 0,
        .encrypt = false,
        .ifidx   = WIFI_IF_STA,
    };
    memcpy(peer.peer_addr, target, 6);
    esp_now_add_peer(&peer);  /* ignore if already exists */

    /* Send request byte: 0x02 for v2 (signed) if we have PSK, else 0x01 */
    uint8_t req = s_has_psk ? 0x02 : 0x01;
    return esp_now_send(target, &req, 1);
}
