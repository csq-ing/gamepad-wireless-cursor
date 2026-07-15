idf_component_register(
    SRCS "ze300.c" "Motor_DataProcess.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_uart Bsp
)
