#include "trigger_input.h"

#include "gamepad_common.h"

#define TRIGGER_ADC_MAX 4095

void trigger_mode_init(trigger_mode_state_t *state, uint16_t hold_threshold_samples)
{
    if (state == NULL) {
        return;
    }

    state->hold_threshold_samples = (hold_threshold_samples == 0U) ? 1U : hold_threshold_samples;
    state->pressed_samples = 0U;
    state->view_active = false;
    state->hold_triggered = false;
}

bool trigger_mode_update(trigger_mode_state_t *state, bool button_pressed)
{
    if (state == NULL) {
        return false;
    }

    if (!button_pressed) {
        if (state->pressed_samples > 0U && !state->hold_triggered) {
            state->view_active = !state->view_active;
        }
        state->pressed_samples = 0U;
        state->hold_triggered = false;
        return false;
    }

    if (state->pressed_samples < state->hold_threshold_samples) {
        state->pressed_samples++;
    }

    if (state->pressed_samples >= state->hold_threshold_samples) {
        state->view_active = false;
        state->hold_triggered = true;
        return true;
    }

    return false;
}

bool trigger_mode_view_active(const trigger_mode_state_t *state)
{
    return state != NULL && state->view_active;
}

uint8_t trigger_input_map_adc_to_u8(int raw)
{
    if (raw <= 0) {
        return 0;
    }

    if (raw >= TRIGGER_ADC_MAX) {
        return 255;
    }

    return (uint8_t)((raw * 255) / TRIGGER_ADC_MAX);
}

uint8_t trigger_input_map_button_to_u8(bool button_pressed)
{
    return button_pressed ? 255U : 0U;
}

void trigger_input_select_sources(uint8_t pot_lt,
                                  uint8_t pot_rt,
                                  uint8_t accel_trigger,
                                  bool accel_mode_active,
                                  uint8_t accel_trigger_target,
                                  uint8_t base_buttons,
                                  uint8_t *out_buttons,
                                  uint8_t *out_lt,
                                  uint8_t *out_rt)
{
    if (out_buttons != NULL) {
        *out_buttons = base_buttons;
    }

    if (out_lt != NULL) {
        *out_lt = (accel_mode_active && accel_trigger_target == GAMEPAD_TRIGGER_TARGET_LT) ?
                  accel_trigger : (accel_mode_active ? 0U : pot_lt);
    }

    if (out_rt != NULL) {
        *out_rt = (accel_mode_active && accel_trigger_target == GAMEPAD_TRIGGER_TARGET_RT) ?
                  accel_trigger : (accel_mode_active ? 0U : pot_rt);
    }
}
