# 树莓派语音模块与 WiFi 通信链路完成功能说明

本文档用于说明本人在本项目中负责并完成的内容，重点包括树莓派语音模块、WiFi/MQTT 通信链路，以及面向联调的状态转发与调试支持。

## 1. 负责内容概述

我负责的部分主要不是单一功能点，而是把“小车能听懂指令”和“小车各端能稳定通信”两件事打通。当前已完成的核心链路是：

```text
用户语音/文本输入
  -> 树莓派语音网关
  -> ASR 识别 / 本地规则 / LLM 解析 / TTS 回复
  -> MQTT 控制命令
  -> ESP32 执行动作并返回回执
  -> 状态数据通过 MQTT 和 WebSocket 展示到网页或 App
```

同时，K230 视觉端、ESP32 底盘端、树莓派语音端、网页看板和 Android App 都围绕同一套 WiFi 局域网与 MQTT topic 协议进行通信，形成了完整的项目联调基础。

## 2. 树莓派语音模块已完成内容

树莓派语音模块位于 `raspberry_pi_voice/` 目录，主入口为 `voice_gateway.py`。该模块已经具备从语音输入到小车动作控制的完整处理流程。

### 2.1 语音识别接入

已完成讯飞 WebSocket ASR 接入，相关实现位于 `car_voice/asr.py`。

完成内容包括：

- 通过讯飞 ASR WebSocket 接口进行中文语音识别。
- 使用 `arecord` 采集树莓派麦克风音频。
- 使用 `sox` 将音频转换为 ASR 所需的 16 kHz、单声道、16 bit PCM 格式。
- 支持根据音量 RMS 判断是否开始识别，减少空白音频和环境噪声干扰。
- 支持最短监听时间、无更新超时、最大监听窗口等参数，避免识别过程卡住。
- 支持通过环境变量配置麦克风设备、监听窗口、音量阈值等参数。

### 2.2 唤醒与对话状态管理

已完成语音助手状态机，相关实现位于 `car_voice/assistant.py`。

完成内容包括：

- 支持默认唤醒词“小车”。
- 支持待机状态和激活状态切换。
- 支持对话超时自动回到待机状态。
- 支持退出词，例如“退出”“再见”“不用了”。
- 支持 `VOICE_ALWAYS_ACTIVE` 配置，让调试时跳过唤醒流程。
- TTS 播放期间会阻塞 ASR 采集，减少语音播报被再次识别的问题。

### 2.3 本地快捷指令处理

已完成常用控制指令的本地规则匹配，相关实现位于 `car_voice/schema.py`。

已经支持的快捷语音包括：

- 前进：如“前进”“向前”“往前走”。
- 后退：如“后退”“倒车”“向后退”。
- 左转：如“左转”“向左转”。
- 右转：如“右转”“向右转”。
- 开始或恢复跟随：如“开始跟随”“继续跟随”“跟着我”。
- 暂停或保持不动：如“暂停跟随”“保持不动”“原地待命”。
- 云台回中：如“云台回中”“回正”“看前面”。
- 云台锁定与解锁：如“锁定云台”“解锁云台”。
- 点头、摇头等云台动作。

本地快捷规则的作用是让高频控制命令不依赖云端大模型，降低响应延迟，提高现场演示稳定性。

### 2.4 急停与安全优先级

已完成本地急停优先级处理。

当识别文本中包含以下风险词时，系统会直接生成停车命令，不等待 LLM：

```text
停下、停止、别动、不要动、刹车、危险、急停、站住
```

该逻辑保证了紧急停止类命令优先执行，避免由于模型解析、网络请求或响应延迟影响安全性。

### 2.5 LLM 动作解析与聊天分流

已完成控制意图和普通聊天的分流，相关实现位于 `car_voice/llm.py` 和 `car_voice/assistant.py`。

完成内容包括：

- 对明显的控制类语句，进入动作规划 LLM。
- 对普通聊天类语句，进入聊天 LLM，只进行语音回复，不下发 MQTT 控制命令。
- 动作 LLM 只允许输出结构化 JSON。
- 聊天 LLM 限制为简短中文回答，避免输出控制 JSON。
- 如果用户询问实时新闻、实时天气等无法可靠获取实时数据的问题，聊天提示词中要求直接说明无法查询实时信息。

