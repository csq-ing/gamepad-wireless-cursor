#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    MOTOR_STATE_NORMAL = 0,
    MOTOR_STATE_BITE_SHAKE,
    MOTOR_STATE_FISH_DAMPING,
} motor_control_state_t;

esp_err_t motor_control_init(void);
esp_err_t motor_control_apply_vibration(uint8_t left, uint8_t right);
