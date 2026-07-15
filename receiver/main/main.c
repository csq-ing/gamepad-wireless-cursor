#include "esp_log.h"
#include "usb_gamepad.h"
#include "espnow_handler.h"
#include "receiver_config.h"
#include "receiver_status.h"
#include "remote_log.h"

static const char *TAG = "receiver";

/* Called when a gamepad input packet arrives via ESP-NOW */
static void on_gamepad_input(const gamepad_packet_t *pkt)
{
    receiver_status_on_input(pkt);

    usb_gamepad_send_report(pkt);
}

/* Called when the PC sends a vibration output report via USB */
static void on_vibration_output(uint8_t motor_left, uint8_t motor_right)
{
    ESP_LOGD(TAG, "Vibration: L=%u R=%u", motor_left, motor_right);
    espnow_send_feedback(motor_left, motor_right);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Gamepad Receiver (ESP32-S2) ===");

    ESP_ERROR_CHECK(receiver_config_init());
    receiver_status_init();
    ESP_LOGI(TAG, "Receiver trigger config loaded");

    usb_gamepad_set_output_cb(on_vibration_output);
    ESP_ERROR_CHECK(usb_gamepad_init());
    ESP_LOGI(TAG, "USB composite initialised (XInput + HID config)");

    ESP_ERROR_CHECK(espnow_handler_init(on_gamepad_input));
    ESP_LOGI(TAG, "ESP-NOW initialised – waiting for controller...");

    ESP_ERROR_CHECK(remote_log_init());
}
