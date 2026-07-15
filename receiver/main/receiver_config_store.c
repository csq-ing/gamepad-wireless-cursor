#include "receiver_config_store.h"

#include <stdbool.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *NVS_NAMESPACE = "gp_cfg";
static const char *NVS_KEY_TRIGGER_TARGET = "trg_tgt";
static const char *NVS_KEY_LEFT_DEADZONE = "left_dz";
static const char *NVS_KEY_RIGHT_DEADZONE = "right_dz";
static const char *TAG = "rx_cfg_store";

static bool s_nvs_ready = false;

static bool is_fatal_key_read_error(esp_err_t err)
{
    return err == ESP_ERR_NVS_INVALID_HANDLE;
}

static esp_err_t load_u8_field(nvs_handle_t handle, const char *key, uint8_t *value)
{
    esp_err_t err = nvs_get_u8(handle, key, value);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    if (is_fatal_key_read_error(err)) {
        return err;
    }

    ESP_LOGW(TAG, "Ignoring unreadable key '%s': %s", key, esp_err_to_name(err));
    return ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t load_u16_field(nvs_handle_t handle, const char *key, uint16_t *value)
{
    esp_err_t err = nvs_get_u16(handle, key, value);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }

    if (is_fatal_key_read_error(err)) {
        return err;
    }

    ESP_LOGW(TAG, "Ignoring unreadable key '%s': %s", key, esp_err_to_name(err));
    return ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t ensure_nvs_ready(void)
{
    if (s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        s_nvs_ready = true;
    }
    return err;
}

esp_err_t receiver_config_store_load(receiver_device_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t trigger_target = 0;
    err = load_u8_field(handle, NVS_KEY_TRIGGER_TARGET, &trigger_target);
    if (err == ESP_OK) {
        config->trigger_target = trigger_target;
    } else if (is_fatal_key_read_error(err)) {
        nvs_close(handle);
        return err;
    }

    uint16_t left_deadzone = 0;
    err = load_u16_field(handle, NVS_KEY_LEFT_DEADZONE, &left_deadzone);
    if (err == ESP_OK) {
        config->left_deadzone = left_deadzone;
    } else if (is_fatal_key_read_error(err)) {
        nvs_close(handle);
        return err;
    }

    uint16_t right_deadzone = 0;
    err = load_u16_field(handle, NVS_KEY_RIGHT_DEADZONE, &right_deadzone);
    if (err == ESP_OK) {
        config->right_deadzone = right_deadzone;
    } else if (is_fatal_key_read_error(err)) {
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t receiver_config_store_save(const receiver_device_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_nvs_ready();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_TRIGGER_TARGET, config->trigger_target);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, NVS_KEY_LEFT_DEADZONE, config->left_deadzone);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, NVS_KEY_RIGHT_DEADZONE, config->right_deadzone);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
