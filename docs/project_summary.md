# ESP32-S3 + K230 + Raspberry Pi 智能追踪小车项目总结

更新时间：2026-05-11

本文档用于小组成员快速理解当前项目状态、三端职责、通信协议、运行方式和后续调参位置。更细的引脚连接请看 [pinout.md](pinout.md)。

## 1. 项目目标

当前原型是一台具备“视觉识别跟随 + 语音控制 + 情绪小屏显示”的智能追踪小车。

系统由三部分组成：

```text
K230 视觉端  --MQTT track/gimbal-->  Raspberry Pi broker  <--MQTT cmd/ack-->  ESP32-S3 底盘端
                                      ^
                                      |
                               Raspberry Pi 语音端
```

三端都连接同一个手机热点或同一个局域网。树莓派运行 Mosquitto MQTT broker，ESP32 和 K230 都通过树莓派 IP 通信。

## 2. 三端职责

| 端 | 主要文件 | 当前职责 |
|---|---|---|
| ESP32-S3 底盘端 | `main/station_example_main.c` | Wi-Fi、MQTT、底盘电机、编码器速度闭环、超声波避障、MPU6050 yaw、跟随控制、ST7735 情绪屏 |
| K230 视觉端 | `main1.py` | 摄像头取帧、人脸检测/识别、二维云台追踪、发布目标位置、接收云台命令 |
| 树莓派语音端 | `raspberry_pi_voice/voice_gateway.py` | 讯飞 ASR、常用指令本地快捷规则、动作 LLM、聊天 LLM、DashScope TTS、MQTT 动作分发 |

## 3. 当前通信协议

MQTT topic 根路径默认：

```text
qrs/robot01
```

| Topic | 发布方 | 订阅方 | 用途 |
|---|---|---|---|
| `qrs/robot01/track` | K230 | ESP32 | 发布视觉追踪数据 |
| `qrs/robot01/gimbal/request` | ESP32 | K230 | ESP32 请求 K230 执行云台动作 |
| `qrs/robot01/cmd/request` | 树莓派语音端 | ESP32 | 下发底盘/模式/云台动作 |
| `qrs/robot01/cmd/ack` | ESP32 | 树莓派语音端 | ESP32 动作执行回执 |

### K230 -> ESP32 追踪数据

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

### 树莓派 -> ESP32 动作请求

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

ESP32 回执：

```json
{
  "request_id": "voice-xxxx-1",
  "cmd": "execute_action",
  "ok": true
}
```

## 4. 当前已实现能力

### ESP32-S3

- 连接 Wi-Fi STA。
- 连接树莓派 MQTT broker。
- 订阅语音动作 topic：`qrs/robot01/cmd/request`。
- 订阅 K230 追踪 topic：`qrs/robot01/track`。
- 发布动作回执：`qrs/robot01/cmd/ack`。
- 发布云台命令：`qrs/robot01/gimbal/request`。
- TB6612 电机控制：前进、后退、左转、右转、差速、停止。
- 新增编码器速度闭环：
  - 左轮编码器：GPIO39 / GPIO40。
  - 右轮编码器：GPIO41 / GPIO42。
  - 每 50 ms 读取编码器，PID 修正 PWM。
- 超声波避障：
  - 低于警戒距离停车。
  - 距离恢复后继续跟随。
- MPU6050：
  - 启动校准。
  - yaw 积分。
  - 静止偏置学习。
- 自动跟随：
  - 使用 K230 的 `pan/tilt/face box`。
  - 结合 MPU yaw 计算左右轮差速。
  - 支持目标丢失、过近、云台角度越界保护。
- ST7735 + LVGL 情绪屏：
  - 显示启动、等待、跟随、搜索、警戒、保持、语音命令等状态。

### K230

- 摄像头取帧和 LCD 显示。
- 人脸检测和人脸识别。
- 读取本地人脸库：`/data/face_database/3889/`。
- 识别目标后驱动二维云台追踪。
- 目标丢失后短暂保持、局部搜索、全局搜索。
- 通过 MQTT 发布目标数据到 `qrs/robot01/track`。
- 订阅 `qrs/robot01/gimbal/request`，执行：
  - `nod`
  - `shake`
  - `center`
  - `lock`
  - `unlock`
  - 上下左右微调

