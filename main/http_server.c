#include "http_server.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "ds3231.h"
#include "espnow_time.h"
#include "sntp_server.h"
#include "mesh_role.h"
#include "repeater.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "http_srv";

static const char *source_str(time_source_t src)
{
    switch (src) {
    case TIME_SRC_NONE:   return "NONE";
    case TIME_SRC_DS3231: return "DS3231";
    case TIME_SRC_NTP:    return "NTP";
    case TIME_SRC_ESPNOW: return "ESPNOW";
    default:              return "?";
    }
}

/* ── GET / ── status page + config form ─────────────────────────── */

static const char INDEX_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>%s</title>"
"<style>"
"body{font-family:monospace;max-width:480px;margin:40px auto;padding:0 16px;background:#111;color:#eee}"
"h1{color:#0af}h2{color:#888;font-size:14px;margin-top:24px}"
".status{background:#1a1a1a;padding:16px;border-radius:8px;margin:16px 0}"
".status div{margin:4px 0}.label{color:#888}"
".ok{color:#0c0}.warn{color:#fa0}.err{color:#f44}"
".batt{display:inline-block;padding:2px 8px;border-radius:4px;font-weight:bold}"
".batt.ok{background:#0c02;border:1px solid #0c0}"
".batt.err{background:#f442;border:1px solid #f44}"
"form{background:#1a1a1a;padding:16px;border-radius:8px;margin:16px 0}"
"label{display:block;margin:8px 0 4px;color:#888}"
"input[type=text],input[type=password]{width:100%%;padding:8px;background:#222;border:1px solid #444;color:#eee;border-radius:4px;box-sizing:border-box;font-family:monospace}"
"button{margin-top:12px;padding:10px 24px;background:#0af;color:#111;border:none;border-radius:4px;cursor:pointer;font-weight:bold}"
"button:hover{background:#08d}"
".btn-warn{background:#fa0}.btn-warn:hover{background:#d90}"
".btn-sm{padding:6px 16px;margin-top:0;font-size:12px}"
".msg{padding:12px;border-radius:4px;margin:12px 0}"
".msg.ok{background:#0c03;border:1px solid #0c0}"
".msg.warn{background:#fa02;border:1px solid #fa0}"
".note{color:#888;font-size:12px;margin-top:8px}"
"</style></head><body>"
"<h1>%s</h1>"
"<h2>Facility Time Server</h2>"
"<div class='status'>"
"<div><span class='label'>Time: </span><span>%s</span></div>"
"<div><span class='label'>Source: </span><span class='%s'>%s</span></div>"
"<div><span class='label'>RTC set by NTP: </span><span class='%s'>%s</span></div>"
"</div>"
"<h2>Mesh</h2>"
"<div class='status'>"
"<div><span class='label'>Role: </span><span class='ok'>%s</span></div>"
"<div><span class='label'>Stratum: </span><span class='%s'>%u</span></div>"
"<div><span class='label'>Parent: </span>%s</div>"
"</div>"
"<h2>Battery &amp; Hardware</h2>"
"<div class='status'>"
"<div><span class='label'>Battery: </span><span class='batt %s'>%s</span></div>"
"<div><span class='label'>RTC Temp: </span>%s</div>"
"</div>"
"<h2>Network</h2>"
"<div class='status'>"
"<div><span class='label'>STA WiFi: </span><span class='%s'>%s</span></div>"
"<div><span class='label'>STA IP: </span>%s</div>"
"<div><span class='label'>AP SNTP: </span>192.168.4.1:123</div>"
"</div>"
"<h2>ESP-NOW Security</h2>"
"<div class='status'>"
"<div><span class='label'>Signed beacons (v2): </span><span class='%s'>%s</span></div>"
"<div><span class='label'>PSK: </span>%s</div>"
"</div>"
"%s"
"<h2>WiFi Configuration</h2>"
"<form method='POST' action='/wifi'>"
"<label>Router SSID</label>"
"<input type='text' name='ssid' maxlength='32' required>"
"<label>Password</label>"
"<input type='password' name='pass' maxlength='63'>"
"<button type='submit'>Connect</button>"
"</form>"
"<h2>ESP-NOW Pre-Shared Key</h2>"
"<form method='POST' action='/psk'>"
"<label>PSK (64 hex characters = 32 bytes)</label>"
"<input type='text' name='psk' id='psk_input' maxlength='64' pattern='[0-9a-fA-F]{64}' placeholder='e.g. aabbccdd...'>"
"<div style='margin-top:8px'>"
"<button type='button' class='btn-sm' onclick=\"fetch('/psk/generate').then(r=>r.text()).then(k=>{document.getElementById('psk_input').value=k})\">Generate Random PSK</button>"
"</div>"
"<button type='submit'>Save PSK</button>"
"<p class='note'>Clients must use the same PSK to verify signed beacons. "
"Leave blank and save to disable v2 signing.</p>"
"</form>"
"<h2>Mesh Role</h2>"
"<form method='POST' action='/role'>"
"<label>Device role (takes effect on reboot)</label>"
"<select name='role' style='width:100%%;padding:8px;background:#222;border:1px solid #444;color:#eee;border-radius:4px;font-family:monospace'>"
"%s"
"%s"
"</select>"
"<button type='submit'>Save Role</button>"
"<p class='note'>ROOT: connects to router, syncs NTP, broadcasts stratum 0. "
"REPEATER: no router, receives from upstream mesh node, re-broadcasts at stratum+1.</p>"
"</form>"
"</body></html>";

