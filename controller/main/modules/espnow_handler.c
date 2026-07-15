#include "espnow_handler.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "espnow_tx";

/*
 * Peer MAC address of the receiver (ESP32-S2).
 * TODO: replace with the real STA MAC of your receiver board.
 */
static uint8_t s_receiver_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static espnow_feedback_cb_t s_fb_cb = NULL;
static espnow_stick_config_cb_t s_stick_config_cb = NULL;
static espnow_ai_pull_cb_t s_ai_pull_cb = NULL;
static bool s_peer_resolved = false;

/* ---- ESP-NOW receive callback --------------------------------------------- */

static void resolve_receiver_peer(const esp_now_recv_info_t *info)
{
    if (s_peer_resolved) {
        return;
    }

    esp_now_peer_info_t peer = {
        .channel = ESPNOW_CHANNEL,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, info->src_addr, ESP_NOW_ETH_ALEN);
    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        memcpy(s_receiver_mac, info->src_addr, ESP_NOW_ETH_ALEN);
        s_peer_resolved = true;
        ESP_LOGI(TAG, "Receiver peer added: " MACSTR, MAC2STR(s_receiver_mac));
    } else {
        ESP_LOGE(TAG, "Failed to add receiver peer: %s", esp_err_to_name(err));
    }
}

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 1) return;

    const gamepad_packet_t *pkt = (const gamepad_packet_t *)data;

    if (pkt->type == PKT_VIBRATION_FB && len >= (int)(1 + sizeof(pkt->feedback))) {
        resolve_receiver_peer(info);
        if (s_fb_cb) {
            s_fb_cb(pkt->feedback.motor_left, pkt->feedback.motor_right);
        }
    }
    else if (pkt->type == PKT_STICK_CONFIG && len >= (int)(1 + sizeof(pkt->stick_config))) {
        resolve_receiver_peer(info);
        if (s_stick_config_cb) {
            s_stick_config_cb(pkt->stick_config.trigger_target,
                              pkt->stick_config.left_deadzone,
                              pkt->stick_config.right_deadzone);
        }
    }
    else if (pkt->type == PKT_AI_PULL && len >= (int)(1 + sizeof(pkt->ai_pull))) {
        resolve_receiver_peer(info);
        if (s_ai_pull_cb) {
            s_ai_pull_cb(pkt->ai_pull.pull_percent);
        }
    }
#if CONFIG_REMOTE_LOG_ENABLE
    else if (data[0] == PKT_LOG_MSG && len > 1) {
        printf("[RX] %.*s", len - 1, (const char *)&data[1]);
    }
#endif
}

/* ---- Public API ----------------------------------------------------------- */

esp_err_t espnow_controller_init(espnow_feedback_cb_t fb_cb,
                                 espnow_stick_config_cb_t stick_config_cb,
                                 espnow_ai_pull_cb_t ai_pull_cb)
{
    s_fb_cb = fb_cb;
    s_stick_config_cb = stick_config_cb;
    s_ai_pull_cb = ai_pull_cb;

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

    /* Add broadcast peer so first packets reach the receiver before pairing */
    esp_now_peer_info_t peer = {
        .channel = ESPNOW_CHANNEL,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, s_receiver_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Controller STA MAC: " MACSTR, MAC2STR(mac));

    return ESP_OK;
}

esp_err_t espnow_send_input(const gamepad_packet_t *pkt)
{
    return esp_now_send(s_receiver_mac, (const uint8_t *)pkt, 1 + sizeof(pkt->input));
}
