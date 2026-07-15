#include "ze300.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "bsp_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ZE300_UART_NUM     1
#define ZE300_UART_BAUD    115200
#define ZE300_UART_RX_BUF  512
#define ZE300_CAN_ADDR     0x01
#define CAN_MAX_DATA_LEN   8

#define CMD_DISABLE        0xCF
#define CMD_TORQUE         0xC0
#define CMD_SPEED          0xC1
#define CMD_ABS_POS        0xC2
#define CMD_REL_POS        0xC3
#define CMD_GO_ORIGIN      0xC4
#define CMD_READ_VERSION   0xA0
#define CMD_READ_SPEED     0xA2
#define CMD_READ_ANGLE     0xA3
#define CMD_READ_ALL       0xA4
#define CMD_READ_STATUS    0xAE
#define CMD_CLEAR_FAULT    0xAF
#define CMD_SET_ORIGIN     0xB1

#define SPEED_TO_RAW(rpm)    ((int32_t)((rpm) * 100))
#define POS_TO_RAW(deg)      ((int32_t)((deg) * 16384.0f / 360))
#define RAW_TO_SPEED(r)      ((float)(r) / 100.0f)
#define RAW_TO_ANGLE(r)      ((float)(r) * 360.0f / 16384.0f)

static const char *TAG = "ze300";
static bool s_uart_ready = false;

static size_t pack_frame(uint8_t *out, uint32_t id, const uint8_t *data, uint8_t dlc)
{
    out[0] = 0x00;
    out[1] = (uint8_t)(id >> 24);
    out[2] = (uint8_t)(id >> 16);
    out[3] = (uint8_t)(id >> 8);
    out[4] = (uint8_t)id;
    out[5] = dlc;
    if (dlc && data) {
        memcpy(out + 6, data, dlc);
    }
    return 6 + dlc;
}

static esp_err_t unpack_frame(const uint8_t *buf, size_t len, uint32_t *out_id, uint8_t *out_data, uint8_t *out_dlc)
{
    if (len < 6 || (buf[0] != 0x00 && buf[0] != 0x02)) {
        return ESP_FAIL;
    }
    const uint8_t dlc = buf[5];
    if (dlc > 8 || len < 6 + dlc) {
        return ESP_FAIL;
    }
    if (out_id) {
        *out_id = ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) |
                  ((uint32_t)buf[3] << 8) | buf[4];
    }
    if (out_dlc) {
        *out_dlc = dlc;
    }
    if (out_data && dlc) {
        memcpy(out_data, buf + 6, dlc);
    }
    return ESP_OK;
}

static esp_err_t uart_tx(const uint8_t *data, size_t len)
{
    int written = uart_write_bytes(ZE300_UART_NUM, (const char *)data, len);
    if (written != (int)len) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(ZE300_UART_NUM, pdMS_TO_TICKS(100));
}

static esp_err_t uart_rx_skip_to_header(uint8_t *buf, size_t want_len, uint32_t timeout_ms)
{
    size_t got = 0;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t deadline = pdMS_TO_TICKS(timeout_ms);
    int discarded = 0;

    while (got < want_len) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        int r = uart_read_bytes(ZE300_UART_NUM, buf + got, want_len - got, deadline - elapsed);
        if (r < 0) {
            return ESP_FAIL;
        }
        got += r;
        while (got > 0 && buf[0] != 0x00) {
            memmove(buf, buf + 1, --got);
            discarded++;
            int r2 = uart_read_bytes(ZE300_UART_NUM, buf + got, 1, pdMS_TO_TICKS(30));
            if (r2 == 1) {
                got++;
            }
        }
    }
    if (discarded) {
        printf("[ze300] skipped %d dirty bytes\n", discarded);
    }
    return ESP_OK;
}

static esp_err_t ze300_send_command_internal(uint8_t cmd, const uint8_t *payload, uint8_t payload_len,
                                             uint8_t *response, uint8_t expected_resp_len,
                                             uint32_t timeout_ms)
{
    uint8_t data[CAN_MAX_DATA_LEN];
    data[0] = cmd;
    if (payload_len && payload) {
        memcpy(data + 1, payload, payload_len);
    }

    if (!s_uart_ready) {
        esp_err_t e = ze300_init();
        if (e != ESP_OK) {
            return e;
        }
    }

    uint8_t tx[14];
    size_t tx_len = pack_frame(tx, ZE300_CAN_ADDR, data, 1 + payload_len);
    uart_flush_input(ZE300_UART_NUM);
    esp_err_t err = uart_tx(tx, tx_len);
    if (err != ESP_OK) {
        return err;
    }

    if (!response || !expected_resp_len) {
        return ESP_OK;
    }

    uint8_t buf[14];
    err = uart_rx_skip_to_header(buf, 6 + expected_resp_len, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cmd 0x%02X no response: %d", cmd, err);
        return err;
    }

    uint8_t rdata[8];
    uint8_t rdlc = 0;
    if (unpack_frame(buf, 6 + expected_resp_len, NULL, rdata, &rdlc) != ESP_OK) {
        printf("[ze300] bad frame raw:");
        for (size_t i = 0; i < 6 + expected_resp_len; ++i) {
            printf(" %02X", buf[i]);
        }
        printf("\n");
        return ESP_FAIL;
    }
    memcpy(response, rdata, expected_resp_len);
    return ESP_OK;
}

esp_err_t ze300_init(void)
{
    if (s_uart_ready) {
        return ESP_OK;
    }

    uart_config_t cfg = {
        .baud_rate = ZE300_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(ZE300_UART_NUM, ZE300_UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = uart_param_config(ZE300_UART_NUM, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(ZE300_UART_NUM, ZE300_TX_GPIO, ZE300_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_mode(ZE300_UART_NUM, UART_MODE_UART);
    if (err != ESP_OK) {
        return err;
    }

    s_uart_ready = true;
    printf("[ze300] UART%d ready TX=%d RX=%d baud=%d\n", ZE300_UART_NUM, ZE300_TX_GPIO, ZE300_RX_GPIO, ZE300_UART_BAUD);
    return ESP_OK;
}

esp_err_t ze300_set_torque(int32_t current_ma)
{
    uint8_t payload[4];
    payload[0] = (uint8_t)(current_ma & 0xFF);
    payload[1] = (uint8_t)((current_ma >> 8) & 0xFF);
    payload[2] = (uint8_t)((current_ma >> 16) & 0xFF);
    payload[3] = (uint8_t)((current_ma >> 24) & 0xFF);
    esp_err_t err = ze300_send_command_internal(CMD_TORQUE, payload, 4, NULL, 0, 100);
    return err;
}

esp_err_t ze300_read_state(ze300_state_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t response[8] = {0};
    esp_err_t err = ze300_send_command_internal(CMD_READ_ALL, NULL, 0, response, 8, 200);
    if (err != ESP_OK) {
        return err;
    }

    out->temperature_c = (float)response[1];
    out->q_current_a = ((int16_t)(response[2] | (response[3] << 8))) * 0.001f;
    out->speed_rpm = ((int16_t)(response[4] | (response[5] << 8))) * 0.01f;
    out->angle_deg = RAW_TO_ANGLE((uint16_t)(response[6] | (response[7] << 8)));
    out->bus_voltage_v = 0.0f;
    out->bus_current_a = 0.0f;
    out->work_mode = 0;
    out->fault_code = 0;
    return ESP_OK;
}

esp_err_t ze300_clear_fault(void)
{
    return ze300_send_command_internal(CMD_CLEAR_FAULT, NULL, 0, NULL, 0, 200);
}
