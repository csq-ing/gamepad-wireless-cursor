#include "usb_gamepad.h"

#include <string.h>
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "usb_config_hid.h"
#include "usb_xinput.h"

static const char *TAG = "usb_gp";

#define USB_STRING_COUNT            6U

#define MS_OS_10_STRING_INDEX       0xEEU
#define MS_VENDOR_CODE              0x20U
#define MS_OS_10_COMPAT_ID_INDEX    0x0004U
#define MS_OS_10_COMPAT_ID_LEN      0x0028U

#define USB_CONFIG_TOTAL_LEN        (TUD_CONFIG_DESC_LEN + USB_XINPUT_DESC_LEN + USB_CONFIG_HID_DESC_LEN)

static uint8_t s_configuration_descriptor[USB_CONFIG_TOTAL_LEN];

static const tusb_desc_device_t s_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200U,
    .bDeviceClass       = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_XINPUT_DEV_VID,
    .idProduct          = CONFIG_XINPUT_DEV_PID,
    .bcdDevice          = 0x0115U,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_string_descriptor[USB_STRING_COUNT] = {
    (char[]){0x09, 0x04},
    "OpenAI Dev Lab",
    "XInput Wireless Gamepad",
    "XINPUTHID0004",
    "Xbox 360 Controller",
    "Receiver Config HID",
};

static const uint8_t s_ms_os_10_compat_id_descriptor[MS_OS_10_COMPAT_ID_LEN] = {
    0x28, 0x00, 0x00, 0x00, /* dwLength */
    0x00, 0x01,             /* bcdVersion 1.0 */
    0x04, 0x00,             /* wIndex: Extended Compat ID */
    0x01,                   /* bCount */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* reserved */
    USB_XINPUT_ITF_INDEX,   /* bFirstInterfaceNumber */
    0x01,                   /* reserved */
    'X', 'U', 'S', 'B', '2', '0', 0x00, 0x00, /* Compatible ID */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Sub Compatible ID */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             /* reserved */
};

#define STRING_DESC_MAX_CHARS 32U
static uint16_t s_string_desc_buf[STRING_DESC_MAX_CHARS];

static void build_configuration_descriptor(void)
{
    uint8_t *p = s_configuration_descriptor;
    const uint16_t total_len = USB_CONFIG_TOTAL_LEN;

    /* Configuration descriptor header */
    p[0] = 0x09;
    p[1] = 0x02;
    p[2] = (uint8_t)(total_len & 0xFFU);
    p[3] = (uint8_t)((total_len >> 8) & 0xFFU);
    p[4] = 0x02; /* bNumInterfaces */
    p[5] = 0x01; /* bConfigurationValue */
    p[6] = 0x00; /* iConfiguration */
    p[7] = 0xA0; /* bmAttributes: bus-powered + remote wakeup */
    p[8] = 0xFA; /* bMaxPower: 500 mA */
    p += TUD_CONFIG_DESC_LEN;

    usb_xinput_copy_interface_descriptor(p, CONFIG_XINPUT_POLL_MS);
    p += USB_XINPUT_DESC_LEN;

    usb_config_hid_copy_interface_descriptor(p, CONFIG_XINPUT_POLL_MS);
}

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return usb_xinput_get_class_driver();
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if ((request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) &&
        (request->bRequest == MS_VENDOR_CODE) &&
        (request->wIndex == MS_OS_10_COMPAT_ID_INDEX)) {
        ESP_LOGI(TAG, "MS OS 1.0 compat-ID descriptor requested");
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)s_ms_os_10_compat_id_descriptor,
                                sizeof(s_ms_os_10_compat_id_descriptor));
    }

    return false;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count = 0;

    if (index == 0U) {
        s_string_desc_buf[1] = 0x0409U;
        chr_count = 1;
    } else if (index == MS_OS_10_STRING_INDEX) {
        s_string_desc_buf[1] = 'M';
        s_string_desc_buf[2] = 'S';
        s_string_desc_buf[3] = 'F';
        s_string_desc_buf[4] = 'T';
        s_string_desc_buf[5] = '1';
        s_string_desc_buf[6] = '0';
        s_string_desc_buf[7] = '0';
        s_string_desc_buf[8] = MS_VENDOR_CODE;
        chr_count = 8;
    } else {
        if (index >= USB_STRING_COUNT) {
            return NULL;
        }

        const char *str = s_string_descriptor[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > (STRING_DESC_MAX_CHARS - 1U)) {
            chr_count = STRING_DESC_MAX_CHARS - 1U;
        }
        for (uint8_t i = 0; i < chr_count; ++i) {
            s_string_desc_buf[1 + i] = str[i];
        }
    }

    s_string_desc_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) |
                                       ((2U * chr_count) + 2U));
    return s_string_desc_buf;
}

void usb_gamepad_set_output_cb(usb_output_cb_t cb)
{
    usb_xinput_set_output_cb(cb);
}

esp_err_t usb_gamepad_init(void)
{
    build_configuration_descriptor();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device            = &s_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = s_configuration_descriptor;
    tusb_cfg.descriptor.string            = s_string_descriptor;
    tusb_cfg.descriptor.string_count      = USB_STRING_COUNT;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = s_configuration_descriptor;
#endif

    ESP_LOGI(TAG, "Installing TinyUSB composite: VID=0x%04X PID=0x%04X poll=%dms",
             CONFIG_XINPUT_DEV_VID, CONFIG_XINPUT_DEV_PID, CONFIG_XINPUT_POLL_MS);

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void usb_gamepad_send_report(const gamepad_packet_t *pkt)
{
    usb_xinput_send_report(pkt);
}

