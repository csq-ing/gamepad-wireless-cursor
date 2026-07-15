# ESP32 Wireless Gamepad

双 ESP32 无线手柄系统。手柄端使用 ESP32-S3 采集按键、单摇杆、扳机、旋转编码器和 BMI088 体感输入，通过 ESP-NOW 发送到接收端；接收端使用 ESP32-S2 作为 USB XInput 手柄连接 PC，并把 PC 的振动反馈反向发送给手柄端驱动 ZE300 电机。

## 功能概览

- ESP-NOW 无线输入链路，首次运行自动发现并绑定对端 MAC。
- USB XInput 手柄输出，支持 PC 侧振动反馈。
- 摇杆死区配置，接收端保存到 NVS，并同步到手柄端；配置器以单个死区值写入协议中的左右死区字段以保持兼容。
- 扳机支持 GPIO39 数字 RT 输入和 BMI088 姿态输入；长按扳机模式按键时，前倾 Pitch 超过死区后直接输出满量程扳机值。
- 旋转编码器映射为 Xbox D-Pad 左/右输入，可用于视角控制等扩展输入。
- 可选远程日志：接收端 USB 被 HID 占用时，可把日志转发到手柄端串口。
- Tauri 桌面配置器，通过 USB HID 配置通道读取状态、设置死区和扳机目标，并可把 AI 画面拉力值转发到手柄端；当前 ZE300 调试阶段由 controller monitor 串口手动切换电机状态。

## 系统架构

```text
Controller (ESP32-S3) -> ESP-NOW -> Receiver (ESP32-S2) -> USB XInput -> PC
         ^                                  |
         +---------- vibration feedback <---+
```

| 模块 | 路径 | 说明 |
|------|------|------|
| 手柄端固件 | `controller/` | 采集输入、处理摇杆/体感/编码器、发送 ESP-NOW 数据 |
| 接收端固件 | `receiver/` | 接收输入、输出 USB XInput、保存并同步配置 |
| 板级配置 | `components/Bsp/` | controller 默认 GPIO / ADC / UART / SPI 定义 |
| 算法组件 | `components/Algorithm/` | 按键消抖、摇杆死区、扳机选择、体感角度映射 |
| 设备组件 | `components/Devices/` | BMI088、ZE300、按键/编码器、公共协议和本地 TinyUSB 组件 |
| 公共协议 | `components/Devices/gamepad_common/` | ESP-NOW 数据包、按键 bitmask、通道定义 |
| 本地 TinyUSB 适配 | `components/Devices/esp_tinyusb/` | 基于 `espressif/esp_tinyusb` 的本地修改版，接收端通过 `override_path` 使用 |
| BMI088 组件 | `components/Devices/bmi088/` | 体感输入和姿态估计支持 |
| 桌面配置器 | `configurator/` | React + TypeScript + Tauri 配置工具 |
| 工具脚本 | `tools/` | Windows 振动测试脚本等辅助工具 |
| 设计文档 | `docs/` | 功能设计和实现计划 |

## 硬件

| 角色 | 芯片 | 连接方式 |
|------|------|----------|
| 接收端 (receiver) | ESP32-S2 | USB 连接 PC |
| 手柄端 (controller) | ESP32-S3 | ESP-NOW 无线连接接收端 |

### 手柄端默认接线

默认引脚定义在 `components/Bsp/include/bsp_config.h`，可按实际 PCB 修改。

| 功能 | 默认 GPIO / 通道 | 说明 |
|------|------------------|------|
| 按键 A | GPIO 37 | 上拉输入，按下接地 |
| 按键 B | GPIO 35 | 上拉输入，按下接地 |
| 按键 X | GPIO 38 | 上拉输入，按下接地 |
| 按键 Y | GPIO 36 | 上拉输入，按下接地 |
| 扳机模式按键 | GPIO 41 | 短按切换 BMI088 视角控制，长按启用 BMI088 Pitch 体感扳机 |
| RT 按键 | GPIO 39 | 上拉输入，按下接地时输出 RT=255 |
| 旋转编码器 A/B | GPIO 47 / GPIO 48 | 正交编码器输入 |
| 摇杆 X/Y | ADC1_CH3 / ADC1_CH4 (GPIO 4 / GPIO 5) | 模拟输入 |
| 视角 X/Y | BMI088 姿态映射 | 输出到协议中的右摇杆轴 |
| LT 模拟扳机 | 暂未接入 | 当前 controller 代码未读取扳机电位器；体感扳机可按配置映射到 LT 或 RT |
| BMI088 SPI | GPIO 11 / GPIO 12 / GPIO 13 / GPIO 14 / GPIO 15 | SCLK / MOSI / MISO / CS_ACC / CS_GYR |
| ZE300 电机通信 | UART1 TX/RX (GPIO 6 / GPIO 7) | 通过 TTL-to-CAN 模块控制电机 |

