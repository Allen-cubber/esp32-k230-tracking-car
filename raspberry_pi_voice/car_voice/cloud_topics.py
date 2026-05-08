from typing import Dict


def build_topics(topic_root: str, device_id: str) -> Dict[str, str]:
    base = f"{topic_root.rstrip('/')}/{device_id}"
    return {
        "cmd_request": f"{base}/cmd/request",
        "cmd_ack": f"{base}/cmd/ack",
        "status": f"{base}/status",
    }
