import json
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Dict, Optional

import paho.mqtt.client as mqtt

from .cloud_topics import build_topics
from .config import AppConfig


def _build_client(client_id: str) -> mqtt.Client:
    callback_api_version = getattr(mqtt, "CallbackAPIVersion", None)
    if callback_api_version is not None:
        return mqtt.Client(callback_api_version.VERSION1, client_id=client_id, clean_session=True)
    return mqtt.Client(client_id=client_id, clean_session=True)


@dataclass
class PendingRequest:
    event: threading.Event = field(default_factory=threading.Event)
    response: Optional[Dict[str, Any]] = None


class RobotMqttClient:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        self.topics = build_topics(config.mqtt_topic_root, config.mqtt_device_id)
        self._lock = threading.Lock()
        self._connected = threading.Event()
        self._pending: Dict[str, PendingRequest] = {}
        self._request_seq = 0
        self.latest_status: Dict[str, Any] = {}

        client_id = config.mqtt_client_id or f"{config.mqtt_device_id}-voice"
        self.client = _build_client(client_id)
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        if config.mqtt_username:
            self.client.username_pw_set(config.mqtt_username, config.mqtt_password)

    def start(self) -> None:
        if self.config.dry_run:
            print("[MQTT] dry-run enabled, commands will be printed only")
            self._connected.set()
            return
        self.client.connect_async(self.config.mqtt_host, self.config.mqtt_port, self.config.mqtt_keepalive_s)
        self.client.loop_start()
        print(f"[MQTT] connecting to {self.config.mqtt_host}:{self.config.mqtt_port}")

    def stop(self) -> None:
        if self.config.dry_run:
            return
        self.client.loop_stop()
        self.client.disconnect()

    def wait_connected(self, timeout_s: float = 5.0) -> bool:
        return self._connected.wait(timeout_s)

    def _next_request_id(self) -> str:
        with self._lock:
            self._request_seq += 1
            seq = self._request_seq
        return f"voice-{int(time.time() * 1000)}-{seq}"

    def execute_action(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        request_id = self._next_request_id()
        packet = {
            "request_id": request_id,
            "cmd": "execute_action",
            "payload": payload,
        }

        if self.config.dry_run:
            print(f"[MQTT dry-run] -> {json.dumps(packet, ensure_ascii=False)}")
            return {"request_id": request_id, "cmd": "execute_action", "ok": True, "dry_run": True}

        if not self.client.is_connected():
            return {"request_id": request_id, "cmd": "execute_action", "ok": False, "error": "mqtt_disconnected"}

        pending = PendingRequest()
        with self._lock:
            self._pending[request_id] = pending

        info = self.client.publish(self.topics["cmd_request"], json.dumps(packet, ensure_ascii=False), qos=1)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            with self._lock:
                self._pending.pop(request_id, None)
            return {"request_id": request_id, "cmd": "execute_action", "ok": False, "error": "publish_failed"}

        print(f"[MQTT] -> {json.dumps(packet, ensure_ascii=False)}")
        if not pending.event.wait(self.config.mqtt_ack_timeout_s):
            with self._lock:
                self._pending.pop(request_id, None)
            return {"request_id": request_id, "cmd": "execute_action", "ok": False, "error": "command_timeout"}

        return pending.response or {"request_id": request_id, "cmd": "execute_action", "ok": False, "error": "empty_ack"}

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Dict[str, Any], rc: int) -> None:
        if rc != 0:
            print(f"[MQTT] connect failed rc={rc}")
            return
        client.subscribe(self.topics["cmd_ack"], qos=1)
        client.subscribe(self.topics["status"], qos=1)
        self._connected.set()
        print("[MQTT] connected")

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, rc: int) -> None:
        self._connected.clear()
        print(f"[MQTT] disconnected rc={rc}")

    def _on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        try:
            payload = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return

        if message.topic == self.topics["cmd_ack"]:
            request_id = str(payload.get("request_id", ""))
            with self._lock:
                pending = self._pending.pop(request_id, None)
            if pending is not None:
                pending.response = payload
                pending.event.set()
            print(f"[MQTT] <- {json.dumps(payload, ensure_ascii=False)}")
            return

        if message.topic == self.topics["status"]:
            self.latest_status = payload
