#pragma once

#include "bsp_config.h"
#include "esp_err.h"
#include "gamepad_common.h"

/**
 * Callback fired every polling cycle (~100 Hz) when input state changes.
 */
typedef void (*input_change_cb_t)(const gamepad_packet_t *pkt);

esp_err_t input_handler_init(input_change_cb_t cb);
void input_handler_set_stick_config(uint8_t trigger_target,
                                    uint16_t left_deadzone,
                                    uint16_t right_deadzone);
