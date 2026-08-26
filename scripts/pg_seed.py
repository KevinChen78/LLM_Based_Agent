#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Sync data/deals.json + data/knowledge.json into PostgreSQL.

The JSON generators (gen_wuhan_deals.py / gen_city_deals.py / gen_knowledge.py)
remain the source of truth. This script makes PostgreSQL match the files:
upsert by primary key + delete rows that no longer exist in the files, all in
one transaction, so it is idempotent and safe to re-run.

Schema must already exist (psql -f sql/001_schema.sql).

DSN resolution (same as retrieval_service/main.py):
    --dsn argument > PG_DSN env var > libpq env vars
    (PGHOST/PGPORT/PGDATABASE/PGUSER/PGPASSWORD, e.g. from
     retrieval_service/.env.local via the same setdefault loader)

Usage (from the project root):
    python scripts/pg_seed.py            # sync
    python scripts/pg_seed.py --check    # dry-run: report drift, write nothing
    python scripts/pg_seed.py --dsn "host=127.0.0.1 dbname=groupbuy user=agent password=..."

NOTE: the retrieval service builds its in-memory BM25 corpus at startup, so
restart retrieval_service after re-seeding for changes to take effect.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
DEALS_PATH = os.path.join(PROJECT, "data", "deals.json")
KB_PATH = os.path.join(PROJECT, "data", "knowledge.json")


# Match retrieval_service/main.py's loader semantics exactly: setdefault, and
# .env before .env.local (so a key in .env wins — define each key once).
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


def load_deals():
    with open(DEALS_PATH, encoding="utf-8") as fh:
        root = json.load(fh)
    return root.get("deals", root if isinstance(root, list) else [])


def load_kb():
    with open(KB_PATH, encoding="utf-8") as fh:
        root = json.load(fh)
    return root.get("passages", root if isinstance(root, list) else [])


ITEM_UPSERT = """
INSERT INTO groupbuy_items (item_id, merchant_id, title, category, city,
    district, price, original_price, sold_count, rating,
    min_people, max_people, tags, description)
VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
ON CONFLICT (item_id) DO UPDATE SET
    merchant_id=EXCLUDED.merchant_id, title=EXCLUDED.title,
    category=EXCLUDED.category, city=EXCLUDED.city, district=EXCLUDED.district,
    price=EXCLUDED.price, original_price=EXCLUDED.original_price,
    sold_count=EXCLUDED.sold_count, rating=EXCLUDED.rating,
    min_people=EXCLUDED.min_people, max_people=EXCLUDED.max_people,
    tags=EXCLUDED.tags, description=EXCLUDED.description, updated_at=NOW()
"""

MERCHANT_UPSERT = """
INSERT INTO merchants (merchant_id, name, city) VALUES (%s, %s, %s)
ON CONFLICT (merchant_id) DO UPDATE SET city = EXCLUDED.city
"""

KB_UPSERT = """
INSERT INTO kb_passages (id, category, title, content, source, tags)
VALUES (%s,%s,%s,%s,%s,%s)
ON CONFLICT (id) DO UPDATE SET category=EXCLUDED.category,
    title=EXCLUDED.title, content=EXCLUDED.content,
    source=EXCLUDED.source, tags=EXCLUDED.tags
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dsn", default=os.environ.get("PG_DSN", ""),
                    help="libpq connection string; empty => PGHOST/PGPORT/... env vars")
    ap.add_argument("--check", action="store_true",
                    help="dry-run: report what would change, write nothing")
    args = ap.parse_args()

    try:
        import psycopg
        from psycopg.types.json import Jsonb
    except ImportError:
        sys.stderr.write(
            "error: psycopg not installed. Run: pip install -r requirements.txt\n")
        return 2

    deals = load_deals()
    kb = load_kb()
    deal_ids = [d["item_id"] for d in deals]
    kb_ids = [p["id"] for p in kb]

    # Merchants are derived from deals (1:1 in the seed data; the JSON has no
    # merchant names, so merchant_id doubles as a placeholder name until a
    # real merchant catalog exists).
    merchants = {}
    for d in deals:
        mid = d["merchant_id"]
        if mid not in merchants:
            merchants[mid] = (mid, mid, d["city"])  # (id, placeholder name, city)

    with psycopg.connect(args.dsn) as conn:

        def counts():
            cur = conn.execute(
                "SELECT (SELECT count(*) FROM groupbuy_items),"
                "       (SELECT count(*) FROM merchants),"
                "       (SELECT count(*) FROM kb_passages)")
            return cur.fetchone()

        if args.check:
            items_db, merch_db, kb_db = counts()
            stale_items = conn.execute(
                "SELECT count(*) FROM groupbuy_items WHERE item_id <> ALL(%s)",
                (deal_ids,)).fetchone()[0]
            stale_kb = conn.execute(
                "SELECT count(*) FROM kb_passages WHERE id <> ALL(%s)",
                (kb_ids,)).fetchone()[0]
            print(f"DB now:      items={items_db} merchants={merch_db} kb={kb_db}")
            print(f"JSON has:    items={len(deals)} merchants={len(merchants)} kb={len(kb)}")
            print(f"stale rows:  items={stale_items} kb={stale_kb}")
            print("(dry-run; nothing written)")
            return 0

        with conn.transaction():
            # 1. Remove stale items first so merchant deletes can't hit the FK.
            del_items = conn.execute(
                "DELETE FROM groupbuy_items WHERE item_id <> ALL(%s)",
                (deal_ids,)).rowcount

            # 2-3. Upsert merchants (FK parents) then items.
            with conn.cursor() as c:
                c.executemany(MERCHANT_UPSERT, list(merchants.values()))
                c.executemany(ITEM_UPSERT, [
                    (d["item_id"], d["merchant_id"], d["title"], d["category"],
                     d["city"], d.get("district"), d["price"],
                     d.get("original_price"), d.get("sold_count", 0),
                     d.get("rating", 5.0), d.get("min_people", 0),
                     d.get("max_people", 0), Jsonb(d.get("tags", [])),
                     d.get("description", ""))
                    for d in deals])

            # 4. Merchants with no remaining items go away.
            del_merch = conn.execute(
                "DELETE FROM merchants m WHERE NOT EXISTS"
                " (SELECT 1 FROM groupbuy_items i WHERE i.merchant_id = m.merchant_id)"
            ).rowcount

            # 5. KB passages: same upsert + stale delete.
            del_kb = conn.execute(
                "DELETE FROM kb_passages WHERE id <> ALL(%s)", (kb_ids,)).rowcount
            with conn.cursor() as c:
                c.executemany(KB_UPSERT, [
                    (p["id"], p.get("category", ""), p.get("title", ""),
                     p.get("content", ""), p.get("source"),
                     Jsonb(p.get("tags", []))) for p in kb])

        items_db, merch_db, kb_db = counts()
        print(f"deals:     {len(deals)} synced, {del_items} stale removed (db={items_db})")
        print(f"merchants: {len(merchants)} synced, {del_merch} stale removed (db={merch_db})"
              "  [name = merchant_id placeholder until a real merchant catalog exists]")
        print(f"kb:        {len(kb)} synced, {del_kb} stale removed (db={kb_db})")
        print("NOTE: restart retrieval_service to rebuild its in-memory BM25 corpus.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
