#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    float angle_deg;
    float speed_rpm;
    float q_current_a;
    float bus_voltage_v;
    float bus_current_a;
    float temperature_c;
    uint8_t work_mode;
    uint8_t fault_code;
} ze300_state_t;

esp_err_t ze300_init(void);
esp_err_t ze300_set_torque(int32_t current_ma);
esp_err_t ze300_read_state(ze300_state_t *out);
esp_err_t ze300_clear_fault(void);
