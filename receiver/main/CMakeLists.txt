idf_component_register(
    SRCS
        "main.c"
        "usb_gamepad.c"
        "usb_xinput.c"
        "usb_config_hid.c"
        "receiver_config.c"
        "receiver_config_store.c"
        "receiver_status.c"
        "stick_config_sync.c"
        "espnow_handler.c"
        "remote_log.c"
    INCLUDE_DIRS "."
    REQUIRES esp_tinyusb esp_wifi nvs_flash gamepad_common
)
