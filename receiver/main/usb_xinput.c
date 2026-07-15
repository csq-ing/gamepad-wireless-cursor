#include "usb_xinput.h"

#include <string.h>
#include "esp_log.h"
#include "tinyusb.h"
#include "tusb.h"

#define XINPUT_EP_IN              0x81U
#define XINPUT_EP_OUT             0x01U
#define XINPUT_EP_SIZE            32U
#define XINPUT_INPUT_REPORT_LEN   20U
#define XINPUT_OUTPUT_REPORT_LEN  8U

static const char *TAG = "usb_xinput";
static void (*s_output_cb)(uint8_t motor_left, uint8_t motor_right);

/* Custom Xbox 360 class driver endpoint state */
static uint8_t s_ep_in_addr;
static uint8_t s_ep_out_addr;
CFG_TUSB_MEM_ALIGN static uint8_t s_epin_buf[XINPUT_EP_SIZE];
CFG_TUSB_MEM_ALIGN static uint8_t s_epout_buf[XINPUT_EP_SIZE];

/* Interface(9) + VendorDesc(17) + EP_IN(7) + EP_OUT(7) = 40 */
static const uint8_t s_xinput_itf_desc_template[USB_XINPUT_DESC_LEN] = {
    /* Interface 0: Xbox 360 Gamepad */
    0x09, 0x04,
    USB_XINPUT_ITF_INDEX,
    0x00,                       /* bAlternateSetting */
    0x02,                       /* bNumEndpoints */
    0xFF,                       /* bInterfaceClass: Vendor Specific */
    0x5D,                       /* bInterfaceSubClass: XInput */
    0x01,                       /* bInterfaceProtocol: Gamepad */
    0x00,                       /* iInterface */

    /* Xbox 360 vendor-specific capability descriptor (17 bytes) */
    0x11, 0x21, 0x00, 0x01, 0x01, 0x25,
    XINPUT_EP_IN,               /* IN endpoint address */
    0x14,                       /* IN report size (20) */
    0x00, 0x00, 0x00, 0x00, 0x13,
    XINPUT_EP_OUT,              /* OUT endpoint address */
    0x08,                       /* OUT report size (8) */
    0x00, 0x00,

    /* Endpoint IN: Interrupt */
    0x07, 0x05,
    XINPUT_EP_IN,
    0x03,                       /* bmAttributes: Interrupt */
    U16_TO_U8S_LE(XINPUT_EP_SIZE),
    0x00,                       /* bInterval, filled at runtime */

    /* Endpoint OUT: Interrupt */
    0x07, 0x05,
    XINPUT_EP_OUT,
    0x03,
    U16_TO_U8S_LE(XINPUT_EP_SIZE),
    0x00,                       /* bInterval, filled at runtime */
};

static void put_u16le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static uint16_t xinput_buttons_from_mask(uint8_t button_mask)
{
    uint16_t buttons = 0;

    if (button_mask & GAMEPAD_BTN_DPAD_UP) {
        buttons |= (1U << 0);
    }
    if (button_mask & GAMEPAD_BTN_DPAD_DOWN) {
        buttons |= (1U << 1);
    }
    if (button_mask & GAMEPAD_BTN_DPAD_LEFT) {
        buttons |= (1U << 2);
    }
    if (button_mask & GAMEPAD_BTN_DPAD_RIGHT) {
        buttons |= (1U << 3);
    }
    if (button_mask & GAMEPAD_BTN_A) {
        buttons |= (1U << 12);
    }
    if (button_mask & GAMEPAD_BTN_B) {
        buttons |= (1U << 13);
    }
    if (button_mask & GAMEPAD_BTN_X) {
        buttons |= (1U << 14);
    }
    if (button_mask & GAMEPAD_BTN_Y) {
        buttons |= (1U << 15);
    }

    return buttons;
}

static void xbox360_drv_init(void)
{
    s_ep_in_addr = 0;
    s_ep_out_addr = 0;
}

static bool xbox360_drv_deinit(void)
{
    return true;
}

static void xbox360_drv_reset(uint8_t rhport)
{
    (void)rhport;
    s_ep_in_addr = 0;
    s_ep_out_addr = 0;
}

