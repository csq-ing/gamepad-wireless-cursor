/**
 * @file bmi088.c
 * @brief BMI088 6-axis IMU driver for ESP-IDF (SPI interface)
 *
 * Adapted from the project: DM-legged_car2025_1_10 User/Devices/BMI088
 * Changed from STM32 HAL to ESP-IDF spi_device API.
 *
 * BMI088 has two internal chips sharing one SPI bus:
 *   - Accelerometer: CS_ACCEL, chip ID = 0x1E
 *   - Gyroscope:     CS_GYRO,  chip ID = 0x0F
 *
 * SPI mode 3 (CPOL=1, CPHA=1), MSB first.
 */

#include "bmi088.h"
#include "bmi088_reg.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bmi088";

#define BMI088_SPI_MODE 3
#define BMI088_SPI_DUMMY 0x00
#define BMI088_BITBANG_DELAY_US 2
#define BMI088_ACC_POWER_DELAY_MS 5
#define BMI088_ACC_CONF_DELAY_MS 40
#define BMI088_SPI_MAX_XFER 16

/* ---------- sensitivity tables (SI units) ---------- */

const float bmi088_accel_sensitivity[4] = {
    0.0008974358974f,   /* 3G  */
    0.00179443359375f,  /* 6G  */
    0.0035888671875f,   /* 12G */
    0.007177734375f,    /* 24G */
};

const float bmi088_gyro_sensitivity[5] = {
    0.00106526443603169530f,   /* 2000 */
    0.00053263221801584765f,   /* 1000 */
    0.00026631610900792382f,   /* 500  */
    0.00013315805450396191f,   /* 250  */
    0.00006657902725198096f,   /* 125  */
};

/* ---------- static state ---------- */

static spi_device_handle_t s_spi    = NULL;
static bool                s_inited = false;
static bool                s_bus_inited = false;

static int s_cs_accel = -1;
static int s_cs_gyro  = -1;
static int s_spi_mosi = -1;
static int s_spi_miso = -1;
static int s_spi_sclk = -1;

static uint8_t s_accel_range_code = 0;
static uint8_t s_gyro_range_code  = 0;

static float s_accel_scale = 0.0f;
static float s_gyro_scale  = 0.0f;

/* ---------- low-level SPI helpers ---------- */

static inline void cs_low(int gpio)  { gpio_set_level(gpio, 0); }
static inline void cs_high(int gpio) { gpio_set_level(gpio, 1); }

static void log_gpio_levels(const char *stage)
{
    if (s_spi_sclk < 0 || s_spi_mosi < 0 || s_spi_miso < 0 ||
        s_cs_accel < 0 || s_cs_gyro < 0) {
        return;
    }

    ESP_LOGW(TAG, "gpio %s: SCK=%d MOSI=%d MISO=%d CS_ACC=%d CS_GYR=%d",
             stage,
             gpio_get_level(s_spi_sclk),
             gpio_get_level(s_spi_mosi),
             gpio_get_level(s_spi_miso),
             gpio_get_level(s_cs_accel),
             gpio_get_level(s_cs_gyro));
}

