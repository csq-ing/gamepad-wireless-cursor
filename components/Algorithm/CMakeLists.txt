idf_component_register(
    SRCS "button_debounce.c"
         "mpu_motion.c"
         "stick_processing.c"
         "trigger_input.c"
    INCLUDE_DIRS "include"
    REQUIRES gamepad_common
)
