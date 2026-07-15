#include "input_handler.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "button_debounce.h"
#include "key_input.h"
#include "mpu_motion.h"
#include "rotary_encoder.h"
#include "stick_processing.h"
#include "trigger_input.h"

static const char *TAG = "input";

/* ---- internal state ------------------------------------------------------ */

static input_change_cb_t s_cb = NULL;
static key_input_state_t s_key_input_state;
static mpu_motion_state_t s_mpu_motion;
static bool s_mpu_motion_available = false;
static stick_processing_state_t s_stick_state;
static portMUX_TYPE s_stick_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_accel_trigger_target = GAMEPAD_TRIGGER_TARGET_LT;
static button_debounce_state_t s_button_debounce;
static rotary_encoder_state_t s_rotary_encoder;
static trigger_mode_state_t s_trigger_mode;
static bool s_trigger_mode_prev_active = false;
static bool s_trigger_view_prev_active = false;

/* Previous packet for change detection */
static gamepad_packet_t s_prev;

#define STICK_CALIBRATION_SAMPLES  32
#define STICK_READ_FALLBACK_CENTER 2048
#define BUTTON_DEBOUNCE_SAMPLES    3
#define ROTARY_ENCODER_TRANSITIONS_PER_PRESS 6
#define MOTION_CALIBRATION_SAMPLES 32
/* 500 ms at the 100 Hz input polling rate. */
#define TRIGGER_MODE_HOLD_SAMPLES  50

/* ---- key input / rotary ------------------------------------------------- */

static void key_input_init_input(void)
{
    esp_err_t err = key_input_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Key input init failed: %s", esp_err_to_name(err));
    }
}

static void rotary_encoder_init_input(void)
{
    ESP_ERROR_CHECK(rotary_encoder_init(&s_rotary_encoder,
                                        PIN_ROTARY_ENCODER_A,
                                        PIN_ROTARY_ENCODER_B,
                                        ROTARY_ENCODER_TRANSITIONS_PER_PRESS));
}

/* ---- ADC sticks / calibration ----------------------------------------- */

static int16_t fallback_center_value(int16_t calibrated_center)
{
    if (calibrated_center > 0) {
        return calibrated_center;
    }
    return STICK_READ_FALLBACK_CENTER;
}

