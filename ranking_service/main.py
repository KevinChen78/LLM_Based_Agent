#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Learning-to-rank service (LightGBM) for the LLM Agent — Phase 2.1.

Scores deal candidates with a LightGBM model trained offline by
scripts/train_ranker.py. Sits behind the C++ DealRanker tool: when this
service is unreachable, unhealthy, or has no model, the C++ side silently
falls back to its built-in rule scoring — this service is purely additive.

Data it reads (all local SQLite, opened read-only; never writes):
  * FEATURES_DB      item_features table (impressions/likes/dislikes per
                     item) built by scripts/build_features.py
  * SESSIONS_DB_PATH user_profiles table (Phase 2.2; absent table => neutral
                     user features, profiles_available=false in /v1/health)

Endpoints:
  GET  /v1/health -> {status, model_loaded, model_version, feature_rows,
                      profiles_available}
                      model_loaded=false is a VALID healthy state (cold start):
                      the C++ side then falls back to rule scoring.
  POST /v1/rank   <- {candidates:[{item_id,title,price,original_price,rating,
                                   sold_count,category,city,district,tags}],
                      context:{budget,people,city,category,user_id},
                      top_n, shadow}
                  -> {model_loaded, model_version,
                      items:[{item_id, model_score}, ...]  (all candidates, desc)}
                  model unavailable -> {"model_loaded": false, "items": []}
                  `shadow` is a semantic flag only — staying off the serving
                  path is the caller's (C++ ExperimentManager) business.

Config (env or ranking_service/.env.local; see .env.example):
    RANKER_PORT        default 8002
    MODEL_PATH         default <this dir>/model.txt
    FEATURES_DB        default ../data/ranking_features.db
    SESSIONS_DB_PATH   default ../data/sessions.db

Run:
    python ranking_service/main.py
