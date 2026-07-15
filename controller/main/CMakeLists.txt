idf_component_register(
    SRCS "main.c" "input_handler.c" "motor_control.c"
         "modules/espnow_handler.c" "modules/bmi088_motion.c"
    INCLUDE_DIRS "." "modules"
    REQUIRES driver gamepad_common bmi088 ze300 Algorithm Bsp key_input rotary_encoder
    PRIV_REQUIRES esp_adc
)