### Controller ZE300 fishing motor debug

控制器启动后会初始化 ZE300 电机驱动串口，并启动三状态钓鱼电机状态机。当前调试阶段使用 `idf.py monitor` 的同一个串口输入临时命令切换状态；ZE300 本身仍使用 UART1 通过 TTL-to-CAN 模块通信，默认引脚如下：

| Function | Default |
|----------|---------|
| UART | UART1 |
| TX / RX | GPIO 6 / GPIO 7 |
| Baud rate | 115200 |
| Initial torque | `0 mA` |

电机状态机包含三个状态：

- `NORMAL`：正常状态，只记录接收到的反馈，不持续控制电机，进入状态时输出一次 `0 mA`。
- `BITE_SHAKE`：刚钓上鱼时锁定当前角度作为中心，围绕中心做小角度闭环抖动，抖动结束后进入阻尼状态。
- `FISH_DAMPING`：读取 ZE300 速度，使用速度低通滤波、速度死区和类似按键消抖的门控状态判断方向；低速段按 `Kp * 速度` 连续输出反向阻尼，超过 `knee_rpm` 后平滑提升，到 `sat_rpm` 后保持 `limit_ma` 封顶力矩；低速/过零连续确认后清零，避免停止附近抖动和回弹。

monitor 串口临时调试命令：

```text
n | normal | stop
s [cycles]
d [limit_ma]
f [speed_alpha] [deadband_rpm]
cfg
```

示例：`s 3` 触发 3 次循环的上鱼抖动；`d 1000` 进入反向阻尼状态并把最大阻尼力矩设为 1000 mA；`f 0.35 1` 设置速度滤波系数和速度死区。该串口调试入口是临时调参用，后续钓鱼状态接入正式游戏信号后可删除。

调试期间 controller 会周期打印 ZE300 遥测和状态机状态，格式包含 `state`、阻尼方向 `dir`、角度、原始速度 `raw_speed`、滤波速度 `filt_speed`、阻尼计算速度 `abs_speed`、Q 轴电流、当前力矩指令和温度，便于实测抖动与阻尼参数。
状态机控制循环当前为 50 Hz；如果 ZE300 连续多次状态读取超时，控制器会清零力矩并回到 `NORMAL`，避免在通信异常时继续输出阻尼或抖动。

## 协议

ESP-NOW 通道固定为 `ESPNOW_CHANNEL = 1`，收发两端必须一致。公共数据结构在 `components/Devices/gamepad_common/include/gamepad_common.h`。

| 包类型 | 值 | 方向 | 载荷 |
|--------|----|------|------|
| `PKT_GAMEPAD_INPUT` | `0x01` | controller -> receiver | 摇杆 X/Y、体感视角 X/Y、按键 bitmask、LT/RT |
| `PKT_VIBRATION_FB` | `0x02` | receiver -> controller | 左右马达强度 `0-255` |
| `PKT_LOG_MSG` | `0x03` | receiver -> controller | 远程日志文本 |
| `PKT_STICK_CONFIG` | `0x04` | receiver -> controller | 扳机目标、左右死区兼容字段 |
| `PKT_AI_PULL` | `0x05` | receiver -> controller | AI 画面拉力百分比 `0-100` |

## 环境依赖

- ESP-IDF >= 5.1
- Node.js / npm，用于配置器前端
- Rust 和 Tauri CLI，用于配置器桌面壳
- OpenCV，可被 Rust `opencv` crate 探测到，用于配置器 AI 画面识别后端

在 Windows PowerShell 下，可先加载 ESP-IDF 环境：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.PowerShell_profile.ps1'
```

## 构建与烧录

### 接收端 ESP32-S2

接收端使用根目录 `components/Devices/esp_tinyusb/` 中的本地修改版 `esp_tinyusb` 组件；`receiver/main/idf_component.yml` 通过 `override_path` 指向该目录，避免 `idf.py fullclean` 后重新拉取官方组件覆盖修改。

```bash
cd receiver
idf.py set-target esp32s2
idf.py build
idf.py -p COMx flash monitor
```

### 手柄端 ESP32-S3

```bash
cd controller
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## 配对流程

首次运行时两端通过 ESP-NOW 广播自动发现对方 MAC 地址：

