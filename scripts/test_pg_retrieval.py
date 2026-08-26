#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Contract-parity check: JSON backend vs PostgreSQL backend of retrieval_service.

Starts two retrieval_service instances on scratch ports — one with
RETRIEVAL_BACKEND=json, one with RETRIEVAL_BACKEND=postgres — runs a request
matrix against both, and diffs the responses exactly (item_id sequence, total,
score within 1e-9, full item/passage dicts). This is the deterministic proof
that the SQL filter pushdown reproduces the Python filter semantics
(two-guard people filter, `is not None` price semantics, NULL district,
rating-fallback tie order).

Skips cleanly (exit 0) when PostgreSQL or psycopg is unavailable so it never
blocks environments without PG.

Usage (from the project root):
    python scripts/test_pg_retrieval.py
"""
import json
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SERVICE = os.path.join(PROJECT, "retrieval_service", "main.py")

JSON_PORT = 8101
PG_PORT = 8102


# Same loader semantics as retrieval_service/main.py, so PG_DSN/PGHOST/... set
# in retrieval_service/.env.local are visible to the probe below.
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

# (path, body) — the parity matrix. Chosen to exercise every filter branch:
# empty filters, each structured filter alone and combined, price `is not None`
# semantics (max_price 0 must filter, not be ignored), people inside/outside
# ranges, a keyword with no text hit (rating-desc fallback), empty-query
# fallback with heavy rating ties, and kb queries.
MATRIX = [
    ("/v1/retrieve/deals", {}),
    ("/v1/retrieve/deals", {"city": "武汉"}),
    ("/v1/retrieve/deals", {"city": "武汉", "category": "小龙虾",
                            "query": "蒜蓉", "top_k": 5}),
    ("/v1/retrieve/deals", {"city": "武汉", "query": "小龙虾", "top_k": 20}),
    ("/v1/retrieve/deals", {"city": "武汉", "max_price": 200.0}),
    ("/v1/retrieve/deals", {"city": "深圳", "min_price": 100.0, "max_price": 300.0}),
    ("/v1/retrieve/deals", {"max_price": 0.0}),            # boundary: filters everything
    ("/v1/retrieve/deals", {"city": "北京", "district": "海淀"}),
    ("/v1/retrieve/deals", {"city": "武汉", "people": 2}),
    ("/v1/retrieve/deals", {"city": "武汉", "people": 6}),
    ("/v1/retrieve/deals", {"city": "上海", "people": 12, "top_k": 7}),
    ("/v1/retrieve/deals", {"query": "不存在的商品关键词xyz", "city": "武汉", "top_k": 5}),
    ("/v1/retrieve/deals", {"query": "火锅", "top_k": 3}),
    ("/v1/retrieve/deals", {"category": "烧烤", "query": "烤", "top_k": 50}),
    ("/v1/retrieve/deals", {"city": "杭州"}),
    ("/v1/retrieve/kb", {"query": "发票"}),
    ("/v1/retrieve/kb", {"query": "包间", "top_k": 1}),
    ("/v1/retrieve/kb", {"query": "退款 政策", "top_k": 5}),
    ("/v1/retrieve/kb", {"query": ""}),
    ("/v1/retrieve/kb", {}),
]


def post(port, path, body):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def get_health(port):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/v1/health",
                                timeout=5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def wait_up(port, proc, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"service on :{port} exited early (rc={proc.returncode})")
        try:
            return get_health(port)
        except Exception:
            time.sleep(0.25)
    raise RuntimeError(f"service on :{port} did not come up in {timeout}s")


def pg_available():
    try:
        import psycopg  # noqa: F401
    except ImportError:
        return False, "psycopg not installed (pip install -r requirements.txt)"
    dsn = os.environ.get("PG_DSN", "")
    try:
        import psycopg
        with psycopg.connect(dsn, connect_timeout=3) as conn:
            n = conn.execute("SELECT count(*) FROM groupbuy_items").fetchone()[0]
            k = conn.execute("SELECT count(*) FROM kb_passages").fetchone()[0]
        if n == 0 or k == 0:
            return False, f"PG reachable but empty (items={n}, kb={k}) — run scripts/pg_seed.py"
        return True, ""
    except Exception as e:
        return False, f"PG unreachable: {e}"


def start_service(port, backend):
    env = dict(os.environ)
    env["RETRIEVAL_PORT"] = str(port)
    env["RETRIEVAL_BACKEND"] = backend
    # Parity here means "BM25 + filter pushdown are identical across backends".
    # The vector channel only exists on postgres and legitimately changes
    # rankings, so pin it off; vector recall is covered by test_pg_vector.py.
    env["RETRIEVAL_VECTOR"] = "off"
    return subprocess.Popen(
        [sys.executable, SERVICE], env=env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def score_close(a, b):
    return abs(float(a) - float(b)) < 1e-9


def diff_items(name, a_items, b_items):
    """Return a list of mismatch descriptions (empty = parity)."""
    errs = []
    if len(a_items) != len(b_items):
        errs.append(f"count differs: json={len(a_items)} pg={len(b_items)}")
        return errs
    for i, (x, y) in enumerate(zip(a_items, b_items)):
        id_x, id_y = x.get("item_id", x.get("id")), y.get("item_id", y.get("id"))
        if id_x != id_y:
            errs.append(f"[{i}] id order differs: json={id_x} pg={id_y}")
            continue
        if not score_close(x.get("score", 0), y.get("score", 0)):
            errs.append(f"[{i}] {id_x} score differs: json={x.get('score')} pg={y.get('score')}")
        dx = {k: v for k, v in x.items() if k != "score"}
        dy = {k: v for k, v in y.items() if k != "score"}
        if dx != dy:
            keys = sorted(set(dx) | set(dy))
            bad = [k for k in keys if dx.get(k) != dy.get(k)]
            errs.append(f"[{i}] {id_x} fields differ: "
                        + ", ".join(f"{k}: json={dx.get(k)!r} pg={dy.get(k)!r}" for k in bad))
    return errs


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    ok, why = pg_available()
    if not ok:
        print(f"SKIP: {why}")
        return 0

    procs = []
    try:
        procs.append(start_service(JSON_PORT, "json"))
        procs.append(start_service(PG_PORT, "postgres"))
        hj = wait_up(JSON_PORT, procs[0])
        hp = wait_up(PG_PORT, procs[1])

        if hj.get("backend") != "json" or hp.get("backend") != "postgres":
            print(f"FAIL: unexpected backends json={hj.get('backend')} pg={hp.get('backend')}")
            return 1
        if hj.get("deal_count") != hp.get("deal_count") or hj.get("kb_count") != hp.get("kb_count"):
            print(f"FAIL: corpus counts differ: json={hj.get('deal_count')}/{hj.get('kb_count')}"
                  f" pg={hp.get('deal_count')}/{hp.get('kb_count')}")
            return 1
        print(f"backends up: json :{JSON_PORT} (deals={hj['deal_count']}) | "
              f"postgres :{PG_PORT} (deals={hp['deal_count']})")

        failures = 0
        for path, body in MATRIX:
            rj = post(JSON_PORT, path, body)
            rp = post(PG_PORT, path, body)
            key = "items" if path.endswith("deals") else "passages"
            errs = diff_items(f"{path} {body}", rj.get(key, []), rp.get(key, []))
            if rj.get("total") != rp.get("total"):
                errs.append(f"total differs: json={rj.get('total')} pg={rp.get('total')}")
            label = f"{path} {json.dumps(body, ensure_ascii=False)}"
            if errs:
                failures += 1
                print(f"FAIL {label}")
                for e in errs[:8]:
                    print(f"     {e}")
            else:
                print(f"ok   {label}  ({len(rj.get(key, []))} rows)")

        if failures:
            print(f"\n{failures}/{len(MATRIX)} cases MISMATCHED")
            return 1
        print(f"\nPASS: all {len(MATRIX)} cases identical across backends "
              f"(deals={hj['deal_count']}, kb={hj['kb_count']})")
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
