#pragma once

#include "esp_err.h"
#include "sdkconfig.h"
#include "gamepad_common.h"

/**
 * Callback invoked from a worker task when a valid gamepad input packet arrives.
 */
typedef void (*espnow_input_cb_t)(const gamepad_packet_t *pkt);

esp_err_t espnow_handler_init(espnow_input_cb_t input_cb);

/**
 * Send a vibration feedback packet to the controller.
 */
esp_err_t espnow_send_feedback(uint8_t motor_left, uint8_t motor_right);
esp_err_t espnow_send_stick_config(uint8_t trigger_target,
                                   uint16_t left_deadzone,
                                   uint16_t right_deadzone);
esp_err_t espnow_send_ai_pull(uint8_t pull_percent);
void espnow_request_stick_config_sync(void);

#if CONFIG_REMOTE_LOG_ENABLE
/**
 * Send a log message to the controller for remote display.
 */
esp_err_t espnow_send_log(const char *msg, size_t len);
#endif
