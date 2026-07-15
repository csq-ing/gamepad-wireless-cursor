#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool has_sent_config;
    uint8_t sent_trigger_target;
    uint16_t sent_left_deadzone;
    uint16_t sent_right_deadzone;
    bool pending;
    uint8_t pending_trigger_target;
    uint16_t pending_left_deadzone;
    uint16_t pending_right_deadzone;
} stick_config_sync_state_t;

bool stick_config_sync_request(stick_config_sync_state_t *state,
                               uint8_t trigger_target,
                               uint16_t left_deadzone,
                               uint16_t right_deadzone,
                               bool force);
bool stick_config_sync_get_pending(const stick_config_sync_state_t *state,
                                   uint8_t *trigger_target,
                                   uint16_t *left_deadzone,
                                   uint16_t *right_deadzone);
void stick_config_sync_mark_sent(stick_config_sync_state_t *state,
                                  uint8_t trigger_target,
                                  uint16_t left_deadzone,
                                  uint16_t right_deadzone);
