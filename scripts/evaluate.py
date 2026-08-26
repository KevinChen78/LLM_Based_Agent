#!/usr/bin/env python3
"""
Offline evaluation report for the LLM group-buying agent.

Reads the audit database (data/observability.db — one recommendation_logs row
per request, one llm_calls row per LLM call) and ATTACHes the sessions DB
(data/sessions.db) for feedback satisfaction, then prints a quality report:

  - request volume & action distribution, FALLBACK rate, empty-retrieve rate
  - clarify follow-through (did the user answer the clarification?)
  - latency avg/p50/p95 (end-to-end and per LLM purpose)
  - LLM token spend by purpose (streaming compose rows honestly report 0)
  - grounding rate
  - feedback satisfaction + most-disliked items

Usage:
    python scripts/evaluate.py [--obs data/observability.db] [--sessions data/sessions.db]

Pure stdlib. Safe to run while the server is up (WAL, read-only ATTACH).
"""

import argparse
import io
import os
import sqlite3
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pct(part, whole):
    return f"{100.0 * part / whole:.1f}%" if whole else "—"


def percentile(sorted_vals, q):
    if not sorted_vals:
        return 0
    idx = min(int(len(sorted_vals) * q), len(sorted_vals) - 1)
    return sorted_vals[idx]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--obs", default=os.path.join(ROOT, "data", "observability.db"))
    ap.add_argument("--sessions", default=os.path.join(ROOT, "data", "sessions.db"))
    args = ap.parse_args()

    if not os.path.exists(args.obs):
        print(f"error: {args.obs} not found — run the agent first "
              f"(it writes audit rows on every /v1/chat request).")
        return 2

    con = sqlite3.connect(args.obs, uri=True)
    con.row_factory = sqlite3.Row
    has_sessions = os.path.exists(args.sessions)
    if has_sessions:
        # Read-only attach: the server may hold the file open. SQLite URIs
        # need forward slashes even on Windows.
        sess_uri = "file:" + os.path.abspath(args.sessions).replace("\\", "/") + "?mode=ro"
        con.execute(f"ATTACH DATABASE '{sess_uri}' AS sessions_db")

    def q(sql, *params):
        return con.execute(sql, params).fetchall()

    print("=" * 60)
    print("团购推荐 Agent 离线评估报告")
    print("=" * 60)

    # ---- 请求量与 action 分布 ----
    rows = q("SELECT COALESCE(action,'?') a, COUNT(*) c FROM recommendation_logs GROUP BY a")
    total = sum(r["c"] for r in rows)
    print(f"\n■ 请求总量: {total}")
    for r in sorted(rows, key=lambda r: -r["c"]):
        print(f"  {r['a']:<10} {r['c']:>5}  ({pct(r['c'], total)})")

    fallback = sum(r["c"] for r in rows if r["a"] in ("FALLBACK", "ERROR"))
    print(f"  FALLBACK/ERROR 率: {pct(fallback, total)}")

    r = q("SELECT COUNT(*) t, SUM(CASE WHEN item_count=0 AND grounding_count=0 "
          "THEN 1 ELSE 0 END) e "
          "FROM recommendation_logs WHERE action='retrieve'")[0]
    print(f"  空推荐率 (retrieve 且无商品无知识): {pct(r['e'] or 0, r['t'])}")

    grounded = q("SELECT COUNT(*) c FROM recommendation_logs WHERE grounding_count>0")[0]["c"]
    print(f"  知识 grounding 触发: {grounded} 次 ({pct(grounded, total)})")

    # ---- clarify 后续答率 ----
    if has_sessions:
        clarifies = q("SELECT session_id, created_at FROM recommendation_logs "
                      "WHERE action='clarify'")
        answered = 0
        for c in clarifies:
            nxt = q("SELECT COUNT(*) c FROM sessions_db.turns "
                    "WHERE session_id=? AND role='user' AND created_at>=?",
                    c["session_id"], c["created_at"])[0]["c"]
            # The clarify-causing user turn itself is recorded before the log,
            # so >=1 more user turn means the user answered.
            if nxt >= 2:
                answered += 1
        print(f"\n■ 追问有效性: {answered}/{len(clarifies)} 次追问后用户继续作答 "
              f"({pct(answered, len(clarifies))})")

    # ---- 延迟 ----
    lats = [r["l"] for r in q("SELECT latency_ms l FROM recommendation_logs ORDER BY l")]
    if lats:
        print(f"\n■ 端到端延迟: avg {sum(lats)/len(lats):.0f} ms | "
              f"p50 {percentile(lats, 0.5)} ms | p95 {percentile(lats, 0.95)} ms")

    print("\n■ LLM 调用（按用途）")
    for r in q("SELECT purpose p, COUNT(*) c, AVG(latency_ms) al, "
               "SUM(prompt_tokens) pt, SUM(completion_tokens) ct "
               "FROM llm_calls GROUP BY purpose"):
        print(f"  {r['p']:<8} 调用 {r['c']:>4} | avg 延迟 {r['al'] or 0:.0f} ms | "
              f"tokens {r['pt'] or 0}+{r['ct'] or 0}")
    print("  (流式 compose 行 tokens 记 0：上游未返回 usage，属口径而非丢失)")

    statuses = q("SELECT COALESCE(status,'?') s, COUNT(*) c FROM llm_calls GROUP BY s")
    if statuses:
        print("  状态分布: " + ", ".join(f"{r['s']}={r['c']}" for r in statuses))

    # ---- 反馈满意率 ----
    if has_sessions:
        fb = q("SELECT feedback_type t, COUNT(*) c FROM sessions_db.feedback GROUP BY t")
        likes = sum(r["c"] for r in fb if r["t"] == "like")
        dislikes = sum(r["c"] for r in fb if r["t"] == "dislike")
        print(f"\n■ 反馈: 👍 {likes} / 👎 {dislikes} "
              f"(满意率 {pct(likes, likes + dislikes)})")
        disliked = q("SELECT item_id, COUNT(*) c FROM sessions_db.feedback "
                     "WHERE feedback_type='dislike' AND item_id != '' "
                     "GROUP BY item_id ORDER BY c DESC LIMIT 5")
        if disliked:
            print("  被 👎 最多的商品:")
            for r in disliked:
                print(f"    {r['item_id']}: {r['c']} 次")
    else:
        print(f"\n(未找到 sessions 库 {args.sessions}，跳过追问有效性/反馈段)")

    con.close()
    print("\n" + "=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
