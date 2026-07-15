#pragma once

#include <stdint.h>

typedef struct {
    uint16_t left_deadzone;
    uint16_t right_deadzone;
} stick_runtime_config_t;

typedef struct {
    int16_t lx;
    int16_t ly;
    int16_t rx;
    int16_t ry;
} stick_axes_t;

typedef struct {
    stick_axes_t centers;
    stick_runtime_config_t config;
} stick_processing_state_t;

void stick_processing_init(stick_processing_state_t *state);
void stick_processing_set_config(stick_processing_state_t *state,
                                 uint16_t left_deadzone,
                                 uint16_t right_deadzone);
void stick_processing_set_centers(stick_processing_state_t *state,
                                  int16_t lx_center,
                                  int16_t ly_center,
                                  int16_t rx_center,
                                  int16_t ry_center);
void stick_processing_calibrate_centers(stick_processing_state_t *state,
                                        const stick_axes_t *samples,
                                        uint32_t sample_count);
void stick_processing_apply(const stick_processing_state_t *state,
                            const stick_axes_t *raw,
                            stick_axes_t *processed);
