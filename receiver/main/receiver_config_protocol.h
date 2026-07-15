#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RECEIVER_CONFIG_PROTOCOL_VERSION 1U
#define RECEIVER_CONFIG_REPORT_SIZE 64U
#define RECEIVER_CONFIG_DEADZONE_MAX 1024U

typedef enum {
    RECEIVER_CFG_CMD_GET_STATUS = 0x01,
    RECEIVER_CFG_CMD_GET_CONFIG = 0x02,
    RECEIVER_CFG_CMD_SET_CONFIG = 0x03,
    RECEIVER_CFG_CMD_SET_TRIGGER_TARGET = RECEIVER_CFG_CMD_SET_CONFIG,
    RECEIVER_CFG_CMD_SAVE_CONFIG = 0x04,
    RECEIVER_CFG_CMD_GET_FULL_CONFIG = 0x05,
    RECEIVER_CFG_CMD_SET_FULL_CONFIG = 0x06,
    RECEIVER_CFG_CMD_SAVE_FULL_CONFIG = 0x07,
    RECEIVER_CFG_CMD_SET_AI_PULL = 0x08,
} receiver_cfg_cmd_t;

typedef enum {
    RECEIVER_CFG_STATUS_OK = 0x00,
    RECEIVER_CFG_STATUS_INVALID_CMD = 0x01,
    RECEIVER_CFG_STATUS_INVALID_ARG = 0x02,
    RECEIVER_CFG_STATUS_INTERNAL_ERR = 0x03,
} receiver_cfg_status_t;

typedef enum {
    RECEIVER_TRIGGER_TARGET_LT = 1,
    RECEIVER_TRIGGER_TARGET_RT = 2,
} receiver_trigger_target_t;

typedef struct {
    uint8_t protocol_version;
    bool usb_online;
    bool controller_connected;
    uint32_t last_input_age_ms;
    uint16_t downward_accel;
    bool trigger_active;
    receiver_trigger_target_t trigger_target;
} receiver_status_snapshot_t;

typedef struct __attribute__((packed)) {
    uint8_t trigger_target;
    uint16_t left_deadzone;
    uint16_t right_deadzone;
} receiver_device_config_t;