static uint16_t xbox360_drv_open(uint8_t rhport,
                                 tusb_desc_interface_t const *desc_intf,
                                 uint16_t max_len)
{
    TU_VERIFY(desc_intf->bInterfaceClass == 0xFF &&
              desc_intf->bInterfaceSubClass == 0x5D &&
              desc_intf->bInterfaceProtocol == 0x01, 0);

    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    uint8_t const *p_desc = tu_desc_next(desc_intf);
    uint8_t const *desc_end = ((uint8_t const *)desc_intf) + max_len;

    while (p_desc < desc_end) {
        if (tu_desc_type(p_desc) == TUSB_DESC_INTERFACE) {
            break;
        }

        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p_desc;
            TU_ASSERT(usbd_edpt_open(rhport, ep), 0);

            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                s_ep_in_addr = ep->bEndpointAddress;
            } else {
                s_ep_out_addr = ep->bEndpointAddress;
            }
        }

        drv_len += tu_desc_len(p_desc);
        p_desc = tu_desc_next(p_desc);
    }

    if (s_ep_out_addr) {
        TU_ASSERT(usbd_edpt_xfer(rhport, s_ep_out_addr, s_epout_buf, sizeof(s_epout_buf)), 0);
    }

    ESP_LOGI(TAG, "XInput driver opened: ep_in=0x%02X ep_out=0x%02X",
             s_ep_in_addr, s_ep_out_addr);
    return drv_len;
}

static bool xbox360_drv_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                        tusb_control_request_t const *request)
{
    (void)rhport;
    (void)stage;
    (void)request;
    return false;
}

static bool xbox360_drv_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                                xfer_result_t result, uint32_t xferred_bytes)
{
    if (ep_addr == s_ep_in_addr) {
        return true;
    }

    if (ep_addr == s_ep_out_addr && result == XFER_RESULT_SUCCESS) {
        if (xferred_bytes >= XINPUT_OUTPUT_REPORT_LEN && s_output_cb) {
            s_output_cb(s_epout_buf[3], s_epout_buf[4]);
        }
        usbd_edpt_xfer(rhport, s_ep_out_addr, s_epout_buf, sizeof(s_epout_buf));
        return true;
    }

    return false;
}

static const usbd_class_driver_t s_xbox360_driver = {
    .name = "XBOX360",
    .init = xbox360_drv_init,
    .deinit = xbox360_drv_deinit,
    .reset = xbox360_drv_reset,
    .open = xbox360_drv_open,
    .control_xfer_cb = xbox360_drv_control_xfer_cb,
    .xfer_cb = xbox360_drv_xfer_cb,
    .xfer_isr = NULL,
    .sof = NULL,
};

void usb_xinput_copy_interface_descriptor(uint8_t *dst, uint8_t poll_interval_ms)
{
    if (dst == NULL) {
        return;
    }

    memcpy(dst, s_xinput_itf_desc_template, sizeof(s_xinput_itf_desc_template));
    dst[32] = poll_interval_ms;
    dst[39] = poll_interval_ms;
}

const usbd_class_driver_t *usb_xinput_get_class_driver(void)
{
    return &s_xbox360_driver;
}

void usb_xinput_set_output_cb(void (*cb)(uint8_t motor_left, uint8_t motor_right))
{
    s_output_cb = cb;
}

void usb_xinput_send_report(const gamepad_packet_t *pkt)
{
    if (pkt == NULL) {
        return;
    }

    if (!tud_mounted() || !s_ep_in_addr) {
        return;
    }

    if (!usbd_edpt_claim(0, s_ep_in_addr)) {
        return;
    }

    uint8_t report[XINPUT_INPUT_REPORT_LEN];
    memset(report, 0, sizeof(report));

    report[0] = 0x00;
    report[1] = 0x14;

    uint16_t buttons = xinput_buttons_from_mask(pkt->input.buttons);

    report[2] = (uint8_t)(buttons & 0xFFU);
    report[3] = (uint8_t)((buttons >> 8) & 0xFFU);

    report[4] = pkt->input.lt;
    report[5] = pkt->input.rt;

    put_u16le(&report[6], (uint16_t)pkt->input.lx);
    put_u16le(&report[8], (uint16_t)pkt->input.ly);
    put_u16le(&report[10], (uint16_t)pkt->input.rx);
    put_u16le(&report[12], (uint16_t)pkt->input.ry);

    memcpy(s_epin_buf, report, sizeof(report));
    if (!usbd_edpt_xfer(0, s_ep_in_addr, s_epin_buf, sizeof(report))) {
        usbd_edpt_release(0, s_ep_in_addr);
    }
}