"""

import json
import os
import sqlite3
import threading

from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

from features import FEATURE_NAMES, build_feature_vector, rule_ranks

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT = os.path.dirname(_HERE)


def _load_env_file(path):
    try:
        with open(path, encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                os.environ.setdefault(key.strip(), val.strip().strip('"').strip("'"))
    except FileNotFoundError:
        pass


_load_env_file(os.path.join(_HERE, ".env"))
_load_env_file(os.path.join(_HERE, ".env.local"))

PORT = int(os.environ.get("RANKER_PORT", "8002"))
MODEL_PATH = os.environ.get("MODEL_PATH", os.path.join(_HERE, "model.txt"))
FEATURES_DB = os.environ.get("FEATURES_DB",
                             os.path.join(_PROJECT, "data", "ranking_features.db"))
SESSIONS_DB_PATH = os.environ.get("SESSIONS_DB_PATH",
                                  os.path.join(_PROJECT, "data", "sessions.db"))


def _connect_ro(path):
    """Read-only SQLite connection; returns None when the file is absent."""
    if not os.path.exists(path):
        return None
    uri = "file:" + path.replace("\\", "/") + "?mode=ro"
    try:
        return sqlite3.connect(uri, uri=True, check_same_thread=False)
    except sqlite3.Error:
        return None


# ---------------------------------------------------------------------------
# Startup: model + feature stats + (optional) user profiles
# ---------------------------------------------------------------------------
BOOSTER = None
MODEL_META = {}
MODEL_LOCK = threading.Lock()   # lightgbm predict is not documented thread-safe


def _init_model():
    global BOOSTER, MODEL_META
    # meta.json sits next to model.txt by convention (train_ranker.py writes both)
    meta_path = os.path.join(os.path.dirname(MODEL_PATH), "meta.json")
    if not os.path.exists(MODEL_PATH):
        print(f"[Ranker] no model file at {MODEL_PATH}; model_loaded=false"
              " (train with scripts/train_ranker.py)")
        return
    try:
        import lightgbm as lgb
    except ImportError:
        print("[Ranker] WARNING: lightgbm not installed (pip install -r"
              " requirements.txt); model_loaded=false")
        return
    try:
        booster = lgb.Booster(model_file=MODEL_PATH)
        meta = {}
        if os.path.exists(meta_path):
            with open(meta_path, encoding="utf-8") as fh:
                meta = json.load(fh)
            trained_names = meta.get("feature_names")
            if trained_names and trained_names != FEATURE_NAMES:
                print(f"[Ranker] WARNING: model feature_names != code"
                      f" FEATURE_NAMES ({trained_names} vs {FEATURE_NAMES});"
                      " model_loaded=false (retrain)")
                return
        BOOSTER, MODEL_META = booster, meta
        print(f"[Ranker] model loaded: {MODEL_PATH}"
              f" (version={meta.get('version', '?')},"
              f" n_samples={meta.get('n_samples', '?')})")
    except Exception as e:  # noqa: BLE001 — any load failure degrades cleanly
        print(f"[Ranker] WARNING: model load failed ({e}); model_loaded=false")
        BOOSTER, MODEL_META = None, {}


_init_model()

FEATURE_STATS = {}     # item_id -> {impressions, likes, dislikes, category_hot}
_profiles_conn = None  # lazily opened read-only connection to sessions.db


def _load_feature_stats():
    conn = _connect_ro(FEATURES_DB)
    if conn is None:
        print(f"[Ranker] no features db at {FEATURES_DB}; stats default to zero"
              " (run scripts/build_features.py)")
        return
    try:
        rows = conn.execute(
            "SELECT item_id, impressions, likes, dislikes, category_hot"
            " FROM item_features").fetchall()
        for item_id, impr, likes, dislikes, hot in rows:
            FEATURE_STATS[item_id] = {
                "impressions": impr, "likes": likes,
                "dislikes": dislikes, "category_hot": hot or 0.0,
            }
        print(f"[Ranker] item_features loaded: {len(FEATURE_STATS)} rows")
    except sqlite3.Error as e:
        print(f"[Ranker] WARNING: reading item_features failed ({e}); zeros")
    finally:
        conn.close()


_load_feature_stats()


def _load_user_profile(user_id):
    """Best-effort read of the Phase 2.2 user_profiles table. Any failure
    (no db, no table, no row) yields neutral defaults — never fatal."""
    global _profiles_conn
    if not user_id:
        return {}
    if _profiles_conn is None:
        _profiles_conn = _connect_ro(SESSIONS_DB_PATH)
        if _profiles_conn is None:
            return {}
    try:
        row = _profiles_conn.execute(
            "SELECT preferred_categories, avg_budget, price_sensitivity"
            " FROM user_profiles WHERE user_id = ?", (user_id,)).fetchone()
        if row is None:
            return {}
        return {
            "preferred_categories": json.loads(row[0] or "[]"),
            "avg_budget": row[1] or 0.0,
            "price_sensitivity": row[2] if row[2] is not None else 0.5,
        }
    except sqlite3.Error:
        return {}   # table may not exist yet (pre-Phase-2.2 db)


def _profiles_available():
    conn = _connect_ro(SESSIONS_DB_PATH)
    if conn is None:
        return False
    try:
        row = conn.execute(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table'"
            " AND name='user_profiles'").fetchone()
        return bool(row and row[0])
    except sqlite3.Error:
        return False
    finally:
        conn.close()


# ---------------------------------------------------------------------------
# Ranking
# ---------------------------------------------------------------------------
def rank(body):
    if BOOSTER is None:
        return {"model_loaded": False, "items": []}
    candidates = body.get("candidates") or []
    ctx = body.get("context") or {}
    if not candidates:
        return {"model_loaded": True,
                "model_version": MODEL_META.get("version", ""),
                "items": []}

    budget = float(ctx.get("budget") or 0)
    ranks = rule_ranks(candidates, budget)
    user = _load_user_profile(str(ctx.get("user_id") or ""))

    rows = []
    for item, rr in zip(candidates, ranks):
        stats = FEATURE_STATS.get(item.get("item_id"), {})
        context = {
            "budget": budget,
            "city": ctx.get("city", ""),
            "category": ctx.get("category", ""),
            "rank_in_rules": rr,
        }
        rows.append(build_feature_vector(item, stats, user, context))

    with MODEL_LOCK:
        import numpy as np  # lightgbm dependency; list-of-lists is rejected
        scores = BOOSTER.predict(np.asarray(rows, dtype=np.float64))
    items = [
        {"item_id": c.get("item_id", ""), "model_score": float(s)}
        for c, s in zip(candidates, scores)
    ]
    items.sort(key=lambda x: x["model_score"], reverse=True)
    return {
        "model_loaded": True,
        "model_version": MODEL_META.get("version", ""),
        "items": items,
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

    def do_GET(self):
        if self.path == "/v1/health":
            self._send_json(200, {
                "status": "ok",
                "model_loaded": BOOSTER is not None,
                "model_version": MODEL_META.get("version", ""),
                "feature_rows": len(FEATURE_STATS),
                "profiles_available": _profiles_available(),
            })
            return
        self.send_error(404)

    def do_POST(self):
        if self.path != "/v1/rank":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length)
        try:
            data = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            self.send_error(400)
            return
        try:
            self._send_json(200, rank(data))
        except Exception as e:  # noqa: BLE001
            self._send_json(500, {"error": str(e)})

    def log_message(self, fmt, *args):
        print(f"[Ranker] {self.address_string()} - {fmt % args}")


def main():
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[Ranker] Running on http://0.0.0.0:{PORT}"
          f"  model_loaded={BOOSTER is not None}"
          f"  feature_rows={len(FEATURE_STATS)}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[Ranker] Shutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
