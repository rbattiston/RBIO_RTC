#include "wifi_manager.h"
#include "time_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

#define RBIO_AP_SSID       "RBIO_RTC"
#define RBIO_AP_PASSWORD   "rbio_time"
#define RBIO_AP_CHANNEL    1
#define RBIO_AP_MAX_STA    8

#define NVS_NAMESPACE      "rbio_wifi"
#define NVS_KEY_STA_SSID   "sta_ssid"
#define NVS_KEY_STA_PASS   "sta_pass"

#define MAX_RETRY_COUNT    10
#define RETRY_INTERVAL_MS  5000

/* AP clients are kicked after this many seconds to free slots.
 * Devices should sync time and disconnect. They can always reconnect. */
#define AP_CLIENT_MAX_AGE_SEC  30
#define AP_KICK_CHECK_SEC      10

static char s_ap_ip[16]   = "192.168.4.1";
static char s_sta_ip[16]  = "";
static char s_sta_ssid[33] = "";
static bool s_sta_connected = false;
static int  s_retry_count = 0;
static esp_netif_t *s_sta_netif = NULL;

/* Track when each AP client connected (by MAC) */
#define AP_CLIENT_SLOTS  10
typedef struct {
    uint8_t mac[6];
    int64_t connect_time;  /* esp_timer_get_time() */
    bool    active;
} ap_client_t;
static ap_client_t s_ap_clients[AP_CLIENT_SLOTS];

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = data;
            ESP_LOGI(TAG, "AP client connected: %02x:%02x:%02x:%02x:%02x:%02x",
                     ev->mac[0], ev->mac[1], ev->mac[2],
                     ev->mac[3], ev->mac[4], ev->mac[5]);
            /* Track connection time */
            for (int i = 0; i < AP_CLIENT_SLOTS; i++) {
                if (!s_ap_clients[i].active) {
                    memcpy(s_ap_clients[i].mac, ev->mac, 6);
                    s_ap_clients[i].connect_time = esp_timer_get_time();
                    s_ap_clients[i].active = true;
                    break;
                }
            }
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = data;
            ESP_LOGI(TAG, "AP client disconnected: %02x:%02x:%02x:%02x:%02x:%02x",
                     ev->mac[0], ev->mac[1], ev->mac[2],
                     ev->mac[3], ev->mac[4], ev->mac[5]);
            /* Remove from tracking */
            for (int i = 0; i < AP_CLIENT_SLOTS; i++) {
                if (s_ap_clients[i].active &&
                    memcmp(s_ap_clients[i].mac, ev->mac, 6) == 0) {
                    s_ap_clients[i].active = false;
                    break;
                }
            }
            break;
        }
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA interface started");
            /* Don't connect here — connect_sta() handles it after setting config */
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_sta_connected = false;
            s_sta_ip[0] = '\0';
            time_manager_stop_ntp();

            if (s_retry_count < MAX_RETRY_COUNT) {
                s_retry_count++;
                ESP_LOGW(TAG, "STA disconnected, retry %d/%d in %dms",
                         s_retry_count, MAX_RETRY_COUNT, RETRY_INTERVAL_MS);
                vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "STA connection failed after %d retries — will retry on next credential update",
                         MAX_RETRY_COUNT);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "STA connected — IP: %s", s_sta_ip);

        /* STA is up — start NTP automatically */
        time_manager_start_ntp();
    }
}

static esp_err_t load_sta_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(s_sta_ssid);
    err = nvs_get_str(h, NVS_KEY_STA_SSID, s_sta_ssid, &len);
    if (err != ESP_OK) {
        s_sta_ssid[0] = '\0';
        nvs_close(h);
        return err;
    }

    /* Password is loaded directly into wifi_config at connect time */
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t connect_sta(void)
{
    if (s_sta_ssid[0] == '\0') return ESP_ERR_NOT_FOUND;

    /* Load password from NVS */
    char password[64] = "";
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(password);
        nvs_get_str(h, NVS_KEY_STA_PASS, password, &len);
        nvs_close(h);
    }

    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid, s_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(err));
        return err;
    }

    s_retry_count = 0;
    ESP_LOGI(TAG, "STA configured for '%s', connecting...", s_sta_ssid);

    return esp_wifi_connect();
}

/* ── AP client auto-kick task ────────────────────────────────────── */

static void ap_kick_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AP_KICK_CHECK_SEC * 1000));

        int64_t now = esp_timer_get_time();
        int64_t max_age_us = (int64_t)AP_CLIENT_MAX_AGE_SEC * 1000000;

        for (int i = 0; i < AP_CLIENT_SLOTS; i++) {
            if (!s_ap_clients[i].active) continue;

            int64_t age = now - s_ap_clients[i].connect_time;
            if (age > max_age_us) {
                ESP_LOGI(TAG, "Kicking AP client %02x:%02x:%02x:%02x:%02x:%02x (connected %llds)",
                         s_ap_clients[i].mac[0], s_ap_clients[i].mac[1],
                         s_ap_clients[i].mac[2], s_ap_clients[i].mac[3],
                         s_ap_clients[i].mac[4], s_ap_clients[i].mac[5],
                         (long long)(age / 1000000));

                /* Deauthenticate the station */
                uint16_t aid = 0;
                esp_wifi_ap_get_sta_aid(s_ap_clients[i].mac, &aid);
                if (aid > 0) {
                    esp_wifi_deauth_sta(aid);
                }
                s_ap_clients[i].active = false;
            }
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    /* NVS required by WiFi */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create netifs for both AP and STA */
    esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    /* AP config */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = RBIO_AP_SSID,
            .password       = RBIO_AP_PASSWORD,
            .ssid_len       = sizeof(RBIO_AP_SSID) - 1,
            .channel        = RBIO_AP_CHANNEL,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = RBIO_AP_MAX_STA,
        },
    };

    /* Always run AP+STA so we can serve time AND reach NTP */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started — SSID: %s, IP: %s", RBIO_AP_SSID, s_ap_ip);

    /* If STA creds exist in NVS, connect automatically */
    if (load_sta_creds() == ESP_OK && s_sta_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Found saved STA credentials for '%s'", s_sta_ssid);
        connect_sta();
    } else {
        ESP_LOGI(TAG, "No STA credentials — configure via http://%s", s_ap_ip);
    }

    /* Start AP client auto-kick task */
    xTaskCreate(ap_kick_task, "ap_kick", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "AP client timeout: %ds (checked every %ds)",
             AP_CLIENT_MAX_AGE_SEC, AP_KICK_CHECK_SEC);

    return ESP_OK;
}

esp_err_t wifi_manager_set_sta_creds(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    /* Save to NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, NVS_KEY_STA_SSID, ssid);
    nvs_set_str(h, NVS_KEY_STA_PASS, password ? password : "");
    nvs_commit(h);
    nvs_close(h);

    /* Update in-memory SSID */
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';

    ESP_LOGI(TAG, "STA credentials saved for '%s'", ssid);

    /* Disconnect if currently connected, then reconnect with new creds */
    esp_wifi_disconnect();
    s_sta_connected = false;
    s_sta_ip[0] = '\0';

    return connect_sta();
}

bool wifi_manager_has_sta_creds(void)
{
    return s_sta_ssid[0] != '\0';
}

bool wifi_manager_sta_connected(void)
{
    return s_sta_connected;
}

const char *wifi_manager_get_ap_ip(void)
{
    return s_ap_ip;
}

const char *wifi_manager_get_sta_ip(void)
{
    return s_sta_ip;
}

const char *wifi_manager_get_sta_ssid(void)
{
    return s_sta_ssid;
}
