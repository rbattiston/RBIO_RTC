#include "espnow_time.h"
#include "time_manager.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "espnow_time";

/* Broadcast MAC */
static const uint8_t BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* NVS keys */
#define NVS_NAMESPACE   "rbio_espnow"
#define NVS_KEY_PSK     "psk"
#define NVS_KEY_SEQ     "seq"

/* PSK state */
static uint8_t s_psk[ESPNOW_PSK_LEN];
static bool    s_psk_set = false;

/* Monotonic sequence number — persisted in NVS */
static uint32_t s_seq = 0;

/* ── PSK management ─────────────────────────────────────────────── */

static esp_err_t load_psk(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = ESPNOW_PSK_LEN;
    err = nvs_get_blob(h, NVS_KEY_PSK, s_psk, &len);
    if (err == ESP_OK && len == ESPNOW_PSK_LEN) {
        s_psk_set = true;
        ESP_LOGI(TAG, "PSK loaded from NVS");
    }
    nvs_close(h);
    return err;
}

static esp_err_t load_seq(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    nvs_get_u32(h, NVS_KEY_SEQ, &s_seq);
    nvs_close(h);
    ESP_LOGI(TAG, "Sequence number loaded: %lu", (unsigned long)s_seq);
    return ESP_OK;
}

static void persist_seq(void)
{
    /* Persist every 64 increments to reduce NVS wear.
     * On reboot we jump ahead by up to 64 — acceptable for replay protection. */
    if ((s_seq & 0x3F) == 0) {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            /* Store seq + 64 so after crash we skip past any beacons
             * that were sent but not yet persisted */
            nvs_set_u32(h, NVS_KEY_SEQ, s_seq + 64);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

esp_err_t espnow_time_set_psk(const uint8_t *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (key) {
        memcpy(s_psk, key, ESPNOW_PSK_LEN);
        nvs_set_blob(h, NVS_KEY_PSK, s_psk, ESPNOW_PSK_LEN);
        s_psk_set = true;
        ESP_LOGI(TAG, "PSK set and saved");
    } else {
        nvs_erase_key(h, NVS_KEY_PSK);
        memset(s_psk, 0, ESPNOW_PSK_LEN);
        s_psk_set = false;
        ESP_LOGI(TAG, "PSK cleared");
    }

    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

bool espnow_time_has_psk(void)
{
    return s_psk_set;
}

bool espnow_time_get_psk_fingerprint(char *buf, size_t buf_len)
{
    if (!s_psk_set) return false;
    snprintf(buf, buf_len, "%02x%02x%02x%02x...",
             s_psk[0], s_psk[1], s_psk[2], s_psk[3]);
    return true;
}

/* ── HMAC-SHA256 ────────────────────────────────────────────────── */

/*
 * HMAC-SHA256 implemented using mbedtls_md plain hash API.
 * HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m))
 * where K' = K if len <= 64, else H(K)
 */
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

    /* Step 1: derive K' — pad or hash the key to block size */
    memset(k_prime, 0, SHA256_BLOCK_SIZE);
    if (key_len <= SHA256_BLOCK_SIZE) {
        memcpy(k_prime, key, key_len);
    } else {
        if (sha256(key, key_len, k_prime) != ESP_OK) return ESP_FAIL;
    }

    /* Step 2: inner hash = H((K' ^ ipad) || data) */
    uint8_t inner_buf[SHA256_BLOCK_SIZE + 250];  /* block + max beacon data */
    if (data_len > 250) return ESP_FAIL;  /* sanity — beacons are tiny */

    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        inner_buf[i] = k_prime[i] ^ 0x36;
    }
    memcpy(inner_buf + SHA256_BLOCK_SIZE, data, data_len);
    if (sha256(inner_buf, SHA256_BLOCK_SIZE + data_len, inner_hash) != ESP_OK)
        return ESP_FAIL;

    /* Step 3: outer hash = H((K' ^ opad) || inner_hash) */
    uint8_t outer_buf[SHA256_BLOCK_SIZE + SHA256_HASH_SIZE];
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++) {
        outer_buf[i] = k_prime[i] ^ 0x5c;
    }
    memcpy(outer_buf + SHA256_BLOCK_SIZE, inner_hash, SHA256_HASH_SIZE);
    if (sha256(outer_buf, sizeof(outer_buf), full_hmac) != ESP_OK)
        return ESP_FAIL;

    /* Truncate */
    size_t copy_len = (out_len < SHA256_HASH_SIZE) ? out_len : SHA256_HASH_SIZE;
    memcpy(out, full_hmac, copy_len);
    return ESP_OK;
}

/* ── Beacon builders ────────────────────────────────────────────── */

