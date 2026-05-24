#include "wifi_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "wifi_config_server.h"

static const char *TAG = "wifi_manager";

#define WIFI_NAMESPACE "wifi"
#define WIFI_KEY_SSID  "ssid"
#define WIFI_KEY_PASS  "pass"

// === HARDCODED CREDENTIALS (for testing) ===
// Set these to your Wi-Fi details to enable auto-connect without using NVS
// Leave empty ("") to disable hardcoded mode and use SoftAP setup portal
#define WIFI_HARDCODED_SSID "Synthewi1"
#define WIFI_HARDCODED_PASS "123456789"
// ===================================

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SOFTAP_BIT    BIT1

#define WIFI_MAX_RETRY 8
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define WIFI_STA_CONNECT_TIMEOUT_MS 15000  // 15 seconds to get IP

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;
static SemaphoreHandle_t s_nvs_mutex;
static int s_retry_count;
static bool s_started;
static wifi_state_t s_wifi_state = WIFI_STATE_IDLE;
static uint32_t s_sta_connect_start_ms;
static esp_ip4_addr_t s_current_ip = {0};  // Store current IP for status reporting

static void wifi_log_sta_mac(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA MAC read failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "STA MAC: " MACSTR, MAC2STR(mac));
}

static void wifi_log_sta_config(void)
{
    wifi_config_t cfg = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA config read failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "STA config: ssid='%s' auth=%d pmf(capable=%d required=%d)",
             (char *)cfg.sta.ssid,
             cfg.sta.threshold.authmode,
             cfg.sta.pmf_cfg.capable,
             cfg.sta.pmf_cfg.required);
}


esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    if ((ssid == NULL) || (ssid[0] == '\0')) {
        ESP_LOGE(TAG, "Invalid SSID: NULL or empty");
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strlen(ssid);
    if (ssid_len > WIFI_SSID_MAX_LEN) {
        ESP_LOGE(TAG, "SSID too long: %zu > %d", ssid_len, WIFI_SSID_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (pass == NULL) {
        pass = "";
    }
    size_t pass_len = strlen(pass);
    if (pass_len > WIFI_PASS_MAX_LEN) {
        ESP_LOGE(TAG, "Password too long: %zu > %d", pass_len, WIFI_PASS_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    // Mutex-protected NVS access
    if (xSemaphoreTake(s_nvs_mutex, pdMS_TO_TICKS(5000)) == pdFALSE) {
        ESP_LOGE(TAG, "NVS mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_nvs_mutex);
        return err;
    }

    err = nvs_set_str(nvs, WIFI_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved Wi-Fi SSID '%s' (len=%zu) to NVS", ssid, ssid_len);
    }

    nvs_close(nvs);
    xSemaphoreGive(s_nvs_mutex);
    return err;
}

esp_err_t wifi_manager_clear_credentials(void)
{
    if (xSemaphoreTake(s_nvs_mutex, pdMS_TO_TICKS(5000)) == pdFALSE) {
        ESP_LOGE(TAG, "NVS mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_nvs_mutex);
        return err;
    }

    nvs_erase_key(nvs, WIFI_KEY_SSID);
    nvs_erase_key(nvs, WIFI_KEY_PASS);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Cleared saved Wi-Fi credentials from NVS");
    }
    
    xSemaphoreGive(s_nvs_mutex);
    return err;
}

wifi_status_t wifi_manager_get_status(void)
{
    wifi_status_t status = {
        .state = s_wifi_state,
        .ip_acquired = (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0,
        .retry_count = s_retry_count,
        .last_connect_attempt_ms = s_sta_connect_start_ms,
    };
    return status;
}

static void wifi_start_softap(void)
{
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    xEventGroupSetBits(s_wifi_event_group, WIFI_SOFTAP_BIT);
    s_wifi_state = WIFI_STATE_AP_ACTIVE;

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Synthewi-Setup-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_LOGW(TAG, "Starting SoftAP '%s' (open)", ap_ssid);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi stop failed: %s", esp_err_to_name(err));
    }
    
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set AP mode failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set config failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }

    err = wifi_config_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config server start failed: %s", esp_err_to_name(err));
    }
    
    ESP_LOGW(TAG, "Config portal: connect to Wi-Fi '%s' then open http://192.168.4.1", ap_ssid);
}

static uint32_t wifi_get_exponential_backoff_ms(int retry_count)
{
    // 1s, 2s, 4s, 8s, 16s, 16s, 16s, 16s
    static const uint32_t backoff_table[] = {1000, 2000, 4000, 8000, 16000, 16000, 16000, 16000};
    int idx = retry_count - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int)(sizeof(backoff_table) / sizeof(backoff_table[0]))) {
        idx = sizeof(backoff_table) / sizeof(backoff_table[0]) - 1;
    }
    return backoff_table[idx];
}

static const char *wifi_disconnect_reason_label(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE: return "auth expired";
        case WIFI_REASON_AUTH_FAIL: return "auth fail";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout";
        case WIFI_REASON_BEACON_TIMEOUT: return "beacon timeout";
        case WIFI_REASON_NO_AP_FOUND: return "no ap found";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "auth mode threshold";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: return "rssi threshold";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "compatible security";
        case WIFI_REASON_ASSOC_FAIL: return "assoc fail";
        case WIFI_REASON_CONNECTION_FAIL: return "connection fail";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD: return "unsupported channel";
        case WIFI_REASON_ASSOC_TOOMANY: return "too many clients";
        default: return "other";
    }
}

