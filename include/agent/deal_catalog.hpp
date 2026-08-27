#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

// In-memory catalog of group-buying deals loaded from a JSON file.
// Thread-safe for read access — DealRetriever / DealRanker are invoked from
// orchestrator threads and may read concurrently.
//
// If the configured file is missing or unreadable, the catalog falls back to a
// built-in default dataset so the agent always has data to serve (important for
// unit tests and offline runs where data/deals.json is absent).
//
// JSON schema (data/deals.json):
// {
//   "version": 1,
//   "deals": [
//     {
//       "item_id": "gb-20001", "merchant_id": "m-30001",
//       "title": "...", "category": "海鲜", "city": "上海", "district": "黄浦",
//       "price": 288.0, "original_price": 598.0,
//       "sold_count": 1200, "rating": 4.7,
//       "min_people": 3, "max_people": 4,
//       "tags": ["大虾", "生蚝"], "description": "..."
//     }
//   ]
// }
class DealCatalog {
public:
    // pg_dsn empty  => JSON file only (historical behaviour).
    // pg_dsn set    => try PostgreSQL (groupbuy_items) first; on any failure
    //                  fall back to the JSON file, then the built-in dataset.
    explicit DealCatalog(const std::string& json_path,
                         const std::string& pg_dsn = "");

    // Returns a deep copy of all deals as a JSON array.
    nlohmann::json Deals() const;

    bool Loaded() const { return loaded_; }
    size_t Size() const;

    // "postgres" | "file:<path>" | "builtin" — which source won, for logging.
    const std::string& Source() const { return source_; }

    // Distinct non-empty `category` values across the loaded catalog, sorted
    // (deterministic). Works for all three sources — they all populate deals_.
    // Used to ground the planner's category slot in reality (Phase 3-A).
    std::vector<std::string> DistinctCategories() const;

private:
    void LoadFromFile(const std::string& path);
    bool LoadFromPostgres(const std::string& dsn);   // false => caller falls through
    static nlohmann::json BuiltInFallback();

    mutable std::mutex mutex_;
    nlohmann::json deals_;   // always a JSON array
    bool loaded_ = false;
    std::string source_;
};

} // namespace agent
