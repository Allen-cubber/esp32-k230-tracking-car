import asyncio
import json
import os
import threading
import time
import uuid
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Optional, Set, Tuple

import paho.mqtt.client as mqtt
import websockets


BASE_DIR = Path(__file__).resolve().parent
DASHBOARD_DIR = BASE_DIR / "dashboard"


def env_int(name: str, default: int) -> int:
    try:
        return int(os.getenv(name, str(default)))
    except ValueError:
        return default


def mqtt_client(client_id: str) -> mqtt.Client:
    callback_api_version = getattr(mqtt, "CallbackAPIVersion", None)
    if callback_api_version is not None:
        return mqtt.Client(callback_api_version.VERSION1, client_id=client_id, clean_session=True)
    return mqtt.Client(client_id=client_id, clean_session=True)


class DashboardBridge:
    def __init__(self) -> None:
        self.mqtt_host = os.getenv("MQTT_HOST", "127.0.0.1")
        self.mqtt_port = env_int("MQTT_PORT", 1883)
        self.mqtt_username = os.getenv("MQTT_USERNAME", "")
        self.mqtt_password = os.getenv("MQTT_PASSWORD", "")
        self.topic_root = os.getenv("MQTT_TOPIC_ROOT", "qrs").rstrip("/")
        self.device_id = os.getenv("MQTT_DEVICE_ID", "robot01")
        self.base_topic = f"{self.topic_root}/{self.device_id}"
        self.clients: Set[Any] = set()
        self.latest: Dict[str, Dict[str, Any]] = {}
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.connected = False

        client_id = os.getenv("DASHBOARD_MQTT_CLIENT_ID", f"{self.device_id}-dashboard")
        self.client = mqtt_client(client_id)
        if self.mqtt_username:
            self.client.username_pw_set(self.mqtt_username, self.mqtt_password)
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

    def start_mqtt(self) -> None:
        self.client.connect_async(self.mqtt_host, self.mqtt_port, 60)
        self.client.loop_start()

    def stop_mqtt(self) -> None:
        self.client.loop_stop()
        self.client.disconnect()

    def on_connect(self, client: mqtt.Client, userdata: Any, flags: Dict[str, Any], rc: int) -> None:
        self.connected = rc == 0
        if rc == 0:
            client.subscribe(f"{self.base_topic}/#", qos=0)
        self.push_threadsafe({
            "type": "broker",
            "connected": self.connected,
            "rc": rc,
            "base_topic": self.base_topic,
            "mqtt_host": self.mqtt_host,
            "mqtt_port": self.mqtt_port,
        })

    def on_disconnect(self, client: mqtt.Client, userdata: Any, rc: int) -> None:
        self.connected = False
        self.push_threadsafe({"type": "broker", "connected": False, "rc": rc})

    def on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        text = message.payload.decode("utf-8", errors="replace")
        try:
            payload: Any = json.loads(text)
        except json.JSONDecodeError:
            payload = {"raw": text}

        packet = {
            "type": "mqtt",
            "topic": message.topic,
            "suffix": message.topic.removeprefix(f"{self.base_topic}/"),
            "payload": payload,
            "received_at": time.time(),
        }
        self.latest[message.topic] = packet
        self.push_threadsafe(packet)

    def push_threadsafe(self, packet: Dict[str, Any]) -> None:
        if self.loop is None:
            return
        self.loop.call_soon_threadsafe(lambda: asyncio.create_task(self.broadcast(packet)))

    async def broadcast(self, packet: Dict[str, Any]) -> None:
        if not self.clients:
            return
        text = json.dumps(packet, ensure_ascii=False)
        stale = []
        for ws in self.clients:
            try:
                await ws.send(text)
            except websockets.ConnectionClosed:
                stale.append(ws)
        for ws in stale:
            self.clients.discard(ws)

    async def ws_handler(self, websocket: Any, path: Optional[str] = None) -> None:
        self.clients.add(websocket)
        await websocket.send(json.dumps({
            "type": "hello",
            "base_topic": self.base_topic,
            "mqtt_host": self.mqtt_host,
            "mqtt_port": self.mqtt_port,
            "connected": self.connected,
            "latest": list(self.latest.values()),
        }, ensure_ascii=False))
        try:
            async for text in websocket:
                await self.handle_ws_message(text)
        finally:
            self.clients.discard(websocket)

    async def handle_ws_message(self, text: str) -> None:
        try:
            packet = json.loads(text)
        except json.JSONDecodeError:
            return

        msg_type = packet.get("type")
        if msg_type == "execute_action":
            payload = packet.get("payload")
            if isinstance(payload, dict):
                self.publish_action(payload)
        elif msg_type == "gimbal":
            action = str(packet.get("action", "center"))
            self.publish_gimbal(action,
                                int(packet.get("pan_delta", 0)),
                                int(packet.get("tilt_delta", 0)),
                                int(packet.get("duration_ms", 800)))
        elif msg_type == "set_pid":
            payload = packet.get("payload")
            if isinstance(payload, dict):
                self.publish_pid(payload)

    def publish_action(self, payload: Dict[str, Any]) -> None:
        request = {
            "request_id": f"dashboard-{int(time.time() * 1000)}-{uuid.uuid4().hex[:6]}",
            "cmd": "execute_action",
            "payload": payload,
        }
        self.client.publish(f"{self.base_topic}/cmd/request",
                            json.dumps(request, ensure_ascii=False),
                            qos=1)

    def publish_gimbal(self, action: str, pan_delta: int, tilt_delta: int, duration_ms: int) -> None:
        payload = {
            "action": action,
            "pan_delta": pan_delta,
            "tilt_delta": tilt_delta,
            "duration_ms": duration_ms,
        }
        self.client.publish(f"{self.base_topic}/gimbal/request",
                            json.dumps(payload, ensure_ascii=False),
                            qos=0)

    def publish_pid(self, payload: Dict[str, Any]) -> None:
        request = {
            "request_id": f"pid-{int(time.time() * 1000)}-{uuid.uuid4().hex[:6]}",
            "target_max_pps": float(payload.get("target_max_pps", 3000)),
            "kp": float(payload.get("kp", 0.65)),
            "ki": float(payload.get("ki", 0.18)),
            "kd": float(payload.get("kd", 0.0)),
            "integral_limit": float(payload.get("integral_limit", 4000)),
            "correction_limit": float(payload.get("correction_limit", 2200)),
            "min_duty": int(float(payload.get("min_duty", 500))),
        }
        self.client.publish(f"{self.base_topic}/pid/request",
                            json.dumps(request, ensure_ascii=False),
                            qos=1)


