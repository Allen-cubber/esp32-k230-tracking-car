import os
from dataclasses import dataclass
from pathlib import Path
from typing import Tuple


DEFAULT_EXIT_WORDS = ("退出", "再见", "不用了")


def _load_dotenv(path: str = ".env") -> None:
    env_path = Path(path)
    if not env_path.exists():
        return

    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


def _bool_env(name: str, default: bool = False) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on", "y"}


def _exit_words_from_env() -> Tuple[str, ...]:
    raw = os.getenv("EXIT_WORDS", "")
    words = tuple(item.strip() for item in raw.split(",") if item.strip())
    return words or DEFAULT_EXIT_WORDS


@dataclass
class AppConfig:
    xf_appid: str
    xf_api_key: str
    xf_api_secret: str
    dashscope_api_key: str
    llm_model: str
    tts_voice: str
    sample_rate: int
    mic_device: str
    wake_word: str
    exit_words: Tuple[str, ...]
    conversation_timeout_s: float
    asr_listen_window_s: float
    asr_rms_threshold: int
    asr_voice_hold_s: float
    asr_no_update_timeout_s: float
    asr_min_listen_s: float
    tts_cooldown_s: float
    tts_enable: bool
    mqtt_host: str
    mqtt_port: int
    mqtt_username: str
    mqtt_password: str
    mqtt_client_id: str
    mqtt_device_id: str
    mqtt_topic_root: str
    mqtt_keepalive_s: int
    mqtt_ack_timeout_s: float
    dry_run: bool
    always_active: bool

    @classmethod
    def from_env(cls) -> "AppConfig":
        _load_dotenv()
        return cls(
            xf_appid=os.getenv("XF_APPID", ""),
            xf_api_key=os.getenv("XF_API_KEY", ""),
            xf_api_secret=os.getenv("XF_API_SECRET", ""),
            dashscope_api_key=os.getenv("DASHSCOPE_API_KEY", ""),
            llm_model=os.getenv("LLM_MODEL", "deepseek-v3"),
            tts_voice=os.getenv("TTS_VOICE", "sambert-zhiya-v1"),
            sample_rate=int(os.getenv("SAMPLE_RATE", "16000")),
            mic_device=os.getenv("MIC_DEVICE", "hw:2,0"),
            wake_word=os.getenv("WAKE_WORD", "小车"),
            exit_words=_exit_words_from_env(),
            conversation_timeout_s=float(os.getenv("CONVERSATION_TIMEOUT_S", "30")),
            asr_listen_window_s=float(os.getenv("ASR_LISTEN_WINDOW_S", "6.0")),
            asr_rms_threshold=int(os.getenv("ASR_RMS_THRESHOLD", "500")),
            asr_voice_hold_s=float(os.getenv("ASR_VOICE_HOLD_S", "0.5")),
            asr_no_update_timeout_s=float(os.getenv("ASR_NO_UPDATE_TIMEOUT_S", "1.2")),
            asr_min_listen_s=float(os.getenv("ASR_MIN_LISTEN_S", "1.0")),
            tts_cooldown_s=float(os.getenv("TTS_COOLDOWN_S", "0.6")),
            tts_enable=_bool_env("TTS_ENABLE", True),
            mqtt_host=os.getenv("MQTT_HOST", "127.0.0.1"),
            mqtt_port=int(os.getenv("MQTT_PORT", "1883")),
            mqtt_username=os.getenv("MQTT_USERNAME", ""),
            mqtt_password=os.getenv("MQTT_PASSWORD", ""),
            mqtt_client_id=os.getenv("MQTT_CLIENT_ID", ""),
            mqtt_device_id=os.getenv("MQTT_DEVICE_ID", "robot01"),
            mqtt_topic_root=os.getenv("MQTT_TOPIC_ROOT", "qrs"),
            mqtt_keepalive_s=int(os.getenv("MQTT_KEEPALIVE_S", "60")),
            mqtt_ack_timeout_s=float(os.getenv("MQTT_ACK_TIMEOUT_S", "4.0")),
            dry_run=_bool_env("VOICE_DRY_RUN", False),
            always_active=_bool_env("VOICE_ALWAYS_ACTIVE", False),
        )

    def has_valid_xf_keys(self) -> bool:
        return bool(self.xf_appid and self.xf_api_key and self.xf_api_secret)

    def has_valid_dashscope_key(self) -> bool:
        return bool(self.dashscope_api_key)
