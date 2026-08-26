#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate pgvector embeddings for groupbuy_items (BAAI/bge-small-zh-v1.5).

Embeds the same deal_text() the retrieval service indexes (shared via
retrieval_service/dealtext.py) and writes the 512-dim vectors into
groupbuy_items.embedding (sql/002_vector.sql must be applied first).

Default: embed only rows where embedding IS NULL (incremental after reseed).
--force: re-embed everything. Idempotent; single transaction.

In China, if the Hugging Face download stalls:
    set HF_ENDPOINT=https://hf-mirror.com

Usage (from the project root):
    python scripts/pg_embed.py [--force] [--check] [--dsn "..."]

NOTE: restart retrieval_service afterwards — it probes embeddings at startup.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(PROJECT, "retrieval_service"))

from dealtext import deal_text, vec_literal  # noqa: E402

MODEL_NAME = "BAAI/bge-small-zh-v1.5"
BATCH = 64


# Same setdefault loader semantics as retrieval_service/main.py.
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


RETR_DIR = os.path.join(PROJECT, "retrieval_service")
_load_env_file(os.path.join(RETR_DIR, ".env"))
_load_env_file(os.path.join(RETR_DIR, ".env.local"))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dsn", default=os.environ.get("PG_DSN", ""),
                    help="libpq connection string; empty => PGHOST/PGPORT/... env vars")
    ap.add_argument("--force", action="store_true",
                    help="re-embed all rows, not just embedding IS NULL")
    ap.add_argument("--check", action="store_true",
                    help="dry-run: report how many rows need embeddings")
    args = ap.parse_args()

    try:
        import psycopg
    except ImportError:
        sys.stderr.write(
            "error: psycopg not installed. Run: pip install -r requirements.txt\n")
        return 2
    try:
        from fastembed import TextEmbedding
    except ImportError:
        sys.stderr.write(
            "error: fastembed not installed. Run: pip install -r requirements.txt\n")
        return 2

    with psycopg.connect(args.dsn, row_factory=psycopg.rows.dict_row) as conn:
        try:
            rows = conn.execute(
                "SELECT item_id, title, category, description, tags"
                " FROM groupbuy_items"
                + ("" if args.force else " WHERE embedding IS NULL")
                + " ORDER BY item_id").fetchall()
        except Exception as e:
            sys.stderr.write(
                f"error: {e}\n(hint: apply sql/002_vector.sql first)\n")
            return 2

        if args.check:
            total = conn.execute("SELECT count(*) AS c FROM groupbuy_items").fetchone()["c"]
            print(f"{len(rows)}/{total} rows need embeddings"
                  + (" (--force ignores this filter)" if args.force else ""))
            return 0

        if not rows:
            print("nothing to embed (all rows have embeddings; use --force to rebuild)")
            return 0

        print(f"loading {MODEL_NAME} ...")
        model = TextEmbedding(model_name=MODEL_NAME)
        texts = [deal_text(r) for r in rows]
        done = 0
        with conn.transaction():
            for i in range(0, len(rows), BATCH):
                vecs = list(model.embed(texts[i:i + BATCH]))
                with conn.cursor() as c:
                    c.executemany(
                        "UPDATE groupbuy_items SET embedding = %s::vector"
                        " WHERE item_id = %s",
                        [(vec_literal(v), r["item_id"])
                         for r, v in zip(rows[i:i + BATCH], vecs)])
                done += len(vecs)
                print(f"  embedded {done}/{len(rows)}")
        print(f"embedded {len(rows)} items ({MODEL_NAME}, 512-dim)")
        print("NOTE: restart retrieval_service to enable the vector channel.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
