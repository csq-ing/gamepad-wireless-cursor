/**
 * bmi088_motion.c - BMI088-based motion sensing
 *
 * Uses the BMI088 6-axis IMU via SPI to compute attitude angles
 * with a complementary filter. Provides the motion API consumed by
 * input_handler.c.
 */

#include "mpu_motion.h"

#include <math.h>
#include <string.h>

#include "bmi088.h"
#include "bsp_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_handler.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- BMI088 SPI pin assignment (ESP32-S3) ---- */
#define BMI088_MOSI_PIN    BMI088_MOSI_GPIO
#define BMI088_MISO_PIN    BMI088_MISO_GPIO
#define BMI088_SCLK_PIN    BMI088_SCLK_GPIO
#define BMI088_CS_ACC_PIN  BMI088_CS_ACC_GPIO
#define BMI088_CS_GYR_PIN  BMI088_CS_GYR_GPIO
#define BMI088_SPI_CLK_HZ  1000000    /* 1 MHz for bring-up */

/* ---- Complementary filter ---- */
#define CF_ALPHA            0.98f     /* gyro trust (high-pass) */
#define CF_DT               0.01f     /* 100 Hz poll rate (10 ms) */

/* ---- Deadzone / smoothing ---- */
#define BMI088_ALT_TRIGGER_PITCH_DEADZONE  5.0f   /* degrees */
#define BMI088_GYRO_CALIBRATION_SAMPLES    200    /* ~2 s at 100 Hz */
#define BMI088_ACCEL_SANITY_SAMPLES        8
#define BMI088_ACCEL_MIN_GRAVITY_MPS2      2.0f

static const char *TAG = "bmi088_motion";

/* ---- internal state ---- */
static mpu_motion_state_t *s_state;
static bool s_ready;

/* Complementary-filter internal angles (radians, then converted) */
static float s_cf_roll_deg;
static float s_cf_pitch_deg;
static float s_cf_yaw_deg;
static int64_t s_last_poll_us;

/* Gyro bias (rad/s), subtracted from raw readings */
static float s_gyro_bias[3];

/* ---- helper: build bmi088_config_t from our pin macros ---- */
static bmi088_config_t bmi088_make_config(void)
{
    bmi088_config_t cfg = BMI088_CONFIG_DEFAULT();

    cfg.pins.spi_mosi_pin = BMI088_MOSI_PIN;
    cfg.pins.spi_miso_pin = BMI088_MISO_PIN;
    cfg.pins.spi_sclk_pin = BMI088_SCLK_PIN;
    cfg.pins.cs_accel_pin = BMI088_CS_ACC_PIN;
    cfg.pins.cs_gyro_pin  = BMI088_CS_GYR_PIN;
    cfg.spi_clock_hz      = BMI088_SPI_CLK_HZ;
    cfg.accel_range       = BMI088_ACC_RANGE_6G;
    cfg.accel_odr         = BMI088_ACC_NORMAL | BMI088_ACC_100_HZ
                            | BMI088_ACC_CONF_MUST_SET;
    cfg.gyro_range        = BMI088_GYRO_2000;
    cfg.gyro_bandwidth    = BMI088_GYRO_2000_230_HZ
                            | BMI088_GYRO_BANDWIDTH_MUST_SET;
    return cfg;
}

/* ---- complementary filter: compute roll & pitch from accel (degrees) ---- */
static void accel_to_rp(const float accel[3], float *roll_deg, float *pitch_deg)
{
    /* accel in m/s²  →  normalized gravity vector */
    const float ax = accel[0];
    const float ay = accel[1];
    const float az = accel[2];

    *roll_deg  = atan2f(ay, az) * 180.0f / (float)M_PI;
    *pitch_deg = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / (float)M_PI;
}

/* ---- complementary filter update (called at ~ poll rate) ---- */
static void cf_update(const float gyro[3], const float accel[3], float dt)
{
    float acc_roll_deg, acc_pitch_deg;

    /* integrate gyro (rad) → (deg) */
    s_cf_roll_deg  += (gyro[0] - s_gyro_bias[0]) * dt * 180.0f / (float)M_PI;
    s_cf_pitch_deg += (gyro[1] - s_gyro_bias[1]) * dt * 180.0f / (float)M_PI;
    s_cf_yaw_deg   += (gyro[2] - s_gyro_bias[2]) * dt * 180.0f / (float)M_PI;

    /* accelerometer correction for roll & pitch only (yaw not observable) */
    accel_to_rp(accel, &acc_roll_deg, &acc_pitch_deg);

    s_cf_roll_deg  = CF_ALPHA * s_cf_roll_deg  + (1.0f - CF_ALPHA) * acc_roll_deg;
    s_cf_pitch_deg = CF_ALPHA * s_cf_pitch_deg + (1.0f - CF_ALPHA) * acc_pitch_deg;
}

/* ---- prepare state ---- */
static void prepare_state(mpu_motion_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->view_config = mpu_motion_default_view_config();
    state->view_center_locked = true;
    state->view_center_pending = true;
    mpu_motion_reset_alt_trigger_center(state);

    memset(s_gyro_bias, 0, sizeof(s_gyro_bias));
    s_cf_roll_deg  = 0.0f;
    s_cf_pitch_deg = 0.0f;
    s_cf_yaw_deg   = 0.0f;
    s_last_poll_us = 0;
}

