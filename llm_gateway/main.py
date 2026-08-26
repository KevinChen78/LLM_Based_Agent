#!/usr/bin/env python3
"""
Phase 0 LLM Gateway

An OpenAI-compatible chat completions server that acts as the data plane
for the C++ control plane (api_server). Two modes:

  1. Real LLM (when LLM_API_KEY is set):
     Forwards the incoming request to any OpenAI-compatible endpoint
     (LLM_API_BASE, LLM_MODEL) and relays the model's JSON plan verbatim.
     This is "接入真实 LLM".

  2. Deterministic stub (default, no key):
     Returns deterministic JSON plans based on simple keyword matching.
     Keeps Phase 0 runnable with zero external dependencies.

Config (env, or llm_gateway/.env.local — see .env.example):
    LLM_GATEWAY_PORT  default 8000
    LLM_API_KEY       if non-empty, enable real-LLM passthrough
    LLM_API_BASE      e.g. https://api.deepseek.com  (default OpenAI)
    LLM_MODEL         e.g. deepseek-v4-flash
    LLM_TIMEOUT_SECS  default 20

Precedence: real environment variables > .env.local > .env.

Run:
    python llm_gateway/main.py

Test:
    curl -X POST http://localhost:8000/v1/chat/completions \\
      -H "Content-Type: application/json" \\
      -d '{"model":"deepseek-v4-flash","messages":[{"role":"user","content":"..."}]}'
"""

import json
import os
import urllib.request
import urllib.error
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

# ---------------------------------------------------------------------------
# Local env file loader (stdlib only; never pip-install dotenv for one file)
# ---------------------------------------------------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))


