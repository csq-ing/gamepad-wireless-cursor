#include "receiver_config.h"

#include "freertos/FreeRTOS.h"
#include "receiver_config_store.h"

static portMUX_TYPE s_cfg_lock = portMUX_INITIALIZER_UNLOCKED;

static const receiver_device_config_t RECEIVER_CONFIG_DEFAULT = {
    .trigger_target = RECEIVER_TRIGGER_TARGET_LT,
    .left_deadzone = 64U,
    .right_deadzone = 64U,
};

static receiver_device_config_t s_runtime_config = {
    .trigger_target = RECEIVER_TRIGGER_TARGET_LT,
    .left_deadzone = 64U,
    .right_deadzone = 64U,
};

static receiver_device_config_t s_saved_config = {
    .trigger_target = RECEIVER_TRIGGER_TARGET_LT,
    .left_deadzone = 64U,
    .right_deadzone = 64U,
};

static bool is_valid_target(uint8_t target)
{
    return (target == RECEIVER_TRIGGER_TARGET_LT || target == RECEIVER_TRIGGER_TARGET_RT);
}

bool receiver_config_is_valid(const receiver_device_config_t *config)
{
    return config != NULL
           && is_valid_target(config->trigger_target)
           && config->left_deadzone <= RECEIVER_CONFIG_DEADZONE_MAX
           && config->right_deadzone <= RECEIVER_CONFIG_DEADZONE_MAX;
}

esp_err_t receiver_config_init(void)
{
    receiver_device_config_t stored = RECEIVER_CONFIG_DEFAULT;
    esp_err_t err = receiver_config_store_load(&stored);
    receiver_device_config_t initial = (err == ESP_OK) ? stored : RECEIVER_CONFIG_DEFAULT;

    if (!is_valid_target(initial.trigger_target)) {
        initial.trigger_target = RECEIVER_CONFIG_DEFAULT.trigger_target;
    }
    if (initial.left_deadzone > RECEIVER_CONFIG_DEADZONE_MAX) {
        initial.left_deadzone = RECEIVER_CONFIG_DEFAULT.left_deadzone;
    }
    if (initial.right_deadzone > RECEIVER_CONFIG_DEADZONE_MAX) {
        initial.right_deadzone = RECEIVER_CONFIG_DEFAULT.right_deadzone;
    }

    taskENTER_CRITICAL(&s_cfg_lock);
    s_runtime_config = initial;
    s_saved_config = initial;
    taskEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

receiver_device_config_t receiver_config_get_runtime(void)
{
    taskENTER_CRITICAL(&s_cfg_lock);
    receiver_device_config_t config = s_runtime_config;
    taskEXIT_CRITICAL(&s_cfg_lock);
    return config;
}

receiver_device_config_t receiver_config_get_saved(void)
{
    taskENTER_CRITICAL(&s_cfg_lock);
    receiver_device_config_t config = s_saved_config;
    taskEXIT_CRITICAL(&s_cfg_lock);
    return config;
}

esp_err_t receiver_config_set_runtime(const receiver_device_config_t *config)
{
    if (!receiver_config_is_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_cfg_lock);
    s_runtime_config = *config;
    taskEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

esp_err_t receiver_config_save(void)
{
    receiver_device_config_t runtime = receiver_config_get_runtime();
    esp_err_t err = receiver_config_store_save(&runtime);
    if (err != ESP_OK) {
        return err;
    }

    taskENTER_CRITICAL(&s_cfg_lock);
    s_saved_config = runtime;
    taskEXIT_CRITICAL(&s_cfg_lock);
    return ESP_OK;
}

receiver_trigger_target_t receiver_config_get_runtime_target(void)
{
    return (receiver_trigger_target_t)receiver_config_get_runtime().trigger_target;
}

receiver_trigger_target_t receiver_config_get_saved_target(void)
{
    return (receiver_trigger_target_t)receiver_config_get_saved().trigger_target;
}

bool receiver_config_is_valid_target(uint8_t target)
{
    receiver_device_config_t config = receiver_config_get_runtime();
    config.trigger_target = target;
    return receiver_config_is_valid(&config);
}

esp_err_t receiver_config_set_runtime_target(uint8_t target)
{
    receiver_device_config_t config = receiver_config_get_runtime();
    config.trigger_target = target;
    return receiver_config_set_runtime(&config);
}
