-- Retrieval schema for the group-buying agent (deals + knowledge base).
--
-- Aligned with 项目规划_修订版.md §5.1 (groupbuy_items / merchants), with two
-- deliberate divergences for this iteration:
--   1. embedding VECTOR(768) omitted — pgvector is deferred to the vector-
--      recall phase; this iteration keeps BM25 ranking in Python.
--   2. min_people / max_people / tags / description are first-class columns
--      (修订版 puts them in attributes JSONB) so structured filters can be
--      pushed down to SQL and use plain B-tree indexes.
--
-- Apply with:
--   psql -U agent -d groupbuy -f sql/001_schema.sql

CREATE TABLE IF NOT EXISTS merchants (
    merchant_id    VARCHAR(64) PRIMARY KEY,
    name           VARCHAR(256) NOT NULL,
    city           VARCHAR(32)  NOT NULL,
    address        VARCHAR(512),
    phone          VARCHAR(32),
    business_hours JSONB,
    latitude       DECIMAL(10,7),
    longitude      DECIMAL(10,7),
    status         SMALLINT NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS groupbuy_items (
    item_id        VARCHAR(64) PRIMARY KEY,
    merchant_id    VARCHAR(64) NOT NULL REFERENCES merchants(merchant_id),
    title          VARCHAR(256) NOT NULL,
    category       VARCHAR(64)  NOT NULL,
    city           VARCHAR(32)  NOT NULL,
    district       VARCHAR(64),
    price          DECIMAL(10,2) NOT NULL,
    original_price DECIMAL(10,2),
    sold_count     INT  NOT NULL DEFAULT 0,
    rating         DECIMAL(3,1) NOT NULL DEFAULT 5.0,
    min_people     INT  NOT NULL DEFAULT 0,   -- 0/0 => no range (keep-any semantics)
    max_people     INT  NOT NULL DEFAULT 0,
    tags           JSONB NOT NULL DEFAULT '[]',
    description    TEXT NOT NULL DEFAULT '',
    valid_start    DATE,
    valid_end      DATE,
    status         SMALLINT NOT NULL DEFAULT 1,   -- 0 下架 1 上架; unused this iteration
    attributes     JSONB NOT NULL DEFAULT '{}',
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Mirrors 修订版 idx_items_city_category_price; district/price get their own
-- indexes because requests filter district without category, and price alone.
CREATE INDEX IF NOT EXISTS idx_items_city_category_price ON groupbuy_items(city, category, price);
CREATE INDEX IF NOT EXISTS idx_items_district ON groupbuy_items(district);
CREATE INDEX IF NOT EXISTS idx_items_price ON groupbuy_items(price);

CREATE TABLE IF NOT EXISTS kb_passages (
    id       VARCHAR(32) PRIMARY KEY,        -- 'kb-NNN'
    category VARCHAR(32)  NOT NULL,          -- faq / policy / dish
    title    VARCHAR(256) NOT NULL,
    content  TEXT         NOT NULL,
    source   VARCHAR(256),
    tags     JSONB NOT NULL DEFAULT '[]'
);
