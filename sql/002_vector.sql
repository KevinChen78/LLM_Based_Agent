-- Vector recall add-on (roadmap 2.3): BM25 + pgvector RRF fusion.
-- Requires pgvector built & installed into PG 17 (see sql/README.md §6).
--
-- Apply with:
--   psql -U agent -d groupbuy -f sql/002_vector.sql
-- Then generate embeddings:
--   python scripts/pg_embed.py
--
-- kb_passages deliberately stays BM25-only (22 rows, no structured filters —
-- no recall problem to solve there).

CREATE EXTENSION IF NOT EXISTS vector;

ALTER TABLE groupbuy_items
    ADD COLUMN IF NOT EXISTS embedding vector(512);   -- BAAI/bge-small-zh-v1.5

-- 5k+ rows: HNSW keeps the ANN query sub-millisecond as the catalog grows
-- (a flat seq scan would do 5320 x 512-dim cosine per request).
CREATE INDEX IF NOT EXISTS idx_items_embedding_hnsw
    ON groupbuy_items USING hnsw (embedding vector_cosine_ops);