动作 LLM 输出会继续经过本地 schema 校验，不会直接控制小车硬件。

### 2.6 动作白名单与参数限制

已完成动作安全白名单，相关实现位于 `car_voice/schema.py`。

当前允许的动作范围为：

| target | name |
|---|---|
| `chassis` | `forward`、`backward`、`turn_left`、`turn_right`、`stop` |
| `mode` | `hold`、`resume_follow` |
| `gimbal` | `nod`、`shake`、`center`、`lock`、`unlock` |
| `none` | `none` |

参数限制包括：

- `speed` 限制在 `0..50`。
- `duration_ms` 限制在 `0..3000`。
- `pan_delta_deg` 限制在 `-45..45`。
- `tilt_delta_deg` 限制在 `-45..45`。
- 非法 JSON、未知 target、未知 action 不会下发。

这部分保证了即使 LLM 返回异常内容，也不会绕过本地规则直接控制车辆。

### 2.7 TTS 语音回复

已完成 DashScope TTS 语音播报接入，相关实现位于 `car_voice/speech.py`。

完成内容包括：

- 控制命令执行后可以播报“好的，前进”等反馈。
- 普通聊天可以直接通过语音回复。
- 动作发送失败或 ESP32 无响应时，可以播报“车辆暂未响应”等提示。
- TTS 播放期间阻塞语音采集，减少自回声误识别。

### 2.8 文本调试模式

已完成语音模块的文本 dry-run 调试方式。

可通过以下方式在不接麦克风、不连接实车的情况下测试语音解析逻辑：

```bash
VOICE_DRY_RUN=1 python3 voice_gateway.py --text
```

该模式可以直接输入文本，例如“前进”“停下”“开始跟随”，观察系统生成的结构化 MQTT 命令，便于快速调试语义解析和动作映射。

## 3. WiFi 通信与 MQTT 链路已完成内容

本项目采用同一局域网内的 WiFi 通信方式，树莓派运行 Mosquitto MQTT broker，ESP32、K230、树莓派语音端、网页看板和 Android App 围绕同一套 MQTT topic 进行数据交换。

### 3.1 树莓派作为通信中心

已完成树莓派端 MQTT 通信中心的设计与代码接入。

树莓派承担以下角色：

- 运行 Mosquitto MQTT broker。
- 运行语音网关，将语音指令转换成 MQTT 控制命令。
- 运行 `dashboard_server.py`，将 MQTT 消息转发给网页和 App。
- 作为 ESP32、K230、前端之间的统一通信中转点。

默认 MQTT 根路径为：

```text
qrs/robot01
```

其中 `qrs` 是 topic root，`robot01` 是设备 ID，均支持通过环境变量或 ESP32 menuconfig 配置。

### 3.2 MQTT topic 协议整理

已完成主要 topic 的划分和收发方向设计。

| Topic | 发布方 | 订阅方 | 用途 |
|---|---|---|---|
| `qrs/robot01/cmd/request` | 树莓派语音端、网页/App | ESP32 | 下发结构化动作命令 |
| `qrs/robot01/cmd/ack` | ESP32 | 树莓派语音端、网页/App | 返回命令执行结果 |
| `qrs/robot01/status` | ESP32 | 树莓派、网页/App | 周期发布小车状态 |
| `qrs/robot01/track` | K230 | ESP32、网页/App | 发布视觉跟踪数据 |
| `qrs/robot01/gimbal/request` | ESP32、网页/App | K230 | 下发云台动作请求 |
| `qrs/robot01/pid/request` | 网页/App | ESP32 | 在线下发底盘 PID 参数 |

这套 topic 设计使各端可以解耦开发，只要遵守统一 JSON 格式即可联调。

### 3.3 语音端 MQTT 客户端

已完成树莓派语音端 MQTT 客户端，相关实现位于 `car_voice/robot_mqtt.py`。

完成内容包括：

