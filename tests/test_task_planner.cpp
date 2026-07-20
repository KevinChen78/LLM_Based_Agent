#include "agent/llm_client.hpp"
#include "agent/task_planner.hpp"

#include <gtest/gtest.h>

using namespace agent;

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
    EXPECT_EQ(plan.tool_calls[0].tool_name, "mock_retriever");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
