#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deterministic simulated feedback for exercising the learning-to-rank loop
(Phase 2.3-C2). Verifies that the training pipeline (build_features ->
train_ranker) works end to end BEFORE enough real feedback has accumulated.

Hard isolation rules (per plan):
  * every simulated row uses a user_id with the `sim-` prefix;
  * build_features.py / train_ranker.py / evaluate.py EXCLUDE sim- rows by
    default (opt-in via --include-sim);
  * simulated data proves the pipeline is wired, never model quality.

Writes (directly, no HTTP — offline by design):
  * sessions.db:      sessions + feedback rows (FK-satisfied)
  * observability.db: recommendation_logs rows with candidates_json
                      (item_id + rule_score), trace-linked to the feedback

Pure stdlib, deterministic (--seed), idempotent per --tag (re-running with
the same tag replaces that tag's rows).

Usage:
    python scripts/simulate_feedback.py --requests 150
    python scripts/simulate_feedback.py --requests 150 --include-model-scores
"""

import argparse
import json
import os
import random
import sqlite3
import sys
from datetime import datetime, timedelta

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_PROJECT, "ranking_service"))

from features import rule_score  # noqa: E402  (shared rule formula)

CATEGORIES = ["火锅", "烧烤", "海鲜", "日料", "奶茶", "川菜"]


def load_deals(deals_path):
    with open(deals_path, encoding="utf-8") as fh:
        root = json.load(fh)
    deals = root.get("deals", root if isinstance(root, list) else [])
    return [d for d in deals if d.get("item_id")]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--requests", type=int, default=150,
                    help="simulated retrieve requests (default 150)")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--tag", default="",
                    help="cohort tag; rows are sim-<tag>-u<N> (default tag '' "
                         "=> prefix 'sim-'). Reruns replace the same cohort.")
    ap.add_argument("--deals", default=os.path.join(_PROJECT, "data", "deals.json"))
    ap.add_argument("--obs", default=os.path.join(_PROJECT, "data", "observability.db"))
    ap.add_argument("--sessions", default=os.path.join(_PROJECT, "data", "sessions.db"))
    args = ap.parse_args()

    rng = random.Random(args.seed)
    deals = load_deals(args.deals)
    if not deals:
        print("[sim] no deals found; nothing to simulate")
        return 2
    prefix = "sim-" if not args.tag else f"sim-{args.tag}-"

    sess = sqlite3.connect(args.sessions)
    obs = sqlite3.connect(args.obs)
    try:
        # Idempotency: drop the previous cohort with the same tag.
        sim_sessions = [r[0] for r in sess.execute(
            "SELECT session_id FROM sessions WHERE user_id LIKE ?",
            (prefix + "%",))]
        if sim_sessions:
            sess.executemany("DELETE FROM feedback WHERE session_id = ?",
                             [(s,) for s in sim_sessions])
            sess.executemany("DELETE FROM turns WHERE session_id = ?",
                             [(s,) for s in sim_sessions])
            sess.executemany("DELETE FROM sessions WHERE session_id = ?",
                             [(s,) for s in sim_sessions])
        obs.execute("DELETE FROM recommendation_logs WHERE user_id LIKE ?",
                    (prefix + "%",))

        base_time = datetime.now() - timedelta(days=2)
        n_feedback = 0
        trace_seq = 0
        for i in range(args.requests):
            user_id = f"{prefix}u{i % 30}"       # 30 sim users
            session_id = f"{prefix}s{i}"
            trace_id = f"{prefix}t{i}"
            ts = (base_time + timedelta(minutes=i * 7)).isoformat(timespec="seconds")

            # Pick a city that actually has deals, then candidates from it.
            city = rng.choice(["武汉", "上海"])
            category = rng.choice(CATEGORIES)
            budget = rng.choice([100, 200, 300, 500, 800])
            pool = [d for d in deals if d.get("city") == city]
            rng.shuffle(pool)
            cands = pool[:rng.randint(8, 15)]
            if not cands:
                continue
            max_sold = max(float(c.get("sold_count") or 0) for c in cands) or 1.0
            cand_audit = [{
                "item_id": c["item_id"],
                "rule_score": rule_score(c, budget, max_sold),
                "model_score": None,
            } for c in cands]
            ranked = sorted(cand_audit, key=lambda c: -c["rule_score"])[:3]
            slots = {"city": city, "category": category, "budget": budget,
                     "people": rng.randint(1, 6)}

            obs.execute(
                "INSERT INTO recommendation_logs "
                "(trace_id, session_id, user_id, request_text, action,"
                " slots_json, item_count, ranked_items, response_text,"
                " grounding_count, compose_mode, latency_ms, created_at,"
                " candidates_json, experiment_group, rank_mode) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (trace_id, session_id, user_id,
                 f"[sim] {city}{category} {budget}元", "retrieve",
                 json.dumps(slots, ensure_ascii=False), len(ranked),
                 json.dumps([{"item_id": c["item_id"], "score": c["rule_score"]}
                             for c in ranked], ensure_ascii=False),
                 "[simulated]", 0, "template", rng.randint(800, 3000), ts,
                 json.dumps(cand_audit, ensure_ascii=False), "", "rule"))

            sess.execute(
                "INSERT INTO sessions (session_id, user_id, current_state,"
                " context, created_at, updated_at) VALUES (?,?,?,?,?,?)",
                (session_id, user_id, "RESPOND",
                 json.dumps(slots, ensure_ascii=False), ts, ts))

            # Simulated user prefers cheap, discounted, well-rated items —
            # the same signal shape train_ranker's --synthetic uses.
            by_id = {c["item_id"]: c for c in cands}
            for r in ranked:
                c = by_id[r["item_id"]]
                price = float(c.get("price") or 0)
                original = float(c.get("original_price") or 0)
                discount = (original - price) / original if original > 0 else 0.0
                signal = (0.4 * (1 if price <= budget else 0)
                          + 0.3 * max(0.0, (float(c.get("rating") or 0) - 3.0)) / 2.0
                          + 0.3 * discount)
                roll = rng.random()
                if roll < signal:
                    ftype = "like"
                elif roll > 0.97:
                    ftype = "dislike"
                else:
                    continue
                sess.execute(
                    "INSERT INTO feedback (session_id, trace_id, item_id,"
                    " feedback_type, comment, created_at)"
                    " VALUES (?,?,?,?,?,?)",
                    (session_id, trace_id, r["item_id"], ftype, "", ts))
                n_feedback += 1
            trace_seq += 1

        sess.commit()
        obs.commit()
    finally:
        sess.close()
        obs.close()

    print(f"[sim] wrote {trace_seq} retrieve logs + {n_feedback} feedback rows"
          f" (user_id prefix '{prefix}')")
    print("[sim] REMINDER: sim rows are excluded from training/eval by default;"
          " use --include-sim only to smoke the pipeline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
