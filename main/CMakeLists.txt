idf_component_register(SRCS "main.c" "touch_telemetry.c" "scale.c"
                            "ui/ui.c" "ui/tft.c" "ui/presets.c"
                            "ui/ui_ribbon.c" "ui/ui_cards.c" "ui/ui_main.c"
                            "ui/ui_presets.c" "ui/ui_calib.c" "ui/ui_legacy.c"
                            "ui/ui_home.c" "ui/ui_anim.c" "ui/ui_params.c" "ui/ui_settings.c"
                       INCLUDE_DIRS "."
                       REQUIRES freertos nvs_flash amy_engine touch_control driver esp_timer looper esp_adc)