static esp_err_t index_get_handler(httpd_req_t *req)
{
    time_t now = time_manager_now();
    struct tm t;
    gmtime_r(&now, &t);
    char time_str[48];
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d UTC",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);

    time_source_t src = time_manager_get_source();
    const char *src_class = (src == TIME_SRC_NTP) ? "ok" : (src == TIME_SRC_DS3231) ? "warn" : "err";

    bool rtc_set = time_manager_rtc_is_set();
    const char *rtc_class = rtc_set ? "ok" : "warn";
    const char *rtc_str = rtc_set ? "Yes" : "No";

    /* Battery */
    bool osf = false;
    const char *batt_class, *batt_str;
    if (ds3231_get_osf(&osf) != ESP_OK) {
        batt_class = "warn";
        batt_str = "RTC not detected";
    } else if (osf) {
        batt_class = "err";
        batt_str = "BAD — oscillator stopped (replace battery)";
    } else {
        batt_class = "ok";
        batt_str = "GOOD";
    }

    float temp_c = 0;
    char temp_str[32];
    if (ds3231_get_temperature(&temp_c) == ESP_OK) {
        snprintf(temp_str, sizeof(temp_str), "%.1fC / %.1fF",
                 (double)temp_c, (double)(temp_c * 1.8f + 32.0f));
    } else {
        snprintf(temp_str, sizeof(temp_str), "N/A");
    }

    /* Network */
    bool sta_conn = wifi_manager_sta_connected();
    const char *sta_class = sta_conn ? "ok" : (wifi_manager_has_sta_creds() ? "warn" : "err");
    const char *sta_str;
    if (sta_conn) {
        sta_str = "Connected";
    } else if (wifi_manager_has_sta_creds()) {
        sta_str = "Connecting...";
    } else {
        sta_str = "Not configured";
    }
    const char *sta_ip = wifi_manager_get_sta_ip();
    if (sta_ip[0] == '\0') sta_ip = "N/A";

    /* ESP-NOW security */
    bool has_psk = espnow_time_has_psk();
    const char *espnow_class = has_psk ? "ok" : "warn";
    const char *espnow_str = has_psk ? "Enabled" : "Disabled (no PSK)";
    char psk_display[32];
    if (!espnow_time_get_psk_fingerprint(psk_display, sizeof(psk_display))) {
        snprintf(psk_display, sizeof(psk_display), "Not set");
    }

    /* Mesh */
    mesh_role_t role = mesh_role_get();
    uint8_t stratum = time_manager_get_stratum();
    const char *stratum_class =
        (stratum == 0) ? "ok" :
        (stratum < 7)  ? "warn" : "err";
    char parent_str[64];
    if (role == MESH_ROLE_ROOT) {
        snprintf(parent_str, sizeof(parent_str), "(root — no parent)");
    } else if (repeater_has_parent()) {
        const uint8_t *pm = repeater_get_parent_mac();
        snprintf(parent_str, sizeof(parent_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%ddBm",
                 pm[0], pm[1], pm[2], pm[3], pm[4], pm[5],
                 repeater_get_parent_channel(),
                 repeater_get_parent_rssi());
    } else {
        snprintf(parent_str, sizeof(parent_str), "searching...");
    }

    /* Role selector option HTML */
    const char *root_opt = (role == MESH_ROLE_ROOT)
        ? "<option value='0' selected>ROOT</option>"
        : "<option value='0'>ROOT</option>";
    const char *repeater_opt = (role == MESH_ROLE_REPEATER)
        ? "<option value='1' selected>REPEATER</option>"
        : "<option value='1'>REPEATER</option>";

    /* Flash messages */
    char msg_html[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (buf && httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char val[16];
            if (httpd_query_key_value(buf, "saved", val, sizeof(val)) == ESP_OK) {
                snprintf(msg_html, sizeof(msg_html),
                         "<div class='msg ok'>WiFi credentials saved. Connecting...</div>");
            }
            if (httpd_query_key_value(buf, "psk", val, sizeof(val)) == ESP_OK) {
                if (strcmp(val, "set") == 0) {
                    snprintf(msg_html, sizeof(msg_html),
                             "<div class='msg ok'>PSK saved. Signed v2 beacons are now active.</div>");
                } else if (strcmp(val, "cleared") == 0) {
                    snprintf(msg_html, sizeof(msg_html),
                             "<div class='msg warn'>PSK cleared. Only unsigned v1 beacons will be sent.</div>");
                } else if (strcmp(val, "bad") == 0) {
                    snprintf(msg_html, sizeof(msg_html),
                             "<div class='msg warn'>Invalid PSK. Must be exactly 64 hex characters (32 bytes).</div>");
                }
            }
            if (httpd_query_key_value(buf, "role", val, sizeof(val)) == ESP_OK) {
                snprintf(msg_html, sizeof(msg_html),
                         "<div class='msg ok'>Role saved. Reboot the device for changes to take effect.</div>");
            }
        }
        free(buf);
    }

    char *page = malloc(sizeof(INDEX_HTML) + 1024);
    if (!page) return ESP_ERR_NO_MEM;

    const char *ap_ssid = wifi_manager_get_ap_ssid();

    int len = snprintf(page, sizeof(INDEX_HTML) + 1024, INDEX_HTML,
                       ap_ssid,      /* <title> */
                       ap_ssid,      /* <h1> */
                       time_str,
                       src_class, source_str(src),
                       rtc_class, rtc_str,
                       mesh_role_str(role),
                       stratum_class, stratum,
                       parent_str,
                       batt_class, batt_str,
                       temp_str,
                       sta_class, sta_str,
                       sta_ip,
                       espnow_class, espnow_str,
                       psk_display,
                       msg_html,
                       root_opt, repeater_opt);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, len);
    free(page);
    return ESP_OK;
}