1. 先烧录并启动接收端，串口会打印 `Receiver STA MAC: xx:xx:xx:xx:xx:xx`。
2. 启动手柄端，串口会打印 `Controller STA MAC: xx:xx:xx:xx:xx:xx`。
3. 手柄端按下任意按键或拨动摇杆触发首包发送。
4. 接收端日志出现 `Controller peer added` 表示接收方向绑定成功。
5. PC 发送振动数据后，手柄端日志出现 `Receiver resolved` 表示反向链路已绑定。

如需固定 MAC，可分别修改：

- `controller/main/modules/espnow_handler.c` 中的 `s_receiver_mac`
- `receiver/main/espnow_handler.c` 中的 `s_controller_mac`

## 配置器

配置器位于 `configurator/`，通过接收端暴露的 USB HID 配置通道工作。它可以读取设备连接状态、最近输入时间、体感触发值，并设置运行时/持久化配置。启用“AI 阻力增益”后，Tauri 后端会以约 30 Hz 抓取当前前台窗口画面并通过 OpenCV 估算画面拉力值，继续发送给前端，同时通过接收端转发到手柄端；当前 controller 固件先保留该接收链路，ZE300 三状态阻尼/抖动由 monitor 串口临时调试命令控制。首次检测到非零拉力后会固定圆心和半径以跳过后续圆心检测，如果连续 5 秒输出为 `0` 或识别失败，则恢复圆心检测以适配用户调整游戏窗口；如果拉力值连续 1 秒为 `0`，后续 `0` 值会暂停转发，直到再次出现非零拉力。AI 画面识别面板提供 `63%` 测试拉力发送按钮，用于绕过图像识别并验证 PC -> receiver -> controller 链路。

```bash
cd configurator
npm install
npm run tauri:dev
```

常用命令：

```bash
npm run dev          # 仅启动 Vite 前端
npm run build        # 构建前端资源
npm run tauri:build  # 构建桌面应用
npm test             # 运行配置器离线资源测试
```

## 测试

配置器测试：

```bash
cd configurator
npm test
npm run test:connection-action
```

固件主机侧单元测试使用 `controller/host_tests` 和 `receiver/host_tests` 下的 CMake 工程：

```bash
cmake -S controller/host_tests -B controller/host_tests/build
cmake --build controller/host_tests/build
ctest --test-dir controller/host_tests/build --output-on-failure

cmake -S receiver/host_tests -B receiver/host_tests/build
cmake --build receiver/host_tests/build
ctest --test-dir receiver/host_tests/build --output-on-failure
```

## 关键源码

| 文件 | 说明 |
|------|------|
| `controller/main/input_handler.c` | 约 100 Hz 输入轮询，采集按键、摇杆、体感和编码器 |
| `components/Algorithm/stick_processing.c` | 摇杆归一化和死区处理 |
| `components/Algorithm/trigger_input.c` | 扳机和体感触发逻辑 |
| `components/Algorithm/mpu_motion.c` | BMI088 姿态角到视角/体感扳机的映射算法 |
| `controller/main/modules/bmi088_motion.c` | 手柄端 BMI088 SPI 读取和姿态估计 |
| `controller/main/modules/espnow_handler.c` | 手柄端 ESP-NOW 收发 |
| `controller/main/motor_control.c` | 手柄端 ZE300 钓鱼电机三状态控制与 monitor 串口临时调试 |
| `components/Devices/ze300/ze300.c` | ZE300 UART / TTL-to-CAN 驱动层 |
| `components/Devices/ze300/Motor_DataProcess.c` | ZE300 电机控制相关的限幅、速度符号、角度误差和曲线插值数据处理 |
| `components/Devices/bmi088/bmi088.c` | BMI088 SPI 设备驱动 |
| `receiver/main/espnow_handler.c` | 接收端 ESP-NOW 收发 |
| `receiver/main/usb_gamepad.c` | USB 手柄报告输出 |
| `receiver/main/usb_config_hid.c` | USB HID 配置通道 |
| `receiver/main/receiver_config.c` | 运行时配置校验和管理 |
| `receiver/main/receiver_config_store.c` | NVS 持久化配置 |
| `receiver/main/stick_config_sync.c` | 配置同步到手柄端 |

## Roadmap

- 完善旋转编码器 + 两个按键的 D-Pad 行为。
- 完善卷线编码器到左右扳机的映射和左右手模式。
- 继续优化 BMI088 体感模式，包括抛竿动作识别和视角移动开关。
- 继续扩展 BMI088 姿态识别在扳机和其他实际控制逻辑中的应用。
- 在配置器首页显示接收端、手柄端等固件版本信息。
