#include "agent/deal_catalog.hpp"
#include "agent/deal_tools.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace agent;
using json = nlohmann::json;

namespace {

ToolCall MakeCall(const std::string& name, const json& args) {
    ToolCall c;
    c.tool_name = name;
    c.call_id = "tc-test";
    c.arguments_json = args.dump();
    return c;
}

json RunResult(const std::string& name, const json& args, std::shared_ptr<DealCatalog> catalog = nullptr) {
    std::shared_ptr<ITool> tool;
    if (name == "deal_retriever") {
        tool = std::make_shared<DealRetriever>(catalog ? catalog : std::make_shared<DealCatalog>(""));
    } else {
        tool = std::make_shared<DealRanker>();
    }
    auto result = tool->Execute(MakeCall(name, args)).result();
    EXPECT_TRUE(result.success) << result.error_message;
    return json::parse(result.result_json);
}

} // namespace

// ---------------------------------------------------------------------------
// DealCatalog
// ---------------------------------------------------------------------------

TEST(DealCatalog, FallsBackToBuiltInWhenFileMissing) {
    DealCatalog catalog("");   // no path -> built-in fallback
    EXPECT_TRUE(catalog.Loaded());
    EXPECT_GT(catalog.Size(), 0u);
    json deals = catalog.Deals();
    EXPECT_TRUE(deals.is_array());
    // Built-in set contains Shanghai seafood deals.
    bool has_shanghai = false;
    for (const auto& d : deals) {
        if (d.value("city", "") == "上海") has_shanghai = true;
    }
    EXPECT_TRUE(has_shanghai);
}

TEST(DealCatalog, LoadsFromFile) {
    DealCatalog catalog("data/deals.json");
    EXPECT_TRUE(catalog.Loaded());
    EXPECT_GE(catalog.Size(), 20u);
}

// ---------------------------------------------------------------------------
// DealRetriever
// ---------------------------------------------------------------------------

TEST(DealRetriever, FiltersByCity) {
    auto out = RunResult("deal_retriever", {
        {"city", "上海"}, {"category", ""}, {"top_k", 50}
    });
    for (const auto& it : out["items"]) {
        EXPECT_EQ(it["city"], "上海");
    }
}

TEST(DealRetriever, FiltersByMaxPrice) {
    auto out = RunResult("deal_retriever", {
        {"city", "上海"}, {"max_price", 250.0}, {"top_k", 50}
    });
    for (const auto& it : out["items"]) {
        EXPECT_LE(it["price"].get<double>(), 250.0);
    }
}

TEST(DealRetriever, FiltersByPeopleRange) {
    // Built-in fallback has gb-20001 (min_people=3,max_people=4).
    // Asking for 2 people should exclude it.
    auto out = RunResult("deal_retriever", {
        {"city", "上海"}, {"category", "海鲜"}, {"people", 2}, {"top_k", 50}
    });
    for (const auto& it : out["items"]) {
        EXPECT_NE(it["item_id"], "gb-20001");
    }
}

TEST(DealRetriever, RespectsTopK) {
    auto out = RunResult("deal_retriever", {
        {"city", ""}, {"top_k", 3}
    });
    EXPECT_LE(out["items"].size(), 3u);
}

TEST(DealRetriever, KeywordBoostsMatch) {
    // Among Shanghai seafood, only gb-20003 contains "龙虾".
    auto out = RunResult("deal_retriever", {
        {"city", "上海"}, {"category", "海鲜"}, {"keywords", "龙虾"}, {"top_k", 50}
    });
    ASSERT_FALSE(out["items"].empty());
    EXPECT_EQ(out["items"][0]["item_id"], "gb-20003");
    EXPECT_GT(out["items"][0]["score"].get<double>(), 0.0);
}

TEST(DealRetriever, StampsReasonAndScore) {
    auto out = RunResult("deal_retriever", {
        {"city", "上海"}, {"top_k", 5}
    });
    for (const auto& it : out["items"]) {
        EXPECT_TRUE(it.contains("score"));
        EXPECT_TRUE(it.contains("reason"));
        EXPECT_FALSE(it["reason"].get<std::string>().empty());
    }
}

