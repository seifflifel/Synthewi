idf_component_register(SRCS "main.c" "touch_telemetry.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver freertos esp_timer nvs_flash amy_engine touch_control)
