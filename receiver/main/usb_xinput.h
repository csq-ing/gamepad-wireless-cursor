#pragma once

#include <stdint.h>
#include "device/usbd_pvt.h"
#include "gamepad_common.h"

#define USB_XINPUT_ITF_INDEX 0U
#define USB_XINPUT_DESC_LEN 40U

void usb_xinput_copy_interface_descriptor(uint8_t *dst, uint8_t poll_interval_ms);
const usbd_class_driver_t *usb_xinput_get_class_driver(void);

void usb_xinput_set_output_cb(void (*cb)(uint8_t motor_left, uint8_t motor_right));
void usb_xinput_send_report(const gamepad_packet_t *pkt);
