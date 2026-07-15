idf_component_register(
    SRCS "rotary_encoder.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio gamepad_common
)
