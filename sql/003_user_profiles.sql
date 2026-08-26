-- User profiles (Phase 2.2) — TARGET PostgreSQL DDL, for the future
-- sessions/profiles migration. NOT applied by this iteration: profiles live
-- in SQLite (data/sessions.db, user_profiles table, auto-created by
-- SqliteUserProfileStore). This file is the reference schema for that
-- migration; keep the two in sync.
--
-- Apply with (when the migration happens):
--   psql -U agent -d groupbuy -f sql/003_user_profiles.sql

CREATE TABLE IF NOT EXISTS user_profiles (
    user_id              TEXT PRIMARY KEY,
    preferred_cities     JSONB NOT NULL DEFAULT '[]',
    preferred_categories JSONB NOT NULL DEFAULT '[]',
    price_sensitivity    REAL NOT NULL DEFAULT 0.5,
    dietary_tags         JSONB NOT NULL DEFAULT '[]',
    avg_budget           REAL NOT NULL DEFAULT 0,
    updated_at           TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ranking_service reads profiles by user_id on the hot path.
-- (PK already covers point lookups; no extra index needed at this scale.)
