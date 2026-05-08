import json
from typing import Any, Dict, Optional, Tuple


ALLOWED_ACTIONS = {
    "chassis": {"forward", "backward", "turn_left", "turn_right", "stop"},
    "mode": {"hold", "resume_follow"},
    "gimbal": {"nod", "shake", "center", "lock", "unlock"},
    "none": {"none"},
}

ALLOWED_EMOTIONS = {"neutral", "happy", "alert", "confused", "sad"}
STOP_WORDS = ("停下", "停止", "别动", "不要动", "刹车", "危险", "急停", "站住")


def clamp_int(value: Any, min_value: int, max_value: int, default: int = 0) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        parsed = default
    return max(min_value, min(max_value, parsed))


def is_stop_request(text: str) -> bool:
    return any(word in text for word in STOP_WORDS)


def make_stop_payload(reply: str = "好的，已停止。") -> Dict[str, Any]:
    return {
        "emotion": "alert",
        "action": {
            "target": "chassis",
            "name": "stop",
            "speed": 0,
            "duration_ms": 0,
            "pan_delta_deg": 0,
            "tilt_delta_deg": 0,
        },
        "voice": {"text": reply},
    }


def make_none_payload(reply: str = "好的。") -> Dict[str, Any]:
    return {
        "emotion": "neutral",
        "action": {
            "target": "none",
            "name": "none",
            "speed": 0,
            "duration_ms": 0,
            "pan_delta_deg": 0,
            "tilt_delta_deg": 0,
        },
        "voice": {"text": reply},
    }


def extract_json_object(text: str) -> Optional[str]:
    if not text:
        return None
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end < start:
        return None
    return text[start : end + 1]


def load_control_json(text: str) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    json_text = extract_json_object(text)
    if json_text is None:
        return None, "no_json_object"
    try:
        value = json.loads(json_text)
    except json.JSONDecodeError as exc:
        return None, f"json_decode_error:{exc.msg}"
    if not isinstance(value, dict):
        return None, "json_root_not_object"
    return value, None


def normalize_control_payload(raw: Dict[str, Any]) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    emotion = str(raw.get("emotion", "neutral")).strip().lower() or "neutral"
    if emotion not in ALLOWED_EMOTIONS:
        emotion = "neutral"

    action = raw.get("action")
    if not isinstance(action, dict):
        return None, "action_not_object"

    target = str(action.get("target", "none")).strip().lower()
    name = str(action.get("name", "none")).strip().lower()
    if target not in ALLOWED_ACTIONS:
        return None, f"unknown_target:{target}"
    if name not in ALLOWED_ACTIONS[target]:
        return None, f"unknown_action:{target}.{name}"

    normalized_action = {
        "target": target,
        "name": name,
        "speed": clamp_int(action.get("speed", 0), 0, 50),
        "duration_ms": clamp_int(action.get("duration_ms", 0), 0, 3000),
        "pan_delta_deg": clamp_int(action.get("pan_delta_deg", 0), -45, 45),
        "tilt_delta_deg": clamp_int(action.get("tilt_delta_deg", 0), -45, 45),
    }

    voice = raw.get("voice")
    if not isinstance(voice, dict):
        voice = {}
    voice_text = str(voice.get("text", "")).replace("\n", " ").strip()

    return {
        "emotion": emotion,
        "action": normalized_action,
        "voice": {"text": voice_text},
    }, None


def should_dispatch(payload: Dict[str, Any]) -> bool:
    action = payload.get("action", {})
    return not (action.get("target") == "none" and action.get("name") == "none")
