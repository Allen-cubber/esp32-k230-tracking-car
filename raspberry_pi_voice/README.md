# Raspberry Pi Voice Gateway

这个目录是树莓派端语音网关，可整体复制到树莓派运行。它默认连接树莓派本机 Mosquitto broker，通过 MQTT 把语音解析出的结构化动作发给 ESP32。

## 安装

```bash
cd ~/raspberry_pi_voice
conda activate xiaoji310
pip install -r requirements.txt

sudo apt install -y mosquitto mosquitto-clients alsa-utils sox
sudo systemctl enable --now mosquitto
```

## 配置

推荐用环境变量：

```bash
export MQTT_HOST=127.0.0.1
export MQTT_PORT=1883
export MQTT_DEVICE_ID=robot01
export MQTT_TOPIC_ROOT=qrs

export XF_APPID=你的讯飞appid
export XF_API_KEY=你的讯飞api_key
export XF_API_SECRET=你的讯飞api_secret
export DASHSCOPE_API_KEY=你的DashScope或兼容模型key
```

也可以复制 `.env.example` 为 `.env`，程序会自动读取 `.env` 中尚未被 shell export 覆盖的配置。

## 启动

语音模式：

```bash
python3 voice_gateway.py
```

文本调试模式：

```bash
VOICE_DRY_RUN=1 python3 voice_gateway.py --text
```

`--text` 模式会直接进入激活状态，不需要先说唤醒词。语音模式默认唤醒词是 `小车`，可通过 `WAKE_WORD` 修改。

## MQTT

树莓派发布命令：

```text
qrs/robot01/cmd/request
```

ESP32 后续应发布回执：

```text
qrs/robot01/cmd/ack
```

发布 payload 示例：

```json
{
  "request_id": "voice-1710000000000-1",
  "cmd": "execute_action",
  "payload": {
    "emotion": "neutral",
    "action": {
      "target": "chassis",
      "name": "forward",
      "speed": 35,
      "duration_ms": 1000,
      "pan_delta_deg": 0,
      "tilt_delta_deg": 0
    },
    "voice": {
      "text": "好的，我向前走一点。"
    }
  }
}
```

可以用下面命令观察发布结果：

```bash
mosquitto_sub -h 127.0.0.1 -t 'qrs/robot01/cmd/request'
```

## 安全策略

- 底盘动作限制为 `forward/backward/turn_left/turn_right/stop`。
- 云台动作限制为 `nod/shake/center/lock/unlock`。
- 模式动作限制为 `hold/resume_follow`。
- `speed` 会被限制在 `0..50`，`duration_ms` 会被限制在 `0..3000`。
- “停下/停止/别动/危险/急停”等文本会在本地优先转换为停车，不等待 LLM。
- “前进/后退/左转/右转/开始跟随/别跟了/点头/摇头/云台回中/锁定云台/解锁云台”等常用文本会走本地快捷规则，不等待 LLM。
- LLM 返回非法 JSON 或未知动作时不会发布 MQTT 动作。
