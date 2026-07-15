# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Dual-ESP32 wireless gamepad: ESP32-S3 reads inputs (buttons, sticks, triggers, accelerometer) and transmits via ESP-NOW to ESP32-S2, which presents as a USB XInput gamepad to the PC. Vibration feedback flows back through the same path.

## Build Commands

### ESP-IDF Firmware

Requires ESP-IDF >= 5.1 installed and IDF_PATH environment variable set.

```bash
# activate ESP-IDF environment
. 'C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1'

# use clang to build host test, for example
clang -DHOST_TEST=1 -I .\components\gamepad_common\include -I .\controller\main .\controller\main\trigger_input.c .\controller\main\mpu_motion.c .\controller\host_tests\test_controller_view_control.c -o .\controller\host_tests\test_controller_view_control.exe

# Receiver (ESP32-S2)
cd receiver
idf.py set-target esp32s2
idf.py build
idf.py -p COMx flash monitor

# Controller (ESP32-S3)
cd controller
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

### Configurator Desktop App

```bash
cd configurator
npm install
npm run tauri:dev      # Development with hot reload
npm run tauri:build     # Production build
```

## Architecture

### Data Flow

```
Controller (ESP32-S3) → ESP-NOW → Receiver (ESP32-S2) → USB → PC (XInput)
         ↑                                  ↓
         └──────── Vibration Feedback ←─────┘
```

### Key Files

| Component | File | Purpose |
|-----------|------|---------|
| Protocol | `components/gamepad_common/include/gamepad_common.h` | Shared packet format (11-byte input, 2-byte vibration, 5-byte config) |
| Controller | `controller/main/input_handler.c` | 100Hz polling: ADC sticks, buttons, MPU6050 accelerometer, rotary encoder |
| Controller | `controller/main/espnow_handler.c` | Sends input, receives vibration/deadzone config |
| Receiver | `receiver/main/espnow_handler.c` | Receives input, sends vibration, periodically syncs config to controller |
| Receiver | `receiver/main/trigger_routing.c` | Routes acceleration trigger data to LT or RT based on config |
| Receiver | `receiver/main/receiver_config.c` | Manages NVS-stored config (deadzone, trigger target) |
| Receiver | `receiver/main/usb_gamepad.c` | TinyUSB composite device (XInput + HID config channel) |

### Packet Types (gamepad_common.h)

- `PKT_GAMEPAD_INPUT` (0x01): Joystick axes (int16), buttons (bitmask), triggers (uint8)
- `PKT_VIBRATION_FB` (0x02): Left/right motor strength (0-255)
- `PKT_STICK_CONFIG` (0x04): Deadzone and trigger target sync to controller
- `PKT_LOG_MSG` (0x03): Remote log forwarding (when `CONFIG_REMOTE_LOG_ENABLE=y`)

### Trigger Mode

The controller supports two trigger sources:
1. **Potentiometer mode**: ADC reads on ESP32-S3 channels 6/7
2. **Accelerometer mode**: MPU-6050 Z-axis (via I2C), activated by holding trigger mode button

The receiver's `trigger_routing_apply()` routes the acceleration value to either LT or RT based on configuration.

### Configuration Persistence

- Config stored in receiver's NVS partition
- Receiver periodically syncs config to controller via ESP-NOW
- Controller applies deadzone settings to stick processing

### GPIO Assignments (controller/main/input_handler.h)

- Buttons: GPIO 35-38 (active-low with pull-up)
- Trigger mode button: GPIO 39
- Rotary encoder: GPIO 45-46 (quadrature)
- Stick ADCs: GPIO 4-7 (ADC1 channels 3-6)
- Trigger pots: GPIO 17-18 (ADC2 channels 6-7)
- Motor PWM: GPIO 47-48
- MPU-6050: GPIO 9-10 (I2C)

## Configurator

Tauri desktop app connecting to receiver via USB HID config channel. Frontend built with React + TypeScript, backend in Rust.

### Tauri Commands

- `connect_device`: Opens USB HID device
- `read_config` / `set_config_command` / `save_config`: Config read/write
- `device-status`: Event stream with connection state, input age, acceleration value

## Pairing

On first boot, devices auto-discover via ESP-NOW broadcast:
1. Receiver prints its STA MAC address
2. Controller prints its STA MAC after first input
3. Devices add each other as ESP-NOW peers automatically

To pre-configure MAC addresses, edit `s_receiver_mac` in `controller/main/espnow_handler.c` and `s_controller_mac` in `receiver/main/espnow_handler.c`.

## Repository Rules

- 每修改或实现功能时，检查 `README.md` 文件中的内容是否需要更新。
- 改动前端代码时，无需编写测试代码，只要改动后前端能成功运行就行。
