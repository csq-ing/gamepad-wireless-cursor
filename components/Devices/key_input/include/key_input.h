#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t buttons;
    bool rt_button_pressed;
    bool trigger_mode_button_pressed;
    int16_t lx;
    int16_t ly;
} key_input_state_t;

esp_err_t key_input_init(void);
esp_err_t key_input_read_state(key_input_state_t *out_state);