- 连接 MQTT broker。
- 发布语音解析后的 `execute_action` 命令到 `cmd/request`。
- 订阅 ESP32 的 `cmd/ack` 回执。
- 订阅 ESP32 的 `status` 状态。
- 为每条命令生成 `request_id`，用于匹配对应回执。
- 支持等待 ESP32 回执，超时后返回 `command_timeout`。
- 支持 dry-run 模式，只打印 MQTT payload，不实际发布。
- 支持 MQTT 用户名、密码、client id、keepalive 等配置。

### 3.4 ESP32 端 MQTT 接入

已完成 ESP32 端 MQTT 客户端接入，相关实现位于 `main/robot_mqtt_client.c` 和 `main/robot_mqtt_client.h`。

完成内容包括：

- 根据配置自动拼接 `cmd/request`、`cmd/ack`、`status`、`track`、`gimbal/request`、`pid/request` 等 topic。
- 连接树莓派 MQTT broker。
- 订阅语音和前端下发的动作命令。
- 订阅 K230 发布的视觉跟踪数据。
- 订阅网页/App 下发的 PID 调参请求。
- 解析 `execute_action` 命令并调用 `vehicle_ai_control_execute_action_payload()` 执行。
- 执行完成后发布 `cmd/ack` 回执。
- 支持解析 K230 的目标数据，包括 `valid`、`pan`、`tilt`、`x`、`y`、`w`、`h`。
- 支持发布云台动作请求到 `gimbal/request`。
- 支持发布小车实时状态到 `status`。

### 3.5 ESP32 WiFi 连接封装

已完成 ESP32 端 WiFi STA 连接封装，相关实现位于 `main/wifi_connection.c` 和 `main/wifi_connection.h`。

完成内容包括：

- ESP32 以 STA 模式连接指定 WiFi。
- WiFi SSID、密码、安全模式可通过 `menuconfig` 配置。
- 支持 WPA/WPA2/WPA3 等认证方式配置。
- 启动后等待获取 IP。
- 提供 `wifi_connection_wait_connected()` 等待连接结果。
- 提供 `wifi_connection_is_connected()` 查询连接状态。
- 断线后自动重连。
- 在超过最大重试次数后，等待调用方可返回失败，但后台仍继续尝试重连。
- 连接成功后打印获取到的 IP 地址，方便联调。

### 3.6 ESP32 MQTT 配置项

已在 `main/Kconfig.projbuild` 中加入 MQTT 相关配置项。

当前支持配置：

- 是否启用 MQTT：`ROBOT_MQTT_ENABLE`
- broker 地址：`ROBOT_MQTT_BROKER_URI`
- topic 根路径：`ROBOT_MQTT_TOPIC_ROOT`
- 设备 ID：`ROBOT_MQTT_DEVICE_ID`
- ESP32 MQTT client id：`ROBOT_MQTT_CLIENT_ID`
- 用户名和密码：`ROBOT_MQTT_USERNAME`、`ROBOT_MQTT_PASSWORD`
- keepalive：`ROBOT_MQTT_KEEPALIVE_S`
- 状态上报周期：`ROBOT_MQTT_STATUS_PERIOD_MS`

这样在不同网络环境下，只需要修改配置即可切换 broker IP 和设备 topic。

## 4. 状态上报与调试看板链路已完成内容

为了方便联调，我完成了 MQTT 到 WebSocket 的桥接服务，并接入了网页/手机端状态看板。

### 4.1 Dashboard Server

`raspberry_pi_voice/dashboard_server.py` 已完成以下功能：

- 连接 MQTT broker。
- 订阅 `qrs/robot01/#` 下所有相关消息。
- 将 MQTT 消息转换为 WebSocket 消息广播给浏览器或 App。
- 提供静态网页服务，直接访问 dashboard 目录。
- 支持网页/App 通过 WebSocket 下发控制动作。
- 支持网页/App 下发云台控制请求。
- 支持网页/App 下发 PID 参数。
- HTTP 端口和 WebSocket 端口被占用时，会自动尝试后续端口。
- 静态网页响应中禁用缓存，避免旧版本 JS 干扰调试。

### 4.2 网页/手机看板接入

`raspberry_pi_voice/dashboard/` 已接入实时看板。

看板目前可以展示：

