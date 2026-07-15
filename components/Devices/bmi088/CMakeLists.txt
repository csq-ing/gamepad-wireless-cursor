idf_component_register(SRCS "bmi088.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES log esp_driver_spi esp_driver_gpio
)