static void wifi_start_sta_with_credentials(const char *ssid, const char *pass)
{
    xEventGroupClearBits(s_wifi_event_group, WIFI_SOFTAP_BIT);
    s_wifi_state = WIFI_STATE_STA_CONNECTING;
    s_sta_connect_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;

    s_retry_count = 0;
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Starting STA Connection");
    ESP_LOGI(TAG, "  SSID: '%s' (len=%zu)", ssid, strlen(ssid));
    ESP_LOGI(TAG, "  PASS: '%s' (len=%zu)", pass, strlen(pass));
    ESP_LOGI(TAG, "  AUTH: WPA2_PSK");
    ESP_LOGI(TAG, "========================================");

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "[1] esp_wifi_stop() failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "[1] esp_wifi_stop() - OK");
    }
    
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[2] esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "[2] esp_wifi_set_mode(STA) - OK");
    
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[3] esp_wifi_set_config() failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "[3] esp_wifi_set_config() - OK");
    
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[4] esp_wifi_start() failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "[4] esp_wifi_start() - OK (WiFi task running)");

    wifi_log_sta_mac();
    wifi_log_sta_config();

    // IMPORTANT: Do NOT call scan here! Blocking scan before connect blocks the WiFi event loop
    // and prevents WiFi events from being delivered. See ESP-IDF documentation:
    // "scanning will not be effective until connection between device and the AP is established"
    
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[5] esp_wifi_connect() failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
    } else {
        ESP_LOGI(TAG, "[5] esp_wifi_connect() - OK (connection attempt started)");
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "Waiting for WiFi connection or timeout...");
        ESP_LOGI(TAG, "  Timeout: %d seconds", WIFI_STA_CONNECT_TIMEOUT_MS / 1000);
        ESP_LOGI(TAG, "  Max retries: %d (with exponential backoff)", WIFI_MAX_RETRY);
        ESP_LOGI(TAG, "========================================");
    }
}