### 树莓派语音端

- 入口：`raspberry_pi_voice/voice_gateway.py`。
- ASR：讯飞 WebSocket ASR。
- 音频采集：`arecord + sox`。
- LLM：
  - 控制意图走动作 LLM，输出结构化动作 JSON。
  - 普通聊天走聊天 LLM，只语音回复，不下发 MQTT。
- 常用控制指令走本地快捷规则，不等待云端 LLM。
- 高风险词本地最高优先级停车：
  - 停下、停止、别动、危险、急停等。
- TTS：DashScope TTS + `pygame` 播放。
- 播放 TTS 时会阻塞 ASR，减少自回声误识别。
- 支持 systemd 上电自启动。
- 支持文本 dry-run：

```bash
VOICE_DRY_RUN=1 python3 voice_gateway.py --text
```

## 5. 语音动作白名单

树莓派和 ESP32 两端都会限制动作范围，避免 LLM 输出任意危险命令。

| target | name |
|---|---|
| `chassis` | `forward`, `backward`, `turn_left`, `turn_right`, `stop` |
| `mode` | `hold`, `resume_follow` |
| `gimbal` | `nod`, `shake`, `center`, `lock`, `unlock` |
| `none` | `none` |

安全限制：

- `speed` 限制在 `0..50`。
- `duration_ms` 限制在 `0..3000`。
- 非法 JSON、未知 target、未知 action 不会下发。

## 6. 运行方式

### 1. 网络准备

手机热点或路由器需要让三端处于同一个局域网：

- ESP32-S3
- K230
- 树莓派

树莓派需要运行 Mosquitto：

```bash
sudo systemctl status mosquitto
```

如果 ESP32/K230 需要从局域网访问 broker，Mosquitto 需要监听 `0.0.0.0:1883`，不能只监听 `127.0.0.1`。

### 2. 树莓派语音端

服务方式：

```bash
sudo systemctl status voice-gateway.service
journalctl -u voice-gateway.service -f
```

手动调试方式：

```bash
cd ~/raspberry_pi_voice
conda activate xiaoji310
python3 voice_gateway.py
```

文本调试：

```bash
VOICE_DRY_RUN=1 python3 voice_gateway.py --text
```

### 3. ESP32-S3

编译：

```bash
idf.py build
```

烧录：

```bash
idf.py -p COM口 flash monitor
```

配置入口：

```bash
idf.py menuconfig
```

重点配置：

- Wi-Fi SSID / password。
- MQTT broker URI。
- LCD GPIO / 分辨率 / offset。
- MPU6050 GPIO。
- AI 控制默认速度和动作时长。

### 4. K230

当前主脚本：

```text
main1.py
```

主要配置在文件顶部：

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `MQTT_BROKER`
- `MQTT_PORT`
- `MQTT_CLIENT_ID`
- `MQTT_TOPIC_ROOT`
- `MQTT_DEVICE_ID`

人脸库路径：

```text
/data/face_database/3889/
```

## 7. 常用调试命令

在树莓派上观察所有 MQTT 消息：

```bash
mosquitto_sub -h 127.0.0.1 -t 'qrs/#' -v
```

只看语音下发命令：

```bash
mosquitto_sub -h 127.0.0.1 -t 'qrs/robot01/cmd/request' -v
```

只看 ESP32 回执：

```bash
mosquitto_sub -h 127.0.0.1 -t 'qrs/robot01/cmd/ack' -v
```

只看 K230 追踪数据：

```bash
mosquitto_sub -h 127.0.0.1 -t 'qrs/robot01/track' -v
```

手动发布停止命令：

```bash
mosquitto_pub -h 127.0.0.1 -t 'qrs/robot01/cmd/request' -m '{"request_id":"manual-stop","cmd":"execute_action","payload":{"emotion":"alert","action":{"target":"chassis","name":"stop","speed":0,"duration_ms":0,"pan_delta_deg":0,"tilt_delta_deg":0},"voice":{"text":"stop"}}}'
```

查看树莓派音频设备：

```bash
arecord -l
aplay -l
```

## 8. 关键调参位置

