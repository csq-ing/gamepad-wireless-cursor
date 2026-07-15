#pragma once

#include <stdint.h>

#if defined(_MSC_VER)
#define GAMEPAD_PACKED
#else
#define GAMEPAD_PACKED __attribute__((packed))
#endif

/* Button bitmask definitions */
#define GAMEPAD_BTN_A      (1 << 0)
#define GAMEPAD_BTN_B      (1 << 1)
#define GAMEPAD_BTN_X      (1 << 2)
#define GAMEPAD_BTN_Y      (1 << 3)
#define GAMEPAD_BTN_DPAD_UP    (1 << 4)
#define GAMEPAD_BTN_DPAD_DOWN  (1 << 5)
#define GAMEPAD_BTN_DPAD_LEFT  (1 << 6)
#define GAMEPAD_BTN_DPAD_RIGHT (1 << 7)
#define GAMEPAD_TRIGGER_TARGET_LT 1
#define GAMEPAD_TRIGGER_TARGET_RT 2

/* ESP-NOW channel; both sides must match. */
#define ESPNOW_CHANNEL     1

typedef enum {
    PKT_GAMEPAD_INPUT = 0x01,
    PKT_VIBRATION_FB  = 0x02,
    PKT_LOG_MSG       = 0x03,
    PKT_STICK_CONFIG  = 0x04,
    PKT_AI_PULL       = 0x05,
} packet_type_t;

typedef struct GAMEPAD_PACKED {
    uint8_t type;       /* packet_type_t */
    union {
        struct {
            int16_t lx, ly;     /* left  stick X / Y  (-32768 ~ 32767) */
            int16_t rx, ry;     /* right stick X / Y */
            uint8_t buttons;    /* GAMEPAD_BTN_* bitmask */
            uint8_t lt;         /* left  trigger 0-255 */
            uint8_t rt;         /* right trigger 0-255 */
        } input;                /* 11 bytes */
        struct {
            uint8_t motor_left;  /* 0-255 */
            uint8_t motor_right; /* 0-255 */
        } feedback;             /* 2 bytes */
        struct {
            uint8_t trigger_target;
            uint16_t left_deadzone;
            uint16_t right_deadzone;
        } stick_config;         /* 5 bytes */
        struct {
            uint8_t pull_percent; /* AI screen pull 0-100 */
        } ai_pull;              /* 1 byte */
    };
} gamepad_packet_t;             /* max 12 bytes, well within ESP-NOW 250-byte limit */
