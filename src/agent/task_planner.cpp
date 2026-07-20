#include "agent/task_planner.hpp"
#include "agent/llm_client.hpp"
#include "agent/prompt_builder.hpp"

#include <spdlog/spdlog.h>

namespace agent {

TaskPlanner::TaskPlanner(std::shared_ptr<LlmClient> llm)
    : llm_(std::move(llm)) {}

coro::Task<TaskPlanner::Plan> TaskPlanner::PlanNextStep(
    const UserContext& ctx,
    const std::vector<ConversationTurn>& history,
    const std::string& user_message,
    const nlohmann::json& current_slots) {
    Plan plan;

    // Build history string
    std::string history_str;
    for (const auto& turn : history) {
        history_str += turn.role + ": " + turn.content + "\n";
    }

    std::string prompt = PromptBuilder::TaskPlanningPrompt(
        history_str, user_message, current_slots.dump());

    std::vector<LlmMessage> messages{
        {"system", "You are a task planner for a group-buying recommendation agent."},
        {"user", prompt}
    };

    auto options = LlmClient::Options{};
    options.temperature = 0.3;
    auto llm_resp = co_await llm_->Chat(messages, options);

    try {
        auto j = nlohmann::json::parse(llm_resp.raw_text);
        plan.next_state = j.value("action", "FALLBACK");
        plan.slots = j.value("slots", nlohmann::json::object());
        plan.missing_slots = j.value("missing_slots", std::vector<std::string>{});

        auto cq = j.value("clarification_question", "");
        if (!cq.empty()) {
            plan.clarification = ClarificationQuestion{
                .question = cq,
                .candidate_options = j.value("candidate_options", std::vector<std::string>{})
            };
        }

        for (const auto& tc : j.value("tool_calls", nlohmann::json::array())) {
            ToolCall call;
            call.tool_name = tc.value("tool_name", "");
            call.arguments_json = tc.value("arguments", nlohmann::json::object()).dump();
            call.call_id = "tc-" + std::to_string(std::hash<std::string>{}(call.arguments_json));
            plan.tool_calls.push_back(std::move(call));
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse LLM plan response: {}", e.what());
        plan.next_state = "FALLBACK";
    }

    co_return plan;
}

} // namespace agent
