#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Vector-recall (pgvector + RRF) tests for retrieval_service.

Starts two postgres-backend instances on scratch ports — RETRIEVAL_VECTOR=on
and =off — and asserts:

  1. health reports the vector channel state correctly;
  2. semantic/synonym queries surface expected categories (pure BM25 misses
     these — that's the point of the channel);
  3. comparative proof: the vector-on result contains at least one
     expected-category item ABSENT from the vector-off top_k;
  4. regression: a query that hits nothing in either channel still returns
     identical rating-desc order on both;
  5. structured filters still constrain vector results.

Skips cleanly (exit 0) when PG, embeddings, or fastembed are unavailable.

Prereqs: sql/002_vector.sql applied + `python scripts/pg_embed.py` run.

Usage (from the project root):
    python scripts/test_pg_vector.py
"""
import json
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SERVICE = os.path.join(PROJECT, "retrieval_service", "main.py")

ON_PORT = 8201
OFF_PORT = 8202

# Category-level expectations (tolerate data regeneration). Grounded in the
# actual catalog: 约会场景 -> 西餐/日料/甜品; 辣 -> 川菜/火锅/湘菜/串串.
SEMANTIC_CASES = [
    ({"query": "情侣约会", "top_k": 5}, {"西餐", "日料", "甜品"}),
    ({"query": "聚餐吃点辣的", "top_k": 5}, {"川菜", "火锅", "湘菜", "串串"}),
]


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


_load_env_file(os.path.join(PROJECT, "retrieval_service", ".env"))
_load_env_file(os.path.join(PROJECT, "retrieval_service", ".env.local"))


def prereqs():
    try:
        import psycopg
    except ImportError:
        return False, "psycopg not installed"
    try:
        import fastembed  # noqa: F401
    except ImportError:
        return False, "fastembed not installed (pip install -r requirements.txt)"
    try:
        with psycopg.connect(os.environ.get("PG_DSN", ""), connect_timeout=3) as conn:
            n = conn.execute(
                "SELECT count(*) FROM groupbuy_items"
                " WHERE embedding IS NOT NULL").fetchone()[0]
        if n == 0:
            return False, "no embeddings — run scripts/pg_embed.py"
        return True, ""
    except Exception as e:
        return False, f"PG/embeddings unavailable: {e}"


def post(port, path, body):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def wait_up(port, proc, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"service on :{port} exited early (rc={proc.returncode})")
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/v1/health",
                                        timeout=5) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception:
            time.sleep(0.5)
    raise RuntimeError(f"service on :{port} did not come up in {timeout}s")


def start_service(port, vector):
    env = dict(os.environ)
    env["RETRIEVAL_PORT"] = str(port)
    env["RETRIEVAL_BACKEND"] = "postgres"
    env["RETRIEVAL_VECTOR"] = vector
    return subprocess.Popen([sys.executable, SERVICE], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ok, why = prereqs()
    if not ok:
        print(f"SKIP: {why}")
        return 0

    procs = []
    try:
        # The vector-on instance loads the ONNX model at startup (may download
        # ~100MB on first run) — generous timeout.
        procs.append(start_service(ON_PORT, "on"))
        hon = wait_up(ON_PORT, procs[0], timeout=180)
        procs.append(start_service(OFF_PORT, "off"))
        hoff = wait_up(OFF_PORT, procs[1], timeout=30)

        failures = []

        def check(name, cond, detail=""):
            if cond:
                print(f"ok   {name}")
            else:
                failures.append(name)
                print(f"FAIL {name}  {detail}")

        check("health.vector_on", hon.get("vector") == "on", str(hon))
        check("health.vector_off", hoff.get("vector") == "off", str(hoff))
        check("health.vector_model",
              hon.get("vector_model") == "BAAI/bge-small-zh-v1.5", str(hon))

        comparative_proven = False
        for body, expected_cats in SEMANTIC_CASES:
            r_on = post(ON_PORT, "/v1/retrieve/deals", body)["items"]
            r_off = post(OFF_PORT, "/v1/retrieve/deals", body)["items"]
            cats_on = {it["category"] for it in r_on}
            label = json.dumps(body, ensure_ascii=False)
            check(f"semantic {label}",
                  bool(cats_on & expected_cats),
                  f"got {sorted(cats_on)}, want one of {sorted(expected_cats)}")
            on_expected_ids = {it["item_id"] for it in r_on
                               if it["category"] in expected_cats}
            off_ids = {it["item_id"] for it in r_off}
            if on_expected_ids - off_ids:
                comparative_proven = True
        check("comparative: vector adds expected-category recall over BM25",
              comparative_proven)

        # Regression: empty query bypasses both channels -> identical
        # rating-desc order on both instances.
        body = {"city": "武汉", "top_k": 5}
        ids_on = [it["item_id"] for it in post(ON_PORT, "/v1/retrieve/deals", body)["items"]]
        ids_off = [it["item_id"] for it in post(OFF_PORT, "/v1/retrieve/deals", body)["items"]]
        check("regression: empty query identical rating-desc",
              ids_on == ids_off and len(ids_on) == 5, f"on={ids_on} off={ids_off}")

        # Regression: empty filter set -> both instances return nothing.
        body = {"query": "火锅", "max_price": 0.0}
        r_on = post(ON_PORT, "/v1/retrieve/deals", body)
        r_off = post(OFF_PORT, "/v1/retrieve/deals", body)
        check("regression: empty filter set both empty",
              r_on == {"items": [], "total": 0} and r_off == {"items": [], "total": 0},
              f"on={r_on.get('total')} off={r_off.get('total')}")

        # Documented behaviour change: a garbage query with BM25 hits nowhere
        # now returns vector nearest-neighbours instead of rating-desc.
        body = {"query": "zzzz不存在xyz", "city": "武汉", "top_k": 5}
        items = post(ON_PORT, "/v1/retrieve/deals", body)["items"]
        check("vector: garbage query still serves vector picks",
              len(items) == 5 and all(0.0 < it["score"] <= 1.0 for it in items),
              f"n={len(items)}")

        # Structured filter still constrains the vector channel.
        body = {"query": "情侣约会", "city": "上海", "top_k": 5}
        items = post(ON_PORT, "/v1/retrieve/deals", body)["items"]
        check("filter: vector results respect city",
              items and all(it["city"] == "上海" for it in items),
              f"cities={sorted({it['city'] for it in items})}")

        if failures:
            print(f"\n{len(failures)} checks FAILED")
            return 1
        print("\nPASS: vector channel adds semantic recall, regressions clean")
        return 0
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()


if __name__ == "__main__":
    sys.exit(main())
