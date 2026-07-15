#pragma once

#include <stdint.h>

#define USB_CONFIG_HID_ITF_INDEX 1U
#define USB_CONFIG_HID_DESC_LEN 32U

void usb_config_hid_copy_interface_descriptor(uint8_t *dst, uint8_t poll_interval_ms);

