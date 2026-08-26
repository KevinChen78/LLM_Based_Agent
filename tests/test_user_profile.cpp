#include "agent/preference_extractor.hpp"
#include "agent/deal_catalog.hpp"
#include "agent/prompt_builder.hpp"
#include "agent/sqlite_session_store.hpp"
#include "agent/user_profile_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace agent;
using json = nlohmann::json;

namespace {

std::string TempPath(const std::string& name) {
    static int n = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("llm_agent_profile_" + name + "_" + std::to_string(++n));
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.string();
}

// A tiny catalog: hotpot in Wuhan (deep discount), BBQ in Shanghai (no
// discount). Extraction assertions key off these.
std::string WriteCatalog() {
    auto path = TempPath("catalog.json");
    std::ofstream f(path);
    f << R"({"deals":[
        {"item_id":"i-hot","title":"火锅","category":"火锅","city":"武汉",
         "price":100.0,"original_price":400.0,"sold_count":100,"rating":4.5},
        {"item_id":"i-bbq","title":"烧烤","category":"烧烤","city":"上海",
         "price":300.0,"original_price":300.0,"sold_count":50,"rating":4.0}
    ]})";
    return path;
}

} // namespace

TEST(UserProfileStore, RoundTrip) {
    SqliteUserProfileStore store(TempPath("rt.db"));

    EXPECT_FALSE(store.Get("ghost").has_value());

    UserProfile p;
    p.user_id = "u1";
    p.preferred_cities = {"武汉", "上海"};
    p.preferred_categories = {"火锅"};
    p.price_sensitivity = 0.75;
    p.dietary_tags = {"不吃辣"};
    p.avg_budget = 250.0;
    ASSERT_TRUE(store.Upsert(p));

    auto got = store.Get("u1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->preferred_cities, p.preferred_cities);
    EXPECT_EQ(got->preferred_categories, p.preferred_categories);
    EXPECT_DOUBLE_EQ(got->price_sensitivity, 0.75);
    EXPECT_EQ(got->dietary_tags, p.dietary_tags);
    EXPECT_DOUBLE_EQ(got->avg_budget, 250.0);
    EXPECT_FALSE(got->updated_at.empty());

    // Upsert overwrites.
    p.avg_budget = 500.0;
    ASSERT_TRUE(store.Upsert(p));
    EXPECT_DOUBLE_EQ(store.Get("u1")->avg_budget, 500.0);
}

TEST(UserProfileStore, LoadsSignalsAcrossTables) {
    auto db = TempPath("sig.db");
    // The session store owns the sessions/feedback schema; the profile store
    // joins against them on the same file.
    SqliteSessionStore sessions(db);
    UserContext ctx;
    ctx.user_id = "u1";
    auto s = sessions.GetOrCreateSession(std::nullopt, ctx).result();
    sessions.UpdateContext(s.session_id, "SLOT_FILL",
                           json{{"budget", 300}, {"taboo", "不吃辣"}}).result();
    SessionMemoryStore::FeedbackRecord rec;
    rec.session_id = s.session_id;
    rec.trace_id = "t-1";
    rec.item_id = "i-hot";
    rec.feedback_type = "like";
    ASSERT_TRUE(sessions.AppendFeedback(rec).result().ok());

    SqliteUserProfileStore store(db);
    auto signals = store.LoadFeedbackSignals("u1");
    ASSERT_EQ(signals.size(), 1u);
    EXPECT_EQ(signals[0].item_id, "i-hot");
    EXPECT_EQ(signals[0].feedback_type, "like");
    auto slots = store.LoadSlotHistory("u1");
    ASSERT_EQ(slots.size(), 1u);
    EXPECT_DOUBLE_EQ(slots[0].budget, 300.0);
    EXPECT_EQ(slots[0].taboo, "不吃辣");

    // Other users see nothing.
    EXPECT_TRUE(store.LoadFeedbackSignals("u2").empty());
    EXPECT_TRUE(store.LoadSlotHistory("u2").empty());
}

TEST(PreferenceExtractor, AggregatesFeedbackAndSlots) {
    DealCatalog catalog(WriteCatalog());
    ASSERT_TRUE(catalog.Loaded());

    std::vector<FeedbackSignal> feedback = {
        {"i-hot", "like"}, {"i-hot", "like"}, {"i-bbq", "dislike"},
    };
    std::vector<SlotRecord> slots = {{200.0, "不吃辣"}, {400.0, "海鲜过敏"}};

    auto p = PreferenceExtractor::Extract("u1", feedback, slots, catalog);
    EXPECT_EQ(p.user_id, "u1");
    ASSERT_FALSE(p.preferred_categories.empty());
    EXPECT_EQ(p.preferred_categories[0], "火锅");   // 2 likes vs bbq's -1
    EXPECT_EQ(p.preferred_cities[0], "武汉");
    EXPECT_DOUBLE_EQ(p.avg_budget, 300.0);
    EXPECT_EQ(p.dietary_tags.size(), 2u);           // deduped
    // i-hot has discount 0.75 > 0.3 and both likes are on it.
    EXPECT_DOUBLE_EQ(p.price_sensitivity, 1.0);
}

TEST(PreferenceExtractor, EmptySignalsYieldDefaults) {
    DealCatalog catalog(WriteCatalog());
    auto p = PreferenceExtractor::Extract("u9", {}, {}, catalog);
    EXPECT_TRUE(p.preferred_categories.empty());
    EXPECT_TRUE(p.preferred_cities.empty());
    EXPECT_TRUE(p.dietary_tags.empty());
    EXPECT_DOUBLE_EQ(p.avg_budget, 0.0);
    EXPECT_DOUBLE_EQ(p.price_sensitivity, 0.5);
}

TEST(PreferenceExtractor, DislikedCategoryDropsOut) {
    DealCatalog catalog(WriteCatalog());
    std::vector<FeedbackSignal> feedback = {{"i-bbq", "dislike"}};
    auto p = PreferenceExtractor::Extract("u1", feedback, {}, catalog);
    // A purely negative score must not become a "preference".
    EXPECT_TRUE(p.preferred_categories.empty());
}

TEST(UserProfilePrompt, EmptyProfileLeavesPromptUntouched) {
    const std::string bare = PromptBuilder::TaskPlanningPrompt("h", "想吃火锅", "{}");
    EXPECT_EQ(bare.find("用户画像"), std::string::npos);
    // Explicit empty object behaves the same as no profile at all.
    const std::string with_empty =
        PromptBuilder::TaskPlanningPrompt("h", "想吃火锅", "{}", "{}");
    EXPECT_EQ(with_empty, bare);
}

TEST(UserProfilePrompt, ProfileInjectsSectionWithPriorityRule) {
    const std::string prompt = PromptBuilder::TaskPlanningPrompt(
        "h", "想吃火锅", "{}",
        R"({"preferred_cities":["武汉"],"preferred_categories":["火锅"],"avg_budget":300})");
    EXPECT_NE(prompt.find("用户画像"), std::string::npos);
    EXPECT_NE(prompt.find("当轮"), std::string::npos);   // 当轮输入优先规则
    EXPECT_NE(prompt.find("火锅"), std::string::npos);
}
