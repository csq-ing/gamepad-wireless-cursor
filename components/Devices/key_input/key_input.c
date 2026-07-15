#include "key_input.h"

#include <stdbool.h>
#include <string.h>

#include "bsp_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gamepad_common.h"

static const char *TAG = "key_input";
static adc_oneshot_unit_handle_t s_adc1 = NULL;
static bool s_initialized = false;

static void buttons_init(void)
{
    const gpio_num_t pins[] = {PIN_BTN_A, PIN_BTN_B, PIN_BTN_X, PIN_BTN_Y};
    const gpio_config_t cfg = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        gpio_config_t c = cfg;
        c.pin_bit_mask = 1ULL << pins[i];
        ESP_ERROR_CHECK(gpio_config(&c));
    }
}

static void rt_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_RT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void trigger_mode_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_TRIGGER_MODE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static esp_err_t adc_init(void)
{
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id = ADC_UNIT_1,
    };

    esp_err_t err = adc_oneshot_new_unit(&cfg, &s_adc1);
    if (err != ESP_OK) {
        return err;
    }

    err = adc_oneshot_config_channel(s_adc1, ADC_STICK_LX, &chan_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = adc_oneshot_config_channel(s_adc1, ADC_STICK_LY, &chan_cfg);
    return err;
}

esp_err_t key_input_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    buttons_init();
    rt_button_init();
    trigger_mode_button_init();

    esp_err_t err = adc_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t key_input_read_state(key_input_state_t *out_state)
{
    if (out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_state, 0, sizeof(*out_state));

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!gpio_get_level(PIN_BTN_B)) out_state->buttons |= GAMEPAD_BTN_B;
    if (!gpio_get_level(PIN_BTN_Y)) out_state->buttons |= GAMEPAD_BTN_Y;
    if (!gpio_get_level(PIN_BTN_A)) out_state->buttons |= GAMEPAD_BTN_A;
    if (!gpio_get_level(PIN_BTN_X)) out_state->buttons |= GAMEPAD_BTN_X;
    out_state->rt_button_pressed = (gpio_get_level(PIN_RT_BUTTON) == 0);
    out_state->trigger_mode_button_pressed = (gpio_get_level(PIN_TRIGGER_MODE) == 0);

    int sample = 0;
    esp_err_t err = adc_oneshot_read(s_adc1, ADC_STICK_LX, &sample);
    if (err == ESP_OK) {
        out_state->lx = (int16_t)sample;
    }

    sample = 0;
    err = adc_oneshot_read(s_adc1, ADC_STICK_LY, &sample);
    if (err == ESP_OK) {
        out_state->ly = (int16_t)sample;
    }

    return ESP_OK;
}