- WebSocket 连接状态。
- MQTT broker 连接状态。
- ESP32 在线/离线状态。
- 小车当前状态，例如 `BOOT`、`WAIT_TRACK`、`FOLLOW`、`LOST`、`OBSTACLE`、`HOLD`。
- 超声波距离。
- MPU6050 yaw 和 yaw rate。
- K230 pan、tilt 和目标框数据。
- 左右轮目标速度、实际速度、PWM duty。
- 编码器计数。
- 跟随控制量，例如目标大小、朝向误差、转向输出、基础速度。
- 当前底盘 PID 参数。
- MQTT 原始事件流。

看板目前可以下发：

- 前进、后退、左转、右转、急停等快捷控制。
- 恢复跟随、保持不动、云台回中等模式控制。
- 云台动作请求。
- 底盘 PID 参数在线调整。

### 4.3 Android App 接入

项目中的 `android_app/` 已能作为移动端看板入口使用。

已具备的能力包括：

- 内置看板页面。
- 支持 JavaScript、DOM Storage、HTTP/WebSocket 明文访问。
- 支持选择不同 WebSocket 后台主机。
- 支持模拟器访问电脑、手机本机、树莓派 IP、自定义 IP 等场景。
- 生成了 debug APK，便于手机端调试展示。

## 5. 与 K230 视觉端的链路配合

虽然 K230 视觉识别本身不是我负责的主要部分，但我完成的通信链路已经支持 K230 数据接入。

当前协作方式为：

- K230 将视觉识别结果发布到 `qrs/robot01/track`。
- ESP32 订阅该 topic，解析目标是否有效、云台角度、目标框位置和大小。
- ESP32 根据 track 数据执行跟随控制。
- Dashboard Server 也订阅该 topic，用于前端实时显示目标框和跟踪数据。
- ESP32 或前端可向 `qrs/robot01/gimbal/request` 发布云台动作请求，K230 订阅后执行云台控制。

对应的 K230 视觉数据格式为：

```json
{
  "valid": true,
  "pan": 90.0,
  "tilt": 90.0,
  "x": 240.0,
  "y": 180.0,
  "w": 120.0,
  "h": 140.0
}
```

这说明视觉端和底盘端之间已经不再依赖串口直连，而是通过 WiFi/MQTT 进行解耦通信。

## 6. 已打通的端到端链路

当前已具备以下端到端链路：

### 6.1 语音控制底盘链路

```text
用户语音
  -> 讯飞 ASR
  -> 本地规则或动作 LLM
  -> MQTT cmd/request
  -> ESP32 执行动作
  -> MQTT cmd/ack
  -> 树莓派 TTS 反馈
```

这条链路用于完成“前进、后退、左转、右转、停止、保持不动、恢复跟随”等动作。

### 6.2 K230 视觉跟踪链路

```text
K230 视觉识别
  -> MQTT track
  -> ESP32 读取目标数据
  -> 底盘跟随控制
  -> MQTT status
  -> 网页/App 实时显示
```

这条链路用于支撑小车自动跟随和目标状态观察。

### 6.3 前端控制链路

```text
网页/App 控制按钮
  -> WebSocket
  -> dashboard_server.py
  -> MQTT cmd/request / pid/request / gimbal/request
  -> ESP32 或 K230 执行
  -> MQTT ack/status
  -> 网页/App 更新
```

这条链路用于现场调试、手动控制和 PID 参数调整。

### 6.4 状态观测链路

```text
ESP32 状态数据
  -> MQTT status
  -> dashboard_server.py
  -> WebSocket
  -> 网页 / Android App
```

这条链路用于实时观察底盘状态、传感器数据、跟随控制量和 PID 参数。

## 7. 典型控制命令格式

语音端或前端向 ESP32 下发的命令格式如下：

```json
{
  "request_id": "voice-xxxx-1",
  "cmd": "execute_action",
  "payload": {
    "emotion": "happy",
    "action": {
      "target": "chassis",
      "name": "forward",
      "speed": 35,
      "duration_ms": 1000,
      "pan_delta_deg": 0,
      "tilt_delta_deg": 0
    },
    "voice": {
      "text": "好的，前进。"
    }
  }
}
```

ESP32 执行后返回：

