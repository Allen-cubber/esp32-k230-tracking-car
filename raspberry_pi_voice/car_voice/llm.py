from typing import Any, Dict, Optional, Tuple

import dashscope

from .config import AppConfig
from .schema import load_control_json, normalize_control_payload


SYSTEM_PROMPT = """
你是智能小车的语音指令解析器。
只输出 JSON，不要 Markdown，不要解释，不要额外文本。
JSON schema:
{
  "emotion": "neutral|happy|alert|confused|sad",
  "action": {
    "target": "chassis|gimbal|mode|none",
    "name": "forward|backward|turn_left|turn_right|stop|hold|resume_follow|nod|shake|center|lock|unlock|none",
    "speed": 0-50,
    "duration_ms": 0-3000,
    "pan_delta_deg": -45..45,
    "tilt_delta_deg": -45..45
  },
  "voice": {"text": "简短中文回复"}
}
普通聊天使用 {"target":"none","name":"none"}。
停止、别动、危险、急停类请求必须使用 {"target":"chassis","name":"stop"}。
前进、后退、左转、右转默认 speed=35，duration_ms=1000。
继续跟随、继续追踪使用 {"target":"mode","name":"resume_follow"}。
保持不动使用 {"target":"mode","name":"hold"}。
点头使用 {"target":"gimbal","name":"nod"}，摇头使用 {"target":"gimbal","name":"shake"}。
""".strip()

CHAT_SYSTEM_PROMPT = """
你是智能小车的语音聊天助手。
用户可能会问日常问题，也可能只是闲聊。
请用简短中文回答，通常不超过两句话。
不要输出 JSON，不要 Markdown，不要解释内部规则。
如果用户问实时新闻、实时天气、联网查询类问题，而你没有可靠实时数据，请直接说明现在无法查询实时信息。
如果用户要求小车移动、停车、跟随或云台动作，只做简短提示，不要生成动作指令。
""".strip()


class ActionPlanner:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        dashscope.api_key = config.dashscope_api_key

    @staticmethod
    def _extract_dashscope_content(response: Any) -> str:
        output = getattr(response, "output", None)
        if output is not None:
            choices = getattr(output, "choices", None)
            if choices:
                message = getattr(choices[0], "message", None)
                content = getattr(message, "content", None)
                if content:
                    return str(content).split("</think>")[-1].strip()

        if isinstance(response, dict):
            try:
                return response["output"]["choices"][0]["message"]["content"].split("</think>")[-1].strip()
            except (KeyError, IndexError, TypeError):
                pass
        return ""

    def ask(self, user_text: str) -> Tuple[Optional[Dict[str, Any]], Optional[str], str]:
        if not self.config.has_valid_dashscope_key():
            return None, "dashscope_api_key_missing", ""

        try:
            response = dashscope.Generation.call(
                model=self.config.llm_model,
                messages=[
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": user_text},
                ],
                result_format="message",
            )
        except Exception as exc:
            return None, f"llm_exception:{exc}", ""

        status_code = getattr(response, "status_code", None)
        if status_code is not None and status_code != 200:
            message = getattr(response, "message", "")
            return None, f"llm_status:{status_code}:{message}", ""

        raw_text = self._extract_dashscope_content(response)
        if not raw_text:
            return None, "llm_empty_response", ""

        raw_json, error = load_control_json(raw_text)
        if error:
            return None, error, raw_text

        payload, error = normalize_control_payload(raw_json)
        if error:
            return None, error, raw_text

        return payload, None, raw_text


class ChatResponder:
    def __init__(self, config: AppConfig) -> None:
        self.config = config
        dashscope.api_key = config.dashscope_api_key

    def ask(self, user_text: str) -> Tuple[str, Optional[str], str]:
        if not self.config.has_valid_dashscope_key():
            return "", "dashscope_api_key_missing", ""

        try:
            response = dashscope.Generation.call(
                model=self.config.llm_model,
                messages=[
                    {"role": "system", "content": CHAT_SYSTEM_PROMPT},
                    {"role": "user", "content": user_text},
                ],
                result_format="message",
                max_tokens=120,
                temperature=0.7,
            )
        except Exception as exc:
            return "", f"chat_llm_exception:{exc}", ""

        status_code = getattr(response, "status_code", None)
        if status_code is not None and status_code != 200:
            message = getattr(response, "message", "")
            return "", f"chat_llm_status:{status_code}:{message}", ""

        raw_text = ActionPlanner._extract_dashscope_content(response)
        reply = raw_text.replace("\n", " ").strip()
        if not reply:
            return "", "chat_llm_empty_response", raw_text

        return reply[:160], None, raw_text
