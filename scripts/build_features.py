#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build the offline item_features store for learning-to-rank (Phase 2.1).

Aggregates exposure/feedback signals into data/ranking_features.db:

  * impressions — how often an item appeared in a ranker candidate set
                  (recommendation_logs.candidates_json, Phase 2.1 column;
                  requests before that column existed simply contribute 0)
  * likes/dislikes — sessions.db feedback rows joined by item_id
  * category_hot — add-one smoothed share of likes in the item's category
                   (item -> category mapping comes from data/deals.json)

Pure stdlib, idempotent (full rebuild each run), safe to run while the
api_server is up: source DBs are opened read-only, the output DB is a
separate file written in one transaction.

Usage:
    python scripts/build_features.py
    python scripts/build_features.py --obs data/observability.db \
        --sessions data/sessions.db --deals data/deals.json \
        --out data/ranking_features.db
"""

import argparse
import json
import os
import sqlite3
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT = os.path.dirname(_HERE)


def connect_ro(path):
    if not os.path.exists(path):
        print(f"[features] WARNING: {path} not found, treating as empty")
        return None
    uri = "file:" + os.path.abspath(path).replace("\\", "/") + "?mode=ro"
    return sqlite3.connect(uri, uri=True)


def load_categories(deals_path):
    """item_id -> category from the deal catalog."""
    if not os.path.exists(deals_path):
        print(f"[features] WARNING: {deals_path} not found, categories empty")
        return {}
    with open(deals_path, encoding="utf-8") as fh:
        root = json.load(fh)
    deals = root.get("deals", root if isinstance(root, list) else [])
    return {d["item_id"]: d.get("category", "") for d in deals
            if d.get("item_id")}


def load_impressions(obs_path):
    """item_id -> exposure count, from recommendation_logs.candidates_json."""
    conn = connect_ro(obs_path)
    if conn is None:
        return Counter()
    try:
        cols = {r[1] for r in conn.execute("PRAGMA table_info(recommendation_logs)")}
        if "candidates_json" not in cols:
            print("[features] recommendation_logs has no candidates_json column"
                  " yet (pre-Phase-2.1 db); impressions all zero")
            return Counter()
        impressions = Counter()
        for (blob,) in conn.execute(
                "SELECT candidates_json FROM recommendation_logs"
                " WHERE candidates_json IS NOT NULL AND candidates_json != ''"):
            try:
                for cand in json.loads(blob):
                    item_id = cand.get("item_id")
                    if item_id:
                        impressions[item_id] += 1
            except (json.JSONDecodeError, AttributeError):
                continue
        return impressions
    finally:
        conn.close()


def load_feedback(sessions_path):
    """item_id -> (likes, dislikes). Item-level rows only (item_id != '')."""
    conn = connect_ro(sessions_path)
    if conn is None:
        return Counter(), Counter()
    try:
        likes, dislikes = Counter(), Counter()
        for item_id, ftype, n in conn.execute(
                "SELECT item_id, feedback_type, COUNT(*) FROM feedback"
                " WHERE item_id IS NOT NULL AND item_id != ''"
                " GROUP BY item_id, feedback_type"):
            if ftype == "like":
                likes[item_id] = n
            elif ftype == "dislike":
                dislikes[item_id] = n
        return likes, dislikes
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--deals", default=os.path.join(_PROJECT, "data", "deals.json"))
    ap.add_argument("--obs", default=os.path.join(_PROJECT, "data", "observability.db"))
    ap.add_argument("--sessions", default=os.path.join(_PROJECT, "data", "sessions.db"))
    ap.add_argument("--out", default=os.path.join(_PROJECT, "data", "ranking_features.db"))
    args = ap.parse_args()

    categories = load_categories(args.deals)
    impressions = load_impressions(args.obs)
    likes, dislikes = load_feedback(args.sessions)

    # category_hot: add-one smoothed share of likes per category.
    cat_likes = Counter()
    for item_id, n in likes.items():
        cat = categories.get(item_id, "")
        if cat:
            cat_likes[cat] += n
    total_likes = sum(cat_likes.values())
    n_cats = max(len(cat_likes), 1)

    def hot(item_id):
        cat = categories.get(item_id, "")
        return (cat_likes.get(cat, 0) + 1.0) / (total_likes + n_cats)

    item_ids = set(impressions) | set(likes) | set(dislikes)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    conn = sqlite3.connect(args.out)
    try:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS item_features (
                item_id      TEXT PRIMARY KEY,
                impressions  INTEGER NOT NULL DEFAULT 0,
                likes        INTEGER NOT NULL DEFAULT 0,
                dislikes     INTEGER NOT NULL DEFAULT 0,
                category_hot REAL NOT NULL DEFAULT 0,
                updated_at   TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
            )""")
        conn.execute("DELETE FROM item_features")
        conn.executemany(
            "INSERT INTO item_features"
            " (item_id, impressions, likes, dislikes, category_hot)"
            " VALUES (?, ?, ?, ?, ?)",
            [(iid, impressions.get(iid, 0), likes.get(iid, 0),
              dislikes.get(iid, 0), hot(iid)) for iid in sorted(item_ids)])
        conn.commit()
    finally:
        conn.close()

    print(f"[features] item_features rebuilt: {len(item_ids)} items"
          f" (impressions>0: {sum(1 for v in impressions.values() if v)},"
          f" with feedback: {len(set(likes) | set(dislikes))}) -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
