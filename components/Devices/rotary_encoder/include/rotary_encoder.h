#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    uint8_t last_state;
    uint8_t transitions_per_press;
    uint8_t pending_left_presses;
    uint8_t pending_right_presses;
    int16_t transition_accumulator;
} rotary_encoder_state_t;

typedef struct {
    uint8_t state;
    uint8_t pending_left_presses;
    uint8_t pending_right_presses;
    int16_t transition_accumulator;
} rotary_encoder_snapshot_t;

esp_err_t rotary_encoder_init(rotary_encoder_state_t *state,
                              gpio_num_t pin_a,
                              gpio_num_t pin_b,
                              uint8_t transitions_per_press);
uint8_t rotary_encoder_consume_dpad(rotary_encoder_state_t *state);
void rotary_encoder_get_snapshot(const rotary_encoder_state_t *state,
                                 rotary_encoder_snapshot_t *snapshot);
