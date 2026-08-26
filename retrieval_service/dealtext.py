# -*- coding: utf-8 -*-
"""Shared text/vector helpers for the retrieval service and embedding job.

Extracted from retrieval_service/main.py so scripts/pg_embed.py can embed the
EXACT same text the service's BM25 corpus indexes (importing main.py would
trigger its import-time PG connection).
"""


def deal_text(deal):
    """The text a deal is indexed/embedded by: title + category + description
    + tags. Keep in sync with anything that writes embeddings."""
    parts = [deal.get("title", ""), deal.get("category", ""), deal.get("description", "")]
    parts += deal.get("tags", []) if isinstance(deal.get("tags"), list) else []
    return " ".join(str(p) for p in parts)


def vec_literal(vec):
    """Format an embedding (numpy array or sequence) as a pgvector text
    literal '[x,y,...]'. 6 decimals is plenty for cosine similarity."""
    return "[" + ",".join(f"{float(x):.6f}" for x in vec) + "]"
