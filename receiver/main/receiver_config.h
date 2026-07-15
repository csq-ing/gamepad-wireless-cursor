#pragma once

#include "esp_err.h"
#include "gamepad_common.h"
#include "receiver_config_protocol.h"

esp_err_t receiver_config_init(void);

receiver_device_config_t receiver_config_get_runtime(void);
receiver_device_config_t receiver_config_get_saved(void);

bool receiver_config_is_valid(const receiver_device_config_t *config);
esp_err_t receiver_config_set_runtime(const receiver_device_config_t *config);
esp_err_t receiver_config_save(void);

/* Compatibility wrappers for existing trigger-only call sites. */
receiver_trigger_target_t receiver_config_get_runtime_target(void);
receiver_trigger_target_t receiver_config_get_saved_target(void);
bool receiver_config_is_valid_target(uint8_t target);
esp_err_t receiver_config_set_runtime_target(uint8_t target);

