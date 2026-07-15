#include "esp_log.h"
#include "espnow_handler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "input_handler.h"
#include "motor_control.h"

#include <inttypes.h>

static const char *TAG = "controller";
#define AI_PULL_TIMEOUT_MS 500

static TimerHandle_t s_ai_pull_timeout_timer;

static void on_ai_pull_timeout(TimerHandle_t timer)
{
  (void)timer;
}

static void ai_pull_timeout_init(void)
{
  s_ai_pull_timeout_timer =
      xTimerCreate("ai_pull_timeout", pdMS_TO_TICKS(AI_PULL_TIMEOUT_MS),
                   pdFALSE, NULL, on_ai_pull_timeout);
  if (!s_ai_pull_timeout_timer) {
    ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
  }
}

static void ai_pull_timeout_reset(void)
{
  if (s_ai_pull_timeout_timer &&
      xTimerReset(s_ai_pull_timeout_timer, 0) != pdPASS) {
    ESP_LOGW(TAG, "Failed to reset AI pull timeout timer");
  }
}

/* ---- callbacks ----------------------------------------------------------- */

static void on_input_change(const gamepad_packet_t *pkt)
{
  esp_err_t err = espnow_send_input(pkt);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ESP-NOW send failed: %s", esp_err_to_name(err));
  }
}

static void on_vibration(uint8_t motor_left, uint8_t motor_right)
{
  esp_err_t err = motor_control_apply_vibration(motor_left, motor_right);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ZE300 torque send failed: %s", esp_err_to_name(err));
  }
}

static void on_stick_config(uint8_t trigger_target, uint16_t left_deadzone,
                            uint16_t right_deadzone) {
  input_handler_set_stick_config(trigger_target, left_deadzone, right_deadzone);
}

static void on_ai_pull(uint8_t pull_percent) {
  (void)pull_percent;
  ai_pull_timeout_reset();
}

/* ---- entry point --------------------------------------------------------- */

void app_main(void)
 {
  ESP_LOGI(TAG, "=== Gamepad Controller (ESP32-S3) ===");

  esp_err_t err = motor_control_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ZE300 motor init failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "ZE300 motor initialised");
  }

  ai_pull_timeout_init();
  ESP_LOGI(TAG, "AI pull timeout initialised");

  ESP_ERROR_CHECK(espnow_controller_init(on_vibration, on_stick_config,
                                         on_ai_pull));
  ESP_LOGI(TAG, "ESP-NOW initialised");

  ESP_ERROR_CHECK(input_handler_init(on_input_change));
  ESP_LOGI(TAG, "Input polling started at 100 Hz");
}
