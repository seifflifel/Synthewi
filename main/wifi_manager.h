#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_STATE_IDLE = 0,           // Not started
    WIFI_STATE_STA_CONNECTING = 1, // Attempting STA connection
    WIFI_STATE_STA_CONNECTED = 2,  // STA connected, IP acquired
    WIFI_STATE_AP_ACTIVE = 3,      // SoftAP active, config portal running
    WIFI_STATE_ERROR = 4,          // Connection failed, retries exhausted
} wifi_state_t;

typedef struct {
    wifi_state_t state;
    bool ip_acquired;
    uint32_t retry_count;
    uint32_t last_connect_attempt_ms;
} wifi_status_t;

// Starts Wi-Fi in Station mode using saved credentials.
// If no saved credentials exist, or connection fails after retries,
// starts SoftAP mode and hosts a simple config portal (http://192.168.4.1).
esp_err_t wifi_manager_start(void);

// Query current Wi-Fi state.
wifi_status_t wifi_manager_get_status(void);

bool wifi_manager_is_connected(void);
bool wifi_manager_is_softap_mode(void);

// Get current IP address as a string (e.g., "192.168.1.100")
void wifi_manager_get_ip_str(char *ip_str, size_t max_len);

// Called by the SoftAP config portal to persist credentials in NVS.
// SSID max 32 bytes, password max 64 bytes. Returns error if invalid.
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass);

// Clear saved credentials and force SoftAP setup mode on next boot.
esp_err_t wifi_manager_clear_credentials(void);