/* ── POST /wifi ── save WiFi credentials ────────────────────────── */

static esp_err_t url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++) {
        if (src[si] == '%' && src[si+1] && src[si+2]) {
            char hex[3] = { src[si+1], src[si+2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
    return ESP_OK;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[256] = "";
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid_raw[64] = "", pass_raw[128] = "";
    char ssid[33] = "", pass[64] = "";

    httpd_query_key_value(body, "ssid", ssid_raw, sizeof(ssid_raw));
    httpd_query_key_value(body, "pass", pass_raw, sizeof(pass_raw));

    url_decode(ssid, sizeof(ssid), ssid_raw);
    url_decode(pass, sizeof(pass), pass_raw);

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi config received — SSID: '%s'", ssid);
    wifi_manager_set_sta_creds(ssid, pass);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved=1");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── POST /psk ── save ESP-NOW pre-shared key ───────────────────── */

static int hex_to_byte(char hi, char lo)
{
    int h = -1, l = -1;
    if (hi >= '0' && hi <= '9') h = hi - '0';
    else if (hi >= 'a' && hi <= 'f') h = hi - 'a' + 10;
    else if (hi >= 'A' && hi <= 'F') h = hi - 'A' + 10;
    if (lo >= '0' && lo <= '9') l = lo - '0';
    else if (lo >= 'a' && lo <= 'f') l = lo - 'a' + 10;
    else if (lo >= 'A' && lo <= 'F') l = lo - 'A' + 10;
    if (h < 0 || l < 0) return -1;
    return (h << 4) | l;
}

static esp_err_t psk_post_handler(httpd_req_t *req)
{
    char body[256] = "";
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char psk_raw[128] = "";
    char psk_hex[128] = "";
    httpd_query_key_value(body, "psk", psk_raw, sizeof(psk_raw));
    url_decode(psk_hex, sizeof(psk_hex), psk_raw);

    /* Empty = clear PSK */
    if (strlen(psk_hex) == 0) {
        espnow_time_set_psk(NULL);
        ESP_LOGI(TAG, "PSK cleared via web UI");
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/?psk=cleared");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* Must be exactly 64 hex chars = 32 bytes */
    if (strlen(psk_hex) != 64) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/?psk=bad");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    uint8_t key[ESPNOW_PSK_LEN];
    for (int i = 0; i < ESPNOW_PSK_LEN; i++) {
        int b = hex_to_byte(psk_hex[i*2], psk_hex[i*2+1]);
        if (b < 0) {
            httpd_resp_set_status(req, "303 See Other");
            httpd_resp_set_hdr(req, "Location", "/?psk=bad");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        key[i] = (uint8_t)b;
    }

    espnow_time_set_psk(key);
    ESP_LOGI(TAG, "PSK set via web UI");

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?psk=set");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /psk/generate ── generate random PSK as hex string ─────── */

static esp_err_t psk_generate_handler(httpd_req_t *req)
{
    uint8_t key[ESPNOW_PSK_LEN];
    esp_fill_random(key, sizeof(key));

    char hex[ESPNOW_PSK_LEN * 2 + 1];
    for (int i = 0; i < ESPNOW_PSK_LEN; i++) {
        snprintf(&hex[i*2], 3, "%02x", key[i]);
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, hex, ESPNOW_PSK_LEN * 2);
    return ESP_OK;
}

/* ── POST /role ── set mesh role (root or repeater) ──────────────── */

static esp_err_t role_post_handler(httpd_req_t *req)
{
    char body[64] = "";
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char role_str[8] = "";
    httpd_query_key_value(body, "role", role_str, sizeof(role_str));

    mesh_role_t role = (strcmp(role_str, "1") == 0) ? MESH_ROLE_REPEATER : MESH_ROLE_ROOT;
    mesh_role_set(role);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?role=saved");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── GET /status ── JSON status for programmatic access ─────────── */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    time_t now = time_manager_now();
    struct tm t;
    gmtime_r(&now, &t);

    bool osf = false;
    ds3231_get_osf(&osf);
    float temp_c = 0;
    ds3231_get_temperature(&temp_c);

    char psk_fp[32] = "null";
    if (espnow_time_has_psk()) {
        char fp[20];
        espnow_time_get_psk_fingerprint(fp, sizeof(fp));
        snprintf(psk_fp, sizeof(psk_fp), "\"%s\"", fp);
    }

    char parent_mac_str[24] = "null";
    mesh_role_t role = mesh_role_get();
    if (role == MESH_ROLE_REPEATER && repeater_has_parent()) {
        const uint8_t *pm = repeater_get_parent_mac();
        snprintf(parent_mac_str, sizeof(parent_mac_str),
                 "\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                 pm[0], pm[1], pm[2], pm[3], pm[4], pm[5]);
    }

    char json[768];
    int len = snprintf(json, sizeof(json),
        "{\"device_ssid\":\"%s\","
        "\"time\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\","
        "\"source\":\"%s\","
        "\"rtc_set\":%s,"
        "\"battery_ok\":%s,"
        "\"rtc_temp_c\":%.1f,"
        "\"sta_connected\":%s,"
        "\"sta_ssid\":\"%s\","
        "\"sta_ip\":\"%s\","
        "\"espnow_v2_active\":%s,"
        "\"espnow_psk_fingerprint\":%s,"
        "\"sntp_served\":%lu,"
        "\"sntp_dropped\":%lu,"
        "\"role\":\"%s\","
        "\"stratum\":%u,"
        "\"parent_mac\":%s,"
        "\"parent_rssi\":%d}",
        wifi_manager_get_ap_ssid(),
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec,
        source_str(time_manager_get_source()),
        time_manager_rtc_is_set() ? "true" : "false",
        osf ? "false" : "true",
        (double)temp_c,
        wifi_manager_sta_connected() ? "true" : "false",
        wifi_manager_get_sta_ssid(),
        wifi_manager_get_sta_ip(),
        espnow_time_has_psk() ? "true" : "false",
        psk_fp,
        (unsigned long)sntp_server_get_served(),
        (unsigned long)sntp_server_get_dropped(),
        mesh_role_str(role),
        time_manager_get_stratum(),
        parent_mac_str,
        (int)repeater_get_parent_rssi());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return ESP_OK;
}

/* ── GET /favicon.ico ── silence browser 404 spam ───────────────── */

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── Server startup ─────────────────────────────────────────────── */

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 10;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/favicon.ico",  .method = HTTP_GET,  .handler = favicon_handler },
        { .uri = "/",             .method = HTTP_GET,  .handler = index_get_handler },
        { .uri = "/wifi",         .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/psk",          .method = HTTP_POST, .handler = psk_post_handler },
        { .uri = "/psk/generate", .method = HTTP_GET,  .handler = psk_generate_handler },
        { .uri = "/role",         .method = HTTP_POST, .handler = role_post_handler },
        { .uri = "/status",       .method = HTTP_GET,  .handler = status_get_handler },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
}
