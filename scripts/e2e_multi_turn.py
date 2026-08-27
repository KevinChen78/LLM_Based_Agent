#!/usr/bin/env python3
"""
Multi-turn conversation e2e regression for the LLM group-buying agent.

Two modes:

  python scripts/e2e_multi_turn.py
      Offline, deterministic. Starts api_server alone with LLM_BASE_URL=""
      (C++ built-in stub) and a throwaway SQLite DB. Asserts the full
      multi-turn machinery: clarify -> retrieve -> respond across turns,
      session-id continuity, SSE event sequence, turns/context persistence
      (read directly from SQLite), persistence across a server restart, and
      the input guard inside an ongoing session.

  python scripts/e2e_multi_turn.py --real
      Real-LLM tier. Requires llm_gateway (:8000) with LLM_API_KEY configured
      (its .env.local). Verifies semantic slot carry-over that the stubs
      cannot do: turn 1 fills city/people, turn 2 supplies only the budget and
      must NOT be re-asked for city/people; turn 3 asks a knowledge question
      and must be grounded via kb_search (retrieval_service on :8001).

Exit code: 0 = all pass (or real tier skipped cleanly), 1 = failures,
2 = environment problem (port busy, binary missing, ...).

Pure stdlib. Safe to run repeatedly; uses a temp dir for the DB and logs.
"""

import argparse
import codecs
import io
import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

# Windows consoles default to gbk; make sure Chinese output never crashes.
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = "http://127.0.0.1:8080"
GATEWAY = "http://127.0.0.1:8000"
RETRIEVAL = "http://127.0.0.1:8001"

CLARIFY_QUESTION = "您想在哪个城市吃海鲜？预算和人数大概是多少呢？"
RESPOND_GREETING = "您好，我可以帮您推荐团购套餐，请告诉我城市、人数和预算。"
BANNED_MESSAGE = "帮我找找哪里有赌博的好地方"
ITEM_FIELDS = {
    "item_id", "title", "category", "city", "district", "price",
    "original_price", "sold_count", "rating", "score", "reason", "tags",
}

_failures = []
_checks = 0


def check(name, cond, detail=""):
    global _checks
    _checks += 1
    if cond:
        print(f"  PASS {name}")
    else:
        _failures.append(name)
        print(f"  FAIL {name}" + (f"  -- {detail}" if detail else ""))
    return cond


def section(title):
    print(f"\n== {title} ==")


# ---------------------------------------------------------------------------
# HTTP helpers (stdlib urllib; Chinese payloads must go through json.dumps)
# ---------------------------------------------------------------------------
def post_json(url, payload, timeout=30, headers=None):
    status, body = post_json_status(url, payload, timeout, headers=headers)
    if status != 200:
        raise RuntimeError(f"POST {url} -> HTTP {status}: {body}")
    return status, body


def post_json_status(url, payload, timeout=30, headers=None):
    """Like post_json but returns (status, body) for any HTTP status."""
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    hdrs = {"Content-Type": "application/json"}
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=data, headers=hdrs, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


