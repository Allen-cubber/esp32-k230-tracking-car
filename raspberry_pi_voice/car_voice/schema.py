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

QUICK_DEFAULT_SPEED = 35
QUICK_DEFAULT_DURATION_MS = 1000

QUICK_ACTION_RULES = (
    ("mode", "resume_follow", ("开始跟随", "继续跟随", "恢复跟随", "跟着我", "跟我走", "跟随我", "追踪我", "继续追踪"), "好的，开始跟随。"),
    ("mode", "hold", ("暂停跟随", "停止跟随", "别跟了", "不要跟了", "保持不动", "原地待命", "待命"), "好的，保持不动。"),
    ("gimbal", "center", ("云台回中", "回中", "回正", "看中间", "看前面"), "好的，云台回中。"),
    ("gimbal", "lock", ("锁定云台", "云台锁定", "镜头锁定", "锁住镜头"), "好的，已锁定云台。"),
    ("gimbal", "unlock", ("解锁云台", "云台解锁", "恢复云台", "恢复镜头"), "好的，已解锁云台。"),
    ("gimbal", "nod", ("点头", "点点头"), "好的。"),
    ("gimbal", "shake", ("摇头", "摇摇头"), "好的。"),
    ("chassis", "turn_left", ("左转", "向左转", "往左转"), "好的，左转。"),
    ("chassis", "turn_right", ("右转", "向右转", "往右转"), "好的，右转。"),
    ("chassis", "backward", ("后退", "倒车", "往后退", "向后退", "退后"), "好的，后退。"),
    ("chassis", "forward", ("前进", "向前", "往前", "往前走", "向前走", "前走"), "好的，前进。"),
)


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


def make_action_payload(
    target: str,
    name: str,
    reply: str,
    emotion: str = "neutral",
    speed: int = 0,
    duration_ms: int = 0,
    pan_delta_deg: int = 0,
    tilt_delta_deg: int = 0,
) -> Dict[str, Any]:
    return {
        "emotion": emotion,
        "action": {
            "target": target,
            "name": name,
            "speed": clamp_int(speed, 0, 50),
            "duration_ms": clamp_int(duration_ms, 0, 3000),
            "pan_delta_deg": clamp_int(pan_delta_deg, -45, 45),
            "tilt_delta_deg": clamp_int(tilt_delta_deg, -45, 45),
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


def _compact_text(text: str) -> str:
    drop_chars = " \t\r\n，。！？!?、,.；;：:\"'“”‘’（）()【】[]"
    return "".join(ch for ch in text.strip() if ch not in drop_chars)


def match_quick_action(text: str) -> Optional[Dict[str, Any]]:
    compact = _compact_text(text)
    if not compact:
        return None

    for target, name, phrases, reply in QUICK_ACTION_RULES:
        if not any(phrase in compact for phrase in phrases):
            continue

        speed = 0
        duration_ms = 0
        emotion = "neutral"
        if target == "chassis":
            speed = QUICK_DEFAULT_SPEED
            duration_ms = QUICK_DEFAULT_DURATION_MS
            emotion = "happy"
        elif target == "mode" and name == "resume_follow":
            emotion = "happy"

        return make_action_payload(
            target=target,
            name=name,
            reply=reply,
            emotion=emotion,
            speed=speed,
            duration_ms=duration_ms,
        )

    return None


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
