#include "mpu_motion.h"

#include <limits.h>
#include <stddef.h>

static void mpu_motion_center_outputs(int16_t *out_rx, int16_t *out_ry)
{
    if (out_rx != NULL) {
        *out_rx = 0;
    }

    if (out_ry != NULL) {
        *out_ry = 0;
    }
}

static float mpu_motion_normalize_degrees(float value)
{
    while (value > 180.0f) {
        value -= 360.0f;
    }

    while (value < -180.0f) {
        value += 360.0f;
    }

    return value;
}

static float mpu_motion_apply_deadzone(float value, float deadzone)
{
    if (deadzone < 0.0f) {
        deadzone = 0.0f;
    }

    if (value >= 0.0f) {
        return (value <= deadzone) ? 0.0f : (value - deadzone);
    }

    return ((-value) <= deadzone) ? 0.0f : (value + deadzone);
}

static int16_t mpu_motion_clamp_axis_from_float(float value)
{
    if (value > (float)INT16_MAX) {
        return INT16_MAX;
    }

    if (value < (float)INT16_MIN) {
        return INT16_MIN;
    }

    return (int16_t)((value >= 0.0f) ? (value + 0.5f) : (value - 0.5f));
}

mpu_motion_view_config_t mpu_motion_default_view_config(void)
{
    const mpu_motion_view_config_t config = {
        .center_yaw_deg = 0.0f,
        .center_roll_deg = 0.0f,
        .deadzone_deg = 1.5f,
        .axis_scale = 32767.0f / 20.0f,
    };

    return config;
}

void mpu_motion_map_angles_to_view(const mpu_motion_view_config_t *config,
                                   const mpu_motion_angles_t *angles,
                                   int16_t *out_rx,
                                   int16_t *out_ry)
{
    mpu_motion_view_config_t effective_config = mpu_motion_default_view_config();
    float yaw_delta = 0.0f;
    float roll_delta = 0.0f;
    float mapped_rx = 0.0f;
    float mapped_ry = 0.0f;

    if (config != NULL) {
        effective_config = *config;
    }

    if (angles == NULL || effective_config.axis_scale <= 0.0f) {
        mpu_motion_center_outputs(out_rx, out_ry);
        return;
    }

    yaw_delta = mpu_motion_normalize_degrees(angles->yaw_deg -
                                             effective_config.center_yaw_deg);
    roll_delta = mpu_motion_normalize_degrees(angles->roll_deg -
                                              effective_config.center_roll_deg);

    yaw_delta = mpu_motion_apply_deadzone(yaw_delta, effective_config.deadzone_deg);
    roll_delta = mpu_motion_apply_deadzone(roll_delta, effective_config.deadzone_deg);

    mapped_rx = yaw_delta * effective_config.axis_scale;
    mapped_ry = roll_delta * effective_config.axis_scale;

    if (out_rx != NULL) {
        *out_rx = mpu_motion_clamp_axis_from_float(mapped_rx);
    }

    if (out_ry != NULL) {
        *out_ry = mpu_motion_clamp_axis_from_float(mapped_ry);
    }

}

uint8_t mpu_motion_map_pitch_to_alt_trigger(float center_pitch_deg,
                                            float current_pitch_deg,
                                            float deadzone_deg)
{
    float pitch_delta = 0.0f;

    if (deadzone_deg < 0.0f) {
        deadzone_deg = 0.0f;
    }

    pitch_delta = mpu_motion_normalize_degrees(current_pitch_deg - center_pitch_deg);
    return (pitch_delta > deadzone_deg) ? UINT8_MAX : 0U;
}

uint8_t mpu_motion_update_alt_trigger_from_pitch(mpu_motion_state_t *state,
                                                 float current_pitch_deg,
                                                 float deadzone_deg)
{
    if (state == NULL) {
        return 0U;
    }

    if (state->alt_trigger_center_pending) {
        state->alt_trigger_center_pitch_deg = current_pitch_deg;
        state->alt_trigger_center_pending = false;
        return 0U;
    }

    return mpu_motion_map_pitch_to_alt_trigger(state->alt_trigger_center_pitch_deg,
                                               current_pitch_deg,
                                               deadzone_deg);
}

void mpu_motion_reset_view_filter(mpu_motion_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->smoothed_rx = 0;
    state->smoothed_ry = 0;
    state->smoothing_active = false;
    state->view_center_locked = true;
    state->view_unlock_sample_count = 0;
}

void mpu_motion_reset_alt_trigger_center(mpu_motion_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->alt_trigger_center_pitch_deg = 0.0f;
    state->alt_trigger_center_pending = true;
}

#define IMU_VIEW_SMOOTHING_SHIFT 2

void mpu_motion_filter_view_output(mpu_motion_state_t *state,
                                   int16_t mapped_rx,
                                   int16_t mapped_ry,
                                   int16_t *out_rx,
                                   int16_t *out_ry)
{
    if (state == NULL) {
        mpu_motion_center_outputs(out_rx, out_ry);
        return;
    }

    if (mapped_rx == 0 && mapped_ry == 0) {
        mpu_motion_reset_view_filter(state);
        mpu_motion_center_outputs(out_rx, out_ry);
        return;
    }

    state->view_center_locked = false;
    if (!state->smoothing_active) {
        state->smoothed_rx = mapped_rx;
        state->smoothed_ry = mapped_ry;
        state->smoothing_active = true;
    } else {
        state->smoothed_rx += (int16_t)((mapped_rx - state->smoothed_rx) >>
                                        IMU_VIEW_SMOOTHING_SHIFT);
        state->smoothed_ry += (int16_t)((mapped_ry - state->smoothed_ry) >>
                                        IMU_VIEW_SMOOTHING_SHIFT);
    }

    if (out_rx != NULL) {
        *out_rx = state->smoothed_rx;
    }

    if (out_ry != NULL) {
        *out_ry = state->smoothed_ry;
    }
}