| 调什么 | 文件 | 位置 |
|---|---|---|
| 跟随转向速度 / 灵敏度 | `main/station_example_main.c` | `FOLLOW_PID_KP`, `FOLLOW_MIN_TURN_OUTPUT`, `FOLLOW_MAX_TURN_OUTPUT` |
| 跟随前进距离 | `main/station_example_main.c` | `FOLLOW_FACE_TARGET_SIZE_PX`, `FOLLOW_FACE_STOP_WIDTH_PX`, `FOLLOW_FACE_STOP_HEIGHT_PX` |
| 障碍物停车距离 | `main/station_example_main.c` | `OBSTACLE_WARN_DISTANCE_CM`, `OBSTACLE_CLEAR_DISTANCE_CM` |
| 编码器速度闭环 | `main/motor_control.c` | `MOTOR_ENCODER_TARGET_MAX_PPS`, `MOTOR_SPEED_PID_KP`, `MOTOR_SPEED_PID_KI` |
| 电机引脚 | `main/motor_control.h` | `MOTOR_*_PIN` |
| 编码器引脚 | `main/encoder.h` | `ENCODER_*_PIN` |
| LCD 引脚 / offset | `main/Kconfig.projbuild` 或 `menuconfig` | `ROBOT_LCD_*` |
| 语音 ASR 参数 | `raspberry_pi_voice/.env` | `ASR_*`, `MIC_DEVICE`, `AUDIO_FILTER` |
| 本地快捷语音规则 | `raspberry_pi_voice/car_voice/schema.py` | `QUICK_ACTION_RULES` |
| K230 发布频率 | `main1.py` | `TRACK_PUBLISH_INTERVAL_MS` |
| K230 云台 PID / 速度 | `main1.py` | `PanTilt` 相关参数 |

## 9. 当前注意事项

- ESP32-S3 GPIO 不耐 5V。
  - 超声波 Echo 如果是 5V，需要分压或电平转换。
  - 编码器 A/B 如果被上拉到 5V，也需要分压或电平转换。
- 电机、舵机、电源模块、ESP32、K230 必须共地。
- 电机和舵机不要直接从 ESP32/K230 的 3.3V 取大电流。
- 当前 ESP32 与 K230 已改为 MQTT 通信，不再依赖 ESP32-K230 UART。
- 手机热点 IP 可能变化，若树莓派 IP 变化，需要同步修改：
  - ESP32 `ROBOT_MQTT_BROKER_URI`
  - K230 `MQTT_BROKER`
- K230 通过 Wi-Fi 发布视觉数据，网络抖动会影响跟随平滑度。
- 编码器闭环初次测试建议架空车轮，确认方向和脉冲读数正常后再落地。

## 10. 小组协作建议

| 成员方向 | 建议负责内容 |
|---|---|
| ESP32 控制 | 跟随 PID、速度闭环、避障策略、LCD 状态显示 |
| K230 视觉 | 人脸库录入、识别稳定性、云台搜索策略、MQTT 发布频率 |
| 树莓派语音 | ASR 参数、快捷规则、聊天/动作分流、TTS 播放设备 |
| 硬件集成 | 电源、共地、电平转换、线束固定、编码器和电机方向校准 |
| 测试记录 | 每次参数变化后的跟随效果、语音响应时间、异常日志 |

## 11. 建议验收流程

1. 树莓派启动后确认：

```bash
systemctl status mosquitto
systemctl status voice-gateway.service
```

2. ESP32 上电后确认串口日志：

```text
Wi-Fi connected
robot_mqtt: connected to broker
encoder: quadrature encoders initialized
motor: speed closed-loop enabled
follow mode enabled
```

3. K230 上电后确认：

```text
WiFi connected
MQTT connected
TRACK MQTT
```

4. MQTT 观察：

```bash
mosquitto_sub -h 127.0.0.1 -t 'qrs/#' -v
```

5. 功能测试顺序：

- K230 能识别人脸。
- ESP32 能收到 `track`。
- 小车能稳定跟随。
- 语音说“停下”，小车立即停车。
- 语音说“前进/后退/左转/右转”，小车 1 秒内动作并回执。
- 语音说普通问题，只语音回答，不下发 MQTT 动作。
