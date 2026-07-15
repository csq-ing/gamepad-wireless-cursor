#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float yaw_deg;
    float pitch_deg;
    float roll_deg;
} mpu_motion_angles_t;

typedef struct {
    float center_yaw_deg;
    float center_roll_deg;
    float deadzone_deg;
    float axis_scale;
} mpu_motion_view_config_t;

typedef struct {
    mpu_motion_view_config_t view_config;
    int16_t smoothed_rx;
    int16_t smoothed_ry;
    bool smoothing_active;
    bool view_center_locked;
    bool view_center_pending;
    uint8_t view_unlock_sample_count;
    float alt_trigger_center_pitch_deg;
    bool alt_trigger_center_pending;
    mpu_motion_angles_t latest_angles;
    bool latest_angles_valid;
} mpu_motion_state_t;

mpu_motion_view_config_t mpu_motion_default_view_config(void);
void mpu_motion_map_angles_to_view(const mpu_motion_view_config_t *config,
                                   const mpu_motion_angles_t *angles,
                                   int16_t *out_rx,
                                   int16_t *out_ry);
uint8_t mpu_motion_map_pitch_to_alt_trigger(float center_pitch_deg,
                                            float current_pitch_deg,
                                            float deadzone_deg);
uint8_t mpu_motion_update_alt_trigger_from_pitch(mpu_motion_state_t *state,
                                                 float current_pitch_deg,
                                                 float deadzone_deg);

void mpu_motion_reset_view_filter(mpu_motion_state_t *state);
void mpu_motion_filter_view_output(mpu_motion_state_t *state,
                                   int16_t mapped_rx,
                                   int16_t mapped_ry,
                                   int16_t *out_rx,
                                   int16_t *out_ry);
void mpu_motion_reset_alt_trigger_center(mpu_motion_state_t *state);

#ifndef HOST_TEST
#include "esp_err.h"

esp_err_t mpu_motion_init(mpu_motion_state_t *state);
esp_err_t mpu_motion_calibrate_gyro_bias(mpu_motion_state_t *state, uint16_t sample_count);
bool mpu_motion_poll(mpu_motion_state_t *state);
bool mpu_motion_read_view(mpu_motion_state_t *state, int16_t *out_rx, int16_t *out_ry);
void mpu_motion_reset_view_smoothing(mpu_motion_state_t *state);
uint8_t mpu_motion_read_alt_trigger(mpu_motion_state_t *state);
#endif

#ifdef __cplusplus
}
#endif
