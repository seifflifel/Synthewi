idf_component_register(SRCS "main.c"
                           "wifi_manager.c"
                           "wifi_config_server.c"
                           "touch_telemetry.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver freertos amy_engine touch_control espressif__usb_device_uac esp_event esp_http_server esp_netif esp_wifi lwip nvs_flash)