// Loads the full data/deals.json (which now contains 100 Wuhan records) and
// confirms the real retriever filters Wuhan correctly across city / category /
// price constraints.
TEST(DealRetriever, FiltersWuhanFromCatalog) {
    auto catalog = std::make_shared<DealCatalog>("data/deals.json");
    ASSERT_GE(catalog->Size(), 120u);
    DealRetriever retriever(catalog);

    auto run = [&](const json& args) {
        ToolCall c;
        c.tool_name = "deal_retriever";
        c.call_id = "tc-wh";
        c.arguments_json = args.dump();
        auto r = retriever.Execute(c).result();
        EXPECT_TRUE(r.success) << r.error_message;
        return json::parse(r.result_json);
    };

    // All 100 Wuhan deals.
    auto all = run({{"city", "武汉"}, {"top_k", 200}});
    EXPECT_EQ(all["items"].size(), 100u);
    for (const auto& it : all["items"]) {
        EXPECT_EQ(it["city"], "武汉");
    }

    // Category filter: 小龙虾 (data has 15 Wuhan crawfish deals).
    auto xlk = run({{"city", "武汉"}, {"category", "小龙虾"}, {"top_k", 200}});
    EXPECT_EQ(xlk["items"].size(), 15u);

    // Price filter: price <= 200 (24 such Wuhan deals per the dataset).
    auto cheap = run({{"city", "武汉"}, {"max_price", 200.0}, {"top_k", 200}});
    EXPECT_EQ(cheap["items"].size(), 24u);
    for (const auto& it : cheap["items"]) {
        EXPECT_LE(it["price"].get<double>(), 200.0);
    }

    // id range stays within the generated Wuhan block.
    auto ids = all["items"][0]["item_id"].get<std::string>();
    EXPECT_GE(ids, "gb-20021");
}

// ---------------------------------------------------------------------------
// DealRanker
// ---------------------------------------------------------------------------

TEST(DealRanker, ReordersByMultiFactorAndTopN) {
    json candidates = json::array({
        {{"item_id","a"},{"title","A"},{"price",150.0},{"original_price",300.0},
         {"sold_count",100},{"rating",4.0}},
        {{"item_id","b"},{"title","B"},{"price",280.0},{"original_price",300.0},
         {"sold_count",1000},{"rating",4.8}},
        {{"item_id","c"},{"title","C"},{"price",120.0},{"original_price",300.0},
         {"sold_count",50},{"rating",3.5}}
    });
    auto out = RunResult("deal_ranker", {
        {"candidates", candidates}, {"budget", 300.0}, {"top_n", 2}
    });
    EXPECT_EQ(out["items"].size(), 2u);
    // The strong, popular, well-discounted deal "b" should win.
    EXPECT_EQ(out["items"][0]["item_id"], "b");
    // Scores are sorted descending.
    EXPECT_GE(out["items"][0]["score"].get<double>(), out["items"][1]["score"].get<double>());
}

TEST(DealRanker, TabooFilters) {
    json candidates = json::array({
        {{"item_id","spicy"},{"title","水煮鱼"},{"category","川菜"},
         {"tags",{"辣"}},{"price",200.0},{"original_price",300.0},
         {"sold_count",500},{"rating",4.5}},
        {{"item_id","mild"},{"title","清蒸鲈鱼"},{"category","海鲜"},
         {"tags",{"清淡"}},{"price",200.0},{"original_price",300.0},
         {"sold_count",500},{"rating",4.5}}
    });
    auto out = RunResult("deal_ranker", {
        {"candidates", candidates}, {"taboo", "辣"}, {"top_n", 5}
    });
    ASSERT_EQ(out["items"].size(), 1u);
    EXPECT_EQ(out["items"][0]["item_id"], "mild");
}

TEST(DealRanker, BudgetPenalizesOverBudget) {
    json candidates = json::array({
        // Same rating/sold/popularity, only price differs.
        {{"item_id","over"},{"title","over"},{"price",458.0},{"original_price",600.0},
         {"sold_count",500},{"rating",4.5}},
        {{"item_id","under"},{"title","under"},{"price",200.0},{"original_price",600.0},
         {"sold_count",500},{"rating",4.5}}
    });
    auto out = RunResult("deal_ranker", {
        {"candidates", candidates}, {"budget", 300.0}, {"top_n", 2}
    });
    ASSERT_EQ(out["items"].size(), 2u);
    // Under-budget deal should rank higher thanks to better price_fit.
    EXPECT_EQ(out["items"][0]["item_id"], "under");
}

TEST(DealRanker, EmptyCandidatesYieldsEmpty) {
    auto out = RunResult("deal_ranker", {
        {"candidates", json::array()}, {"top_n", 3}
    });
    EXPECT_TRUE(out["items"].empty());
}
