idf_component_register(
    SRCS "key_input.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_adc gamepad_common Bsp
)
