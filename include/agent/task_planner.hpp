#pragma once

#include "agent/common.hpp"
#include "coro/core/task.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent {

class TaskPlanner {
public:
    struct Plan {
        std::string next_state;   // SLOT_FILL / RETRIEVE / RANK / EXPLAIN / RESPOND / FALLBACK
        nlohmann::json slots;
        std::vector<std::string> missing_slots;
        std::optional<ClarificationQuestion> clarification;
        std::vector<ToolCall> tool_calls;
    };

    explicit TaskPlanner(std::shared_ptr<class LlmClient> llm);

    coro::Task<Plan> PlanNextStep(
        const UserContext& ctx,
        const std::vector<ConversationTurn>& history,
        const std::string& user_message,
        const nlohmann::json& current_slots);

private:
    std::shared_ptr<LlmClient> llm_;
};

} // namespace agent
