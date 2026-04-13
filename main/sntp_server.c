#include "sntp_server.h"
#include "time_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "sntp_srv";

#define NTP_PORT        123
#define NTP_PKT_LEN     48

/* ── Rate limiter ───────────────────────────────────────────────── *
 * Per-IP tracking: each IP gets at most 1 response per MIN_INTERVAL.
 * Uses a small fixed-size table with LRU eviction.
 * Designed for a facility with ~20-30 devices, not the open internet.
 */
#define RATE_TABLE_SIZE     32       /* track up to 32 unique IPs */
#define RATE_MIN_INTERVAL_US 2000000 /* 2 seconds between queries per IP */
#define RATE_BURST_LIMIT    3        /* allow this many rapid queries before limiting */
#define RATE_BURST_WINDOW_US 10000000 /* burst counter resets after 10s of silence */

typedef struct {
    uint32_t ip;           /* client IP (network byte order) */
    int64_t  last_time;    /* esp_timer_get_time() of last accepted query */
    uint8_t  burst_count;  /* queries in current burst window */
    int64_t  burst_start;  /* when the current burst window started */
} rate_entry_t;

static rate_entry_t s_rate_table[RATE_TABLE_SIZE];
static int s_rate_count = 0;
static uint32_t s_total_served = 0;
static uint32_t s_total_dropped = 0;

static rate_entry_t *rate_find(uint32_t ip)
{
    for (int i = 0; i < s_rate_count; i++) {
        if (s_rate_table[i].ip == ip) return &s_rate_table[i];
    }
    return NULL;
}

static rate_entry_t *rate_alloc(uint32_t ip, int64_t now)
{
    /* Find empty slot or evict oldest */
    if (s_rate_count < RATE_TABLE_SIZE) {
        rate_entry_t *e = &s_rate_table[s_rate_count++];
        e->ip = ip;
        e->last_time = 0;
        e->burst_count = 0;
        e->burst_start = now;
        return e;
    }

    /* Evict: find the entry with the oldest last_time */
    int oldest = 0;
    for (int i = 1; i < RATE_TABLE_SIZE; i++) {
        if (s_rate_table[i].last_time < s_rate_table[oldest].last_time) {
            oldest = i;
        }
    }
    rate_entry_t *e = &s_rate_table[oldest];
    e->ip = ip;
    e->last_time = 0;
    e->burst_count = 0;
    e->burst_start = now;
    return e;
}

/* Returns true if the query should be served, false if rate-limited */
static bool rate_check(uint32_t ip)
{
    int64_t now = esp_timer_get_time();

    rate_entry_t *e = rate_find(ip);
    if (!e) {
        e = rate_alloc(ip, now);
        e->last_time = now;
        e->burst_count = 1;
        return true;  /* first query from this IP — always allow */
    }

    int64_t elapsed = now - e->last_time;

    /* Reset burst counter if enough silence has passed */
    if ((now - e->burst_start) > RATE_BURST_WINDOW_US) {
        e->burst_count = 0;
        e->burst_start = now;
    }

    e->burst_count++;

    /* Allow if enough time has passed since last query */
    if (elapsed >= RATE_MIN_INTERVAL_US) {
        e->last_time = now;
        return true;
    }

    /* Allow if within burst allowance */
    if (e->burst_count <= RATE_BURST_LIMIT) {
        e->last_time = now;
        return true;
    }

    /* Rate limited */
    return false;
}

/* ── NTP timestamp packing ──────────────────────────────────────── */

/* NTP epoch starts 1900-01-01, Unix epoch 1970-01-01. */
#define NTP_UNIX_OFFSET 2208988800ULL

static void pack_ntp_ts(uint8_t *dst, const struct timeval *tv)
{
    uint32_t secs = (uint32_t)(tv->tv_sec + NTP_UNIX_OFFSET);
    uint32_t frac = (uint32_t)((double)tv->tv_usec * 4294.967296);

    dst[0] = (secs >> 24) & 0xFF;
    dst[1] = (secs >> 16) & 0xFF;
    dst[2] = (secs >> 8)  & 0xFF;
    dst[3] =  secs        & 0xFF;
    dst[4] = (frac >> 24) & 0xFF;
    dst[5] = (frac >> 16) & 0xFF;
    dst[6] = (frac >> 8)  & 0xFF;
    dst[7] =  frac        & 0xFF;
}

/* ── SNTP server task ───────────────────────────────────────────── */

static void sntp_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(NTP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SNTP server listening on UDP:%d (rate limit: %d queries/IP/%ds, burst: %d)",
             NTP_PORT, 1, RATE_MIN_INTERVAL_US / 1000000, RATE_BURST_LIMIT);

    uint8_t buf[NTP_PKT_LEN];
    struct sockaddr_in client_addr;
    socklen_t client_len;

    for (;;) {
        client_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &client_len);

        if (len < NTP_PKT_LEN) continue;

        uint8_t client_mode = buf[0] & 0x07;
        if (client_mode != 3) continue;  /* only client mode */

        /* Rate limit check */
        if (!rate_check(client_addr.sin_addr.s_addr)) {
            s_total_dropped++;
            continue;  /* silently drop — standard NTP behavior for KoD */
        }

        s_total_served++;

        struct timeval recv_time;
        gettimeofday(&recv_time, NULL);

        uint8_t resp[NTP_PKT_LEN];
        memset(resp, 0, sizeof(resp));

        /* LI=0, VN=4, Mode=4 (server) */
        resp[0] = (0 << 6) | (4 << 3) | 4;
        resp[1] = 2;         /* Stratum 2 */
        resp[2] = buf[2];    /* Poll interval from client */
        resp[3] = (uint8_t)(int8_t)-10;  /* Precision ~1ms */

        /* Root delay: ~1ms */
        resp[4] = 0; resp[5] = 0; resp[6] = 0; resp[7] = 0x42;
        /* Root dispersion: ~10ms */
        resp[8] = 0; resp[9] = 0; resp[10] = 0x02; resp[11] = 0x8F;

        /* Reference ID: "LOCL" */
        resp[12] = 'L'; resp[13] = 'O'; resp[14] = 'C'; resp[15] = 'L';

        pack_ntp_ts(&resp[16], &recv_time);       /* Reference timestamp */
        memcpy(&resp[24], &buf[40], 8);            /* Originate (client's transmit) */
        pack_ntp_ts(&resp[32], &recv_time);        /* Receive timestamp */

        struct timeval tx_time;
        gettimeofday(&tx_time, NULL);
        pack_ntp_ts(&resp[40], &tx_time);          /* Transmit timestamp */

        sendto(sock, resp, sizeof(resp), 0,
               (struct sockaddr *)&client_addr, client_len);
    }
}

esp_err_t sntp_server_start(void)
{
    BaseType_t ret = xTaskCreate(sntp_server_task, "sntp_srv", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SNTP server task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

uint32_t sntp_server_get_served(void)  { return s_total_served; }
uint32_t sntp_server_get_dropped(void) { return s_total_dropped; }
