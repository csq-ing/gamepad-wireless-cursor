#pragma once

#include "gamepad_common.h"
#include "esp_err.h"

esp_err_t usb_gamepad_init(void);

/**
 * Send an input report built from the latest controller packet.
 * Safe to call from any task; internally serialises access.
 */
void usb_gamepad_send_report(const gamepad_packet_t *pkt);

/**
 * Callback type invoked when the host sends an Output Report (vibration).
 * Registered via usb_gamepad_set_output_cb() before init.
 */
typedef void (*usb_output_cb_t)(uint8_t motor_left, uint8_t motor_right);
void usb_gamepad_set_output_cb(usb_output_cb_t cb);
