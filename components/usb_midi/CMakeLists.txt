idf_component_register(SRCS "usb_midi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver freertos espressif__esp_tinyusb)
