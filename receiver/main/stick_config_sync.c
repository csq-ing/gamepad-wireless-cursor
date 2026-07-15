#include "stick_config_sync.h"

bool stick_config_sync_request(stick_config_sync_state_t *state,
                               uint8_t trigger_target,
                               uint16_t left_deadzone,
                               uint16_t right_deadzone,
                               bool force)
{
    bool should_queue;

    if (state == NULL) {
        return false;
    }

    should_queue = force ||
                   state->pending_trigger_target != trigger_target ||
                   state->pending_left_deadzone != left_deadzone ||
                   state->pending_right_deadzone != right_deadzone ||
                   (!state->pending &&
                    (!state->has_sent_config ||
                     state->sent_trigger_target != trigger_target ||
                     state->sent_left_deadzone != left_deadzone ||
                     state->sent_right_deadzone != right_deadzone));

    if (!should_queue) {
        return false;
    }

    state->pending = true;
    state->pending_trigger_target = trigger_target;
    state->pending_left_deadzone = left_deadzone;
    state->pending_right_deadzone = right_deadzone;
    return true;
}

bool stick_config_sync_get_pending(const stick_config_sync_state_t *state,
                                   uint8_t *trigger_target,
                                   uint16_t *left_deadzone,
                                   uint16_t *right_deadzone)
{
    if (state == NULL || !state->pending) {
        return false;
    }

    if (trigger_target != NULL) {
        *trigger_target = state->pending_trigger_target;
    }
    if (left_deadzone != NULL) {
        *left_deadzone = state->pending_left_deadzone;
    }
    if (right_deadzone != NULL) {
        *right_deadzone = state->pending_right_deadzone;
    }
    return true;
}

void stick_config_sync_mark_sent(stick_config_sync_state_t *state,
                                  uint8_t trigger_target,
                                  uint16_t left_deadzone,
                                  uint16_t right_deadzone)
{
    if (state == NULL) {
        return;
    }

    state->has_sent_config = true;
    state->sent_trigger_target = trigger_target;
    state->sent_left_deadzone = left_deadzone;
    state->sent_right_deadzone = right_deadzone;

    if (state->pending &&
        state->pending_trigger_target == trigger_target &&
        state->pending_left_deadzone == left_deadzone &&
        state->pending_right_deadzone == right_deadzone) {
        state->pending = false;
    }
}
