#pragma once

#include "esp_err.h"
#include "gamepad_common.h"

/**
 * Callback invoked when a vibration feedback packet arrives from receiver.
 */
typedef void (*espnow_feedback_cb_t)(uint8_t motor_left, uint8_t motor_right);
typedef void (*espnow_stick_config_cb_t)(uint8_t trigger_target,
                                         uint16_t left_deadzone,
                                         uint16_t right_deadzone);
typedef void (*espnow_ai_pull_cb_t)(uint8_t pull_percent);

esp_err_t espnow_controller_init(espnow_feedback_cb_t fb_cb,
                                 espnow_stick_config_cb_t stick_config_cb,
                                 espnow_ai_pull_cb_t ai_pull_cb);

/**
 * Send a gamepad input packet to the receiver.
 */
esp_err_t espnow_send_input(const gamepad_packet_t *pkt);