static void get_time_fields(uint32_t *epoch, uint16_t *ms, uint32_t *uptime)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *epoch  = (uint32_t)tv.tv_sec;
    *ms     = (uint16_t)(tv.tv_usec / 1000);
    *uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void put_u32_be(uint8_t *dst, uint32_t val)
{
    dst[0] = (val >> 24) & 0xFF;
    dst[1] = (val >> 16) & 0xFF;
    dst[2] = (val >> 8)  & 0xFF;
    dst[3] =  val        & 0xFF;
}

static void put_u16_be(uint8_t *dst, uint16_t val)
{
    dst[0] = (val >> 8) & 0xFF;
    dst[1] =  val       & 0xFF;
}

static void build_v1(uint8_t *buf)
{
    uint32_t epoch, uptime;
    uint16_t ms;
    get_time_fields(&epoch, &ms, &uptime);

    buf[0] = ESPNOW_TIME_MAGIC;
    put_u32_be(&buf[1], epoch);
    put_u16_be(&buf[5], ms);
    buf[7] = (uint8_t)time_manager_get_source();
    put_u32_be(&buf[8], uptime);

    /* XOR checksum */
    uint8_t cs = 0;
    for (int i = 0; i < 12; i++) cs ^= buf[i];
    buf[12] = cs;
}

static bool build_v2(uint8_t *buf)
{
    if (!s_psk_set) return false;

    uint32_t epoch, uptime;
    uint16_t ms;
    get_time_fields(&epoch, &ms, &uptime);

    /* Header (bytes 0..15) — this is the HMAC input */
    buf[0] = ESPNOW_V2_VERSION;
    put_u32_be(&buf[1], epoch);
    put_u16_be(&buf[5], ms);
    buf[7] = (uint8_t)time_manager_get_source();
    put_u32_be(&buf[8], uptime);

    s_seq++;
    persist_seq();
    put_u32_be(&buf[12], s_seq);

    /* HMAC-SHA256 over bytes 0..15, truncated to 20 bytes */
    compute_hmac(buf, 16, s_psk, ESPNOW_PSK_LEN, &buf[16], ESPNOW_HMAC_LEN);

    buf[36] = 0x00;  /* reserved */
    return true;
}

/* ── Receive callback ───────────────────────────────────────────── */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 1) return;

    bool want_v2 = (data[0] == ESPNOW_TIME_REQ_V2);
    bool want_v1 = (data[0] == ESPNOW_TIME_REQ_V1);

    if (!want_v1 && !want_v2) return;

    ESP_LOGI(TAG, "Time request (v%d) from %02x:%02x:%02x:%02x:%02x:%02x",
             want_v2 ? 2 : 1,
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5]);

    /* Add peer temporarily for unicast reply */
    esp_now_peer_info_t peer = {
        .channel = 0,
        .encrypt = false,
        .ifidx   = WIFI_IF_AP,
    };
    memcpy(peer.peer_addr, info->src_addr, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&peer);  /* ignore error if already exists */

    if (want_v2 && s_psk_set) {
        uint8_t beacon[ESPNOW_V2_LEN];
        if (build_v2(beacon)) {
            esp_now_send(info->src_addr, beacon, sizeof(beacon));
        }
    } else {
        uint8_t beacon[ESPNOW_V1_LEN];
        build_v1(beacon);
        esp_now_send(info->src_addr, beacon, sizeof(beacon));
    }
}

/* ── Init and broadcast ─────────────────────────────────────────── */

esp_err_t espnow_time_init(void)
{
    /* Load PSK and sequence from NVS */
    load_psk();
    load_seq();

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

    /* Add broadcast peer */
    esp_now_peer_info_t broadcast_peer = {
        .channel = 0,
        .encrypt = false,
        .ifidx   = WIFI_IF_AP,
    };
    memcpy(broadcast_peer.peer_addr, BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    err = esp_now_add_peer(&broadcast_peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add broadcast peer: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW initialized — PSK: %s, seq: %lu, interval: %ds",
             s_psk_set ? "configured" : "none",
             (unsigned long)s_seq,
             ESPNOW_BROADCAST_INTERVAL_SEC);

    return ESP_OK;
}

static void broadcast_task(void *arg)
{
    for (;;) {
        /* Always send v1 (unsigned) for backwards compatibility */
        uint8_t v1[ESPNOW_V1_LEN];
        build_v1(v1);
        esp_now_send(BROADCAST_MAC, v1, sizeof(v1));

        /* Send v2 (signed) if PSK is configured */
        if (s_psk_set) {
            uint8_t v2[ESPNOW_V2_LEN];
            if (build_v2(v2)) {
                esp_now_send(BROADCAST_MAC, v2, sizeof(v2));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ESPNOW_BROADCAST_INTERVAL_SEC * 1000));
    }
}

esp_err_t espnow_time_start_broadcast(void)
{
    BaseType_t ret = xTaskCreate(broadcast_task, "espnow_bcast", 4096, NULL, 4, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create broadcast task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