class NoCacheDashboardHandler(SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()


def start_http_server(host: str, port: int) -> Tuple[ThreadingHTTPServer, int]:
    handler = partial(NoCacheDashboardHandler, directory=str(DASHBOARD_DIR))
    last_error: Optional[OSError] = None
    for candidate in range(port, port + 20):
        try:
            server = ThreadingHTTPServer((host, candidate), handler)
        except OSError as exc:
            last_error = exc
            continue
        thread = threading.Thread(target=server.serve_forever, name="dashboard-http", daemon=True)
        thread.start()
        return server, candidate
    raise RuntimeError(f"no available HTTP port from {port} to {port + 19}: {last_error}")


async def start_ws_server(bridge: DashboardBridge, host: str, port: int) -> Tuple[Any, int]:
    last_error: Optional[OSError] = None
    for candidate in range(port, port + 20):
        try:
            server = await websockets.serve(bridge.ws_handler, host, candidate)
        except OSError as exc:
            last_error = exc
            continue
        return server, candidate
    raise RuntimeError(f"no available WebSocket port from {port} to {port + 19}: {last_error}")


async def main() -> None:
    http_host = os.getenv("DASHBOARD_HTTP_HOST", "127.0.0.1")
    http_port = env_int("DASHBOARD_HTTP_PORT", 8080)
    ws_host = os.getenv("DASHBOARD_WS_HOST", "127.0.0.1")
    ws_port = env_int("DASHBOARD_WS_PORT", 8765)

    bridge = DashboardBridge()
    bridge.loop = asyncio.get_running_loop()
    http_server, http_port = start_http_server(http_host, http_port)
    ws_server, ws_port = await start_ws_server(bridge, ws_host, ws_port)
    bridge.start_mqtt()

    print(f"[dashboard] http://{http_host}:{http_port}/?wsPort={ws_port}")
    print(f"[dashboard] ws://{ws_host}:{ws_port}")
    print(f"[dashboard] MQTT {bridge.mqtt_host}:{bridge.mqtt_port} topic={bridge.base_topic}/#")

    try:
        await asyncio.Future()
    finally:
        bridge.stop_mqtt()
        ws_server.close()
        await ws_server.wait_closed()
        http_server.shutdown()


if __name__ == "__main__":
    asyncio.run(main())
