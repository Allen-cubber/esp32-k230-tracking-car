import asyncio
import time
from typing import Dict

from .asr import XfAsrClient
from .config import AppConfig
from .llm import ActionPlanner
from .robot_mqtt import RobotMqttClient
from .schema import is_stop_request, make_stop_payload, should_dispatch
from .speech import SpeechEngine


STATE_IDLE = 0
STATE_ACTIVE = 1


class VoiceCarAssistant:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        self.state = STATE_ACTIVE if config.always_active else STATE_IDLE
        self.active_deadline = 0.0
        self.robot = RobotMqttClient(config)
        self.speech = SpeechEngine(config)
        self.planner = ActionPlanner(config)
        self.asr = XfAsrClient(config)
        if config.always_active:
            self._refresh_active_deadline()

    def start(self) -> None:
        self.robot.start()
        if not self.robot.wait_connected(timeout_s=5.0):
            print("[MQTT] not connected yet; voice loop will continue")

    def stop(self) -> None:
        self.robot.stop()

    def _status_label(self) -> str:
        return "待机" if self.state == STATE_IDLE else "激活"

    def _refresh_active_deadline(self) -> None:
        self.active_deadline = time.monotonic() + self.config.conversation_timeout_s

    def _enter_active(self, prompt: str = "我在，请讲") -> None:
        self.state = STATE_ACTIVE
        self._refresh_active_deadline()
        self.speech.speak_async(prompt)

    def _exit_active(self, prompt: str = "") -> None:
        self.state = STATE_IDLE
        self.active_deadline = 0.0
        if prompt:
            self.speech.speak_async(prompt)

    def _check_timeout(self) -> None:
        if self.config.always_active or self.state != STATE_ACTIVE:
            return
        if time.monotonic() >= self.active_deadline:
            print("[状态] 对话超时，返回待机")
            self._exit_active()

    @staticmethod
    def _ack_text(ack: Dict) -> str:
        if ack.get("ok"):
            return ""
        error = ack.get("error", "unknown")
        if error in {"command_timeout", "mqtt_disconnected"}:
            return "车辆暂未响应"
        return f"动作发送失败，原因 {error}"

    def _dispatch_payload(self, payload: Dict) -> Dict:
        if not should_dispatch(payload):
            return {"ok": True, "skipped": "none_action"}
        return self.robot.execute_action(payload)

    def handle_user_text(self, text: str) -> None:
        clean_text = text.replace("。", "").strip()
        if not clean_text:
            self._check_timeout()
            return

        if self.state == STATE_IDLE:
            if self.config.wake_word in clean_text:
                print("[状态] 已唤醒")
                self._enter_active()
            return

        if clean_text in self.config.exit_words:
            self._exit_active("再见")
            return

        self._refresh_active_deadline()
        print(f"\n[用户] {clean_text}")

        if is_stop_request(clean_text):
            payload = make_stop_payload()
            print(f"[本地安全] 停止请求 -> {payload}")
            ack = self._dispatch_payload(payload)
            spoken = payload["voice"]["text"]
            ack_text = self._ack_text(ack)
            self.speech.speak_async("，".join(part for part in (spoken, ack_text) if part))
            return

        payload, error, raw_reply = self.planner.ask(clean_text)
        if raw_reply:
            print(f"[LLM] {raw_reply}")

        if error or payload is None:
            print(f"[解析失败] {error}")
            self.speech.speak_async("我没有解析出安全动作")
            return

        print(f"[动作] {payload}")
        ack = self._dispatch_payload(payload)
        voice_text = payload.get("voice", {}).get("text", "") or "好的"
        ack_text = self._ack_text(ack)
        self.speech.speak_async("，".join(part for part in (voice_text, ack_text) if part))

    async def run_voice_loop(self) -> None:
        while True:
            try:
                await self.speech.wait_until_capture_ready()
                text = await self.asr.recognize_once(
                    self._status_label(),
                    self.speech.capture_blocked,
                    self.config.asr_listen_window_s,
                )
                self.handle_user_text(text)
                self._check_timeout()
            except Exception as exc:
                print(f"[错误] {exc}")
                await asyncio.sleep(1)

    def run_text_loop(self) -> None:
        print("[文本模式] 输入模拟 ASR 文本，Ctrl+C 退出")
        while True:
            try:
                text = input("> ")
            except EOFError:
                break
            self.handle_user_text(text)
            self._check_timeout()