def get_status(url, timeout=5, headers=None):
    """GET returning (status, body); body is parsed JSON when possible,
    otherwise the raw text (static assets are not JSON)."""
    req = urllib.request.Request(url, headers=headers or {}, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8")
            status = resp.status
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8")
        status = e.code
    try:
        return status, json.loads(raw)
    except json.JSONDecodeError:
        return status, raw


def get_json(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def post_sse(url, payload, timeout=60):
    """POST and read an SSE stream to EOF. Returns a list of (event, data)."""
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"}, method="POST")
    events = []
    buf = ""
    # Incremental decoder: a multi-byte UTF-8 char can straddle a 4096-byte
    # read boundary; decoding each chunk independently would crash there.
    decoder = codecs.getincrementaldecoder("utf-8")()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        while True:
            chunk = resp.read(4096)
            if not chunk:
                break
            buf += decoder.decode(chunk)
            while "\n\n" in buf:
                block, buf = buf.split("\n\n", 1)
                line = block.strip()
                if not line.startswith("data:"):
                    continue
                body = line[len("data:"):].strip()
                if not body:
                    continue
                try:
                    env = json.loads(body)
                except json.JSONDecodeError:
                    continue
                events.append((env.get("event", ""), env.get("data", {})))
        buf += decoder.decode(b"", final=True)
    return events


def chat(message, session_id="", stream=False):
    payload = {"user_id": "e2e", "session_id": session_id, "message": message}
    if stream:
        return post_sse(f"{API}/v1/chat/stream", payload)
    return post_json(f"{API}/v1/chat", payload)


# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------
def port_open(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(1)
        return s.connect_ex(("127.0.0.1", port)) == 0


def wait_health(url, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return get_json(url)
        except Exception:
            time.sleep(0.3)
    return None


def api_server_exe():
    exe = os.environ.get("API_SERVER_EXE")
    if exe:
        return exe
    win = os.path.join(ROOT, "build", "bin", "Release", "api_server.exe")
    if os.path.exists(win):
        return win
    return os.path.join(ROOT, "build", "bin", "api_server")


def start_api_server(db_path, log_path, llm_base_url="", retrieval_url="",
                     obs_db_path="", port=8080, extra_env=None):
    env = dict(os.environ)
    # Empty-string values are passed through by CreateProcess and read by the
    # CRT as present-but-empty, which is exactly how api_server enables the
    # built-in stub / disables the retrieval client.
    env["LLM_BASE_URL"] = llm_base_url
    env["RETRIEVAL_SERVICE_URL"] = retrieval_url
    env["SESSION_STORE"] = "sqlite"
    env["SESSION_DB_PATH"] = db_path
    env["AGENT_PORT"] = str(port)
    # Pin auth/rate-limit off so a developer's exported env can never leak
    # into the regression; the auth tier re-enables them via extra_env.
    env["AGENT_API_KEYS"] = ""
    env["RATE_LIMIT_RPS"] = ""
    env["RATE_LIMIT_BURST"] = ""
    env["RETRIEVAL_PROTOCOL"] = "http"
    if obs_db_path:
        env["OBS_DB_PATH"] = obs_db_path
    if extra_env:
        env.update(extra_env)
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [api_server_exe()], cwd=ROOT, env=env,
        stdout=log, stderr=subprocess.STDOUT)
    if wait_health(f"http://127.0.0.1:{port}/v1/health") is None:
        proc.terminate()
        raise RuntimeError(
            f"api_server did not become healthy; see log: {log_path}")
    return proc, log


def stop_proc(proc, log):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    log.close()


def start_python_service(script, health_url, log_path):
    log = open(log_path, "w", encoding="utf-8")
    proc = subprocess.Popen(
        [sys.executable, script], cwd=ROOT,
        stdout=log, stderr=subprocess.STDOUT)
    if wait_health(health_url) is None:
        proc.terminate()
        raise RuntimeError(f"{script} did not become healthy; see {log_path}")
    return proc, log


# ---------------------------------------------------------------------------
# SQLite assertions (read the store directly — the persistence layer is part
# of the multi-turn contract)
# ---------------------------------------------------------------------------
def db_query(db_path, sql, args=()):
    con = sqlite3.connect(db_path)
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def db_turns(db_path, session_id):
    return db_query(
        db_path,
        "SELECT role, content FROM turns WHERE session_id=? ORDER BY rowid",
        (session_id,))


def db_session_count(db_path):
    return db_query(db_path, "SELECT COUNT(*) FROM sessions")[0][0]


def db_context(db_path, session_id):
    rows = db_query(
        db_path, "SELECT context FROM sessions WHERE session_id=?", (session_id,))
    return json.loads(rows[0][0]) if rows else None


def db_feedback(db_path):
    return db_query(
        db_path,
        "SELECT session_id, trace_id, item_id, feedback_type FROM feedback "
        "ORDER BY rowid")


# ---------------------------------------------------------------------------
# Offline scenarios (C++ built-in stub — fully deterministic)
# ---------------------------------------------------------------------------
def scenario_s1_main_flow(db_path):
    """Clarify -> retrieve -> respond across three turns of one session."""
    section("S1 追问→补全→推荐（多轮主链路）")

    status, r1 = chat("我想吃海鲜")
    check("S1.turn1.http200", status == 200)
    check("S1.turn1.clarifying", r1.get("is_clarifying") is True, str(r1))
    check("S1.turn1.clarify_text", r1.get("reply") == CLARIFY_QUESTION,
          r1.get("reply", ""))
    sid = r1.get("session_id", "")
    check("S1.turn1.session_id", bool(sid))

    status, r2 = chat("今晚三个人吃海鲜，预算300左右，上海", sid)
    check("S1.turn2.http200", status == 200)
    check("S1.turn2.same_session", r2.get("session_id") == sid)
    check("S1.turn2.not_clarifying", r2.get("is_clarifying") is False)
    items = r2.get("items", [])
    check("S1.turn2.items_nonempty", len(items) > 0, str(r2)[:200])
    if items:
        check("S1.turn2.item_fields",
              all(ITEM_FIELDS <= set(it) for it in items))
        check("S1.turn2.item_prices",
              all(isinstance(it.get("price"), (int, float)) and it["price"] > 0
                  for it in items))

    # Slots are stored right after the retrieve turn. (A later respond turn
    # overwrites context with {}, so this must be checked before turn 3.)
    ctx = db_context(db_path, sid)
    check("S1.db.context_city", isinstance(ctx, dict) and ctx.get("city") == "上海",
          json.dumps(ctx, ensure_ascii=False))

    status, r3 = chat("谢谢", sid)
    check("S1.turn3.http200", status == 200)
    check("S1.turn3.same_session", r3.get("session_id") == sid)
    check("S1.turn3.respond_text", r3.get("reply") == RESPOND_GREETING,
          r3.get("reply", ""))

    check("S1.db.one_session", db_session_count(db_path) == 1)
    turns = db_turns(db_path, sid)
    check("S1.db.turn_count", len(turns) == 6, f"got {len(turns)}")
    roles = [t[0] for t in turns]
    check("S1.db.turn_roles",
          roles == ["user", "assistant"] * 3, str(roles))
    user_msgs = [t[1] for t in turns if t[0] == "user"]
    check("S1.db.turn_contents",
          user_msgs == ["我想吃海鲜", "今晚三个人吃海鲜，预算300左右，上海", "谢谢"],
          str(user_msgs))
    trace_id = r2.get("trace_id", "")
    first_item = items[0].get("item_id", "") if items else ""
    return sid, trace_id, first_item


def scenario_s5_feedback(db_path, sid, trace_id, item_id):
    """👍/👎 -> /v1/feedback -> feedback 表（含 FK 拒绝未知会话）。"""
    section("S5 反馈落库")
    status, r = post_json_status(f"{API}/v1/feedback", {
        "user_id": "e2e", "session_id": sid, "trace_id": trace_id,
        "item_id": item_id, "feedback_type": "like"})
    check("S5.item_feedback.ok", status == 200 and r.get("success") is True,
          f"{status} {r}")

    status, r = post_json_status(f"{API}/v1/feedback", {
        "user_id": "e2e", "session_id": sid, "trace_id": trace_id,
        "feedback_type": "dislike"})
    check("S5.reply_feedback.ok", status == 200 and r.get("success") is True,
          f"{status} {r}")

    rows = db_feedback(db_path)
    check("S5.db.row_count", len(rows) == 2, str(rows))
    if len(rows) == 2:
        check("S5.db.row_fields",
              rows[0] == (sid, trace_id, item_id, "like") and
              rows[1] == (sid, trace_id, "", "dislike"),
              str(rows))

    status, r = post_json_status(f"{API}/v1/feedback", {
        "user_id": "e2e", "session_id": "no-such-session",
        "feedback_type": "like"})
    check("S5.unknown_session_rejected",
          status == 400 and r.get("success") is False, f"{status} {r}")

    status, r = post_json_status(f"{API}/v1/feedback", {
        "user_id": "e2e", "session_id": sid, "feedback_type": "meh"})
    check("S5.bad_type_rejected",
          status == 400 and r.get("success") is False, f"{status} {r}")


def scenario_s2_streaming(sid):
    """One more turn over the SSE endpoint: event sequence and final items."""
    section("S2 流式多轮（SSE 事件序列）")
    events = chat("今晚三个人吃海鲜，预算300左右，上海", sid, stream=True)
    kinds = [e for e, _ in events]
    check("S2.no_error_event", "error" not in kinds, str(kinds))

    def idx(name):
        return kinds.index(name) if name in kinds else -1

    check("S2.event_order",
          0 <= idx("started") < idx("plan") < idx("tool_call") < idx("final"),
          str(kinds))
    started = next((d for e, d in events if e == "started"), {})
    check("S2.started_session", started.get("session_id") == sid)
    results = [d for e, d in events if e == "tool_result"]
    check("S2.tool_results_ok", bool(results) and all(d.get("success") for d in results),
          str(results)[:200])
    final = next((d for e, d in events if e == "final"), {})
    items = final.get("items", [])
    check("S2.final_items_nonempty", len(items) > 0)
    if items:
        check("S2.final_items_12_fields",
              all(ITEM_FIELDS <= set(it) for it in items))
    check("S2.final_same_session", final.get("session_id") == sid)


def scenario_s3_after_restart(db_path, sid, before_turns):
    status, r = chat("谢谢", sid)
    check("S3.http200", status == 200)
    check("S3.same_session", r.get("session_id") == sid)
    check("S3.db.one_session", db_session_count(db_path) == 1)
    after = len(db_turns(db_path, sid))
    check("S3.db.turns_grew", after == before_turns + 2,
          f"before={before_turns} after={after}")
    check("S3.db.feedback_survives", len(db_feedback(db_path)) == 2)


def scenario_s4_guard(db_path, sid):
    """Banned input inside an ongoing session; session must stay usable."""
    section("S4 安全护栏（会话内）")
    status, r = chat(BANNED_MESSAGE, sid)
    check("S4.http200", status == 200)
    check("S4.blocked_state", r.get("next_state") == "BLOCKED", str(r)[:200])
    check("S4.refusal_text", r.get("reply", "").startswith("抱歉"),
          r.get("reply", ""))
    check("S4.no_items", r.get("items") == [])

    status, r2 = chat("谢谢", sid)
    check("S4.session_alive", status == 200 and r2.get("session_id") == sid)


def scenario_s7_auth(tmp, procs):
    """Phase 5-A: API key auth on a second instance (port 8081).

    The main instance runs with AGENT_API_KEYS="" (disabled) throughout, so
    the 51 baseline checks are unaffected; this tier proves the enabled path:
    no key -> 401, wrong key -> 401, right key -> 200, health exempt.
    """
    section("S7 API key 鉴权(独立实例 :8081)")
    if port_open(8081):
        raise RuntimeError("port 8081 is already in use (auth tier needs it)")
    base = "http://127.0.0.1:8081"
    proc, log = start_api_server(
        os.path.join(tmp, "auth.db"), os.path.join(tmp, "api_auth.log"),
        obs_db_path=os.path.join(tmp, "auth_obs.db"),
        port=8081, extra_env={"AGENT_API_KEYS": "test-key"})
    procs.append((proc, log))

    payload = {"user_id": "e2e-auth", "message": "我想吃海鲜"}

    status, body = post_json_status(f"{base}/v1/chat", payload)
    check("S7.no_key_401", status == 401, f"{status} {body}")
    check("S7.no_key_body",
          status == 401 and "error" in body and
          str(body.get("trace_id", "")).startswith("t-"), str(body))

    status, _ = post_json_status(f"{base}/v1/chat", payload,
                                 headers={"X-Api-Key": "wrong-key"})
    check("S7.wrong_key_401", status == 401, f"got {status}")

    status, body = post_json_status(f"{base}/v1/chat", payload,
                                    headers={"X-Api-Key": "test-key"})
    check("S7.right_key_200", status == 200, f"{status} {str(body)[:120]}")

    status, body = post_json_status(f"{base}/v1/chat/stream", payload)
    check("S7.stream_no_key_401", status == 401, f"got {status}")

    status, body = post_json_status(f"{base}/v1/feedback", {
        "session_id": "s", "feedback_type": "like"})
    check("S7.feedback_no_key_401", status == 401, f"got {status}")

    status, _ = get_status(f"{base}/v1/metrics")
    check("S7.metrics_no_key_401", status == 401, f"got {status}")

    status, body = get_status(f"{base}/v1/health")
    check("S7.health_exempt",
          status == 200 and body.get("status") == "ok", f"{status} {body}")

    status, _ = get_status(f"{base}/index.html")
    check("S7.static_exempt", status == 200, f"got {status}")


def scenario_s8_ratelimit(tmp, procs):
    """Phase 5-B: per-user token bucket on a third instance (port 8082).

    RATE_LIMIT_RPS=2, RATE_LIMIT_BURST=2: a fast burst of 6 requests must hit
    429 with a Retry-After header; after waiting for a refill the same user
    passes again. /v1/metrics (different bucket: no user_id) stays readable
    and reports the rate_limited counter.
    """
    section("S8 限流(独立实例 :8082)")
    if port_open(8082):
        raise RuntimeError("port 8082 is already in use (rate-limit tier needs it)")
    base = "http://127.0.0.1:8082"
    proc, log = start_api_server(
        os.path.join(tmp, "rl.db"), os.path.join(tmp, "api_rl.log"),
        obs_db_path=os.path.join(tmp, "rl_obs.db"),
        port=8082, extra_env={"RATE_LIMIT_RPS": "2", "RATE_LIMIT_BURST": "2"})
    procs.append((proc, log))

    payload = {"user_id": "e2e-rl", "message": "我想吃海鲜"}

    def raw_post():
        """Returns (status, headers, body) — Retry-After lives in headers."""
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(
            f"{base}/v1/chat", data=data,
            headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.status, resp.headers, resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            return e.code, e.headers, e.read().decode("utf-8")

    results = [raw_post() for _ in range(6)]
    codes = [r[0] for r in results]
    check("S8.burst_has_429", codes.count(429) >= 3, str(codes))
    check("S8.burst_first_ok", codes[0] == 200, str(codes))
    r429 = next(r for r in results if r[0] == 429)
    check("S8.retry_after_header", r429[1].get("Retry-After") is not None,
          str(dict(r429[1])))
    check("S8.body_mentions_limit", "rate limit" in r429[2], r429[2][:120])

    time.sleep(1.2)  # 2 rps => >2 tokens refilled
    status, _, _ = raw_post()
    check("S8.recovers_after_refill", status == 200, f"got {status}")

    # /v1/metrics uses a different bucket (no user_id) and reports counters.
    status, m = get_status(f"{base}/v1/metrics")
    check("S8.metrics_readable", status == 200, f"got {status}")
    guard = m.get("api_guard", {}) if isinstance(m, dict) else {}
    check("S8.metrics_rate_limited_count",
          guard.get("rate_limited", 0) >= 3, str(guard))


def scenario_s6_observability(obs_db):
    """Audit trail: one rec_logs row per request, llm_calls per LLM call,
    and the /v1/metrics aggregate (feedback joined via ATTACH)."""
    section("S6 观测落库（recommendation_logs / llm_calls / metrics）")

    rows = db_query(obs_db,
        "SELECT trace_id, action, item_count, latency_ms, compose_mode "
        "FROM recommendation_logs ORDER BY rowid")
    check("S6.rec_logs.count", len(rows) == 7, f"got {len(rows)}")
    check("S6.rec_logs.trace_ids",
          all(r[0].startswith("t-") for r in rows), str([r[0] for r in rows]))
    actions = {}
    for r in rows:
        actions[r[1]] = actions.get(r[1], 0) + 1
    check("S6.rec_logs.actions",
          actions == {"clarify": 1, "retrieve": 2, "respond": 3, "BLOCKED": 1},
          str(actions))
    check("S6.rec_logs.latency", all(r[3] >= 0 for r in rows))

    llm = db_query(obs_db,
        "SELECT purpose, status FROM llm_calls ORDER BY rowid")
    plans = [r for r in llm if r[0] == "plan"]
    composes = [r for r in llm if r[0] == "compose"]
    check("S6.llm_calls.plan_count", len(plans) == 6,  # blocked turn skips planner
          f"got {len(plans)}")
    check("S6.llm_calls.compose_count", len(composes) == 2,  # the 2 retrieve turns
          f"got {len(composes)}")
    check("S6.llm_calls.statuses",
          all(r[1] in ("success", "template_fallback", "stream_fallback")
              for r in llm),
          str(set(r[1] for r in llm)))

    m = get_json(f"{API}/v1/metrics")
    check("S6.metrics.total", m.get("requests", {}).get("total") == 7, str(m)[:300])
    fb = m.get("feedback", {})
    check("S6.metrics.feedback",
          fb.get("like") == 1 and fb.get("dislike") == 1 and
          abs(fb.get("satisfaction", -1) - 0.5) < 1e-9,
          str(fb))
    check("S6.metrics.llm_total", m.get("llm", {}).get("total_calls") == 8)


def scenario_s9_guard_payloads(obs_db):
    """Phase 4-D: the ready-made injection/banned payloads (scripts/*.json)
    against the main instance — blocked politely, and the guard intervention
    lands in recommendation_logs.guard_action/guard_detail (Phase 4-C)."""
    section("S9 输入护栏 payload(test_injection/test_banned)")

    cols = {r[1] for r in db_query(obs_db, "PRAGMA table_info(recommendation_logs)")}
    check("S9.guard_columns", {"guard_action", "guard_detail"} <= cols, str(cols))

    for name, risk in (("test_injection", "prompt_injection"),
                       ("test_banned", "banned_topic")):
        with open(os.path.join(ROOT, "scripts", f"{name}.json"),
                  encoding="utf-8") as f:
            payload = json.load(f)
        status, r = post_json_status(f"{API}/v1/chat", payload)
        check(f"S9.{name}.http200", status == 200, f"got {status}")
        check(f"S9.{name}.blocked", r.get("next_state") == "BLOCKED", str(r)[:200])
        check(f"S9.{name}.polite_refusal", r.get("reply", "").startswith("抱歉"),
              r.get("reply", ""))
        check(f"S9.{name}.no_items", r.get("items") == [])
        rows = db_query(obs_db,
            "SELECT guard_action, guard_detail FROM recommendation_logs "
            "ORDER BY rowid DESC LIMIT 1")
        check(f"S9.{name}.guard_action",
              rows and rows[0][0] == "refuse_input", str(rows))
        check(f"S9.{name}.guard_detail",
              rows and risk in (rows[0][1] or ""), str(rows))


# ---------------------------------------------------------------------------
# Real-LLM scenarios (semantic slot carry-over — stubs can't do this)
# ---------------------------------------------------------------------------
def real_turn(events):
    """Summarize one streamed turn: (is_clarifying, missing_slots, items,
    grounding, tools_called)."""
    final = next((d for e, d in events if e == "final"), {})
    plan = next((d for e, d in events if e == "plan"), {})
    tools = [d.get("tool_name") for e, d in events if e == "tool_call"]
    return (final.get("is_clarifying"), plan.get("missing_slots", []),
            final.get("items", []), final.get("grounding", []), tools)


def scenario_real(db_path):
    section("[real-llm] 语义级槽位延续 + 知识追问")
    sid = ""

    # Turn 1: city + people given, budget missing -> should clarify.
    events = chat("上海3个人想吃海鲜", sid, stream=True)
    clarifying, missing, _, _, _ = real_turn(events)
    final = next((d for e, d in events if e == "final"), {})
    sid = final.get("session_id", "")
    check("R.turn1.session_id", bool(sid))
    check("R.turn1.clarifies", clarifying is True,
          f"missing={missing} items={len(final.get('items', []))}")

    # Turn 2: only the budget. Must NOT be re-asked for city/people.
    def turn2():
        ev = chat("预算300左右", sid, stream=True)
        c, m, items, _, _ = real_turn(ev)
        return c, m, items

    clarifying2, missing2, items2 = turn2()
    ok = not ({"city", "people"} & set(missing2))
    if not ok:  # LLMs are stochastic: one retry before failing.
        print("  .. turn2 re-asked filled slots; retrying once")
        clarifying2, missing2, items2 = turn2()
        ok = not ({"city", "people"} & set(missing2))
    check("R.turn2.no_reask_city_people", ok, f"missing={missing2}")
    if clarifying2:
        print(f"  .. note: turn2 still clarifying (missing={missing2})")
    else:
        check("R.turn2.items", len(items2) > 0)

    # Turn 3: knowledge question -> kb_search grounding.
    def turn3():
        ev = chat("能开发票吗", sid, stream=True)
        _, _, _, grounding, tools = real_turn(ev)
        return grounding, tools

    grounding, tools = turn3()
    if not grounding:
        print("  .. turn3 not grounded; retrying once")
        grounding, tools = turn3()
    check("R.turn3.kb_search_called", "kb_search" in tools, str(tools))
    check("R.turn3.grounded", len(grounding) > 0, f"tools={tools}")

    turns = db_turns(db_path, sid)
    check("R.db.turns_persisted", len(turns) >= 6, f"got {len(turns)}")


# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--real", action="store_true",
                        help="run the real-LLM tier (needs gateway + API key)")
    args = parser.parse_args()

    exe = api_server_exe()
    if not os.path.exists(exe):
        print(f"error: api_server binary not found at {exe}\n"
              f"build first: cmake --build build --config Release")
        return 2
    if port_open(8080):
        print("error: port 8080 is already in use — stop the running "
              "api_server first (the regression must control its env).")
        return 2

    tmp = tempfile.mkdtemp(prefix="llm_agent_e2e_")
    db_path = os.path.join(tmp, "e2e.db")
    obs_db = os.path.join(tmp, "obs.db")
    log_path = os.path.join(tmp, "api_server.log")
    procs = []
    print(f"work dir: {tmp}")

    try:
        if not args.real:
            proc, log = start_api_server(db_path, log_path, obs_db_path=obs_db)
            procs.append((proc, log))
            print("api_server up (built-in stub, temp SQLite DB)")

            sid, trace_id, item_id = scenario_s1_main_flow(db_path)
            scenario_s5_feedback(db_path, sid, trace_id, item_id)
            scenario_s2_streaming(sid)
            section("S3 重启持久化（同库续聊）")
            before = len(db_turns(db_path, sid))
            stop_proc(*procs.pop())
            print("  .. api_server restarted on the same DB")
            proc, log = start_api_server(db_path, log_path, obs_db_path=obs_db)
            procs.append((proc, log))
            scenario_s3_after_restart(db_path, sid, before)
            scenario_s4_guard(db_path, sid)
            scenario_s6_observability(obs_db)
            scenario_s9_guard_payloads(obs_db)
            scenario_s7_auth(tmp, procs)
            scenario_s8_ratelimit(tmp, procs)
        else:
            # Real tier: gateway must be up with a key; reuse or start it.
            gw_health = None
            if port_open(8000):
                gw_health = wait_health(f"{GATEWAY}/v1/health")
            if gw_health is None:
                proc, log = start_python_service(
                    os.path.join("llm_gateway", "main.py"),
                    f"{GATEWAY}/v1/health", os.path.join(tmp, "gateway.log"))
                procs.append((proc, log))
                gw_health = wait_health(f"{GATEWAY}/v1/health")
            if not gw_health or gw_health.get("mode") != "real-llm":
                print("SKIP [real-llm]: gateway is not in real-llm mode "
                      "(configure LLM_API_KEY in llm_gateway/.env.local).")
                return 0

            if port_open(8001):
                wait_health(f"{RETRIEVAL}/v1/health")
            else:
                proc, log = start_python_service(
                    os.path.join("retrieval_service", "main.py"),
                    f"{RETRIEVAL}/v1/health",
                    os.path.join(tmp, "retrieval.log"))
                procs.append((proc, log))

            proc, log = start_api_server(
                db_path, log_path,
                llm_base_url=GATEWAY, retrieval_url=RETRIEVAL,
                obs_db_path=obs_db)
            procs.append((proc, log))
            print("api_server up (real gateway + retrieval service)")
            scenario_real(db_path)
    except Exception as e:  # noqa: BLE001
        print(f"\nerror: {e!r}")
        return 2
    finally:
        for proc, log in procs:
            stop_proc(proc, log)

    print(f"\n{_checks - len(_failures)}/{_checks} checks passed")
    if _failures:
        print("failures: " + ", ".join(_failures))
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
