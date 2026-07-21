#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Lightweight retrieval service (BM25) for the LLM Agent.

Pure-stdlib HTTP service providing two RAG retrieval corpora:

  1. deals  — BM25 over data/deals.json (semantic-ish deal search; replaces
              the C++ DealRetriever substring match when this service is used).
  2. kb     — BM25 over data/knowledge.json (FAQ / merchant policy / dish info
              passages used to ground the LLM reply).

Chinese text is tokenized with character bigrams (no jieba/segmenter needed);
ASCII runs are split on whitespace. BM25 (k1=1.5, b=0.75) over each corpus.

Designed to mirror llm_gateway/main.py (ThreadingHTTPServer, env-file loader).
The C++ control plane calls it only when RETRIEVAL_SERVICE_URL is set; with the
URL empty the C++ side falls back to its local substring retriever and no KB.

Config (env or retrieval_service/.env.local; see .env.example):
    RETRIEVAL_PORT   default 8001
    DEALS_PATH       default ../data/deals.json  (relative to this file)
    KB_PATH          default ../data/knowledge.json

Run:
    python retrieval_service/main.py

Test:
    curl http://localhost:8001/v1/health
    curl -X POST http://localhost:8001/v1/retrieve/deals \
      -H "Content-Type: application/json" \
      -d '{"query":"小龙虾","city":"武汉","top_k":5}'
"""

import json
import math
import os
import re
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

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


def deal_text(deal):
    parts = [deal.get("title", ""), deal.get("category", ""), deal.get("description", "")]
    parts += deal.get("tags", []) if isinstance(deal.get("tags"), list) else []
    return " ".join(str(p) for p in parts)


class Corpus:
    def __init__(self, docs, index):
        self.docs = docs          # raw list
        self.index = index        # BM25Index


def load_deal_corpus(path):
    root = _load_json(path)
    deals = root.get("deals", root if isinstance(root, list) else [])
    docs = [tokenize(deal_text(d)) for d in deals]
    return Corpus(deals, BM25Index().build(docs))


def load_kb_corpus(path):
    root = _load_json(path)
    items = root.get("passages", root if isinstance(root, list) else [])
    docs = [tokenize((p.get("title", "") + " " + p.get("content", "") + " " +
                      " ".join(p.get("tags", []) if isinstance(p.get("tags"), list) else [])))
            for p in items]
    return Corpus(items, BM25Index().build(docs))


DEAL_CORPUS = load_deal_corpus(DEALS_PATH)
KB_CORPUS = load_kb_corpus(KB_PATH)


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


def retrieve_deals(body):
    query_str = body.get("query", "") or ""
    top_k = int(body.get("top_k", 20))
    candidates = filter_deals(DEAL_CORPUS.docs, body)

    if query_str.strip():
        qtok = tokenize(query_str)
        cand_ids = [i for i, _ in candidates]
        ranked = DEAL_CORPUS.index.search(qtok, candidate_ids=cand_ids, top_k=top_k)
        if not ranked:
            # No text match among survivors -> fall back to popularity order.
            ranked = []
        items = []
        for doc_idx, score in ranked:
            d = dict(DEAL_CORPUS.docs[doc_idx])
            d["score"] = score
            items.append(d)
    else:
        # No query: rank by rating desc (mirrors C++ no-keyword behaviour).
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
            self._send_json(200, {
                "status": "ok",
                "corpora": ["deals", "kb"],
                "deal_count": len(DEAL_CORPUS.docs),
                "kb_count": len(KB_CORPUS.docs),
                "tokenizer": "char-bigram + ascii-word (BM25)",
            })
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
    print(f"[Retrieval] Running on http://0.0.0.0:{PORT}")
    print(f"[Retrieval] deals={DEALS_PATH} ({len(DEAL_CORPUS.docs)})  kb={KB_PATH} ({len(KB_CORPUS.docs)})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[Retrieval] Shutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
