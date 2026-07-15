#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t hold_threshold_samples;
    uint16_t pressed_samples;
    bool view_active;
    bool hold_triggered;
} trigger_mode_state_t;

void trigger_mode_init(trigger_mode_state_t *state, uint16_t hold_threshold_samples);
bool trigger_mode_update(trigger_mode_state_t *state, bool button_pressed);
bool trigger_mode_view_active(const trigger_mode_state_t *state);

uint8_t trigger_input_map_adc_to_u8(int raw);
uint8_t trigger_input_map_button_to_u8(bool button_pressed);

void trigger_input_select_sources(uint8_t pot_lt,
                                  uint8_t pot_rt,
                                  uint8_t accel_trigger,
                                  bool accel_mode_active,
                                  uint8_t accel_trigger_target,
                                  uint8_t base_buttons,
                                  uint8_t *out_buttons,
                                  uint8_t *out_lt,
                                  uint8_t *out_rt);
