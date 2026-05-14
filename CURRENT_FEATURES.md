# 当前已完成功能说明

更新时间：2026-05-13

本项目目前已经形成了 K230 视觉识别、ESP32-S3 底盘控制、树莓派 MQTT 中转、电脑网页端仪表盘、Android App 监控控制的整体链路。

## 1. 整体通信链路

- K230、ESP32、树莓派可以接入同一个 Wi-Fi 网络。
- 树莓派作为 Mosquitto MQTT broker 主机。
- MQTT topic 根路径默认为 `qrs/robot01`。
- ESP32 订阅来自 K230 和前端的控制消息。
- 前端通过 `dashboard_server.py` 连接 MQTT，并用 WebSocket 把实时数据转发给网页/App。

主要 topic：

```text
qrs/robot01/track        K230 目标识别数据
qrs/robot01/status       ESP32 实时状态
qrs/robot01/cmd/request  前端控制命令
qrs/robot01/cmd/ack      ESP32 命令反馈
qrs/robot01/pid/request  前端 PID 参数下发
qrs/robot01/gimbal/request 云台控制请求
```

## 2. ESP32 小车端功能

ESP32 端已经实现：

- Wi-Fi 连接配置。
- MQTT 客户端连接树莓派 broker。
- 接收 K230 的目标跟踪数据。
- 接收网页/App 下发的控制命令。
- 底盘左右轮差速控制。
- 编码器测速闭环。
- 超声波避障检测。
- MPU6050 姿态角读取。
- OLED/LCD 表情状态显示。
- 自动跟随、目标丢失、避障暂停、手动保持等状态切换。
- 周期性发布小车实时状态到 MQTT。

ESP32 状态上报内容包括：

- 小车状态：`BOOT`、`WAIT_TRACK`、`FOLLOW`、`LOST`、`OBSTACLE`、`HOLD` 等。
- 超声波距离。
- MPU6050 yaw / yaw rate。
- K230 目标框、pan、tilt。
- 左右轮目标速度、实际速度、PWM duty。
- 编码器计数。
- 跟随控制量：目标大小、朝向误差、转向输出、基础速度。
- 当前底盘 PID 参数。

## 3. 自动跟随逻辑

当前跟随逻辑位于：

```text
main/station_example_main.c
```

跟随策略：

- K230 提供目标是否有效、目标框大小、云台 pan/tilt 角度。
- ESP32 使用 `pan - 90度` 估计目标左右偏差。
- 如果 MPU6050 可用，则结合 yaw 角计算目标朝向。
- 根据目标朝向误差计算左右轮转向差速。
- 根据人脸框大小计算前进速度：目标框太小则前进，目标框接近目标大小则停止。
- 超声波检测到障碍物过近时停车保持。
- 目标丢失、目标过近、tilt 超范围时停车。

目前已切换为“灵敏版跟随参数”：

```c
FOLLOW_PAN_DEADBAND_DEG      2.0f
FOLLOW_PID_KP               42.0f
FOLLOW_HEADING_FILTER_ALPHA  0.55f
FOLLOW_SPEED_PID_KP         150.0f
FOLLOW_FORWARD_ACCEL_LIMIT  6000.0f
FOLLOW_FACE_FILTER_ALPHA     0.45f
FOLLOW_MAX_TURN_OUTPUT      3600.0f
```

这版响应更快，但如果出现左右摆动，可以适当降低 `FOLLOW_PID_KP` 或 `FOLLOW_HEADING_FILTER_ALPHA`。

## 4. 底盘 PID 在线调节

底盘电机闭环 PID 已经支持运行时修改。

参数包括：

- `kp`
- `ki`
- `kd`
- `target_max_pps`
- `integral_limit`
- `correction_limit`
- `min_duty`

前端通过 MQTT 下发到：

```text
qrs/robot01/pid/request
```

ESP32 收到后会调用 `motor_set_pid_config()` 更新底盘 PID，并通过 `cmd/ack` 返回执行结果。

状态上报 `status.pid` 会显示 ESP32 当前实际使用的 PID 参数。

PID 参数现在会保存到 ESP32 NVS：

- `motor_init()` 启动时会尝试从 NVS 读取上次保存的 PID。
- 网页/App 下发 PID 后，ESP32 会先应用参数，再写入 NVS。
- 保存成功后 `cmd/ack` 返回 `ok=true`。
- 如果 NVS 保存失败，`cmd/ack` 会返回失败原因，便于前端提示。
- 第一次启动或 NVS 中没有保存值时，使用固件默认 PID。

