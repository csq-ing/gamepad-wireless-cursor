#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_REMOTE_LOG_ENABLE
/**
 * Hook ESP_LOG vprintf to additionally forward log output to the controller
 * via ESP-NOW.  Must be called after espnow_handler_init().
 */
esp_err_t remote_log_init(void);
#else
static inline esp_err_t remote_log_init(void) { return ESP_OK; }
#endif