static esp_err_t spi_transfer_bytes(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {0};

    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    esp_err_t ret = spi_device_transmit(s_spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI transfer failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* --- accel register access --- */

static void accel_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t tx[2] = {reg, data};

    cs_low(s_cs_accel);
    (void)spi_transfer_bytes(tx, NULL, sizeof(tx));
    cs_high(s_cs_accel);
}

static uint8_t accel_read_reg_on_cs(int cs_gpio, uint8_t reg)
{
    uint8_t tx[3] = {reg | 0x80, BMI088_SPI_DUMMY, BMI088_SPI_DUMMY};
    uint8_t rx[3] = {0};

    cs_low(cs_gpio);
    esp_err_t ret = spi_transfer_bytes(tx, rx, sizeof(tx));
    cs_high(cs_gpio);

    return (ret == ESP_OK) ? rx[2] : 0;
}

static uint8_t accel_read_reg(uint8_t reg)
{
    return accel_read_reg_on_cs(s_cs_accel, reg);
}

static void accel_read_multi(uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t tx[BMI088_SPI_MAX_XFER] = {0};
    uint8_t rx[BMI088_SPI_MAX_XFER] = {0};
    const size_t total_len = (size_t)len + 2;

    if (total_len > BMI088_SPI_MAX_XFER) {
        ESP_LOGE(TAG, "Accel SPI read too long: %u bytes", (unsigned)len);
        memset(buf, 0, len);
        return;
    }

    tx[0] = reg | 0x80;

    cs_low(s_cs_accel);
    esp_err_t ret = spi_transfer_bytes(tx, rx, total_len);
    cs_high(s_cs_accel);

    if (ret == ESP_OK) {
        memcpy(buf, &rx[2], len);
    } else {
        memset(buf, 0, len);
    }
}

/* --- gyro register access --- */

static void gyro_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t tx[2] = {reg, data};

    cs_low(s_cs_gyro);
    (void)spi_transfer_bytes(tx, NULL, sizeof(tx));
    cs_high(s_cs_gyro);
}

static uint8_t gyro_read_reg_on_cs(int cs_gpio, uint8_t reg)
{
    uint8_t tx[2] = {reg | 0x80, BMI088_SPI_DUMMY};
    uint8_t rx[2] = {0};

    cs_low(cs_gpio);
    esp_err_t ret = spi_transfer_bytes(tx, rx, sizeof(tx));
    cs_high(cs_gpio);

    return (ret == ESP_OK) ? rx[1] : 0;
}

static uint8_t gyro_read_reg(uint8_t reg)
{
    return gyro_read_reg_on_cs(s_cs_gyro, reg);
}

static void gyro_read_multi(uint8_t reg, uint8_t *buf, uint8_t len)
{
    uint8_t tx[BMI088_SPI_MAX_XFER] = {0};
    uint8_t rx[BMI088_SPI_MAX_XFER] = {0};
    const size_t total_len = (size_t)len + 1;

    if (total_len > BMI088_SPI_MAX_XFER) {
        ESP_LOGE(TAG, "Gyro SPI read too long: %u bytes", (unsigned)len);
        memset(buf, 0, len);
        return;
    }

    tx[0] = reg | 0x80;

    cs_low(s_cs_gyro);
    esp_err_t ret = spi_transfer_bytes(tx, rx, total_len);
    cs_high(s_cs_gyro);

    if (ret == ESP_OK) {
        memcpy(buf, &rx[1], len);
    } else {
        memset(buf, 0, len);
    }
}

/* ---------- init helpers ---------- */

static esp_err_t init_spi_bus(const bmi088_config_t *cfg)
{
    const bmi088_spi_pins_t *p = &cfg->pins;

    s_spi_mosi = p->spi_mosi_pin;
    s_spi_miso = p->spi_miso_pin;
    s_spi_sclk = p->spi_sclk_pin;
    s_cs_accel = p->cs_accel_pin;
    s_cs_gyro  = p->cs_gyro_pin;

    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << p->cs_accel_pin) | (1ULL << p->cs_gyro_pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cs_cfg));
    cs_high(p->cs_accel_pin);
    cs_high(p->cs_gyro_pin);
    gpio_set_pull_mode(p->spi_sclk_pin, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(p->spi_mosi_pin, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(p->spi_miso_pin, GPIO_PULLUP_ONLY);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = p->spi_mosi_pin,
        .miso_io_num     = p->spi_miso_pin,
        .sclk_io_num     = p->spi_sclk_pin,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .max_transfer_sz = 32,
    };

    spi_device_interface_config_t dev_cfg = {
        .mode            = BMI088_SPI_MODE,
        .clock_speed_hz  = cfg->spi_clock_hz,
        .spics_io_num    = -1,
        .queue_size      = 4,
        .flags           = 0,
    };

    esp_err_t ret;

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %d", ret);
        return ret;
    }
    s_bus_inited = true;

    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %d", ret);
        spi_bus_free(SPI2_HOST);
        s_bus_inited = false;
        return ret;
    }

    return ESP_OK;
}

