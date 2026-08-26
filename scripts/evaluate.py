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

    # ---- 流量口径拆分(Phase 2.3-B1)----
    # 三档口径:sim 模拟数据(user_id 以 sim- 开头,simulate_feedback.py 写入)
    # / 测试流量(e2e 的 user_id='e2e',或 plan 调用模型为占位 stub 名
    # 'gpt-4o-mini'——网关 stub 与 C++ 内置 stub 都落这个名)/ 真实流量(其余)。
    stub_traces = {r["t"] for r in q(
        "SELECT DISTINCT trace_id t FROM llm_calls WHERE model='gpt-4o-mini'")}

    def cohort_of(row):
        uid = row["user_id"] or ""
        if uid.startswith("sim-"):
            return "sim"
        if uid == "e2e" or row["trace_id"] in stub_traces:
            return "test"
        return "real"

    COHORT_LABEL = {"real": "真实", "test": "测试/stub", "sim": "模拟(sim-)"}
    rec_rows = q("SELECT trace_id, user_id, action, item_count, grounding_count,"
                 " request_text, slots_json FROM recommendation_logs")
    cohorts = {"real": [0, 0], "test": [0, 0], "sim": [0, 0]}   # [retrieve_total, empty]
    gaps = []
    for row in rec_rows:
        if row["action"] != "retrieve":
            continue
        c = cohort_of(row)
        cohorts[c][0] += 1
        if (row["item_count"] or 0) == 0 and (row["grounding_count"] or 0) == 0:
            cohorts[c][1] += 1
            if c == "real":
                gaps.append(row)
    print("\n■ 空推荐率(按流量口径拆分)")
    for c in ("real", "test", "sim"):
        t, e = cohorts[c]
        if t:
            print(f"  {COHORT_LABEL[c]:<10} {pct(e, t)}  ({e}/{t})")
    if not any(t for t, _ in cohorts.values()):
        print("  (无 retrieve 请求)")
    if gaps and cohorts["real"][0] and cohorts["real"][1] / cohorts["real"][0] > 0.2:
        # 真实流量空推荐率 >20%:暴露召回缺口清单(本阶段只暴露,不修召回)
        import json as _json2
        print("  ⚠ 真实流量空推荐率 >20%,召回缺口清单:")
        for row in gaps[:10]:
            try:
                slots = _json2.loads(row["slots_json"] or "{}")
            except ValueError:
                slots = {}
            print(f"    [{slots.get('city','?')}/{slots.get('category','?')}] "
                  f"{(row['request_text'] or '')[:40]}")

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

    # ---- 排序 / A/B 实验对比（Phase 2.1；旧库无这些列则整段跳过）----
    cols = {r["name"] for r in q("PRAGMA table_info(recommendation_logs)")}
    if {"candidates_json", "experiment_group", "rank_mode"} <= cols:
        print("\n■ 排序 / 实验对比（Phase 2.1）")
        modes = q("SELECT COALESCE(NULLIF(rank_mode,''),'(none)') m, COUNT(*) c "
                  "FROM recommendation_logs GROUP BY m ORDER BY c DESC")
        print("  rank_mode 分布: " + (", ".join(f"{r['m']}={r['c']}" for r in modes)
                                     if modes else "(无数据)"))
        n_fallback = sum(r["c"] for r in modes if r["m"] == "rule_fallback")
        n_ranked = sum(r["c"] for r in modes if r["m"] != "(none)")
        if n_ranked:
            print(f"  模型回退率 (rule_fallback): {pct(n_fallback, n_ranked)}"
                  + ("  ⚠ 偏高,检查 ranking_service" if n_fallback > 0 else ""))

        # Per-group satisfaction: feedback joined back by trace_id.
        if has_sessions:
            grp = q("SELECT rl.experiment_group g, f.feedback_type t, COUNT(*) c "
                    "FROM sessions_db.feedback f "
                    "JOIN recommendation_logs rl ON rl.trace_id = f.trace_id "
                    "WHERE rl.experiment_group IS NOT NULL AND rl.experiment_group != '' "
                    "GROUP BY g, t")
            by_group = {}
            for r in grp:
                by_group.setdefault(r["g"], {"like": 0, "dislike": 0})
                by_group[r["g"]][r["t"]] = r["c"]
            if by_group:
                print("  分组满意率 (feedback 按 trace_id 回连):")
                for g in sorted(by_group):
                    lk, dl = by_group[g]["like"], by_group[g]["dislike"]
                    note = "" if lk + dl >= 30 else "  (样本<30,仅供参考)"
                    print(f"    {g:<10} 👍{lk:>4} 👎{dl:>4} 满意率 "
                          f"{pct(lk, lk + dl)}{note}")
            else:
                print("  分组满意率: 暂无带实验分组的反馈")

            # Position sensitivity: like rate by position in ranked_items.
            pos_rows = q(
                "SELECT je.key AS pos, f.feedback_type AS t, COUNT(*) AS c "
                "FROM sessions_db.feedback f "
                "JOIN recommendation_logs rl ON rl.trace_id = f.trace_id, "
                "  json_each(rl.ranked_items) je "
                "WHERE f.item_id != '' AND json_extract(je.value, '$.item_id') = f.item_id "
                "GROUP BY pos, t ORDER BY CAST(pos AS INTEGER)")
            by_pos = {}
            for r in pos_rows:
                by_pos.setdefault(r["pos"], {"like": 0, "dislike": 0})
                by_pos[r["pos"]][r["t"]] = r["c"]
            if by_pos:
                print("  位置敏感度 (该位置的反馈中 like 占比):")
                for pos in sorted(by_pos, key=lambda p: int(p)):
                    lk, dl = by_pos[pos]["like"], by_pos[pos]["dislike"]
                    print(f"    第 {int(pos) + 1} 位: 👍{lk} 👎{dl} "
                          f"({pct(lk, lk + dl)})")

        # Candidate-set health: average candidates per ranked request.
        cand = q("SELECT candidates_json FROM recommendation_logs "
                 "WHERE candidates_json IS NOT NULL AND candidates_json != ''")
        if cand:
            import json as _json
            counts = []
            with_model = 0
            for r in cand:
                try:
                    arr = _json.loads(r["candidates_json"])
                    counts.append(len(arr))
                    if any(c.get("model_score") is not None for c in arr):
                        with_model += 1
                except (ValueError, AttributeError):
                    continue
            if counts:
                print(f"  候选集: avg {sum(counts)/len(counts):.1f} 条/请求, "
                      f"含模型分 {pct(with_model, len(counts))}")

    con.close()
    print("\n" + "=" * 60)
    return 0


if __name__ == "__main__":
    sys.exit(main())
