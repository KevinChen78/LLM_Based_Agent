#include "agent/task_planner.hpp"
#include "agent/json_extract.hpp"
#include "agent/llm_client.hpp"
#include "agent/prompt_builder.hpp"

#include <spdlog/spdlog.h>

namespace agent {

namespace {

// Fill a Plan from an already-parsed plan JSON object. Kept separate so the
// caller can attempt parsing multiple times (tolerant extract + retry).
void ApplyPlanJson(const nlohmann::json& j, TaskPlanner::Plan& plan) {
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
}

} // namespace

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
    // Reasoning models (deepseek-v4-flash) burn many tokens on reasoning before
    // emitting the JSON plan; the old 1024 default truncated the output mid-key
    // (observed in e2e: parse error at '"tool_name'), which dropped the request
    // to FALLBACK with zero tool calls. Give the plan ample room.
    options.max_tokens = 4096;

    // Up to two attempts: the normal sample, then one deterministic retry.
    // LLM plan JSON is occasionally malformed (truncated / fenced / embedded in
    // prose); the tolerant extractor handles most of it, and a temperature-0
    // retry covers the rest before we give up and FALLBACK.
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto llm_resp = co_await llm_->Chat(messages, options);
        if (auto j = ExtractJsonObject(llm_resp.raw_text)) {
            ApplyPlanJson(*j, plan);
            co_return plan;
        }
        if (attempt == 0) {
            spdlog::warn("TaskPlanner: plan JSON unparseable ({}...), retrying at temperature=0",
                         llm_resp.raw_text.substr(0, 80));
            options.temperature = 0.0;
        }
    }

    spdlog::error("TaskPlanner: plan JSON unparseable after retry, FALLBACK");
    plan.next_state = "FALLBACK";
    co_return plan;
}

} // namespace agent
