#include "wifi_config_server.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

#include "wifi_manager.h"

static const char *TAG = "wifi_cfg";

static httpd_handle_t s_server;
static volatile bool s_scan_in_progress = false;

static const char s_index_html[] =
    "<!doctype html>"
    "<html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<title>Synthewi Wi-Fi Setup</title>"
    "</head><body style='font-family:sans-serif;max-width:520px;margin:32px auto;padding:0 12px'>"
    "<h2>Synthewi Wi-Fi Setup</h2>"
    "<p>Enter your Wi-Fi credentials. Device will reboot and try to connect.</p>"
    "<form method='POST' action='/save'>"
    "<label>SSID<br/><input id='ssid' name='ssid' style='width:100%;padding:10px' required></label><br/><br/>"
    "<label>Password<br/><input id='pass' name='pass' type='password' style='width:100%;padding:10px'></label><br/><br/>"
    "<button type='submit' style='padding:10px 14px'>Save & Reboot</button>"
    "</form>"
    "</body></html>";

static void url_decode_inplace(char *s)
{
    // Minimal x-www-form-urlencoded decode: '+' => ' ', %HH decoding.
    char *src = s;
    char *dst = s;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if ((*src == '%') && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static bool form_get_value(char *body, const char *key, char *out, size_t out_size)
{
    if ((body == NULL) || (key == NULL) || (out == NULL) || (out_size == 0)) {
        return false;
    }

    size_t key_len = strlen(key);
    char *p = body;
    while (p && *p) {
        char *pair = p;
        char *amp = strchr(pair, '&');
        if (amp) {
            *amp = '\0';
            p = amp + 1;
        } else {
            p = NULL;
        }

        char *eq = strchr(pair, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        const char *k = pair;
        char *v = eq + 1;

        if ((strlen(k) == key_len) && (memcmp(k, key, key_len) == 0)) {
            url_decode_inplace(v);
            strlcpy(out, v, out_size);
            return true;
        }
    }
    return false;
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    // Prevent concurrent scans.
    if (s_scan_in_progress) {
        ESP_LOGW(TAG, "Scan already in progress, rejecting");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan_already_running");
        return ESP_OK;
    }
    s_scan_in_progress = true;
    
    ESP_LOGI(TAG, "Starting Wi-Fi scan...");

    // Perform Wi-Fi scan with watchdog feeding.
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    // Feed watchdog before blocking scan.
    esp_task_wdt_reset();
    
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);  // true = blocking
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan_start_failed");
        s_scan_in_progress = false;
        return ESP_OK;
    }

    // Feed watchdog after scan completes.
    esp_task_wdt_reset();

    // Retrieve scan results.
    uint16_t ap_count = 20;
    wifi_ap_record_t ap_records[20];
    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan get failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan_get_failed");
        s_scan_in_progress = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Scan found %d networks", ap_count);

    // Build JSON payload.
    char *buf = malloc(4096);
    if (!buf) {
        ESP_LOGE(TAG, "Malloc failed for scan response");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem");
        s_scan_in_progress = false;
        return ESP_OK;
    }
    
    size_t off = 0;
    off += snprintf(buf + off, 4096 - off, "[");
    
    for (uint16_t i = 0; i < ap_count; ++i) {
        // Escape SSID for JSON.
        char ssid_esc[128] = {0};
        const char *ssid = (const char *)ap_records[i].ssid;
        size_t p = 0;
        for (size_t j = 0; j < sizeof(ap_records[i].ssid) && ssid[j] && p + 2 < sizeof(ssid_esc); ++j) {
            char c = ssid[j];
            if (c == '"' || c == '\\') {
                ssid_esc[p++] = '\\';
                ssid_esc[p++] = c;
            } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
                ssid_esc[p++] = c;
            }
            // Skip control chars and non-ASCII.
        }
        ssid_esc[p] = '\0';

        // Map authmode to string.
        const char *auth = "OPEN";
        if (ap_records[i].authmode == WIFI_AUTH_WEP) auth = "WEP";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA_PSK) auth = "WPA";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA2_PSK) auth = "WPA2";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA_WPA2_PSK) auth = "WPA/WPA2";
        else if (ap_records[i].authmode == WIFI_AUTH_WPA3_PSK) auth = "WPA3";

        off += snprintf(buf + off, 4096 - off,
                        "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}%s",
                        ssid_esc, ap_records[i].rssi, auth, (i + 1 < ap_count) ? "," : "");
        
        if (off + 200 >= 4096) {
            ESP_LOGW(TAG, "Scan buffer full at %d/%d networks", i + 1, ap_count);
            break;
        }
    }
    
    off += snprintf(buf + off, 4096 - off, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, off);
    free(buf);
    
    ESP_LOGI(TAG, "Scan response sent successfully");
    s_scan_in_progress = false;
    return ESP_OK;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    // Body is usually small; cap it.
    const size_t max_len = 256;
    size_t total = req->content_len;
    if (total == 0 || total > max_len) {
        ESP_LOGW(TAG, "Invalid POST body size: %zu", total);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_OK;
    }

    char body[257] = {0};
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST body");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Read failed");
        return ESP_OK;
    }
    body[received] = '\0';

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid))) {
        ESP_LOGW(TAG, "POST: missing SSID field");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_OK;
    }
    (void)form_get_value(body, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "POST /save: SSID='%s', pass_len=%zu", ssid, strlen(pass));

    esp_err_t err = wifi_manager_save_credentials(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed saving creds: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h3>Saved.</h3><p>Rebooting in 1 second...</p></body></html>");
    xTaskCreate(restart_task, "wifi_restart", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

esp_err_t wifi_config_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    config.max_uri_handlers = 8;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &root);

    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = handle_save,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &save);

    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = handle_scan,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &scan);

    return ESP_OK;
}

void wifi_config_server_stop(void)
{
    if (!s_server) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
}

bool wifi_config_server_is_running(void)
{
    return s_server != NULL;
}
