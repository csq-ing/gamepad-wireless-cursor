#include "rotary_encoder.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "gamepad_common.h"

static rotary_encoder_state_t *s_state = NULL;
static gpio_num_t s_pin_a = GPIO_NUM_NC;
static gpio_num_t s_pin_b = GPIO_NUM_NC;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static inline uint8_t rotary_encoder_read_state(void)
{
    return (uint8_t)(((uint8_t)gpio_get_level(s_pin_a) << 1) |
                     (uint8_t)gpio_get_level(s_pin_b));
}

static void IRAM_ATTR rotary_encoder_gpio_isr(void *arg)
{
    static const int8_t transition_table[16] = {
        0, 1, -1, 0,
        -1, 0, 0, 1,
        1, 0, 0, -1,
        0, -1, 1, 0,
    };
    rotary_encoder_state_t *state = (rotary_encoder_state_t *)arg;
    uint8_t next_state;
    uint8_t table_index;
    int8_t delta;

    if (state == NULL) {
        return;
    }

    next_state = rotary_encoder_read_state();

    portENTER_CRITICAL_ISR(&s_lock);
    table_index = (uint8_t)((state->last_state << 2) | next_state);
    delta = transition_table[table_index];
    state->last_state = next_state;

    if (delta != 0) {
        state->transition_accumulator += delta;

        while (state->transition_accumulator >= state->transitions_per_press) {
            if (state->pending_left_presses < UCHAR_MAX) {
                state->pending_left_presses++;
            }
            state->transition_accumulator -= state->transitions_per_press;
        }

        while (state->transition_accumulator <= -(int16_t)state->transitions_per_press) {
            if (state->pending_right_presses < UCHAR_MAX) {
                state->pending_right_presses++;
            }
            state->transition_accumulator += state->transitions_per_press;
        }
    }
    portEXIT_CRITICAL_ISR(&s_lock);
}

esp_err_t rotary_encoder_init(rotary_encoder_state_t *state,
                              gpio_num_t pin_a,
                              gpio_num_t pin_b,
                              uint8_t transitions_per_press)
{
    esp_err_t err;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin_a) | (1ULL << pin_b),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    if (state == NULL || pin_a == GPIO_NUM_NC || pin_b == GPIO_NUM_NC || pin_a == pin_b) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    state->transitions_per_press = (transitions_per_press == 0U) ? 1U : transitions_per_press;

    s_state = state;
    s_pin_a = pin_a;
    s_pin_b = pin_b;

    err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    state->last_state = rotary_encoder_read_state();

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(pin_a, rotary_encoder_gpio_isr, s_state);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_isr_handler_add(pin_b, rotary_encoder_gpio_isr, s_state);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

uint8_t rotary_encoder_consume_dpad(rotary_encoder_state_t *state)
{
    uint8_t buttons = 0;

    if (state == NULL) {
        return 0;
    }

    taskENTER_CRITICAL(&s_lock);
    if (state->pending_left_presses > 0U) {
        state->pending_left_presses--;
        buttons = GAMEPAD_BTN_DPAD_LEFT;
    } else if (state->pending_right_presses > 0U) {
        state->pending_right_presses--;
        buttons = GAMEPAD_BTN_DPAD_RIGHT;
    }
    taskEXIT_CRITICAL(&s_lock);

    return buttons;
}

void rotary_encoder_get_snapshot(const rotary_encoder_state_t *state,
                                 rotary_encoder_snapshot_t *snapshot)
{
    if (state == NULL || snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    snapshot->state = state->last_state;
    snapshot->pending_left_presses = state->pending_left_presses;
    snapshot->pending_right_presses = state->pending_right_presses;
    snapshot->transition_accumulator = state->transition_accumulator;
    taskEXIT_CRITICAL(&s_lock);
}
