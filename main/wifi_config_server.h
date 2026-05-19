#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_config_server_start(void);
void wifi_config_server_stop(void);
bool wifi_config_server_is_running(void);
