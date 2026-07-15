#pragma once

#include <stdint.h>

typedef struct {
    uint8_t stable_state;
    uint8_t last_raw_state;
    uint8_t threshold;
    uint8_t counters[8];
} button_debounce_state_t;

void button_debounce_init(button_debounce_state_t *state, uint8_t threshold);
uint8_t button_debounce_update(button_debounce_state_t *state, uint8_t raw_state);
