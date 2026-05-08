#!/usr/bin/env python3
import argparse
import asyncio
import signal
import sys

from car_voice.assistant import VoiceCarAssistant
from car_voice.config import AppConfig


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Raspberry Pi voice gateway for the ESP32/K230 tracking car")
    parser.add_argument(
        "--text",
        action="store_true",
        help="Read recognized text from stdin instead of the microphone. This mode starts active.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    config = AppConfig.from_env()
    if args.text:
        config.always_active = True

    app = VoiceCarAssistant(config)

    def _handle_signal(signum, frame):
        app.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, _handle_signal)
    signal.signal(signal.SIGTERM, _handle_signal)

    app.start()
    try:
        if args.text:
            app.run_text_loop()
        else:
            asyncio.run(app.run_voice_loop())
    finally:
        app.stop()


if __name__ == "__main__":
    main()
