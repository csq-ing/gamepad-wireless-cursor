#include "receiver_status.h"

#include <limits.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "receiver_config.h"
#include "tusb.h"

#define CONTROLLER_LINK_TIMEOUT_MS 1500U

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_has_input = false;
static uint32_t s_last_input_ms = 0;
static uint16_t s_last_downward_accel = 0;
static bool s_last_trigger_active = false;

static uint32_t now_ms(void)
{
    uint64_t us = (uint64_t)esp_timer_get_time();
    return (uint32_t)(us / 1000ULL);
}

void receiver_status_init(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_has_input = false;
    s_last_input_ms = 0;
    s_last_downward_accel = 0;
    s_last_trigger_active = false;
    taskEXIT_CRITICAL(&s_status_lock);
}

void receiver_status_on_input(const gamepad_packet_t *pkt)
{
    if (pkt == NULL) {
        return;
    }

    const uint16_t downward = pkt->input.lt;
    taskENTER_CRITICAL(&s_status_lock);
    s_last_input_ms = now_ms();
    s_has_input = true;
    s_last_downward_accel = downward;
    s_last_trigger_active = (downward > 0U);
    taskEXIT_CRITICAL(&s_status_lock);
}

void receiver_status_get_snapshot(receiver_status_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->protocol_version = RECEIVER_CONFIG_PROTOCOL_VERSION;
    snapshot->usb_online = tud_mounted();
    snapshot->trigger_target = receiver_config_get_runtime_target();

    taskENTER_CRITICAL(&s_status_lock);
    snapshot->downward_accel = s_last_downward_accel;
    snapshot->trigger_active = s_last_trigger_active;

    if (!s_has_input) {
        snapshot->controller_connected = false;
        snapshot->last_input_age_ms = UINT_MAX;
        taskEXIT_CRITICAL(&s_status_lock);
        return;
    }

    const uint32_t current_ms = now_ms();
    const uint32_t age = current_ms - s_last_input_ms;
    snapshot->last_input_age_ms = age;
    snapshot->controller_connected = (age <= CONTROLLER_LINK_TIMEOUT_MS);
    taskEXIT_CRITICAL(&s_status_lock);
}