static void on_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "[WIFI_EVENT_STA_START] STA started, WiFi stack ready");
            ESP_LOGI(TAG, "  STATE: %d | RETRY: %d", s_wifi_state, s_retry_count);
            return;
        }

        if (event_id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "[WIFI_EVENT_STA_CONNECTED] Successfully joined AP, waiting for DHCP...");
            return;
        }

        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *disconnected = (const wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGW(TAG, "[WIFI_EVENT_STA_DISCONNECTED] reason=%u (%s)",
                     disconnected ? disconnected->reason : 0,
                     disconnected ? wifi_disconnect_reason_label(disconnected->reason) : "unknown");

            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

            // Check if we're in AP mode or if connection timed out
            bool in_ap_mode = (xEventGroupGetBits(s_wifi_event_group) & WIFI_SOFTAP_BIT) != 0;
            if (in_ap_mode) {
                ESP_LOGI(TAG, "[WIFI_EVENT_STA_DISCONNECTED] Already in AP mode, ignoring");
                return;
            }

            // Check connection timeout
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t elapsed_ms = now_ms - s_sta_connect_start_ms;
            if (s_sta_connect_start_ms > 0 && elapsed_ms > WIFI_STA_CONNECT_TIMEOUT_MS) {
                ESP_LOGE(TAG, "[WIFI_EVENT_STA_DISCONNECTED] Connection timeout after %lu ms", elapsed_ms);
                s_wifi_state = WIFI_STATE_ERROR;
                s_retry_count = WIFI_MAX_RETRY + 1;  // Force fallback
            }

            s_retry_count++;
            if (s_retry_count <= WIFI_MAX_RETRY) {
                uint32_t backoff_ms = wifi_get_exponential_backoff_ms(s_retry_count);
                ESP_LOGW(TAG, "[WIFI_EVENT_STA_DISCONNECTED] Retry %d/%d in %lu ms", 
                         s_retry_count, WIFI_MAX_RETRY, backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "WiFi connect retry failed: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGE(TAG, "[WIFI_EVENT_STA_DISCONNECTED] Retries exhausted (%d), falling back to SoftAP", 
                         WIFI_MAX_RETRY);
                wifi_start_softap();
            }
        }

        return;
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "[IP_EVENT_STA_GOT_IP] SUCCESS!");
        ESP_LOGI(TAG, "  IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "  GW: " IPSTR, IP2STR(&evt->ip_info.gw));
        ESP_LOGI(TAG, "  NM: " IPSTR, IP2STR(&evt->ip_info.netmask));
        ESP_LOGI(TAG, "========================================");
        
        // Store IP for status reporting
        s_current_ip = evt->ip_info.ip;
        
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi_event_group, WIFI_SOFTAP_BIT);
        s_wifi_state = WIFI_STATE_STA_CONNECTED;
        s_retry_count = 0;

        if (wifi_config_server_is_running()) {
            wifi_config_server_stop();
        }

        return;
    }
}

static void wifi_manager_init_once(void)
{
    if (s_started) {
        return;
    }
    s_started = true;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WiFi Manager Initialization Start");
    ESP_LOGI(TAG, "========================================");

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }

    s_nvs_mutex = xSemaphoreCreateMutex();
    if (!s_nvs_mutex) {
        ESP_LOGE(TAG, "Failed to create NVS mutex");
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "[OK] esp_netif_init");

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "[OK] esp_event_loop_create_default");

    s_netif_sta = esp_netif_create_default_wifi_sta();
    if (!s_netif_sta) {
        ESP_LOGE(TAG, "Failed to create STA netif");
    }
    s_netif_ap = esp_netif_create_default_wifi_ap();
    if (!s_netif_ap) {
        ESP_LOGE(TAG, "Failed to create AP netif");
    }
    ESP_LOGI(TAG, "[OK] esp_netif_create (STA + AP)");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "[OK] esp_wifi_init");

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_storage failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "[OK] esp_wifi_set_storage (RAM)");

    // Disable power-save for lower control latency (at the cost of power).
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "[OK] esp_wifi_set_ps (power save disabled)");

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP_EVENT handler: %s", esp_err_to_name(err));
        s_wifi_state = WIFI_STATE_ERROR;
        return;
    }
    ESP_LOGI(TAG, "[OK] Event handlers registered");
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WiFi Manager Initialization Complete");
    ESP_LOGI(TAG, "========================================");
}

esp_err_t wifi_manager_start(void)
{
    wifi_manager_init_once();
    if (s_wifi_state == WIFI_STATE_ERROR) {
        ESP_LOGE(TAG, "Wi-Fi manager initialization failed");
        return ESP_FAIL;
    }

    if (strlen(WIFI_HARDCODED_SSID) == 0) {
        ESP_LOGW(TAG, "No hardcoded Wi-Fi SSID configured; entering SoftAP setup mode");
        wifi_start_softap();
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Using hardcoded Wi-Fi credentials only");
    ESP_LOGW(TAG, "  SSID: '%s'", WIFI_HARDCODED_SSID);
    ESP_LOGW(TAG, "  PASS: '%s'", WIFI_HARDCODED_PASS);
    wifi_start_sta_with_credentials(WIFI_HARDCODED_SSID, WIFI_HARDCODED_PASS);
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_softap_mode(void)
{
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_SOFTAP_BIT) != 0;
}

void wifi_manager_get_ip_str(char *ip_str, size_t max_len)
{
    if (!ip_str || max_len == 0) {
        return;
    }
    
    if (s_current_ip.addr == 0) {
        snprintf(ip_str, max_len, "0.0.0.0");
    } else {
        snprintf(ip_str, max_len, IPSTR, IP2STR(&s_current_ip));
    }
}
