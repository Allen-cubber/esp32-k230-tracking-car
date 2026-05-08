import asyncio
import base64
import hashlib
import hmac
import json
import math
import struct
import subprocess
import time
from datetime import datetime, timezone
from typing import Callable
from urllib.parse import urlencode

import websockets

from .config import AppConfig


class XfAsrClient:
    def __init__(self, config: AppConfig) -> None:
        self.config = config

    def build_auth_url(self) -> str:
        date = datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S GMT")
        signature_origin = f"host: iat.xf-yun.com\ndate: {date}\nGET /v1 HTTP/1.1"
        signature_sha = hmac.new(
            self.config.xf_api_secret.encode("utf-8"),
            signature_origin.encode("utf-8"),
            hashlib.sha256,
        ).digest()
        signature = base64.b64encode(signature_sha).decode("utf-8")
        auth_str = (
            f'api_key="{self.config.xf_api_key}", algorithm="hmac-sha256", '
            f'headers="host date request-line", signature="{signature}"'
        )
        params = {
            "authorization": base64.b64encode(auth_str.encode("utf-8")).decode("utf-8"),
            "date": date,
            "host": "iat.xf-yun.com",
        }
        return f"wss://iat.xf-yun.com/v1?{urlencode(params)}"

    @staticmethod
    def _pcm_rms(frame: bytes) -> int:
        sample_count = len(frame) // 2
        if sample_count <= 0:
            return 0
        samples = struct.unpack_from(f"<{sample_count}h", frame)
        square_sum = sum(sample * sample for sample in samples)
        return int(math.sqrt(square_sum / sample_count))

    @staticmethod
    def _extract_words(response: dict) -> str:
        header = response.get("header", {})
        if header.get("code") != 0 or "payload" not in response:
            return ""

        try:
            raw_text = json.loads(
                base64.b64decode(response["payload"]["result"]["text"]).decode("utf-8")
            )
        except (KeyError, ValueError, json.JSONDecodeError):
            return ""

        words = "".join(
            candidate.get("w", "")
            for item in raw_text.get("ws", [])
            for candidate in item.get("cw", [])
        )
        if words and words.strip() not in {"。", ".", "？", "?", "，", ","}:
            return words
        return ""

    async def _send_audio_frame(self, ws, seq: int, status: int, audio: bytes) -> None:
        await ws.send(json.dumps({
            "header": {"app_id": self.config.xf_appid, "status": status},
            "payload": {
                "audio": {
                    "encoding": "raw",
                    "sample_rate": 16000,
                    "seq": seq,
                    "status": status,
                    "audio": base64.b64encode(audio).decode("utf-8"),
                }
            },
        }))

    async def recognize_once(
        self,
        status_label: str,
        capture_blocked: Callable[[], bool],
        listen_window_s: float,
    ) -> str:
        if not self.config.has_valid_xf_keys():
            raise RuntimeError("XF_APPID/XF_API_KEY/XF_API_SECRET are not configured")

        async with websockets.connect(self.build_auth_url()) as ws:
            await ws.send(json.dumps({
                "header": {"app_id": self.config.xf_appid, "status": 0},
                "parameter": {
                    "iat": {
                        "domain": "slm",
                        "language": "zh_cn",
                        "accent": "mandarin",
                        "dwa": "wpgs",
                        "result": {"encoding": "utf8", "format": "json"},
                    }
                },
                "payload": {
                    "audio": {
                        "encoding": "raw",
                        "sample_rate": 16000,
                        "channels": 1,
                        "bit_depth": 16,
                        "seq": 0,
                        "status": 0,
                        "audio": "",
                    }
                },
            }))

            cmd = (
                f"arecord -D {self.config.mic_device} -f S16_LE -r 48000 -c 2 -t raw -q | "
                "sox -t raw -r 48000 -c 2 -b 16 -e signed-integer - "
                "-t raw -r 16000 -c 1 -b 16 -e signed-integer -"
            )
            proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            print(f"\n[状态: {status_label}] 正在听...")

            try:
                if proc.stdout is None:
                    return ""

                full_text = ""
                seq = 1
                started_at = time.monotonic()
                window_deadline = time.monotonic() + listen_window_s
                voice_hold_until = 0.0
                last_text_update = 0.0
                speech_started = self.config.asr_rms_threshold <= 0

                while time.monotonic() < window_deadline:
                    data = proc.stdout.read(1280)
                    if not data:
                        break

                    if capture_blocked():
                        await asyncio.sleep(0.02)
                        continue

                    now = time.monotonic()
                    rms = self._pcm_rms(data)
                    if speech_started:
                        pass
                    elif rms >= self.config.asr_rms_threshold:
                        speech_started = True
                        voice_hold_until = now + self.config.asr_voice_hold_s
                    elif now > voice_hold_until:
                        await asyncio.sleep(0)
                        continue

                    await self._send_audio_frame(ws, seq, 1, data)
                    seq += 1

                    try:
                        resp = await asyncio.wait_for(ws.recv(), timeout=0.01)
                        res = json.loads(resp)
                        words = self._extract_words(res)
                        if words:
                            full_text = words
                            last_text_update = time.monotonic()
                            print(f"\r[识别]: {full_text}", end="", flush=True)
                        if res.get("header", {}).get("status") == 2:
                            break
                    except asyncio.TimeoutError:
                        pass

                    now = time.monotonic()
                    if (
                        full_text
                        and now - started_at >= self.config.asr_min_listen_s
                        and last_text_update > 0
                        and now - last_text_update >= self.config.asr_no_update_timeout_s
                    ):
                        break

                await self._send_audio_frame(ws, seq, 2, b"")

                flush_deadline = time.monotonic() + 1.0
                while time.monotonic() < flush_deadline:
                    try:
                        resp = await asyncio.wait_for(ws.recv(), timeout=0.1)
                    except asyncio.TimeoutError:
                        break
                    res = json.loads(resp)
                    words = self._extract_words(res)
                    if words:
                        full_text = words
                        print(f"\r[识别]: {full_text}", end="", flush=True)
                    if res.get("header", {}).get("status") == 2:
                        break

                return full_text.replace("。", "").strip()
            finally:
                proc.terminate()
