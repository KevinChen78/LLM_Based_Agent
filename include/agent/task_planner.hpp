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
        // Canned reply for action=="respond" (chitchat / thanks) — used
        // directly instead of running the recommendation pipeline.
        std::string direct_response;
        // One entry per LLM attempt made to produce this plan (observability).
        std::vector<LlmCallInfo> llm_calls;
    };

    explicit TaskPlanner(std::shared_ptr<class LlmClient> llm);

    coro::Task<Plan> PlanNextStep(
        const UserContext& ctx,
        const std::vector<ConversationTurn>& history,
        const std::string& user_message,
        const nlohmann::json& current_slots,
        // Cross-session user profile (Phase 2.2); empty object => the prompt
        // carries no profile section and behaviour is unchanged.
        const nlohmann::json& user_profile = nlohmann::json::object(),
        // Pre-joined ("、") catalog category whitelist (Phase 3-A); empty =>
        // no category section, prompt byte-identical to before.
        const std::string& category_list = "");

private:
    std::shared_ptr<LlmClient> llm_;
};

} // namespace agent