static void debug_release_spi_for_bitbang(void)
{
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_bus_inited) {
        spi_bus_free(SPI2_HOST);
        s_bus_inited = false;
    }
}

static void bitbang_delay(void)
{
    esp_rom_delay_us(BMI088_BITBANG_DELAY_US);
}

static void bitbang_set_idle(int mode)
{
    gpio_set_level(s_spi_sclk, (mode >> 1) & 0x01);
}

static uint8_t bitbang_transfer(uint8_t tx, int mode)
{
    const int cpol = (mode >> 1) & 0x01;
    const int cpha = mode & 0x01;
    uint8_t rx = 0;

    bitbang_set_idle(mode);
    for (int bit = 7; bit >= 0; bit--) {
        const int mosi_bit = (tx >> bit) & 0x01;

        if (!cpha) {
            gpio_set_level(s_spi_mosi, mosi_bit);
            bitbang_delay();
            gpio_set_level(s_spi_sclk, !cpol);
            bitbang_delay();
            rx = (uint8_t)((rx << 1) | gpio_get_level(s_spi_miso));
            gpio_set_level(s_spi_sclk, cpol);
            bitbang_delay();
        } else {
            gpio_set_level(s_spi_sclk, !cpol);
            gpio_set_level(s_spi_mosi, mosi_bit);
            bitbang_delay();
            gpio_set_level(s_spi_sclk, cpol);
            bitbang_delay();
            rx = (uint8_t)((rx << 1) | gpio_get_level(s_spi_miso));
            bitbang_delay();
        }
    }

    return rx;
}

static uint8_t bitbang_accel_read_reg_on_cs(int cs_gpio, uint8_t reg, int mode)
{
    uint8_t val;

    cs_high(s_cs_accel);
    cs_high(s_cs_gyro);
    bitbang_set_idle(mode);
    bitbang_delay();
    cs_low(cs_gpio);
    bitbang_delay();
    bitbang_transfer(reg | 0x80, mode);
    bitbang_transfer(BMI088_SPI_DUMMY, mode);
    val = bitbang_transfer(BMI088_SPI_DUMMY, mode);
    cs_high(cs_gpio);
    bitbang_delay();

    return val;
}

static uint8_t bitbang_gyro_read_reg_on_cs(int cs_gpio, uint8_t reg, int mode)
{
    uint8_t val;

    cs_high(s_cs_accel);
    cs_high(s_cs_gyro);
    bitbang_set_idle(mode);
    bitbang_delay();
    cs_low(cs_gpio);
    bitbang_delay();
    bitbang_transfer(reg | 0x80, mode);
    val = bitbang_transfer(BMI088_SPI_DUMMY, mode);
    cs_high(cs_gpio);
    bitbang_delay();

    return val;
}

static void debug_bitbang_probe_all_modes(void)
{
    debug_release_spi_for_bitbang();

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << s_spi_sclk) | (1ULL << s_spi_mosi) |
                        (1ULL << s_cs_accel) | (1ULL << s_cs_gyro),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << s_spi_miso),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    gpio_config(&out_cfg);
    gpio_config(&in_cfg);
    cs_high(s_cs_accel);
    cs_high(s_cs_gyro);
    gpio_set_level(s_spi_mosi, 1);
    bitbang_set_idle(BMI088_SPI_MODE);
    vTaskDelay(pdMS_TO_TICKS(10));
    log_gpio_levels("bitbang idle");

    for (int mode = 0; mode < 4; mode++) {
        const uint8_t acc_on_acc = bitbang_accel_read_reg_on_cs(s_cs_accel, BMI088_ACC_CHIP_ID, mode);
        const uint8_t gyr_on_acc = bitbang_gyro_read_reg_on_cs(s_cs_accel, BMI088_GYRO_CHIP_ID, mode);
        const uint8_t acc_on_gyr = bitbang_accel_read_reg_on_cs(s_cs_gyro, BMI088_ACC_CHIP_ID, mode);
        const uint8_t gyr_on_gyr = bitbang_gyro_read_reg_on_cs(s_cs_gyro, BMI088_GYRO_CHIP_ID, mode);

        ESP_LOGW(TAG, "bitbang mode%d CS_ACC: accel_id=0x%02X gyro_id=0x%02X; CS_GYR: accel_id=0x%02X gyro_id=0x%02X",
                 mode, acc_on_acc, gyr_on_acc, acc_on_gyr, gyr_on_gyr);
    }
}

