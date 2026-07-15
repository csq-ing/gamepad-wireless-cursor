#include "usb_config_hid.h"

#include <string.h>
#include "esp_log.h"
#include "espnow_handler.h"
#include "receiver_config.h"
#include "receiver_config_protocol.h"
#include "receiver_status.h"
#include "tusb.h"

#define USB_CONFIG_HID_EP_OUT 0x02U
#define USB_CONFIG_HID_EP_IN  0x82U
#define USB_CONFIG_HID_EP_SIZE RECEIVER_CONFIG_REPORT_SIZE

static const char *TAG = "usb_cfg_hid";

static const uint8_t s_hid_report_descriptor[] = {
    0x06, 0x00, 0xFF, /* Usage Page (Vendor Defined) */
    0x09, 0x01,       /* Usage (Vendor Usage 1) */
    0xA1, 0x01,       /* Collection (Application) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x26, 0xFF, 0x00, /* Logical Maximum (255) */
    0x75, 0x08,       /* Report Size (8 bits) */
    0x95, RECEIVER_CONFIG_REPORT_SIZE, /* Report Count (64 bytes) */
    0x09, 0x02,       /* Usage (Vendor Usage 2) */
    0x81, 0x02,       /* Input (Data,Var,Abs) */
    0x95, RECEIVER_CONFIG_REPORT_SIZE, /* Report Count (64 bytes) */
    0x09, 0x03,       /* Usage (Vendor Usage 3) */
    0x91, 0x02,       /* Output (Data,Var,Abs) */
    0xC0              /* End Collection */
};

/* Interface(9) + HID(9) + EP_OUT(7) + EP_IN(7) = 32 */
static const uint8_t s_hid_itf_desc_template[USB_CONFIG_HID_DESC_LEN] = {
    /* Interface 1: HID configuration channel */
    0x09, 0x04,
    USB_CONFIG_HID_ITF_INDEX,
    0x00,
    0x02,
    TUSB_CLASS_HID,
    0x00,
    0x00,
    0x00,

    /* HID descriptor */
    0x09, 0x21,
    U16_TO_U8S_LE(0x0111), /* HID spec 1.11 */
    0x00,
    0x01,
    0x22,
    U16_TO_U8S_LE(sizeof(s_hid_report_descriptor)),

    /* Endpoint OUT */
    0x07, 0x05,
    USB_CONFIG_HID_EP_OUT,
    0x03,
    U16_TO_U8S_LE(USB_CONFIG_HID_EP_SIZE),
    0x00,

    /* Endpoint IN */
    0x07, 0x05,
    USB_CONFIG_HID_EP_IN,
    0x03,
    U16_TO_U8S_LE(USB_CONFIG_HID_EP_SIZE),
    0x00,
};

