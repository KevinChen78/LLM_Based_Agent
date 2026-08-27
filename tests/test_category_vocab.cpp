// Phase 3-A: category-vocabulary grounding — DealCatalog::DistinctCategories
// and the planner prompt's category whitelist section.

#include "agent/deal_catalog.hpp"
#include "agent/prompt_builder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using namespace agent;

TEST(CategoryVocab, DistinctCategoriesSortedAndUnique) {
    // Built-in fallback covers 海鲜/火锅/烧烤/川菜/中餐 (data/deals.json path
    // may or may not exist in the test environment — use a bogus path to pin
    // the builtin dataset deterministically).
    DealCatalog catalog("definitely/not/a/real/path.json");
    ASSERT_TRUE(catalog.Loaded());
    const auto cats = catalog.DistinctCategories();
    ASSERT_EQ(cats.size(), 5u);
    // Sorted (std::set byte order over UTF-8) and unique.
    for (size_t i = 1; i < cats.size(); ++i) {
        EXPECT_LT(cats[i - 1], cats[i]);
    }
    for (const char* expect : {"中餐", "川菜", "烧烤", "火锅", "海鲜"}) {
        EXPECT_NE(std::find(cats.begin(), cats.end(), expect), cats.end())
            << "missing category: " << expect;
    }
}

TEST(CategoryVocabPrompt, NoListLeavesPromptUntouched) {
    const std::string bare =
        PromptBuilder::TaskPlanningPrompt("h", "想吃火锅", "{}");
    EXPECT_EQ(bare.find("有效类目列表"), std::string::npos);
    EXPECT_NE(bare.find("category：类目。如"), std::string::npos);
    // Explicit empty list behaves the same as no list at all.
    const std::string with_empty =
        PromptBuilder::TaskPlanningPrompt("h", "想吃火锅", "{}", "", "");
    EXPECT_EQ(with_empty, bare);
}

TEST(CategoryVocabPrompt, ListInjectsSectionAndTightensRule) {
    const std::string prompt = PromptBuilder::TaskPlanningPrompt(
        "h", "想吃早茶", "{}", "", "中餐、火锅、海鲜");
    EXPECT_NE(prompt.find("有效类目列表"), std::string::npos);
    EXPECT_NE(prompt.find("中餐、火锅、海鲜"), std::string::npos);
    // Slot rule tightened: whitelist-only + leave empty when unmatched.
    EXPECT_NE(prompt.find("只能从下方「有效类目列表」中原样取值"),
              std::string::npos);
    EXPECT_NE(prompt.find("把用户原词写进 keywords"), std::string::npos);
    // The old free-text example rule is gone.
    EXPECT_EQ(prompt.find("category：类目。如"), std::string::npos);
    // Section sits inside the prompt before 关键规则.
    const auto sec = prompt.find("有效类目列表");
    const auto rules = prompt.find("# 关键规则");
    ASSERT_NE(sec, std::string::npos);
    ASSERT_NE(rules, std::string::npos);
    EXPECT_LT(sec, rules);
}

TEST(CategoryVocabPrompt, ProfileAndCategorySectionsCoexist) {
    const std::string prompt = PromptBuilder::TaskPlanningPrompt(
        "h", "想吃火锅", "{}",
        R"({"preferred_cities":["武汉"]})",
        "中餐、火锅");
    EXPECT_NE(prompt.find("用户画像"), std::string::npos);
    EXPECT_NE(prompt.find("有效类目列表"), std::string::npos);
}
