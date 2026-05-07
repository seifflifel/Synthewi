idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver freertos amy_engine espressif__usb_device_uac)
