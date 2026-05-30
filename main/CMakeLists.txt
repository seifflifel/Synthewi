idf_component_register(SRCS "main.c" "touch_telemetry.c"
                       INCLUDE_DIRS "."
                       REQUIRES freertos nvs_flash amy_engine touch_control driver esp_timer)