static void output_center(mpu_motion_state_t *state, int16_t *rx, int16_t *ry)
{
    if (state && state->smoothing_active) {
        if (rx) *rx = state->smoothed_rx;
        if (ry) *ry = state->smoothed_ry;
        return;
    }
    if (rx) *rx = 0;
    if (ry) *ry = 0;
}

static void log_accel_sanity_once(void)
{
    float max_norm = 0.0f;
    float last_accel[3] = {0.0f, 0.0f, 0.0f};

    for (int i = 0; i < BMI088_ACCEL_SANITY_SAMPLES; i++) {
        if (bmi088_read_accel(last_accel) == ESP_OK) {
            float norm = sqrtf(last_accel[0] * last_accel[0] +
                               last_accel[1] * last_accel[1] +
                               last_accel[2] * last_accel[2]);
            if (norm > max_norm) {
                max_norm = norm;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (max_norm < BMI088_ACCEL_MIN_GRAVITY_MPS2) {
        ESP_LOGW(TAG, "BMI088 accel data abnormal: accel=(%.2f %.2f %.2f), |a|max=%.2f m/s2",
                 (double)last_accel[0],
                 (double)last_accel[1],
                 (double)last_accel[2],
                 (double)max_norm);
    }
}

/* ==================================================================
 *  Public API
 * ================================================================ */

esp_err_t mpu_motion_init(mpu_motion_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;

    bmi088_config_t cfg = bmi088_make_config();
    esp_err_t err = bmi088_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BMI088 init failed: %s", esp_err_to_name(err));
        return err;
    }

    prepare_state(state);
    s_state  = state;
    s_ready  = true;
    log_accel_sanity_once();

    return ESP_OK;
}

esp_err_t mpu_motion_calibrate_gyro_bias(mpu_motion_state_t *state,
                                          uint16_t sample_count)
{
    if (!state || !s_ready) return ESP_ERR_INVALID_STATE;

    const uint16_t n = (sample_count > 0) ? sample_count
                                           : BMI088_GYRO_CALIBRATION_SAMPLES;
    float sum[3] = {0};

    for (uint16_t i = 0; i < n; i++) {
        float gyro[3];
        if (bmi088_read_gyro(gyro) == ESP_OK) {
            sum[0] += gyro[0];
            sum[1] += gyro[1];
            sum[2] += gyro[2];
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_gyro_bias[0] = sum[0] / (float)n;
    s_gyro_bias[1] = sum[1] / (float)n;
    s_gyro_bias[2] = sum[2] / (float)n;

    state->view_center_pending = true;
    return ESP_OK;
}

bool mpu_motion_poll(mpu_motion_state_t *state)
{
    if (!state || !s_ready) return false;

    bmi088_data_t imu;
    if (bmi088_read_all(&imu) != ESP_OK) {
        state->latest_angles_valid = false;
        return false;
    }

    int64_t now = esp_timer_get_time();
    float dt = (s_last_poll_us > 0)
                   ? (float)(now - s_last_poll_us) / 1e6f
                   : CF_DT;
    if (dt <= 0.0f || dt > 0.5f) dt = CF_DT;  /* clamp absurd intervals */
    s_last_poll_us = now;

    cf_update(imu.gyro, imu.accel, dt);

    state->latest_angles.yaw_deg   = s_cf_yaw_deg;
    state->latest_angles.pitch_deg = s_cf_pitch_deg;
    state->latest_angles.roll_deg  = s_cf_roll_deg;
    state->latest_angles_valid     = true;

    return true;
}

bool mpu_motion_read_view(mpu_motion_state_t *state,
                           int16_t *out_rx, int16_t *out_ry)
{
    int16_t mapped_rx = 0, mapped_ry = 0;

    if (!state || !s_ready) {
        output_center(state, out_rx, out_ry);
        return false;
    }

    if (!state->latest_angles_valid && !mpu_motion_poll(state)) {
        output_center(state, out_rx, out_ry);
        return false;
    }

    if (state->view_center_pending) {
        state->view_config.center_yaw_deg  = state->latest_angles.yaw_deg;
        state->view_config.center_roll_deg = state->latest_angles.roll_deg;
        state->view_center_pending = false;
        mpu_motion_reset_view_filter(state);
        output_center(state, out_rx, out_ry);
        return true;
    }

    mpu_motion_map_angles_to_view(&state->view_config,
                                  &state->latest_angles,
                                  &mapped_rx, &mapped_ry);
    mpu_motion_filter_view_output(state, mapped_rx, mapped_ry, out_rx, out_ry);
    return true;
}

void mpu_motion_reset_view_smoothing(mpu_motion_state_t *state)
{
    if (!state) return;
    mpu_motion_reset_view_filter(state);
    state->view_center_pending = true;
}

uint8_t mpu_motion_read_alt_trigger(mpu_motion_state_t *state)
{
    if (!state || !s_ready) return 0;
    if (!state->latest_angles_valid && !mpu_motion_poll(state)) return 0;

    return mpu_motion_update_alt_trigger_from_pitch(
        state,
        state->latest_angles.pitch_deg,
        BMI088_ALT_TRIGGER_PITCH_DEADZONE);
}
