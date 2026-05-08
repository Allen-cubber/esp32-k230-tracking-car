import asyncio
import os
import queue
import tempfile
import threading
import time

import dashscope
import pygame
from dashscope.audio.tts import SpeechSynthesizer

from .config import AppConfig


class SpeechEngine:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        dashscope.api_key = config.dashscope_api_key
        self._is_speaking = False
        self._cooldown_until = 0.0
        self._pending_count = 0
        self._lock = threading.Lock()
        self._queue: queue.Queue[str] = queue.Queue()
        self._worker = threading.Thread(target=self._worker_loop, daemon=True)
        self._worker.start()

    def capture_blocked(self) -> bool:
        with self._lock:
            return self._is_speaking or self._pending_count > 0 or time.time() < self._cooldown_until

    async def wait_until_capture_ready(self, poll_interval_s: float = 0.05) -> None:
        while self.capture_blocked():
            await asyncio.sleep(poll_interval_s)

    def _play_text(self, text: str) -> None:
        clean_text = text.replace("\n", " ").strip()
        if not clean_text:
            return

        if not self.config.tts_enable:
            print(f"[TTS disabled] {clean_text}")
            return

        with self._lock:
            self._is_speaking = True

        wav_file = ""
        try:
            if not self.config.has_valid_dashscope_key():
                raise RuntimeError("DASHSCOPE_API_KEY is not configured")

            result = SpeechSynthesizer.call(
                model=self.config.tts_voice,
                text=clean_text,
                sample_rate=self.config.sample_rate,
                format="wav",
            )
            audio = result.get_audio_data()
            if not audio:
                raise RuntimeError("empty TTS audio")

            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
                tmp.write(audio)
                wav_file = tmp.name

            if not pygame.mixer.get_init():
                pygame.mixer.init(frequency=self.config.sample_rate)
            pygame.mixer.music.load(wav_file)
            pygame.mixer.music.play()
            while pygame.mixer.music.get_busy():
                time.sleep(0.05)
        except Exception as exc:
            print(f"[TTS error] {exc}; text={clean_text}")
        finally:
            if wav_file:
                try:
                    os.unlink(wav_file)
                except OSError:
                    pass
            with self._lock:
                self._is_speaking = False
                self._cooldown_until = time.time() + self.config.tts_cooldown_s

    def _worker_loop(self) -> None:
        while True:
            text = self._queue.get()
            try:
                self._play_text(text)
            finally:
                with self._lock:
                    self._pending_count = max(0, self._pending_count - 1)
                self._queue.task_done()

    def speak_async(self, text: str) -> None:
        clean_text = text.replace("\n", " ").strip()
        if not clean_text:
            return
        with self._lock:
            self._pending_count += 1
        self._queue.put(clean_text)
