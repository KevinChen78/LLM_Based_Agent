// DealCatalog PostgreSQL backend tests.
//
// The deterministic cases deliberately use a dead DSN (port 1) so they
// exercise the fallback chain WITHOUT requiring a running PostgreSQL — they
// pass identically in builds with ENABLE_PG_CATALOG=OFF. The live-PG case is
// gated on PG_TEST_DSN and skips otherwise.
#include <gtest/gtest.h>

#include <cstdlib>

#include "agent/deal_catalog.hpp"

namespace {

constexpr const char* kDeadDsn =
    "host=127.0.0.1 port=1 dbname=groupbuy connect_timeout=1";

TEST(DealCatalogPg, BadDsnFallsBackToJsonFile) {
    agent::DealCatalog c("data/deals.json", kDeadDsn);
    EXPECT_TRUE(c.Loaded());
    EXPECT_EQ(c.Size(), 9713u);   // same data-file convention as test_deals_tools.cpp
    EXPECT_EQ(c.Source(), "file:data/deals.json");
    EXPECT_EQ(c.Deals()[0]["item_id"], "gb-20001");
}

TEST(DealCatalogPg, BadDsnAndBadFileFallsBackToBuiltIn) {
    agent::DealCatalog c("", kDeadDsn);
    EXPECT_TRUE(c.Loaded());
    EXPECT_EQ(c.Size(), 8u);
    EXPECT_EQ(c.Source(), "builtin");
}

TEST(DealCatalogPg, EmptyDsnBehavesExactlyAsBefore) {
    agent::DealCatalog a("data/deals.json");      // old 1-arg form still compiles
    agent::DealCatalog b("data/deals.json", "");
    EXPECT_EQ(a.Size(), b.Size());
    EXPECT_EQ(a.Deals(), b.Deals());
}

TEST(DealCatalogPg, LivePostgresMatchesFile) {
    const char* dsn = std::getenv("PG_TEST_DSN");
    if (!dsn || !*dsn) GTEST_SKIP() << "PG_TEST_DSN not set";
    agent::DealCatalog pg("nonexistent.json", dsn);
    ASSERT_TRUE(pg.Loaded());
    EXPECT_EQ(pg.Source(), "postgres");
    agent::DealCatalog file("data/deals.json");
    ASSERT_EQ(pg.Size(), file.Size());
    ASSERT_EQ(pg.Size(), 9713u);
    auto pg_deals = pg.Deals();
    auto file_deals = file.Deals();
    for (size_t i = 0; i < pg_deals.size(); ++i) {
        // Exact equality on identity/text fields; tolerance on DECIMAL->text->
        // strtod conversions to absorb last-ULP differences vs JSON doubles.
        const auto& p = pg_deals[i];
        const auto& f = file_deals[i];
        EXPECT_EQ(p["item_id"], f["item_id"]) << i;
        EXPECT_EQ(p["merchant_id"], f["merchant_id"]) << i;
        EXPECT_EQ(p["title"], f["title"]) << i;
        EXPECT_EQ(p["category"], f["category"]) << i;
        EXPECT_EQ(p["city"], f["city"]) << i;
        EXPECT_EQ(p["district"], f["district"]) << i;
        EXPECT_NEAR(p["price"].get<double>(), f["price"].get<double>(), 1e-9) << i;
        EXPECT_NEAR(p["original_price"].get<double>(),
                    f["original_price"].get<double>(), 1e-9) << i;
        EXPECT_EQ(p["sold_count"], f["sold_count"]) << i;
        EXPECT_NEAR(p["rating"].get<double>(), f["rating"].get<double>(), 1e-9) << i;
        EXPECT_EQ(p["min_people"], f["min_people"]) << i;
        EXPECT_EQ(p["max_people"], f["max_people"]) << i;
        EXPECT_EQ(p["tags"], f["tags"]) << i;
        EXPECT_EQ(p["description"], f["description"]) << i;
    }
}

} // namespace