def _load_env_file(path):
    """Populate os.environ with KEY=VALUE lines, but never override values
    that are already set in the real environment."""
    try:
        with open(path, encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                key = key.strip()
                val = val.strip().strip('"').strip("'")
                os.environ.setdefault(key, val)
    except FileNotFoundError:
        pass


# .env first, then .env.local overrides it; real env vars always win.
_load_env_file(os.path.join(_HERE, ".env"))
_load_env_file(os.path.join(_HERE, ".env.local"))


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PORT = int(os.environ.get("LLM_GATEWAY_PORT", "8000"))

LLM_API_KEY = os.environ.get("LLM_API_KEY", "").strip()
LLM_API_BASE = os.environ.get("LLM_API_BASE", "https://api.openai.com/v1").rstrip("/")
LLM_MODEL = os.environ.get("LLM_MODEL", "gpt-4o-mini")
LLM_TIMEOUT = float(os.environ.get("LLM_TIMEOUT_SECS", "20"))

REAL_LLM_ENABLED = bool(LLM_API_KEY)


# ---------------------------------------------------------------------------
# Deterministic fallback planner (mirrors C++ stub semantics)
# ---------------------------------------------------------------------------
def extract_user_input(messages):
    """The C++ TaskPlanner sends the full planning prompt as the user turn.
    The real user text lives under the '# 用户当前输入' marker."""
    last_user = ""
    for m in reversed(messages):
        if m.get("role") == "user":
            last_user = m.get("content", "")
            break
    marker = "# 用户当前输入"
    pos = last_user.find(marker)
    if pos == -1:
        return last_user
    pos += len(marker)
    while pos < len(last_user) and last_user[pos] in "\r\n":
        pos += 1
    end = last_user.find("#", pos)
    user_input = last_user[pos:] if end == -1 else last_user[pos:end]
    return user_input.strip()


def make_plan(user_message):
    if "海鲜" in user_message and "上海" in user_message:
        return {
            "action": "retrieve",
            "slots": {
                "city": "上海", "category": "海鲜", "budget": 300,
                "people": 3, "time": "今晚", "preference": "", "taboo": "",
            },
            "missing_slots": [],
            "clarification_question": "",
            # Tool names must match what api_server registers (deal_retriever /
            # deal_ranker) — a stale name here makes retrieval silently empty.
            "tool_calls": [
                {
                    "tool_name": "deal_retriever",
                    "arguments": {
                        "city": "上海", "category": "海鲜", "max_price": 300,
                        "people": 3, "keywords": "", "top_k": 20,
                    },
                },
                {
                    "tool_name": "deal_ranker",
                    "arguments": {
                        "candidates": [], "budget": 300, "people": 3,
                        "taboo": "", "top_n": 3,
                    },
                },
            ],
        }

    if "吃" in user_message or "想" in user_message:
        return {
            "action": "clarify",
            "slots": {
                "city": "", "category": "海鲜", "budget": 0,
                "people": 0, "time": "", "preference": "", "taboo": "",
            },
            "missing_slots": ["city", "budget", "people"],
            "clarification_question": "您想在哪个城市吃海鲜？预算和人数大概是多少呢？",
            "tool_calls": [],
        }

    return {
        "action": "respond",
        "slots": {},
        "missing_slots": [],
        "clarification_question": "",
        "tool_calls": [],
        "response": "您好，我可以帮您推荐团购套餐，请告诉我城市、人数和预算。",
    }


# ---------------------------------------------------------------------------
# Real LLM passthrough (OpenAI-compatible)
# ---------------------------------------------------------------------------
def call_real_llm(body):
    """Forward to LLM_API_BASE and return the upstream chat completion dict.
    The gateway is authoritative over the backend model: callers may send a
    placeholder (e.g. the C++ default 'gpt-4o-mini') and we always substitute
    the configured LLM_MODEL so the upstream provider never sees an unknown name."""
    forwarded = {
        "model": LLM_MODEL,
        "messages": body.get("messages", []),
        "temperature": body.get("temperature", 0.3),
    }
    if "max_tokens" in body:
        forwarded["max_tokens"] = body["max_tokens"]

    payload = json.dumps(forwarded, ensure_ascii=False).encode("utf-8")
    url = LLM_API_BASE + "/chat/completions"
    req = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {LLM_API_KEY}",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=LLM_TIMEOUT) as resp:
        return json.loads(resp.read().decode("utf-8"))


def stream_real_llm(body, write):
    """Forward a stream=true request to the upstream and pipe its SSE bytes
    straight to `write` (a callable accepting bytes) as they arrive. The
    upstream's `data: {...}\\n\\n` framing is preserved verbatim."""
    forwarded = {
        "model": LLM_MODEL,
        "messages": body.get("messages", []),
        "temperature": body.get("temperature", 0.3),
        "stream": True,
    }
    if "max_tokens" in body:
        forwarded["max_tokens"] = body["max_tokens"]

    payload = json.dumps(forwarded, ensure_ascii=False).encode("utf-8")
    url = LLM_API_BASE + "/chat/completions"
    req = urllib.request.Request(
        url,
        data=payload,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {LLM_API_KEY}",
            "Accept": "text/event-stream",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=LLM_TIMEOUT) as resp:
        while True:
            chunk = resp.read(4096)
            if not chunk:
                break
            write(chunk)


def stub_stream(body, write_sse):
    """Deterministic streaming fallback (no API key): emit the canned plan as a
    single delta so the wire format is still exercised end to end."""
    user_message = extract_user_input(body.get("messages", []))
    content = json.dumps(make_plan(user_message), ensure_ascii=False)
    write_sse({"choices": [{"index": 0, "delta": {"content": content}, "finish_reason": None}]})
    write_sse(None)  # [DONE] sentinel


def generate_response(body):
    messages = body.get("messages", [])

    if REAL_LLM_ENABLED:
        try:
            upstream = call_real_llm(body)
            # Stamp model + usage consistency, then relay as-is.
            upstream["model"] = upstream.get("model", LLM_MODEL)
            return upstream
        except urllib.error.HTTPError as e:
            print(f"[LLM Gateway] upstream HTTP error: {e.code} {e.reason}")
        except Exception as e:  # noqa: BLE001
            print(f"[LLM Gateway] upstream call failed: {e!r}; using deterministic fallback")

    # Deterministic fallback
    user_message = extract_user_input(messages)
    plan = make_plan(user_message)
    content = json.dumps(plan, ensure_ascii=False)
    return {
        "id": "chatcmpl-phase0",
        "object": "chat.completion",
        "created": 0,
        "model": body.get("model", LLM_MODEL),
        "choices": [{
            "index": 0,
            "message": {"role": "assistant", "content": content},
            "finish_reason": "stop",
        }],
        "usage": {
            "prompt_tokens": len(user_message),
            "completion_tokens": len(content),
            "total_tokens": len(user_message) + len(content),
        },
    }


# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def _send_json(self, status, obj):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # --- SSE streaming helpers ---
    def _begin_sse(self):
        # HTTP/1.0 + Connection: close: the body is delimited by connection
        # close (no Content-Length / chunked needed), which httplib on the
        # client side reads incrementally via its content receiver.
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

    def _write_sse_obj(self, obj):
        # obj=None writes the terminating [DONE] sentinel.
        if obj is None:
            payload = b"data: [DONE]\n\n"
        else:
            payload = ("data: " + json.dumps(obj, ensure_ascii=False) + "\n\n").encode("utf-8")
        try:
            self.wfile.write(payload)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path == "/v1/health":
            self._send_json(200, {
                "status": "ok",
                "mode": "real-llm" if REAL_LLM_ENABLED else "deterministic",
                "model": LLM_MODEL,
            })
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        try:
            data = json.loads(raw)
        except json.JSONDecodeError:
            self.send_error(400)
            return

        if data.get("stream"):
            self._begin_sse()
            try:
                if REAL_LLM_ENABLED:
                    # Pipe upstream SSE bytes verbatim, then a terminating [DONE].
                    stream_real_llm(data, lambda b: self._write_raw(b))
                    self._write_sse_obj(None)
                else:
                    stub_stream(data, self._write_sse_obj)
            except Exception as e:  # noqa: BLE001
                print(f"[LLM Gateway] streaming failed: {e!r}")
                self._write_sse_obj({"error": str(e)})
                self._write_sse_obj(None)
            return

        self._send_json(200, generate_response(data))

    def _write_raw(self, data_bytes):
        try:
            self.wfile.write(data_bytes)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, fmt, *args):
        print(f"[LLM Gateway] {self.address_string()} - {fmt % args}")


def main():
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    mode = "REAL LLM passthrough" if REAL_LLM_ENABLED else "deterministic stub"
    print(f"[LLM Gateway] Running on http://0.0.0.0:{PORT}  (mode: {mode})")
    print(f"[LLM Gateway] Model: {LLM_MODEL}  Base: {LLM_API_BASE}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[LLM Gateway] Shutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
