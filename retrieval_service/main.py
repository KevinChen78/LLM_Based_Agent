#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Retrieval service (BM25 + PostgreSQL filter pushdown) for the LLM Agent.

HTTP service providing two RAG retrieval corpora:

  1. deals  — BM25 over the group-buy catalog; replaces the C++ DealRetriever
              substring match when this service is used.
  2. kb     — BM25 over knowledge passages (FAQ / merchant policy / dish info
              used to ground the LLM reply).

Two storage backends, selected by RETRIEVAL_BACKEND:

  * postgres (default) — deals/kb live in PostgreSQL (schema: sql/001_schema.sql,
    seed: scripts/pg_seed.py). Structured filters (city/category/district/
    price/people) are pushed down to an indexed SQL query; BM25 ranking still
    runs in Python over an in-memory corpus loaded from PG at startup, so
    scores are bit-identical to the json backend. Requires psycopg
    (pip install -r requirements.txt) — the project's only third-party dep.
  * json — legacy file backend over data/deals.json + data/knowledge.json,
    pure stdlib, zero third-party dependencies.

If the postgres backend fails to come up (no psycopg, PG unreachable, schema
missing), the service logs a warning and falls back to the json backend.

Vector recall (RETRIEVAL_VECTOR=on, default; postgres backend only): query
embeddings via fastembed BAAI/bge-small-zh-v1.5 + pgvector cosine ANN over the
same SQL-filtered candidates, fused with BM25 by reciprocal rank fusion
(RRF k=60). Recovers synonyms/paraphrases pure BM25 misses. Auto-disables
(with WARNING, pure-BM25 behaviour) when fastembed, the pgvector column, or
embeddings are missing — apply sql/002_vector.sql then scripts/pg_embed.py.

Chinese text is tokenized with character bigrams (no jieba/segmenter needed);
ASCII runs are split on whitespace. BM25 (k1=1.5, b=0.75) over each corpus.

Designed to mirror llm_gateway/main.py (ThreadingHTTPServer, env-file loader).
The C++ control plane calls it only when RETRIEVAL_SERVICE_URL is set; with the
URL empty the C++ side falls back to its local substring retriever and no KB.

Config (env or retrieval_service/.env.local; see .env.example):
    RETRIEVAL_PORT     default 8001
    RETRIEVAL_BACKEND  postgres (default) | json
    RETRIEVAL_VECTOR   on (default) | off — pgvector RRF channel, postgres only
    PG_DSN             libpq conninfo string; empty => libpq reads
                       PGHOST/PGPORT/PGDATABASE/PGUSER/PGPASSWORD
    DEALS_PATH         default ../data/deals.json  (relative to this file)
    KB_PATH            default ../data/knowledge.json

Run:
    python retrieval_service/main.py

Test:
    curl http://localhost:8001/v1/health
    curl -X POST http://localhost:8001/v1/retrieve/deals \
      -H "Content-Type: application/json" \
      -d '{"query":"小龙虾","city":"武汉","top_k":5}'
    python scripts/test_pg_retrieval.py   # json vs postgres parity matrix