static void log_probe_ids(void)
{
    uint8_t acc_on_acc = accel_read_reg_on_cs(s_cs_accel, BMI088_ACC_CHIP_ID);
    uint8_t gyr_on_acc = gyro_read_reg_on_cs(s_cs_accel, BMI088_GYRO_CHIP_ID);
    uint8_t acc_on_gyr = accel_read_reg_on_cs(s_cs_gyro, BMI088_ACC_CHIP_ID);
    uint8_t gyr_on_gyr = gyro_read_reg_on_cs(s_cs_gyro, BMI088_GYRO_CHIP_ID);

    ESP_LOGW(TAG, "probe CS_ACC(GPIO%d): accel_id=0x%02X gyro_id=0x%02X",
             s_cs_accel, acc_on_acc, gyr_on_acc);
    ESP_LOGW(TAG, "probe CS_GYR(GPIO%d): accel_id=0x%02X gyro_id=0x%02X",
             s_cs_gyro, acc_on_gyr, gyr_on_gyr);
}

static void accel_set_active_power(void)
{
    accel_write_reg(BMI088_ACC_PWR_CONF, BMI088_ACC_PWR_ACTIVE_MODE);
    vTaskDelay(pdMS_TO_TICKS(BMI088_ACC_POWER_DELAY_MS));

    accel_write_reg(BMI088_ACC_PWR_CTRL, BMI088_ACC_ENABLE_ACC_ON);
    vTaskDelay(pdMS_TO_TICKS(BMI088_ACC_POWER_DELAY_MS));
}

static esp_err_t init_accel(const bmi088_config_t *cfg)
{
    uint8_t id;

    id = accel_read_reg(BMI088_ACC_CHIP_ID);
    vTaskDelay(pdMS_TO_TICKS(1));
    id = accel_read_reg(BMI088_ACC_CHIP_ID);
    vTaskDelay(pdMS_TO_TICKS(1));

    if (id != BMI088_ACC_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "Accel chip ID mismatch: got 0x%02X, expected 0x%02X",
                 id, BMI088_ACC_CHIP_ID_VALUE);
        log_probe_ids();
        log_gpio_levels("after failed hw probe");
        debug_bitbang_probe_all_modes();
        return ESP_ERR_NOT_FOUND;
    }
    accel_write_reg(BMI088_ACC_SOFTRESET, BMI088_ACC_SOFTRESET_VALUE);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* After soft-reset, accel enters suspend mode — re-power before re-checking */
    accel_set_active_power();

    id = accel_read_reg(BMI088_ACC_CHIP_ID);
    if (id != BMI088_ACC_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "Accel missing after reset + power-on");
        return ESP_ERR_NOT_FOUND;
    }

    accel_write_reg(BMI088_ACC_CONF, cfg->accel_odr);
    vTaskDelay(pdMS_TO_TICKS(BMI088_ACC_CONF_DELAY_MS));

    accel_write_reg(BMI088_ACC_RANGE, cfg->accel_range);
    vTaskDelay(pdMS_TO_TICKS(1));

    accel_write_reg(BMI088_INT1_IO_CTRL,
                    BMI088_ACC_INT1_IO_ENABLE |
                    BMI088_ACC_INT1_GPIO_PP   |
                    BMI088_ACC_INT1_GPIO_LOW);
    vTaskDelay(pdMS_TO_TICKS(1));

    accel_write_reg(BMI088_INT_MAP_DATA,
                    BMI088_ACC_INT1_DRDY);
    vTaskDelay(pdMS_TO_TICKS(1));

    s_accel_range_code = cfg->accel_range & 0x03;
    s_accel_scale      = bmi088_accel_sensitivity[s_accel_range_code];

    return ESP_OK;
}