## 5. 网页端实时仪表盘

网页端位置：

```text
raspberry_pi_voice/dashboard/
```

已实现功能：

- WebSocket 连接状态显示。
- MQTT broker 连接状态显示。
- ESP32 在线/离线状态显示。
- 小车状态卡片。
- 超声波距离显示。
- 左右轮速度显示。
- 左右轮 PWM / command 显示。
- yaw、yaw rate、pan、tilt、heading error、turn output 显示。
- 实时曲线：平均速度、距离、yaw。
- 目标框可视化。
- MQTT 原始事件流显示。
- 快捷控制：前进、后退、左转、右转、急停。
- 模式控制：恢复跟随、保持、云台回中。
- 底盘 PID 参数在线编辑和下发。
- 手机端响应式布局。

网页服务入口：

```text
raspberry_pi_voice/dashboard_server.py
```

启动示例：

```powershell
python raspberry_pi_voice\dashboard_server.py
```

如果需要让手机访问电脑或树莓派上的 WebSocket：

```powershell
$env:DASHBOARD_WS_HOST="0.0.0.0"
python raspberry_pi_voice\dashboard_server.py
```

## 6. Android App

Android App 位置：

```text
android_app/
```

已完成：

- 生成 debug APK。
- App 名称为“小车控制台”。
- 内置仪表盘页面，不再需要用户输入完整网页 URL。
- WebView 支持 JavaScript、DOM Storage、HTTP/WebSocket 明文访问。
- 顶部显示当前后台地址。
- 提供“刷新”按钮。
- 提供“后台”按钮选择 WebSocket 后台主机。
- 支持以下后台选项：
  - Android 模拟器访问电脑：`10.0.2.2:8765`
  - 手机本机：`127.0.0.1:8765`
  - 树莓派：`10.179.231.220:8765`
  - 自定义 IP
- APK 构建脚本：

```text
android_app/scripts/build_debug_apk.ps1
```

当前 APK 输出：

```text
android_app/tracking-car-dashboard-debug.apk
```

## 7. 树莓派 / 电脑桥接服务

`dashboard_server.py` 已完成：

- 连接 Mosquitto MQTT broker。
- 订阅 `qrs/robot01/#`。
- 将 MQTT 消息转发给 WebSocket 客户端。
- 将网页/App 控制命令转成 MQTT request。
- 提供静态网页服务。
- HTTP 端口占用时会自动尝试后续端口。
- 前端文件禁用缓存，避免旧版本 JS 干扰。

常用环境变量：

```text
MQTT_HOST
MQTT_PORT
MQTT_TOPIC_ROOT
MQTT_DEVICE_ID
DASHBOARD_HTTP_HOST
DASHBOARD_HTTP_PORT
DASHBOARD_WS_HOST
DASHBOARD_WS_PORT
```

## 8. 已修复的问题

- 修复 ESP32 flash 配置过小导致分区表构建失败的问题。
- 修复网页端 PID 参数显示被旧缓存覆盖的问题。
- 修复 PID 应用后输入框被旧 `status.pid` 自动改回的问题。
- 修复 Android App 顶部地址栏拥挤、中文乱码的问题。
- 修复 Android APK 内置页面路径加载失败的问题。
- 修复 Android APK 因 JS 内联替换导致启动崩溃的问题。
- 将跟随参数调整为更灵敏的版本。

## 9. 当前仍需注意

- 真手机里的 `127.0.0.1` 是手机自己，不是电脑。
- Android 模拟器访问电脑本机一般使用 `10.0.2.2`。
- 真手机访问电脑或树莓派时，需要手机和后台主机在同一局域网。
- App 内置的是界面，但实时数据仍然依赖 `dashboard_server.py` 的 WebSocket 服务。
- 小车没开时 App 也应该能打开界面，只是显示离线状态。
- PID 调节目前只影响底盘轮速闭环，不等于跟随策略参数。
- 跟随策略参数目前仍是固件编译期宏，尚未做成网页/App 动态调节。

## 10. 后续可继续完善

- 将跟随策略参数也做成网页/App 在线调节。
- App 增加自动扫描局域网后台服务。
- Android App 改为原生页面，而不是 WebView。
- 增加实时摄像头画面预览。
- 增加小车运行日志下载。
- 增加急停保护确认和危险参数限制。
