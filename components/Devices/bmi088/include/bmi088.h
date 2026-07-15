/**
 * @file bmi088.h
 * @brief BMI088 6-axis IMU driver for ESP-IDF (SPI interface)
 *
 * BMI088 contains two separate chips:
 *   - Accelerometer (chip ID 0x1E)
 *   - Gyroscope     (chip ID 0x0F)
 *
 * Each chip has its own chip-select (CS) pin.
 * SPI mode 3 (CPOL=1, CPHA=1), MSB first.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "bmi088_reg.h"

#ifdef __cplusplus
extern "C" {
#endif

/** BMI088 IMU data structure (SI units) */
typedef struct {
    float accel[3];        /**< Acceleration: X, Y, Z (m/s²) */
    float gyro[3];         /**< Angular rate: X, Y, Z (rad/s) */
    float temperature;     /**< Die temperature (°C) */
} bmi088_data_t;

/** BMI088 SPI pin configuration */
typedef struct {
    int spi_mosi_pin;      /**< SPI MOSI (SDI) GPIO number */
    int spi_miso_pin;      /**< SPI MISO (SDO) GPIO number */
    int spi_sclk_pin;      /**< SPI SCK GPIO number */
    int cs_accel_pin;      /**< Chip-select for accelerometer (active low) */
    int cs_gyro_pin;       /**< Chip-select for gyroscope (active low) */
} bmi088_spi_pins_t;

/** BMI088 configuration */
typedef struct {
    bmi088_spi_pins_t pins;         /**< SPI pin assignments */
    uint32_t spi_clock_hz;          /**< SPI clock frequency (Hz), default 1 MHz */
    uint8_t accel_range;            /**< Accelerometer range (e.g. BMI088_ACC_RANGE_6G) */
    uint8_t accel_odr;              /**< Accelerometer ODR + filter (e.g. BMI088_ACC_NORMAL | BMI088_ACC_800_HZ) */
    uint8_t gyro_range;             /**< Gyroscope range (e.g. BMI088_GYRO_2000) */
    uint8_t gyro_bandwidth;         /**< Gyroscope bandwidth (e.g. BMI088_GYRO_2000_230_HZ) */
} bmi088_config_t;

/** Default BMI088 configuration */
#define BMI088_CONFIG_DEFAULT()                                             \
    {                                                                       \
        .pins          = { .spi_mosi_pin = -1, .spi_miso_pin = -1,          \
                           .spi_sclk_pin = -1, .cs_accel_pin = -1,          \
                           .cs_gyro_pin  = -1 },                            \
        .spi_clock_hz  = 1 * 1000 * 1000,                                   \
        .accel_range   = BMI088_ACC_RANGE_6G,                               \
        .accel_odr     = BMI088_ACC_NORMAL | BMI088_ACC_800_HZ              \
                         | BMI088_ACC_CONF_MUST_SET,                        \
        .gyro_range    = BMI088_GYRO_2000,                                  \
        .gyro_bandwidth = BMI088_GYRO_2000_230_HZ                            \
                          | BMI088_GYRO_BANDWIDTH_MUST_SET,                 \
    }

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

esp_err_t bmi088_init(const bmi088_config_t *config);
esp_err_t bmi088_deinit(void);
esp_err_t bmi088_read_all(bmi088_data_t *out);
esp_err_t bmi088_read_accel(float out[3]);
esp_err_t bmi088_read_gyro(float out[3]);
esp_err_t bmi088_read_temperature(float *out);
bool bmi088_self_test(void);
esp_err_t bmi088_calibrate_gyro(float offset[3], uint32_t n);

extern const float bmi088_accel_sensitivity[4];
extern const float bmi088_gyro_sensitivity[5];

#ifdef __cplusplus
}
#endif