static void calibrate_sticks(void)
{
    stick_axes_t samples[STICK_CALIBRATION_SAMPLES];
    int valid_sample_count = 0;

    for (int i = 0; i < STICK_CALIBRATION_SAMPLES; ++i) {
        key_input_state_t state;
        if (key_input_read_state(&state) == ESP_OK) {
            stick_axes_t sample = {0};
            sample.lx = state.lx;
            sample.ly = state.ly;
            sample.rx = fallback_center_value(s_stick_state.centers.rx);
            sample.ry = fallback_center_value(s_stick_state.centers.ry);
            samples[valid_sample_count++] = sample;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (valid_sample_count > 0) {
        stick_processing_calibrate_centers(&s_stick_state,
                                           samples,
                                           (uint32_t)valid_sample_count);
        return;
    }

    stick_processing_set_centers(&s_stick_state,
                                 STICK_READ_FALLBACK_CENTER,
                                 STICK_READ_FALLBACK_CENTER,
                                 STICK_READ_FALLBACK_CENTER,
                                 STICK_READ_FALLBACK_CENTER);
    ESP_LOGW(TAG, "Stick calibration falling back to default centers");
}

static void read_sticks(int16_t *lx, int16_t *ly)
{
    stick_axes_t raw;
    stick_axes_t processed;

    /* s_key_input_state is refreshed once at the start of each poll cycle. */
    raw.lx = s_key_input_state.lx;
    raw.ly = s_key_input_state.ly;
    raw.rx = fallback_center_value(s_stick_state.centers.rx);
    raw.ry = fallback_center_value(s_stick_state.centers.ry);

    taskENTER_CRITICAL(&s_stick_state_lock);
    stick_processing_apply(&s_stick_state, &raw, &processed);
    taskEXIT_CRITICAL(&s_stick_state_lock);

    *lx = processed.lx;
    *ly = processed.ly;
}

/* ---- BMI088 motion ------------------------------------------------------- */
static void mpu_motion_init_input(void)
{
    esp_err_t err = mpu_motion_init(&s_mpu_motion);

    if (err != ESP_OK) {
        s_mpu_motion_available = false;
        ESP_LOGW(TAG, "BMI088 motion init failed: %s", esp_err_to_name(err));
        return;
    }

    err = mpu_motion_calibrate_gyro_bias(&s_mpu_motion, MOTION_CALIBRATION_SAMPLES);
    if (err != ESP_OK) {
        s_mpu_motion_available = false;
        ESP_LOGW(TAG, "BMI088 motion calibration failed: %s", esp_err_to_name(err));
        return;
    }

    s_mpu_motion_available = true;
}

/* ---- polling task -------------------------------------------------------- */

static void input_poll_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(10);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        gamepad_packet_t pkt = {.type = PKT_GAMEPAD_INPUT};
        int16_t lx = 0;
        int16_t ly = 0;
        int16_t view_rx = 0;
        int16_t view_ry = 0;
        uint8_t buttons = 0;
        uint8_t rotary_buttons = 0;
        uint8_t pot_lt = 0;
        uint8_t pot_rt = 0;
        uint8_t alt_trigger = 0;
        bool alt_mode_active = false;
        bool view_mode_active = false;

        key_input_read_state(&s_key_input_state);
        buttons = button_debounce_update(&s_button_debounce, s_key_input_state.buttons);
        rotary_buttons = rotary_encoder_consume_dpad(&s_rotary_encoder);
        buttons |= rotary_buttons;

        read_sticks(&lx, &ly);
        if (s_mpu_motion_available) {
            (void)mpu_motion_poll(&s_mpu_motion);
        }
        // read_trigger_pots(&pot_lt, &pot_rt);
        pot_rt = trigger_input_map_button_to_u8(s_key_input_state.rt_button_pressed);
        alt_mode_active = trigger_mode_update(&s_trigger_mode, s_key_input_state.trigger_mode_button_pressed);
        view_mode_active = trigger_mode_view_active(&s_trigger_mode);

        if ((alt_mode_active != s_trigger_mode_prev_active ||
             view_mode_active != s_trigger_view_prev_active) &&
            s_mpu_motion_available) {
            mpu_motion_reset_view_smoothing(&s_mpu_motion);
            mpu_motion_reset_alt_trigger_center(&s_mpu_motion);
        }
        s_trigger_mode_prev_active = alt_mode_active;
        s_trigger_view_prev_active = view_mode_active;

        if (alt_mode_active) {
            view_rx = 0;
            view_ry = 0;
            if (s_mpu_motion_available) {
                alt_trigger = mpu_motion_read_alt_trigger(&s_mpu_motion);
            }
        } else if (view_mode_active && s_mpu_motion_available) {
            if (!mpu_motion_read_view(&s_mpu_motion, &view_rx, &view_ry)) {
                view_rx = 0;
                view_ry = 0;
            }
        }

        pkt.input.lx = lx;
        pkt.input.ly = ly;
        pkt.input.rx = view_rx;
        pkt.input.ry = view_ry;
        trigger_input_select_sources(pot_lt,
                                     pot_rt,
                                     alt_trigger,
                                     alt_mode_active,
                                     s_accel_trigger_target,
                                     buttons,
                                     &pkt.input.buttons,
                                     &pkt.input.lt,
                                     &pkt.input.rt);

        if (memcmp(&pkt.input, &s_prev.input, sizeof(pkt.input)) != 0) {
            s_prev = pkt;
            if (s_cb) {
                s_cb(&pkt);
            }
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

/* ---- public API ---------------------------------------------------------- */

esp_err_t input_handler_init(input_change_cb_t cb)
{
    s_cb = cb;
    memset(&s_prev, 0, sizeof(s_prev));
    stick_processing_init(&s_stick_state);
    button_debounce_init(&s_button_debounce, BUTTON_DEBOUNCE_SAMPLES);
    trigger_mode_init(&s_trigger_mode, TRIGGER_MODE_HOLD_SAMPLES);
    s_trigger_mode_prev_active = false;
    s_trigger_view_prev_active = false;

    key_input_init_input();
    ESP_LOGI(TAG, "Key input driver initialised");

    rotary_encoder_init_input();
    ESP_LOGI(TAG, "Rotary encoder initialised for D-pad left/right");

    calibrate_sticks();
    ESP_LOGI(TAG, "Stick calibration complete");

    mpu_motion_init_input();
    if (s_mpu_motion_available) {
        ESP_LOGI(TAG, "BMI088 SPI initialised; short press trigger mode for view control, long press for BMI088 pitch trigger");
    } else {
        ESP_LOGW(TAG, "BMI088 motion unavailable; view output will remain centered");
    }

    BaseType_t rc = xTaskCreatePinnedToCore(input_poll_task, "input_poll", 4096, NULL, 5, NULL, 0);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to start input poll task: %d", (int)rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Input poll task started");
    return ESP_OK;
}

void input_handler_set_stick_config(uint8_t trigger_target,
                                    uint16_t left_deadzone,
                                    uint16_t right_deadzone)
{
    taskENTER_CRITICAL(&s_stick_state_lock);
    if (trigger_target == GAMEPAD_TRIGGER_TARGET_LT ||
        trigger_target == GAMEPAD_TRIGGER_TARGET_RT) {
        s_accel_trigger_target = trigger_target;
    }
    stick_processing_set_config(&s_stick_state, left_deadzone, right_deadzone);
    taskEXIT_CRITICAL(&s_stick_state_lock);
}
