# 小车实时仪表盘

这个仪表盘运行在树莓派或同一局域网电脑上，通过 MQTT 订阅：

```text
qrs/robot01/status
qrs/robot01/track
qrs/robot01/cmd/ack
```

ESP32 会把速度、超声波距离、MPU yaw、K230 pan/tilt、人脸框和控制量发布到 `status` topic。浏览器不直接连 MQTT，而是连接 `dashboard_server.py` 暴露的 WebSocket。

## 启动

```bash
cd raspberry_pi_voice
pip install -r requirements.txt
python3 dashboard_server.py
```

Windows PowerShell 本地连接树莓派 broker：

```powershell
cd C:\11\esp32-k230-tracking-car\raspberry_pi_voice
$env:MQTT_HOST="10.179.231.220"
$env:MQTT_PORT="1883"
python dashboard_server.py
```

手机访问时，服务要监听局域网地址：

```bash
export DASHBOARD_HTTP_HOST=0.0.0.0
export DASHBOARD_WS_HOST=0.0.0.0
export MQTT_HOST=127.0.0.1
python3 dashboard_server.py
```

然后手机浏览器打开：

```text
http://树莓派IP:8080
```

页面会在手机宽度下切换成底部 Tab：状态、控制、曲线、日志。

## 底盘 PID 调节

控制页包含“底盘 PID”面板，点击“应用 PID”会发布到：

```text
qrs/robot01/pid/request
```

ESP32 应用后会通过 `qrs/robot01/cmd/ack` 返回 `cmd=set_pid`，当前 PID 会随 `qrs/robot01/status` 的 `pid` 字段回显。

## 安装到手机主屏幕

当前页面已经带 PWA manifest、图标和 service worker。手机浏览器如果允许安装，会在“控制”页显示“安装”按钮；也可以用浏览器菜单里的“添加到主屏幕”。

注意：标准 PWA 安装通常要求 HTTPS。局域网 `http://树莓派IP:8080` 可以先作为手机网页 App 使用；如果需要正式 APK，可以后续把这个页面用 Capacitor 包成 Android 应用。

可用环境变量：

```bash
export MQTT_HOST=127.0.0.1
export MQTT_PORT=1883
export MQTT_TOPIC_ROOT=qrs
export MQTT_DEVICE_ID=robot01
export DASHBOARD_HTTP_PORT=8080
export DASHBOARD_WS_PORT=8765
```

如果 `8080` 或 `8765` 被占用，服务会自动尝试后续端口，并在启动日志里打印最终地址。

如果修改了 WebSocket 端口，打开页面时加上查询参数，例如：

```text
http://树莓派IP:8080/?wsPort=8766
```

如果是在 Windows 本机预览，默认地址是：

```text
http://127.0.0.1:8080
```
