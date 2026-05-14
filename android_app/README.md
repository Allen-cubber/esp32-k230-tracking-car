# Tracking Car Android App

这是一个 Android WebView App，用来打开小车实时状态界面。界面文件已经打包进 APK，不需要再输入网页地址。

默认连接后台：

```text
ws://10.0.2.2:8765
```

注意：`10.0.2.2` 是 Android 模拟器访问电脑本机的地址。真手机访问电脑时，需要点 App 顶部“后台”，选择“自定义 IP”，填电脑的局域网 IP。

App 内置页面仍然需要 `dashboard_server.py` 提供 WebSocket/MQTT 桥接服务。

## 生成 APK

```powershell
cd C:\11\esp32-k230-tracking-car\android_app
.\scripts\build_debug_apk.ps1
```

输出文件：

```text
android_app\tracking-car-dashboard-debug.apk
```
