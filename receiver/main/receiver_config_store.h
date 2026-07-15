#pragma once

#include "esp_err.h"
#include "receiver_config_protocol.h"

esp_err_t receiver_config_store_load(receiver_device_config_t *config);
esp_err_t receiver_config_store_save(const receiver_device_config_t *config);