static void put_u16le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static uint16_t get_u16le(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

static void put_u32le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void send_response(uint8_t cmd_id, receiver_cfg_status_t status,
                          const uint8_t *payload, uint16_t payload_len)
{
    uint8_t report[RECEIVER_CONFIG_REPORT_SIZE] = {0};
    report[0] = cmd_id;
    report[1] = (uint8_t)status;

    if (payload != NULL) {
        uint16_t max_payload = RECEIVER_CONFIG_REPORT_SIZE - 2U;
        if (payload_len > max_payload) {
            payload_len = max_payload;
        }
        memcpy(&report[2], payload, payload_len);
    }

    if (!tud_hid_report(0U, report, sizeof(report))) {
        ESP_LOGW(TAG, "Failed to send response for cmd 0x%02X", cmd_id);
    }
}

static void handle_get_status(void)
{
    receiver_status_snapshot_t snap;
    receiver_status_get_snapshot(&snap);

    uint8_t payload[11] = {0};
    payload[0] = snap.protocol_version;
    payload[1] = snap.usb_online ? 1U : 0U;
    payload[2] = snap.controller_connected ? 1U : 0U;
    put_u32le(&payload[3], snap.last_input_age_ms);
    put_u16le(&payload[7], snap.downward_accel);
    payload[9] = snap.trigger_active ? 1U : 0U;
    payload[10] = (uint8_t)snap.trigger_target;

    send_response(RECEIVER_CFG_CMD_GET_STATUS, RECEIVER_CFG_STATUS_OK,
                  payload, sizeof(payload));
}

static void send_legacy_config_response(uint8_t cmd_id, receiver_trigger_target_t trigger_target)
{
    uint8_t payload[1] = {(uint8_t)trigger_target};
    send_response(cmd_id, RECEIVER_CFG_STATUS_OK, payload, sizeof(payload));
}

static void send_full_config_response(uint8_t cmd_id, receiver_device_config_t config)
{
    uint8_t payload[sizeof(config)] = {0};
    payload[0] = (uint8_t)config.trigger_target;
    put_u16le(&payload[1], config.left_deadzone);
    put_u16le(&payload[3], config.right_deadzone);
    send_response(cmd_id, RECEIVER_CFG_STATUS_OK, payload, sizeof(payload));
}

static void handle_get_config(void)
{
    receiver_device_config_t config = receiver_config_get_saved();
    send_legacy_config_response(RECEIVER_CFG_CMD_GET_CONFIG, config.trigger_target);
}

static void handle_set_config(const uint8_t *buffer, uint16_t len)
{
    if (len < 2U) {
        send_response(RECEIVER_CFG_CMD_SET_CONFIG,
                      RECEIVER_CFG_STATUS_INVALID_ARG, NULL, 0);
        return;
    }

    receiver_device_config_t config = receiver_config_get_runtime();
    config.trigger_target = buffer[1];

    if (!receiver_config_is_valid(&config)) {
        send_response(RECEIVER_CFG_CMD_SET_CONFIG,
                      RECEIVER_CFG_STATUS_INVALID_ARG, NULL, 0);
        return;
    }

    esp_err_t err = receiver_config_set_runtime(&config);
    if (err != ESP_OK) {
        send_response(RECEIVER_CFG_CMD_SET_CONFIG,
                      RECEIVER_CFG_STATUS_INTERNAL_ERR, NULL, 0);
        return;
    }

    espnow_request_stick_config_sync();
    send_legacy_config_response(RECEIVER_CFG_CMD_SET_CONFIG, config.trigger_target);
}

static void handle_save_config(void)
{
    esp_err_t err = receiver_config_save();
    if (err != ESP_OK) {
        send_response(RECEIVER_CFG_CMD_SAVE_CONFIG,
                      RECEIVER_CFG_STATUS_INTERNAL_ERR, NULL, 0);
        return;
    }

    receiver_device_config_t config = receiver_config_get_saved();
    send_legacy_config_response(RECEIVER_CFG_CMD_SAVE_CONFIG, config.trigger_target);
}

static void handle_get_full_config(void)
{
    receiver_device_config_t config = receiver_config_get_saved();
    send_full_config_response(RECEIVER_CFG_CMD_GET_FULL_CONFIG, config);
}

static void handle_set_full_config(const uint8_t *buffer, uint16_t len)
{
    if (len < (1U + sizeof(receiver_device_config_t))) {
        send_response(RECEIVER_CFG_CMD_SET_FULL_CONFIG,
                      RECEIVER_CFG_STATUS_INVALID_ARG, NULL, 0);
        return;
    }

    receiver_device_config_t config = {
        .trigger_target = buffer[1],
        .left_deadzone = get_u16le(&buffer[2]),
        .right_deadzone = get_u16le(&buffer[4]),
    };

    if (!receiver_config_is_valid(&config)) {
        send_response(RECEIVER_CFG_CMD_SET_FULL_CONFIG,
                      RECEIVER_CFG_STATUS_INVALID_ARG, NULL, 0);
        return;
    }

    esp_err_t err = receiver_config_set_runtime(&config);
    if (err != ESP_OK) {
        send_response(RECEIVER_CFG_CMD_SET_FULL_CONFIG,
                      RECEIVER_CFG_STATUS_INTERNAL_ERR, NULL, 0);
        return;
    }

    espnow_request_stick_config_sync();
    send_full_config_response(RECEIVER_CFG_CMD_SET_FULL_CONFIG, config);
}

static void handle_save_full_config(void)
{
    esp_err_t err = receiver_config_save();
    if (err != ESP_OK) {
        send_response(RECEIVER_CFG_CMD_SAVE_FULL_CONFIG,
                      RECEIVER_CFG_STATUS_INTERNAL_ERR, NULL, 0);
        return;
    }

    receiver_device_config_t config = receiver_config_get_saved();
    send_full_config_response(RECEIVER_CFG_CMD_SAVE_FULL_CONFIG, config);
}

static void handle_set_ai_pull(const uint8_t *buffer, uint16_t len)
{
    if (len < 2U) {
        send_response(RECEIVER_CFG_CMD_SET_AI_PULL,
                      RECEIVER_CFG_STATUS_INVALID_ARG, NULL, 0);
        return;
    }

    uint8_t pull_percent = buffer[1];
    if (pull_percent > 100U) {
        pull_percent = 100U;
    }

    ESP_LOGI(TAG, "AI pull HID command: %u%%", pull_percent);

    esp_err_t err = espnow_send_ai_pull(pull_percent);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "AI pull dropped: controller peer not ready");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "AI pull ESP-NOW send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "AI pull forwarded over ESP-NOW: %u%%", pull_percent);
    }

    send_response(RECEIVER_CFG_CMD_SET_AI_PULL, RECEIVER_CFG_STATUS_OK, NULL, 0);
}

void usb_config_hid_copy_interface_descriptor(uint8_t *dst, uint8_t poll_interval_ms)
{
    if (dst == NULL) {
        return;
    }

    memcpy(dst, s_hid_itf_desc_template, sizeof(s_hid_itf_desc_template));
    dst[24] = poll_interval_ms;
    dst[31] = poll_interval_ms;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;

    if (instance != 0U || report_type != HID_REPORT_TYPE_OUTPUT || buffer == NULL || bufsize < 1U) {
        return;
    }

    const uint8_t cmd_id = buffer[0];
    switch (cmd_id) {
    case RECEIVER_CFG_CMD_GET_STATUS:
        handle_get_status();
        break;
    case RECEIVER_CFG_CMD_GET_CONFIG:
        handle_get_config();
        break;
    case RECEIVER_CFG_CMD_SET_CONFIG:
        handle_set_config(buffer, bufsize);
        break;
    case RECEIVER_CFG_CMD_SAVE_CONFIG:
        handle_save_config();
        break;
    case RECEIVER_CFG_CMD_GET_FULL_CONFIG:
        handle_get_full_config();
        break;
    case RECEIVER_CFG_CMD_SET_FULL_CONFIG:
        handle_set_full_config(buffer, bufsize);
        break;
    case RECEIVER_CFG_CMD_SAVE_FULL_CONFIG:
        handle_save_full_config();
        break;
    case RECEIVER_CFG_CMD_SET_AI_PULL:
        handle_set_ai_pull(buffer, bufsize);
        break;
    default:
        send_response(cmd_id, RECEIVER_CFG_STATUS_INVALID_CMD, NULL, 0);
        break;
    }
}