```json
{
  "request_id": "voice-xxxx-1",
  "cmd": "execute_action",
  "ok": true
}
```

如果命令无法解析、动作不合法或执行失败，ESP32 会在 `ok=false` 的回执中返回错误原因，语音端可据此播报失败提示。

## 8. 当前可用于汇报的完成成果

可以在进展汇报中说明我已完成以下内容：

1. 完成树莓派语音网关基础框架。
2. 完成讯飞 ASR 接入，实现中文语音识别。
3. 完成唤醒词、激活状态、超时退出、退出词等对话状态管理。
4. 完成高频语音指令的本地快捷规则，降低常用动作响应延迟。
5. 完成急停类命令本地最高优先级处理。
6. 完成动作 LLM 与聊天 LLM 的分流逻辑。
7. 完成动作白名单和参数边界限制，避免非法动作下发。
8. 完成 DashScope TTS 语音回复接入。
9. 完成语音模块文本 dry-run 调试模式。
10. 完成树莓派 MQTT 客户端，支持命令发布、回执等待、状态订阅。
11. 完成 ESP32 WiFi STA 连接封装，支持断线重连和连接状态查询。
12. 完成 ESP32 MQTT 客户端，支持动作命令、视觉数据、PID 请求和状态发布。
13. 完成 MQTT topic 协议设计，统一 ESP32、K230、树莓派、前端之间的通信格式。
14. 完成 Dashboard Server，把 MQTT 消息桥接到 WebSocket。
15. 完成网页/手机端实时看板的数据接入。
16. 完成前端控制、云台请求、PID 参数下发到 MQTT 的桥接。
17. 完成 K230 视觉数据通过 MQTT 接入 ESP32 跟随控制链路。
18. 完成 ESP32 状态上报，使底盘状态、传感器数据、跟踪数据和 PID 参数可视化。

## 9. 当前仍需注意的问题

当前链路已经具备联调和演示基础，但仍有一些使用时需要注意的问题：

- 树莓派、ESP32、K230、手机或电脑必须处于同一局域网。
- 如果树莓派 IP 变化，需要同步修改 ESP32 的 MQTT broker URI 和 K230 的 broker 地址。
- 真机手机中的 `127.0.0.1` 表示手机自身，不是电脑或树莓派。
- Mosquitto 如果只监听 `127.0.0.1`，ESP32 和 K230 无法从局域网访问。
- 语音识别效果受麦克风设备、环境噪声和播报回声影响，需要根据现场调整 ASR 参数。
- MQTT 网络抖动可能影响控制命令和视觉跟踪数据的实时性。
- 语音模块依赖讯飞 ASR 和 DashScope/兼容模型 API key，正式演示前需要确认环境变量配置完整。

## 10. 后续计划

后续可以继续完善以下内容：

1. 将语音网关配置为树莓派 systemd 服务，实现上电自启动。
2. 增加语音识别、LLM 解析、MQTT 下发、ESP32 回执的端到端耗时统计。
3. 增加常用语音指令测试集，验证不同说法下的识别和动作映射稳定性。
4. 增加 MQTT 断连、ESP32 离线、命令超时等异常状态的可视化提示。
5. 增加局域网 broker 自动发现或配置页面，减少手动修改 IP 的成本。
6. 在看板中显示最近一次语音识别文本、解析动作和执行回执，方便现场排查。
7. 继续优化 ASR 阈值和 TTS 播放策略，减少噪声和自回声误触发。

## 11. 汇报时可直接使用的总结

我负责的树莓派语音模块已经完成从语音输入到结构化控制命令的完整流程，支持讯飞 ASR、本地快捷指令、急停优先级、动作 LLM、聊天 LLM、动作白名单校验、TTS 语音反馈和文本 dry-run 调试。通信链路方面，我完成了基于 WiFi 和 MQTT 的多端通信框架，树莓派作为 broker 和桥接中心，ESP32、K230、网页看板和 Android App 都围绕 `qrs/robot01` topic 体系进行通信。目前已经打通语音控制底盘、K230 视觉数据接入、ESP32 状态上报、网页/App 实时监控和前端控制下发等链路，为后续实车联调和现场演示提供了基础。

