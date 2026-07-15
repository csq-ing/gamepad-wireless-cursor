#include "stick_processing.h"

#include <stddef.h>

#define STICK_DEFAULT_CENTER    2048
#define STICK_DEFAULT_DEADZONE  64
#define STICK_ADC_MAX           4095
#define STICK_DEADZONE_MAX      4094
#define STICK_AXIS_POS_MAX      32767
#define STICK_AXIS_NEG_MIN     -32768

static uint16_t clamp_deadzone(uint16_t deadzone)
{
    if (deadzone > STICK_DEADZONE_MAX) {
        return STICK_DEADZONE_MAX;
    }
    return deadzone;
}

static int16_t clamp_axis(int32_t value)
{
    if (value > STICK_AXIS_POS_MAX) {
        return STICK_AXIS_POS_MAX;
    }
    if (value < STICK_AXIS_NEG_MIN) {
        return STICK_AXIS_NEG_MIN;
    }
    return (int16_t)value;
}

static int16_t invert_axis(int16_t value)
{
    return clamp_axis(-(int32_t)value);
}

static int16_t process_axis(int16_t raw, int16_t center, uint16_t deadzone)
{
    int32_t delta = (int32_t)raw - center;
    int32_t effective_deadzone;
    int32_t magnitude;
    int32_t side_range;
    int32_t scaled;

    if (delta == 0) {
        return 0;
    }

    magnitude = (delta >= 0) ? delta : -delta;
    side_range = (delta >= 0) ? (STICK_ADC_MAX - center) : center;
    effective_deadzone = clamp_deadzone(deadzone);
    if (side_range <= 0) {
        return 0;
    }
    if (effective_deadzone >= side_range) {
        effective_deadzone = side_range - 1;
    }

    if (magnitude <= effective_deadzone) {
        return 0;
    }

    scaled = ((magnitude - effective_deadzone) * STICK_AXIS_POS_MAX)
           / (side_range - effective_deadzone);
    if (scaled > STICK_AXIS_POS_MAX) {
        scaled = STICK_AXIS_POS_MAX;
    }

    if (delta >= 0) {
        return (int16_t)scaled;
    }

    if (magnitude == side_range) {
        return STICK_AXIS_NEG_MIN;
    }

    return clamp_axis(-scaled);
}

void stick_processing_init(stick_processing_state_t *state)
{
    if (state == NULL) {
        return;
    }

    stick_processing_set_centers(state,
                                 STICK_DEFAULT_CENTER,
                                 STICK_DEFAULT_CENTER,
                                 STICK_DEFAULT_CENTER,
                                 STICK_DEFAULT_CENTER);
    stick_processing_set_config(state,
                                STICK_DEFAULT_DEADZONE,
                                STICK_DEFAULT_DEADZONE);
}

void stick_processing_set_config(stick_processing_state_t *state,
                                 uint16_t left_deadzone,
                                 uint16_t right_deadzone)
{
    if (state == NULL) {
        return;
    }

    state->config.left_deadzone = clamp_deadzone(left_deadzone);
    state->config.right_deadzone = clamp_deadzone(right_deadzone);
}

void stick_processing_set_centers(stick_processing_state_t *state,
                                  int16_t lx_center,
                                  int16_t ly_center,
                                  int16_t rx_center,
                                  int16_t ry_center)
{
    if (state == NULL) {
        return;
    }

    state->centers.lx = lx_center;
    state->centers.ly = ly_center;
    state->centers.rx = rx_center;
    state->centers.ry = ry_center;
}

void stick_processing_calibrate_centers(stick_processing_state_t *state,
                                        const stick_axes_t *samples,
                                        uint32_t sample_count)
{
    uint32_t i;
    int32_t sum_lx = 0;
    int32_t sum_ly = 0;
    int32_t sum_rx = 0;
    int32_t sum_ry = 0;

    if (state == NULL || samples == NULL || sample_count == 0) {
        return;
    }

    for (i = 0; i < sample_count; ++i) {
        sum_lx += samples[i].lx;
        sum_ly += samples[i].ly;
        sum_rx += samples[i].rx;
        sum_ry += samples[i].ry;
    }

    stick_processing_set_centers(state,
                                 (int16_t)(sum_lx / (int32_t)sample_count),
                                 (int16_t)(sum_ly / (int32_t)sample_count),
                                 (int16_t)(sum_rx / (int32_t)sample_count),
                                 (int16_t)(sum_ry / (int32_t)sample_count));
}

void stick_processing_apply(const stick_processing_state_t *state,
                            const stick_axes_t *raw,
                            stick_axes_t *processed)
{
    if (state == NULL || raw == NULL || processed == NULL) {
        return;
    }

    processed->lx = invert_axis(process_axis(raw->lx,
                                             state->centers.lx,
                                             state->config.left_deadzone));
    processed->ly = invert_axis(process_axis(raw->ly,
                                 state->centers.ly,
                                 state->config.left_deadzone));
    processed->rx = process_axis(raw->rx,
                                 state->centers.rx,
                                 state->config.right_deadzone);
    processed->ry = process_axis(raw->ry,
                                 state->centers.ry,
                                 state->config.right_deadzone);
}
