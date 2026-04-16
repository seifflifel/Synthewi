idf_component_register(SRCS "touch_control.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_touch_sens esp_timer)
