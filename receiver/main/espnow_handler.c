#include "espnow_handler.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "receiver_config.h"
#include "stick_config_sync.h"

static const char *TAG = "espnow_rx";

/*
 * Peer MAC address of the controller (ESP32-S3).
 * TODO: replace with the real STA MAC of your controller board.
 */
static uint8_t s_controller_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static espnow_input_cb_t s_input_cb = NULL;
static bool s_peer_added = false;
static stick_config_sync_state_t s_stick_config_sync = {0};
static QueueHandle_t s_input_queue = NULL;
static portMUX_TYPE s_peer_lock = portMUX_INITIALIZER_UNLOCKED;

#define INPUT_QUEUE_DEPTH    1
#define INPUT_TASK_STACK_SZ  4096
#define INPUT_TASK_PRIORITY  5
#define INPUT_TASK_IDLE_TICK pdMS_TO_TICKS(100)

static bool try_sync_pending_stick_config(void)
{
    bool can_attempt = false;
    uint8_t trigger_target = 0;
    uint16_t left_deadzone = 0;
    uint16_t right_deadzone = 0;

    taskENTER_CRITICAL(&s_peer_lock);
    can_attempt = s_peer_added &&
                  stick_config_sync_get_pending(&s_stick_config_sync,
                                                &trigger_target,
                                                &left_deadzone,
                                                &right_deadzone);
    taskEXIT_CRITICAL(&s_peer_lock);
    if (!can_attempt) {
        return false;
    }

    esp_err_t err = espnow_send_stick_config(trigger_target, left_deadzone, right_deadzone);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_peer_lock);
        stick_config_sync_mark_sent(&s_stick_config_sync,
                                    trigger_target,
                                    left_deadzone,
                                    right_deadzone);
        taskEXIT_CRITICAL(&s_peer_lock);
        return true;
    }

    return false;
}

static void input_dispatch_task(void *arg)
{
    gamepad_packet_t pkt;

    for (;;) {
        if (xQueueReceive(s_input_queue, &pkt, INPUT_TASK_IDLE_TICK) == pdTRUE && s_input_cb) {
            s_input_cb(&pkt);
        }

        (void)try_sync_pending_stick_config();
    }
}

/* ----- ESP-NOW receive callback (runs in Wi-Fi task context) --------------- */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 1) return;

    const gamepad_packet_t *pkt = (const gamepad_packet_t *)data;

    if (pkt->type == PKT_GAMEPAD_INPUT && len >= (int)(1 + sizeof(pkt->input))) {
        /* Remember peer MAC on first valid packet so we can reply */
        if (!s_peer_added) {
            receiver_device_config_t config = receiver_config_get_runtime();
            memcpy(s_controller_mac, info->src_addr, ESP_NOW_ETH_ALEN);
            esp_now_peer_info_t peer = {
                .channel = ESPNOW_CHANNEL,
                .encrypt = false,
            };
            memcpy(peer.peer_addr, s_controller_mac, ESP_NOW_ETH_ALEN);
            esp_err_t err = esp_now_add_peer(&peer);
            if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
                taskENTER_CRITICAL(&s_peer_lock);
                s_peer_added = true;
                (void)stick_config_sync_request(&s_stick_config_sync,
                                                config.trigger_target,
                                                config.left_deadzone,
                                                config.right_deadzone,
                                                true);
                taskEXIT_CRITICAL(&s_peer_lock);
                ESP_LOGI(TAG, "Controller peer added: " MACSTR, MAC2STR(s_controller_mac));
            } else {
                ESP_LOGE(TAG, "Failed to add controller peer: %s", esp_err_to_name(err));
            }
        }

        if (s_input_queue) {
            gamepad_packet_t pkt_copy = *pkt;
            (void)xQueueOverwrite(s_input_queue, &pkt_copy);
        }
    }
}

/* ----- Public API ---------------------------------------------------------- */

esp_err_t espnow_handler_init(espnow_input_cb_t input_cb)
{
    s_input_cb = input_cb;

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(gamepad_packet_t));
    if (!s_input_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ok = xTaskCreate(input_dispatch_task, "espnow_input",
                                     INPUT_TASK_STACK_SZ, NULL,
                                     INPUT_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Receiver STA MAC: " MACSTR, MAC2STR(mac));

    return ESP_OK;
}

esp_err_t espnow_send_feedback(uint8_t motor_left, uint8_t motor_right)
{
    if (!s_peer_added) {
        return ESP_ERR_INVALID_STATE;
    }

    gamepad_packet_t pkt = {
        .type = PKT_VIBRATION_FB,
        .feedback = {
            .motor_left  = motor_left,
            .motor_right = motor_right,
        },
    };

    return esp_now_send(s_controller_mac, (const uint8_t *)&pkt, 1 + sizeof(pkt.feedback));
}

esp_err_t espnow_send_stick_config(uint8_t trigger_target,
                                   uint16_t left_deadzone,
                                   uint16_t right_deadzone)
{
    if (!s_peer_added) {
        return ESP_ERR_INVALID_STATE;
    }

    gamepad_packet_t pkt = {
        .type = PKT_STICK_CONFIG,
        .stick_config = {
            .trigger_target = trigger_target,
            .left_deadzone = left_deadzone,
            .right_deadzone = right_deadzone,
        },
    };

    return esp_now_send(s_controller_mac, (const uint8_t *)&pkt, 1 + sizeof(pkt.stick_config));
}

esp_err_t espnow_send_ai_pull(uint8_t pull_percent)
{
    if (!s_peer_added) {
        return ESP_ERR_INVALID_STATE;
    }

    if (pull_percent > 100U) {
        pull_percent = 100U;
    }

    gamepad_packet_t pkt = {
        .type = PKT_AI_PULL,
        .ai_pull = {
            .pull_percent = pull_percent,
        },
    };

    return esp_now_send(s_controller_mac, (const uint8_t *)&pkt, 1 + sizeof(pkt.ai_pull));
}

void espnow_request_stick_config_sync(void)
{
    receiver_device_config_t config = receiver_config_get_runtime();

    taskENTER_CRITICAL(&s_peer_lock);
    (void)stick_config_sync_request(&s_stick_config_sync,
                                    config.trigger_target,
                                    config.left_deadzone,
                                    config.right_deadzone,
                                    false);
    taskEXIT_CRITICAL(&s_peer_lock);

    (void)try_sync_pending_stick_config();
}

#if CONFIG_REMOTE_LOG_ENABLE
esp_err_t espnow_send_log(const char *msg, size_t len)
{
    if (!s_peer_added) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[250];
    buf[0] = PKT_LOG_MSG;
    if (len > sizeof(buf) - 1) {
        len = sizeof(buf) - 1;
    }
    memcpy(&buf[1], msg, len);

    return esp_now_send(s_controller_mac, buf, 1 + len);
}
#endif