static esp_err_t init_gyro(const bmi088_config_t *cfg)
{
    uint8_t id;

    id = gyro_read_reg(BMI088_GYRO_CHIP_ID);
    vTaskDelay(pdMS_TO_TICKS(1));
    id = gyro_read_reg(BMI088_GYRO_CHIP_ID);
    vTaskDelay(pdMS_TO_TICKS(1));

    if (id != BMI088_GYRO_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "Gyro chip ID mismatch: got 0x%02X, expected 0x%02X",
                 id, BMI088_GYRO_CHIP_ID_VALUE);
        return ESP_ERR_NOT_FOUND;
    }
    gyro_write_reg(BMI088_GYRO_SOFTRESET, BMI088_GYRO_SOFTRESET_VALUE);
    vTaskDelay(pdMS_TO_TICKS(80));

    id = gyro_read_reg(BMI088_GYRO_CHIP_ID);
    if (id != BMI088_GYRO_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "Gyro missing after reset");
        return ESP_ERR_NOT_FOUND;
    }

    gyro_write_reg(BMI088_GYRO_RANGE, cfg->gyro_range);
    vTaskDelay(pdMS_TO_TICKS(1));

    gyro_write_reg(BMI088_GYRO_BANDWIDTH, cfg->gyro_bandwidth);
    vTaskDelay(pdMS_TO_TICKS(1));

    gyro_write_reg(BMI088_GYRO_LPM1, BMI088_GYRO_NORMAL_MODE);
    vTaskDelay(pdMS_TO_TICKS(1));

    gyro_write_reg(BMI088_GYRO_CTRL, BMI088_DRDY_ON);
    vTaskDelay(pdMS_TO_TICKS(1));

    gyro_write_reg(BMI088_GYRO_INT3_INT4_IO_CONF,
                   BMI088_GYRO_INT3_GPIO_PP | BMI088_GYRO_INT3_GPIO_LOW);
    vTaskDelay(pdMS_TO_TICKS(1));

    gyro_write_reg(BMI088_GYRO_INT3_INT4_IO_MAP,
                   BMI088_GYRO_DRDY_IO_INT3);
    vTaskDelay(pdMS_TO_TICKS(1));

    s_gyro_range_code = cfg->gyro_range & 0x07;
    s_gyro_scale      = bmi088_gyro_sensitivity[s_gyro_range_code];

    return ESP_OK;
}

/* ---------- public API ---------- */

esp_err_t bmi088_init(const bmi088_config_t *config)
{
    if (s_inited) {
        ESP_LOGW(TAG, "Already initialised, re-initing");
        bmi088_deinit();
    }

    if (config->pins.cs_accel_pin < 0 || config->pins.cs_gyro_pin < 0 ||
        config->pins.spi_mosi_pin < 0 || config->pins.spi_miso_pin < 0 ||
        config->pins.spi_sclk_pin < 0) {
        ESP_LOGE(TAG, "All SPI pins must be set");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    ret = init_spi_bus(config);
    if (ret != ESP_OK) return ret;

    ret = init_accel(config);
    if (ret != ESP_OK) goto fail;

    ret = init_gyro(config);
    if (ret != ESP_OK) goto fail;

    s_inited = true;
    ESP_LOGI(TAG, "BMI088 initialised (accel %dG, gyro %d dps)",
             (int[]){3, 6, 12, 24}[s_accel_range_code],
             (int[]){2000, 1000, 500, 250, 125}[s_gyro_range_code]);
    return ESP_OK;

fail:
    bmi088_deinit();
    return ret;
}

esp_err_t bmi088_deinit(void)
{
    s_inited = false;
    if (s_spi) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_bus_inited) {
        spi_bus_free(SPI2_HOST);
        s_bus_inited = false;
    }
    s_cs_accel = s_cs_gyro = -1;
    s_spi_mosi = s_spi_miso = s_spi_sclk = -1;
    return ESP_OK;
}

