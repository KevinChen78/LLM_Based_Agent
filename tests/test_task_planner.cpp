#include "agent/json_extract.hpp"
#include "agent/llm_client.hpp"
#include "agent/task_planner.hpp"

#include <gtest/gtest.h>

using namespace agent;

namespace {

// Fake LLM that replays canned responses in order (last one repeats), so we can
// exercise the planner's tolerant parsing and temperature-0 retry.
class SequenceLlm : public LlmClient {
public:
    coro::Task<LlmResponse> Chat(const std::vector<LlmMessage>&,
                                 const Options& opts) override {
        ++call_count;
        last_temperature = opts.temperature;
        last_max_tokens = opts.max_tokens;
        LlmResponse r;
        r.raw_text = responses[std::min(call_count, static_cast<int>(responses.size())) - 1];
        co_return r;
    }
    bool Healthy() const override { return true; }

    std::vector<std::string> responses;
    int call_count = 0;
    double last_temperature = -1.0;
    int last_max_tokens = 0;
};

const char* kRetrievePlan = R"({
    "action": "retrieve",
    "slots": {"city": "上海", "category": "海鲜"},
    "missing_slots": [],
    "clarification_question": "",
    "tool_calls": [
        {"tool_name": "deal_retriever", "arguments": {"city": "上海", "top_k": 20}}
    ]
})";

TaskPlanner::Plan RunPlan(std::shared_ptr<LlmClient> llm) {
    TaskPlanner planner(llm);
    UserContext ctx;
    nlohmann::json slots = nlohmann::json::object();
    return planner.PlanNextStep(ctx, {}, "上海三个人吃海鲜", slots).result();
}

} // namespace

TEST(TaskPlanner, SlotFillingTriggersClarify) {
    auto llm = std::make_shared<HttpLlmClient>("", "");
    TaskPlanner planner(llm);

    UserContext ctx;
    ctx.user_id = "u1";
    ctx.city = "上海";

    std::vector<ConversationTurn> history;
    nlohmann::json current_slots = nlohmann::json::object();

    auto plan = planner.PlanNextStep(ctx, history, "我想吃海鲜", current_slots).result();

    EXPECT_EQ(plan.next_state, "clarify");
    EXPECT_FALSE(plan.clarification.has_value() == false || plan.clarification->question.empty());
}

TEST(TaskPlanner, EnoughSlotsTriggersRetrieve) {
    auto llm = std::make_shared<HttpLlmClient>("", "");
    TaskPlanner planner(llm);

    UserContext ctx;
    ctx.user_id = "u1";

    std::vector<ConversationTurn> history;
    nlohmann::json current_slots = nlohmann::json::object();

    auto plan = planner.PlanNextStep(
        ctx, history, "今晚三个人吃海鲜，预算 300 左右，上海", current_slots).result();

    EXPECT_EQ(plan.next_state, "retrieve");
    EXPECT_FALSE(plan.tool_calls.empty());
    EXPECT_EQ(plan.tool_calls[0].tool_name, "deal_retriever");
}

TEST(TaskPlanner, ChitchatTriggersRespondWithDirectResponse) {
    auto llm = std::make_shared<HttpLlmClient>("", "");
    TaskPlanner planner(llm);

    UserContext ctx;
    ctx.user_id = "u1";

    std::vector<ConversationTurn> history;
    nlohmann::json current_slots = nlohmann::json::object();

    auto plan = planner.PlanNextStep(ctx, history, "谢谢", current_slots).result();

    EXPECT_EQ(plan.next_state, "respond");
    EXPECT_TRUE(plan.tool_calls.empty());
    EXPECT_FALSE(plan.direct_response.empty());
}

// ---------------------------------------------------------------------------
// Dirty-JSON tolerance (the P0 fix: fenced/prose-wrapped plans must parse,
// garbage gets one temperature-0 retry before FALLBACK)
// ---------------------------------------------------------------------------

TEST(TaskPlanner, MarkdownFencedPlanParses) {
    auto llm = std::make_shared<SequenceLlm>();
    llm->responses = {std::string("好的，计划如下：\n```json\n") + kRetrievePlan + "\n```\n"};

    auto plan = RunPlan(llm);

    EXPECT_EQ(llm->call_count, 1);   // parsed on the first attempt
    EXPECT_EQ(plan.next_state, "retrieve");
    ASSERT_EQ(plan.tool_calls.size(), 1u);
    EXPECT_EQ(plan.tool_calls[0].tool_name, "deal_retriever");
}

TEST(TaskPlanner, ProseEmbeddedPlanParses) {
    auto llm = std::make_shared<SequenceLlm>();
    llm->responses = {std::string("我认为应该检索：") + kRetrievePlan + " 希望有帮助"};

    auto plan = RunPlan(llm);

    EXPECT_EQ(llm->call_count, 1);
    EXPECT_EQ(plan.next_state, "retrieve");
    EXPECT_EQ(plan.tool_calls[0].tool_name, "deal_retriever");
}

TEST(TaskPlanner, GarbageThenRetrySucceeds) {
    auto llm = std::make_shared<SequenceLlm>();
    // First response is truncated mid-key (the actual e2e failure mode);
    // the retry at temperature=0 returns valid JSON.
    llm->responses = {
        R"({"action":"retrieve","slots":{"city":"上海"},"tool_calls":[{"tool_name)",
        kRetrievePlan
    };

    auto plan = RunPlan(llm);

    EXPECT_EQ(llm->call_count, 2);
    EXPECT_DOUBLE_EQ(llm->last_temperature, 0.0);   // retry used temperature=0
    EXPECT_EQ(plan.next_state, "retrieve");
    EXPECT_EQ(plan.tool_calls[0].tool_name, "deal_retriever");
}

TEST(TaskPlanner, GarbageTwiceFallsBack) {
    auto llm = std::make_shared<SequenceLlm>();
    llm->responses = {"完全不是 JSON", "还是不是 JSON"};

    auto plan = RunPlan(llm);

    EXPECT_EQ(llm->call_count, 2);   // one retry, then give up
    EXPECT_EQ(plan.next_state, "FALLBACK");
    EXPECT_TRUE(plan.tool_calls.empty());
}

TEST(TaskPlanner, PlannerRequestsLargerMaxTokens) {
    // Reasoning models truncate plans at the old 1024 default (the P0 bug).
    auto llm = std::make_shared<SequenceLlm>();
    llm->responses = {kRetrievePlan};

    RunPlan(llm);

    EXPECT_GE(llm->last_max_tokens, 4096);
}

// ---------------------------------------------------------------------------
// ExtractJsonObject (shared helper)
// ---------------------------------------------------------------------------

TEST(JsonExtract, ParsesCleanJson) {
    auto j = ExtractJsonObject(R"({"a":1})");
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ((*j)["a"], 1);
}

TEST(JsonExtract, StripsMarkdownFence) {
    auto j = ExtractJsonObject("前言\n```json\n{\"a\":2}\n```\n后记");
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ((*j)["a"], 2);
}

TEST(JsonExtract, ExtractsEmbeddedObject) {
    auto j = ExtractJsonObject("答：{\"a\":3} 完");
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ((*j)["a"], 3);
}

TEST(JsonExtract, RejectsGarbageAndNonObjects) {
    EXPECT_FALSE(ExtractJsonObject("没有 JSON").has_value());
    EXPECT_FALSE(ExtractJsonObject("[1,2,3]").has_value());   // array, not object
    EXPECT_FALSE(ExtractJsonObject("{\"a\":").has_value());   // truncated
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
