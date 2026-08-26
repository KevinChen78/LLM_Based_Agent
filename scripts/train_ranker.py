#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Train the LightGBM learning-to-rank model (Phase 2.1).

Training data comes from the live audit trail:
  * recommendation_logs.candidates_json — the ranker's candidate sets with
    rule scores (one group per trace_id)
  * sessions.db feedback                — labels: like=1, dislike=0,
    no feedback => row skipped (or 0 with --implicit-negatives)
  * data/deals.json                     — static candidate features
  * data/ranking_features.db            — per-item stats (build_features.py)
  * sessions.db user_profiles           — user features (absent => neutral)

Feature assembly is imported from ranking_service/features.py — the same
code the ranking_service serves with, so training/inference cannot drift.

Cold-start honesty: with fewer than --min-samples labelled rows or fewer
than --min-positives likes, no model file is written and the service keeps
serving model_loaded=false (C++ falls back to rule scoring).

Usage:
    python scripts/build_features.py            # refresh stats first
    python scripts/train_ranker.py              # real data (sim- excluded)
    python scripts/train_ranker.py --synthetic  # smoke test, no real DBs
    python scripts/train_ranker.py --include-sim  # pipeline smoke on sim rows
"""

import argparse
import json
import math
import os
import random
import sqlite3
import sys
from datetime import datetime

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_PROJECT, "ranking_service"))

from features import FEATURE_NAMES, build_feature_vector  # noqa: E402


def connect_ro(path):
    if not path or not os.path.exists(path):
        return None
    uri = "file:" + os.path.abspath(path).replace("\\", "/") + "?mode=ro"
    return sqlite3.connect(uri, uri=True)


def load_items(deals_path):
    with open(deals_path, encoding="utf-8") as fh:
        root = json.load(fh)
    deals = root.get("deals", root if isinstance(root, list) else [])
    return {d["item_id"]: d for d in deals if d.get("item_id")}


def load_stats(features_db):
    conn = connect_ro(features_db)
    if conn is None:
        print(f"[train] WARNING: {features_db} missing, stats default to zero"
              " (run scripts/build_features.py)")
        return {}
    try:
        return {r[0]: {"impressions": r[1], "likes": r[2],
                       "dislikes": r[3], "category_hot": r[4] or 0.0}
                for r in conn.execute(
                    "SELECT item_id, impressions, likes, dislikes, category_hot"
                    " FROM item_features")}
    except sqlite3.Error as e:
        print(f"[train] WARNING: reading item_features failed ({e}); zeros")
        return {}
    finally:
        conn.close()


def load_labels(sessions_db):
    """(trace_id, item_id) -> 1 (like) / 0 (dislike)."""
    conn = connect_ro(sessions_db)
    if conn is None:
        return {}
    try:
        labels = {}
        for trace_id, item_id, ftype in conn.execute(
                "SELECT trace_id, item_id, feedback_type FROM feedback"
                " WHERE item_id IS NOT NULL AND item_id != ''"
                " AND trace_id IS NOT NULL AND trace_id != ''"):
            labels[(trace_id, item_id)] = 1 if ftype == "like" else 0
        return labels
    finally:
        conn.close()


def load_profiles(sessions_db):
    conn = connect_ro(sessions_db)
    if conn is None:
        return {}
    try:
        out = {}
        for user_id, cats, budget, sens in conn.execute(
                "SELECT user_id, preferred_categories, avg_budget,"
                " price_sensitivity FROM user_profiles"):
            out[user_id] = {
                "preferred_categories": json.loads(cats or "[]"),
                "avg_budget": budget or 0.0,
                "price_sensitivity": sens if sens is not None else 0.5,
            }
        return out
    except sqlite3.Error:
        return {}   # table may not exist yet
    finally:
        conn.close()


def _ranks_from_rule_scores(rule_scores):
    """rank/max(n-1,1) by rule_score desc — same semantics as
    features.rule_ranks but from the logged scores (the candidate payloads
    are not logged, only item_id + scores)."""
    order = sorted(range(len(rule_scores)), key=lambda i: (-rule_scores[i], i))
    n = len(rule_scores)
    ranks = [0.0] * n
    for rank, idx in enumerate(order):
        ranks[idx] = rank / max(n - 1, 1)
    return ranks


def dataset_from_logs(obs_db, sessions_db, deals_path, features_db,
                      implicit_negatives, include_sim=False):
    """Returns (rows, labels, groups) or None when the obs db lacks data."""
    conn = connect_ro(obs_db)
    if conn is None:
        print(f"[train] {obs_db} not found; no training data")
        return None
    try:
        cols = {r[1] for r in conn.execute("PRAGMA table_info(recommendation_logs)")}
        if "candidates_json" not in cols:
            print("[train] recommendation_logs has no candidates_json column"
                  " yet; deploy the Phase 2.1 api_server first")
            return None
        sql = ("SELECT trace_id, user_id, slots_json, candidates_json"
               " FROM recommendation_logs"
               " WHERE candidates_json IS NOT NULL AND candidates_json != ''")
        if not include_sim:
            sql += " AND (user_id IS NULL OR user_id NOT LIKE 'sim-%')"
        logs = conn.execute(sql).fetchall()
    finally:
        conn.close()
    if not logs:
        print("[train] no recommendation_logs rows with candidates_json yet"
              + ("" if include_sim else " (sim- rows excluded by default)"))
        return None

    items = load_items(deals_path)
    stats = load_stats(features_db)
    labels = load_labels(sessions_db)
    profiles = load_profiles(sessions_db)

    rows, y, group_sizes = [], [], []
    skipped_unknown_item = 0
    for trace_id, user_id, slots_blob, cands_blob in logs:
        try:
            slots = json.loads(slots_blob or "{}")
            cands = json.loads(cands_blob)
        except json.JSONDecodeError:
            continue
        if not cands:
            continue
        rule_scores = [float(c.get("rule_score") or 0) for c in cands]
        ranks = _ranks_from_rule_scores(rule_scores)
        user = profiles.get(user_id or "", {})
        group_rows, group_labels = [], []
        for cand, rr in zip(cands, ranks):
            item_id = cand.get("item_id")
            item = items.get(item_id)
            if item is None:
                skipped_unknown_item += 1
                continue
            label = labels.get((trace_id, item_id))
            if label is None:
                if not implicit_negatives:
                    continue
                label = 0
            context = {
                "budget": slots.get("budget") or 0,
                "city": slots.get("city", ""),
                "category": slots.get("category", ""),
                "rank_in_rules": rr,
            }
            group_rows.append(build_feature_vector(
                item, stats.get(item_id, {}), user, context))
            group_labels.append(label)
        if group_rows:
            rows.extend(group_rows)
            y.extend(group_labels)
            group_sizes.append(len(group_rows))
    if skipped_unknown_item:
        print(f"[train] note: {skipped_unknown_item} candidates not in"
              " deals.json (catalog changed since); skipped")
    return rows, y, group_sizes


def synthetic_dataset(n_requests, seed):
    """Deterministic synthetic data: labels correlate with discount, rating
    and price_fit so the model has something real to learn. Reads nothing."""
    rng = random.Random(seed)
    categories = ["火锅", "烧烤", "小龙虾", "日料", "奶茶"]
    cities = ["武汉", "上海", "北京"]
    rows, y, group_sizes = [], [], []
    for _ in range(n_requests):
        budget = rng.choice([100, 200, 300, 500, 800])
        ctx = {"budget": budget, "city": rng.choice(cities),
               "category": rng.choice(categories)}
        n_cand = rng.randint(5, 20)
        cands = [{
            "item_id": f"syn-{i}",
            "price": rng.uniform(50, 900),
            "original_price": 0,
            "rating": rng.uniform(3.0, 5.0),
            "sold_count": rng.randint(0, 20000),
            "category": rng.choice(categories),
            "city": rng.choice(cities),
        } for i in range(n_cand)]
        for c in cands:
            c["original_price"] = c["price"] * rng.uniform(1.0, 2.0)
        from features import rule_ranks
        ranks = rule_ranks(cands, budget)
        for c, rr in zip(cands, ranks):
            price = c["price"]
            # Synthetic "user": likes cheap, discounted, well-rated items.
            signal = (0.4 * (1 if price <= budget else 0)
                      + 0.3 * (c["rating"] - 3.0) / 2.0
                      + 0.3 * (c["original_price"] - price) / c["original_price"])
            label = 1 if rng.random() < signal else 0
            rows.append(build_feature_vector(c, {}, {}, {**ctx, "rank_in_rules": rr}))
            y.append(label)
        group_sizes.append(n_cand)
    return rows, y, group_sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--obs", default=os.path.join(_PROJECT, "data", "observability.db"))
    ap.add_argument("--sessions", default=os.path.join(_PROJECT, "data", "sessions.db"))
    ap.add_argument("--deals", default=os.path.join(_PROJECT, "data", "deals.json"))
    ap.add_argument("--features-db",
                    default=os.path.join(_PROJECT, "data", "ranking_features.db"))
    ap.add_argument("--out-dir", default=os.path.join(_PROJECT, "ranking_service"),
                    help="directory for model.txt + meta.json")
    ap.add_argument("--objective", choices=["lambdarank", "binary"],
                    default="lambdarank")
    ap.add_argument("--implicit-negatives", action="store_true",
                    help="treat shown-but-unrated candidates as label 0")
    ap.add_argument("--min-samples", type=int, default=100)
    ap.add_argument("--min-positives", type=int, default=10)
    ap.add_argument("--force", action="store_true",
                    help="train even below the sample gates")
    ap.add_argument("--synthetic", type=int, nargs="?", const=200, metavar="N",
                    help="ignore real DBs; train on N synthetic requests (smoke)")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--include-sim", action="store_true",
                    help="include simulate_feedback.py rows (user_id LIKE"
                         " 'sim-%%'); default EXCLUDES them — simulated data"
                         " validates the pipeline, never model quality")
    args = ap.parse_args()

    if args.synthetic:
        data = synthetic_dataset(args.synthetic, args.seed)
    else:
        data = dataset_from_logs(args.obs, args.sessions, args.deals,
                                 args.features_db, args.implicit_negatives,
                                 include_sim=args.include_sim)
    if data is None:
        print("[train] no model written (no data)")
        return 2
    rows, y, groups = data
    n, pos = len(rows), sum(y)
    print(f"[train] samples={n} positives={pos} groups={len(groups)}"
          f" features={len(FEATURE_NAMES)}")
    if not args.force and (n < args.min_samples or pos < args.min_positives):
        print(f"[train] below gates (min_samples={args.min_samples},"
              f" min_positives={args.min_positives}); no model written."
              " The ranking service keeps model_loaded=false and the C++ side"
              " falls back to rule scoring. Use --force to override.")
        return 2

    try:
        import lightgbm as lgb
        import numpy as np
    except ImportError:
        print("[train] lightgbm not installed: pip install -r requirements.txt")
        return 1

    rows = np.asarray(rows, dtype=np.float64)

    params = {
        "objective": args.objective,
        "learning_rate": 0.05,
        "num_leaves": 31,
        "min_data_in_leaf": 10,
        "feature_pre_filter": False,
        "verbose": -1,
        "seed": args.seed,
    }
    if args.objective == "lambdarank":
        params.update({"lambdarank_truncation_level": 5,
                       "eval_at": [3, 5], "metric": ["ndcg"]})
    else:
        params["metric"] = ["binary_logloss", "auc"]

    # `group` is only meaningful (and only accepted) for lambdarank.
    train_set = (lgb.Dataset(rows, label=y, group=groups,
                             feature_name=FEATURE_NAMES)
                 if args.objective == "lambdarank"
                 else lgb.Dataset(rows, label=y, feature_name=FEATURE_NAMES))
    booster = lgb.train(params, train_set, num_boost_round=200)

    os.makedirs(args.out_dir, exist_ok=True)
    model_path = os.path.join(args.out_dir, "model.txt")
    booster.save_model(model_path)
    meta = {
        "version": "lgbm-" + datetime.now().strftime("%Y%m%d-%H%M%S"),
        "trained_at": datetime.now().isoformat(timespec="seconds"),
        "objective": args.objective,
        "feature_names": FEATURE_NAMES,
        "n_samples": n,
        "n_positives": pos,
        "n_groups": len(groups),
        "synthetic": bool(args.synthetic),
        # True when the training set included simulate_feedback.py rows —
        # such a model is a pipeline-smoke artifact, not an effect candidate.
        "includes_sim": bool(args.include_sim) and not args.synthetic,
    }
    with open(os.path.join(args.out_dir, "meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
    print(f"[train] model written: {model_path} (version={meta['version']})"
          " — restart ranking_service to pick it up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