"""

import json
import math
import os
import re
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

from dealtext import deal_text, vec_literal

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

PORT = int(os.environ.get("RETRIEVAL_PORT", "8001"))
DEALS_PATH = os.environ.get("DEALS_PATH", os.path.join(_PROJECT, "data", "deals.json"))
KB_PATH = os.environ.get("KB_PATH", os.path.join(_PROJECT, "data", "knowledge.json"))
RETRIEVAL_BACKEND = os.environ.get("RETRIEVAL_BACKEND", "postgres").lower()
# Empty DSN => pass "" to psycopg and let libpq consume PGHOST/PGPORT/
# PGDATABASE/PGUSER/PGPASSWORD (already populated from .env.local above).
PG_DSN = os.environ.get("PG_DSN", "")
# Vector recall channel (pgvector + fastembed bge-small-zh-v1.5), fused with
# BM25 via RRF. Only meaningful on the postgres backend; auto-disables with a
# WARNING when fastembed/pgvector/embeddings are unavailable.
RETRIEVAL_VECTOR = os.environ.get("RETRIEVAL_VECTOR", "on").lower() == "on"
EMBED_MODEL_NAME = "BAAI/bge-small-zh-v1.5"


# ---------------------------------------------------------------------------
# Tokenizer + BM25
# ---------------------------------------------------------------------------
_ASCII_WORD = re.compile(r"[A-Za-z0-9]+")


def tokenize(text):
    """Tokenize mixed CJK/ASCII text.

    CJK characters -> overlapping character bigrams ("小龙虾" -> ["小龙","龙虾"]).
    ASCII alnum runs -> lowercased whole tokens. Whitespace/punctuation ignored.
    """
    if not text:
        return []
    tokens = []
    # Pull out ASCII words first, then handle CJK on what's left.
    for m in _ASCII_WORD.findall(text):
        tokens.append(m.lower())
    # Build a CJK-only string (drop the ASCII runs already captured).
    cjk = "".join(ch for ch in text if not (ch.isascii() and (ch.isalnum())))
    for i in range(len(cjk) - 1):
        a, b = cjk[i], cjk[i + 1]
        if a.isspace() or b.isspace():
            continue
        tokens.append(a + b)
    return tokens


class BM25Index:
    """In-memory BM25 over a list of documents (each a token list)."""

    def __init__(self, k1=1.5, b=0.75):
        self.k1 = k1
        self.b = b
        self.doc_len = []          # tokens per doc
        self.avgdl = 0.0
        self.df = {}               # term -> document frequency
        self.postings = {}         # term -> {doc_idx: tf}
        self.n = 0

    def build(self, docs_tokens):
        self.n = len(docs_tokens)
        self.doc_len = [len(d) for d in docs_tokens]
        total = sum(self.doc_len)
        self.avgdl = (total / self.n) if self.n else 0.0
        self.df = {}
        self.postings = {}
        for i, toks in enumerate(docs_tokens):
            tf = {}
            for t in toks:
                tf[t] = tf.get(t, 0) + 1
            for t, f in tf.items():
                self.df[t] = self.df.get(t, 0) + 1
                self.postings.setdefault(t, {})[i] = f
        return self

    def _idf(self, term):
        df = self.df.get(term, 0)
        if df == 0:
            return 0.0
        # BM25+ style idf (always positive).
        return math.log(1.0 + (self.n - df + 0.5) / (df + 0.5))

    def search(self, query_tokens, candidate_ids=None, top_k=20):
        """Return list of (doc_idx, score) sorted desc. Optionally restrict to
        candidate_ids (a set/list of doc indexes)."""
        scores = {}
        cand = set(candidate_ids) if candidate_ids is not None else None
        for t in query_tokens:
            idf = self._idf(t)
            if idf == 0.0:
                continue
            for doc_idx, tf in self.postings.get(t, {}).items():
                if cand is not None and doc_idx not in cand:
                    continue
                dl = self.doc_len[doc_idx] or 1
                denom = tf + self.k1 * (1 - self.b + self.b * dl / (self.avgdl or 1))
                scores[doc_idx] = scores.get(doc_idx, 0.0) + idf * (tf * (self.k1 + 1)) / denom
        ranked = sorted(scores.items(), key=lambda kv: kv[1], reverse=True)
        return ranked[:top_k] if top_k else ranked


# ---------------------------------------------------------------------------
# Corpus loading
# ---------------------------------------------------------------------------
def _load_json(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


class Corpus:
    def __init__(self, docs, index):
        self.docs = docs          # raw list
        self.index = index        # BM25Index


def deal_corpus_from_rows(deals):
    docs = [tokenize(deal_text(d)) for d in deals]
    return Corpus(deals, BM25Index().build(docs))


def kb_corpus_from_rows(items):
    docs = [tokenize((p.get("title", "") + " " + p.get("content", "") + " " +
                      " ".join(p.get("tags", []) if isinstance(p.get("tags"), list) else [])))
            for p in items]
    return Corpus(items, BM25Index().build(docs))


def load_deal_corpus(path):
    root = _load_json(path)
    deals = root.get("deals", root if isinstance(root, list) else [])
    return deal_corpus_from_rows(deals)


def load_kb_corpus(path):
    root = _load_json(path)
    items = root.get("passages", root if isinstance(root, list) else [])
    return kb_corpus_from_rows(items)


# ---------------------------------------------------------------------------
# PostgreSQL backend (filter pushdown; BM25 ranking stays in Python)
# ---------------------------------------------------------------------------
def _pg_connect_pool():
    """Lazy-import psycopg so RETRIEVAL_BACKEND=json needs zero third-party
    packages. Raises on any unavailability."""
    from psycopg.rows import dict_row
    from psycopg_pool import ConnectionPool
    pool = ConnectionPool(
        PG_DSN, min_size=1, max_size=4, open=False,
        kwargs={"row_factory": dict_row, "autocommit": True,
                "connect_timeout": 5})
    pool.open(wait=True, timeout=5)   # raises if PG is unreachable
    return pool


def _to_float(v):
    """psycopg returns Decimal for DECIMAL columns; json.dumps would choke on
    those, and the C++ side expects floats. Single conversion point."""
    return float(v) if v is not None else None


def pg_load_deals(pool):
    """All deals as plain dicts with exactly the 14 JSON keys, in item_id
    order (== data/deals.json file order, which the rating-desc fallback's
    stable sort depends on for tie order)."""
    with pool.connection() as conn:
        rows = conn.execute(
            "SELECT item_id, merchant_id, title, category, city, district,"
            " price, original_price, sold_count, rating,"
            " min_people, max_people, tags, description"
            " FROM groupbuy_items ORDER BY item_id").fetchall()
    deals = []
    for r in rows:
        deals.append({
            "item_id": r["item_id"],
            "merchant_id": r["merchant_id"],
            "title": r["title"],
            "category": r["category"],
            "city": r["city"],
            "district": r["district"],
            "price": _to_float(r["price"]),
            "original_price": _to_float(r["original_price"]),
            "sold_count": r["sold_count"],
            "rating": _to_float(r["rating"]),
            "min_people": r["min_people"],
            "max_people": r["max_people"],
            "tags": r["tags"] or [],          # JSONB arrives decoded by psycopg3
            "description": r["description"],
        })
    return deals


def pg_load_kb(pool):
    with pool.connection() as conn:
        rows = conn.execute(
            "SELECT id, category, title, content, source, tags"
            " FROM kb_passages ORDER BY id").fetchall()
    return [{
        "id": r["id"], "category": r["category"], "title": r["title"],
        "content": r["content"], "source": r["source"],
        "tags": r["tags"] or [],
    } for r in rows]


def _pg_filter_where(query):
    """Returns (where_fragment_without_WHERE_keyword, params).

    Single source of truth for the structured hard-filters — used by both
    pg_filter_item_ids (BM25 channel) and pg_vector_ranked (vector channel).
    Mirrors filter_deals clause-for-clause — keep the two in sync:
      * price uses `is not None` semantics (max_price=0 must filter);
      * people keeps the two guards: skipped entirely when people is falsy,
        and deals with a (0,0) range are never dropped by it;
      * `district =` excludes NULL districts, matching d.get('district') != x."""
    clauses, params = [], {}
    if query.get("city"):
        clauses.append("city = %(city)s")
        params["city"] = query["city"]
    if query.get("category"):
        clauses.append("category = %(category)s")
        params["category"] = query["category"]
    if query.get("district"):
        clauses.append("district = %(district)s")
        params["district"] = query["district"]
    if query.get("max_price") is not None:
        clauses.append("price <= %(max_price)s")
        params["max_price"] = query["max_price"]
    if query.get("min_price") is not None:
        clauses.append("price >= %(min_price)s")
        params["min_price"] = query["min_price"]
    people = query.get("people")
    if people:
        # Keep if the deal has NO range (0/0) or people is inside the range —
        # the exact complement of filter_deals' drop condition
        # `mn and mx and not (mn <= people <= mx)`.
        clauses.append("NOT (min_people > 0 AND max_people > 0"
                       " AND NOT (min_people <= %(people)s AND %(people)s <= max_people))")
        params["people"] = people
    return " AND ".join(clauses), params


def pg_filter_item_ids(pool, query):
    """Filter-surviving item_ids (no ORDER BY — caller re-sorts by doc order)."""
    where, params = _pg_filter_where(query)
    sql = "SELECT item_id FROM groupbuy_items"
    if where:
        sql += " WHERE " + where
    with pool.connection() as conn:
        return [r["item_id"] for r in conn.execute(sql, params).fetchall()]


def pg_vector_ranked(pool, query, vec, limit):
    """Vector channel: nearest neighbours by cosine distance, with the same
    structured hard-filters pushed down. Returns [(item_id, distance)] asc;
    item_id as secondary key keeps distance ties deterministic."""
    where, params = _pg_filter_where(query)
    clauses = ([where] if where else []) + ["embedding IS NOT NULL"]
    params = {**params, "vec": vec, "vn": limit}
    sql = ("SELECT item_id, embedding <=> %(vec)s::vector AS dist"
           " FROM groupbuy_items WHERE " + " AND ".join(clauses)
           + " ORDER BY embedding <=> %(vec)s::vector, item_id LIMIT %(vn)s")
    with pool.connection() as conn:
        return [(r["item_id"], float(r["dist"]))
                for r in conn.execute(sql, params).fetchall()]


# ---------------------------------------------------------------------------
# Startup: load corpora from the configured backend
# ---------------------------------------------------------------------------
POOL = None
BACKEND = RETRIEVAL_BACKEND
if BACKEND == "postgres":
    try:
        POOL = _pg_connect_pool()
        DEAL_CORPUS = deal_corpus_from_rows(pg_load_deals(POOL))
        KB_CORPUS = kb_corpus_from_rows(pg_load_kb(POOL))
    except Exception as e:  # noqa: BLE001 — any PG failure degrades to JSON
        print(f"[Retrieval] WARNING: postgres backend unavailable ({e});"
              " falling back to JSON")
        POOL, BACKEND = None, "json"
if BACKEND == "json":
    DEAL_CORPUS = load_deal_corpus(DEALS_PATH)
    KB_CORPUS = load_kb_corpus(KB_PATH)

DEAL_DOC_IDX = {d["item_id"]: i for i, d in enumerate(DEAL_CORPUS.docs)}


# ---------------------------------------------------------------------------
# Vector channel startup gate (postgres backend only)
# ---------------------------------------------------------------------------
EMBED_MODEL = None


def _init_vector():
    """Load the embedding model and probe PG for embeddings. Any failure
    degrades to pure BM25 with a WARNING — never fatal."""
    global EMBED_MODEL
    if not RETRIEVAL_VECTOR or BACKEND != "postgres":
        return False
    try:
        from fastembed import TextEmbedding
        EMBED_MODEL = TextEmbedding(model_name=EMBED_MODEL_NAME)
        with POOL.connection() as conn:
            n = conn.execute(
                "SELECT count(*) AS c FROM groupbuy_items"
                " WHERE embedding IS NOT NULL").fetchone()["c"]
        if n == 0:
            print("[Retrieval] WARNING: no embeddings in groupbuy_items;"
                  " vector channel off (run scripts/pg_embed.py)")
            EMBED_MODEL = None
            return False
        print(f"[Retrieval] vector channel on ({EMBED_MODEL_NAME}, {n} embeddings)")
        return True
    except Exception as e:  # noqa: BLE001 — ImportError, UndefinedColumn, PG down
        print(f"[Retrieval] WARNING: vector channel unavailable ({e}); BM25 only")
        EMBED_MODEL = None
        return False


VECTOR_ON = _init_vector()


# ---------------------------------------------------------------------------
# Retrieval logic
# ---------------------------------------------------------------------------
def filter_deals(deals, query):
    """Apply the same structured hard-filters as the C++ DealRetriever."""
    city = query.get("city", "")
    category = query.get("category", "")
    district = query.get("district", "")
    max_price = query.get("max_price")
    min_price = query.get("min_price")
    people = query.get("people")

    out = []
    for i, d in enumerate(deals):
        if city and d.get("city") != city:
            continue
        if category and d.get("category") != category:
            continue
        if district and d.get("district") != district:
            continue
        price = d.get("price", 0)
        if max_price is not None and price > max_price:
            continue
        if min_price is not None and price < min_price:
            continue
        if people:
            mn = d.get("min_people", 0)
            mx = d.get("max_people", 0)
            if mn and mx and not (mn <= people <= mx):
                continue
        out.append((i, d))
    return out


def candidate_deals(query):
    """Filter-surviving (doc_idx, deal) pairs, in corpus (== file) order.

    postgres backend: SQL does the hard filter and returns item_ids; map back
    to doc indexes and re-sort so the rating-desc fallback's stable-sort tie
    order matches the json backend exactly."""
    if BACKEND == "postgres":
        pairs = [(DEAL_DOC_IDX[i], DEAL_CORPUS.docs[DEAL_DOC_IDX[i]])
                 for i in pg_filter_item_ids(POOL, query) if i in DEAL_DOC_IDX]
        pairs.sort(key=lambda t: t[0])
        return pairs
    return filter_deals(DEAL_CORPUS.docs, query)


RRF_K = 60          # standard reciprocal-rank-fusion constant
VEC_POOL = 40       # min per-channel pool size (also top_k*4 when larger)


def retrieve_deals(body):
    query_str = body.get("query", "") or ""
    top_k = int(body.get("top_k", 20))
    candidates = candidate_deals(body)

    bm25_ranked, vec_ranked = [], []
    if query_str.strip():
        pool_n = max(top_k * 4, VEC_POOL)
        bm25_ranked = DEAL_CORPUS.index.search(
            tokenize(query_str),
            candidate_ids=[i for i, _ in candidates],
            top_k=pool_n if VECTOR_ON else top_k)
        if VECTOR_ON and POOL is not None:
            try:
                vec = vec_literal(next(iter(EMBED_MODEL.embed([query_str]))))
                vec_ranked = pg_vector_ranked(POOL, body, vec, pool_n)
            except Exception as e:  # noqa: BLE001 — degrade this request only
                print(f"[Retrieval] WARNING: vector query failed ({e});"
                      " BM25 only this request")

    if vec_ranked:
        # RRF over the two channels; absence from a channel contributes 0.
        rrf = {}
        for rank, (doc_idx, _) in enumerate(bm25_ranked, 1):
            rrf[doc_idx] = rrf.get(doc_idx, 0.0) + 1.0 / (RRF_K + rank)
        for rank, (item_id, _) in enumerate(vec_ranked, 1):
            doc_idx = DEAL_DOC_IDX.get(item_id)
            if doc_idx is not None:
                rrf[doc_idx] = rrf.get(doc_idx, 0.0) + 1.0 / (RRF_K + rank)
        # Normalize by the two-channel theoretical max (2/(K+1)) -> 0..1:
        # topping both channels ~= 1.0, topping one ~= 0.5.
        norm = 2.0 / (RRF_K + 1)
        ordered = sorted(rrf.items(), key=lambda kv: (-kv[1], kv[0]))[:top_k]
        items = []
        for doc_idx, s in ordered:
            d = dict(DEAL_CORPUS.docs[doc_idx])
            d["score"] = s / norm
            items.append(d)
    elif bm25_ranked:
        items = []
        for doc_idx, score in bm25_ranked:
            d = dict(DEAL_CORPUS.docs[doc_idx])
            d["score"] = score
            items.append(d)
    else:
        # Empty query OR no hit in ANY channel: fall back to rating order
        # (mirrors the C++ no-keyword behaviour) so an odd keyword that matches
        # nothing still yields the best filtered deals.
        ordered = sorted(candidates, key=lambda id_: id_[1].get("rating", 0), reverse=True)
        items = []
        for i, d in ordered[:top_k]:
            d2 = dict(d)
            d2["score"] = d.get("rating", 0) / 5.0
            items.append(d2)

    return {"items": items, "total": len(candidates)}


def retrieve_kb(body):
    query_str = body.get("query", "") or ""
    top_k = int(body.get("top_k", 3))
    if not query_str.strip():
        return {"passages": []}
    ranked = KB_CORPUS.index.search(tokenize(query_str), top_k=top_k)
    passages = []
    for doc_idx, score in ranked:
        p = dict(KB_CORPUS.docs[doc_idx])
        p["score"] = score
        passages.append(p)
    return {"passages": passages}


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
            payload = {
                "status": "ok",
                "corpora": ["deals", "kb"],
                "deal_count": len(DEAL_CORPUS.docs),
                "kb_count": len(KB_CORPUS.docs),
                "backend": BACKEND,
                "vector": "on" if VECTOR_ON else "off",
                "tokenizer": "char-bigram + ascii-word (BM25)",
            }
            if VECTOR_ON:
                payload["vector_model"] = EMBED_MODEL_NAME
            self._send_json(200, payload)
            return
        self.send_error(404)

    def do_POST(self):
        if self.path not in ("/v1/retrieve/deals", "/v1/retrieve/kb"):
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
            if self.path == "/v1/retrieve/deals":
                self._send_json(200, retrieve_deals(data))
            else:
                self._send_json(200, retrieve_kb(data))
        except Exception as e:  # noqa: BLE001
            self._send_json(500, {"error": str(e)})

    def log_message(self, fmt, *args):
        print(f"[Retrieval] {self.address_string()} - {fmt % args}")


def main():
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"[Retrieval] Running on http://0.0.0.0:{PORT}  backend={BACKEND}"
          f"  vector={'on' if VECTOR_ON else 'off'}"
          f"  deals={len(DEAL_CORPUS.docs)}  kb={len(KB_CORPUS.docs)}")
    if BACKEND == "json":
        print(f"[Retrieval] (json files: {DEALS_PATH}, {KB_PATH})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[Retrieval] Shutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