esp_err_t bmi088_read_all(bmi088_data_t *out)
{
    uint8_t buf[8] = {0};

    accel_read_multi(BMI088_ACCEL_XOUT_L, buf, 6);
    int16_t raw;
    raw = (int16_t)((buf[1] << 8) | buf[0]);
    out->accel[0] = (float)raw * s_accel_scale;
    raw = (int16_t)((buf[3] << 8) | buf[2]);
    out->accel[1] = (float)raw * s_accel_scale;
    raw = (int16_t)((buf[5] << 8) | buf[4]);
    out->accel[2] = (float)raw * s_accel_scale;

    accel_read_multi(BMI088_TEMP_M, buf, 2);
    int16_t temp_raw = (int16_t)((buf[0] << 3) | (buf[1] >> 5));
    if (temp_raw > 1023) temp_raw -= 2048;
    out->temperature = (float)temp_raw * 0.125f + 23.0f;

    gyro_read_multi(BMI088_GYRO_X_L, buf, 6);
    raw = (int16_t)((buf[1] << 8) | buf[0]);
    out->gyro[0] = (float)raw * s_gyro_scale;
    raw = (int16_t)((buf[3] << 8) | buf[2]);
    out->gyro[1] = (float)raw * s_gyro_scale;
    raw = (int16_t)((buf[5] << 8) | buf[4]);
    out->gyro[2] = (float)raw * s_gyro_scale;

    return ESP_OK;
}

esp_err_t bmi088_read_accel(float out[3])
{
    uint8_t buf[6];
    accel_read_multi(BMI088_ACCEL_XOUT_L, buf, 6);

    int16_t raw = (int16_t)((buf[1] << 8) | buf[0]);
    out[0] = (float)raw * s_accel_scale;
    raw = (int16_t)((buf[3] << 8) | buf[2]);
    out[1] = (float)raw * s_accel_scale;
    raw = (int16_t)((buf[5] << 8) | buf[4]);
    out[2] = (float)raw * s_accel_scale;

    return ESP_OK;
}

esp_err_t bmi088_read_gyro(float out[3])
{
    uint8_t buf[6];
    gyro_read_multi(BMI088_GYRO_X_L, buf, 6);

    int16_t raw = (int16_t)((buf[1] << 8) | buf[0]);
    out[0] = (float)raw * s_gyro_scale;
    raw = (int16_t)((buf[3] << 8) | buf[2]);
    out[1] = (float)raw * s_gyro_scale;
    raw = (int16_t)((buf[5] << 8) | buf[4]);
    out[2] = (float)raw * s_gyro_scale;

    return ESP_OK;
}

esp_err_t bmi088_read_temperature(float *out)
{
    uint8_t buf[2];
    accel_read_multi(BMI088_TEMP_M, buf, 2);

    int16_t raw = (int16_t)((buf[0] << 3) | (buf[1] >> 5));
    if (raw > 1023) raw -= 2048;
    *out = (float)raw * 0.125f + 23.0f;

    return ESP_OK;
}

bool bmi088_self_test(void)
{
    uint8_t id = accel_read_reg(BMI088_ACC_CHIP_ID);
    if (id != BMI088_ACC_CHIP_ID_VALUE) return false;

    id = gyro_read_reg(BMI088_GYRO_CHIP_ID);
    return (id == BMI088_GYRO_CHIP_ID_VALUE);
}

esp_err_t bmi088_calibrate_gyro(float offset[3], uint32_t n)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    float sum[3] = {0, 0, 0};
    uint8_t buf[6];

    for (uint32_t i = 0; i < n; i++) {
        gyro_read_multi(BMI088_GYRO_X_L, buf, 6);
        int16_t raw = (int16_t)((buf[1] << 8) | buf[0]);
        sum[0] += (float)raw * s_gyro_scale;
        raw = (int16_t)((buf[3] << 8) | buf[2]);
        sum[1] += (float)raw * s_gyro_scale;
        raw = (int16_t)((buf[5] << 8) | buf[4]);
        sum[2] += (float)raw * s_gyro_scale;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    offset[0] = sum[0] / (float)n;
    offset[1] = sum[1] / (float)n;
    offset[2] = sum[2] / (float)n;

    ESP_LOGI(TAG, "Gyro offset: %.6f, %.6f, %.6f rad/s",
             (double)offset[0], (double)offset[1], (double)offset[2]);
    return ESP_OK;
}
