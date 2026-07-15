#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

/* Button and stick pins */
#define PIN_BTN_B          GPIO_NUM_35
#define PIN_BTN_Y          GPIO_NUM_36
#define PIN_BTN_A          GPIO_NUM_37
#define PIN_BTN_X          GPIO_NUM_38
#define PIN_TRIGGER_MODE   GPIO_NUM_41
#define PIN_RT_BUTTON      GPIO_NUM_39

/* Rotary encoder pins */
#define PIN_ROTARY_ENCODER_A  GPIO_NUM_47
#define PIN_ROTARY_ENCODER_B  GPIO_NUM_48

/* ADC stick channels */
#define ADC_STICK_LX       ADC_CHANNEL_3
#define ADC_STICK_LY       ADC_CHANNEL_4

/* BMI088 SPI pins */
#define BMI088_MOSI_GPIO   GPIO_NUM_12
#define BMI088_MISO_GPIO   GPIO_NUM_13
#define BMI088_SCLK_GPIO   GPIO_NUM_11
#define BMI088_CS_ACC_GPIO GPIO_NUM_14
#define BMI088_CS_GYR_GPIO GPIO_NUM_15

/* ZE300 UART pins */
#define ZE300_TX_GPIO      GPIO_NUM_6
#define ZE300_RX_GPIO      GPIO_NUM_7
