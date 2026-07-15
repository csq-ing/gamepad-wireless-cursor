#include "button_debounce.h"

#include <stddef.h>
#include <string.h>

void button_debounce_init(button_debounce_state_t *state, uint8_t threshold)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->threshold = (threshold == 0) ? 1 : threshold;
}

uint8_t button_debounce_update(button_debounce_state_t *state, uint8_t raw_state)
{
    if (state == NULL) {
        return raw_state;
    }

    for (uint8_t i = 0; i < 8; ++i) {
        uint8_t mask = (uint8_t)(1U << i);
        uint8_t raw_bit = raw_state & mask;
        uint8_t last_raw_bit = state->last_raw_state & mask;

        if (raw_bit != last_raw_bit) {
            state->counters[i] = 1;
            if (raw_bit) {
                state->last_raw_state |= mask;
            } else {
                state->last_raw_state &= (uint8_t)~mask;
            }
        } else if (state->counters[i] < state->threshold) {
            state->counters[i]++;
        }

        if (state->counters[i] >= state->threshold) {
            if (raw_bit) {
                state->stable_state |= mask;
            } else {
                state->stable_state &= (uint8_t)~mask;
            }
        }
    }

    return state->stable_state;
}
