#include "agent/agent_orchestrator.hpp"
#include "agent/task_planner.hpp"
#include "agent/tool_registry.hpp"
#include "agent/session_memory.hpp"
#include "agent/llm_client.hpp"
#include "agent/response_composer.hpp"
#include "agent/safety_guard.hpp"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <random>

namespace agent {

namespace {

std::string GenerateTraceId() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(100000, 999999);
    return "t-" + std::to_string(dist(rng));
}

} // namespace

AgentOrchestrator::AgentOrchestrator(
    std::shared_ptr<TaskPlanner> planner,
    std::shared_ptr<ToolRegistry> tools,
    std::shared_ptr<SessionMemoryStore> memory,
    std::shared_ptr<LlmClient> llm,
    std::shared_ptr<ResponseComposer> composer,
    std::shared_ptr<SafetyGuard> guard)
    : planner_(std::move(planner))
    , tools_(std::move(tools))
    , memory_(std::move(memory))
    , llm_(std::move(llm))
    , composer_(std::move(composer))
    , guard_(std::move(guard)) {}

coro::Task<RecommendationResult> AgentOrchestrator::Chat(Request req) {
    RecommendationResult result;
    result.trace_id = GenerateTraceId();

    // 1. Get or create session
    auto session = co_await memory_->GetOrCreateSession(
        req.user_context.session_id.empty() ? std::optional<std::string>{} : req.user_context.session_id,
        req.user_context);
    result.session_id = session.session_id;

    // 2. Input safety guard (before planning). Optional: skipped if no guard.
    if (guard_) {
        auto guard_result = guard_->CheckInput(req.user_message);
        if (!guard_result.is_safe) {
            spdlog::warn("Input blocked: risk_type={}, reason={}",
                         guard_result.risk_type, guard_result.reason);
            result.response_text = guard_result.refusal_reply;
            result.next_state = "BLOCKED";
            result.is_clarifying = false;
            // Record both turns so the transcript stays coherent.
            co_await memory_->AppendTurn(session.session_id,
                ConversationTurn{.role = "user", .content = req.user_message});
            co_await memory_->AppendTurn(session.session_id,
                ConversationTurn{.role = "assistant", .content = result.response_text});
            co_return result;
        }
    }

    // 3. Append user turn
    ConversationTurn user_turn;
    user_turn.role = "user";
    user_turn.content = req.user_message;
    co_await memory_->AppendTurn(session.session_id, user_turn);

    // 4. Retrieve history
    auto history = co_await memory_->GetRecentTurns(session.session_id, 10);

    // 5. Plan next step
    auto plan = co_await planner_->PlanNextStep(
        req.user_context, history, req.user_message, session.context);

    result.next_state = plan.next_state;

    // 6. Handle clarify
    if (plan.next_state == "clarify" || plan.next_state == "SLOT_FILL") {
        result.is_clarifying = true;
        if (plan.clarification) {
            result.response_text = plan.clarification->question;
        } else {
            result.response_text = "请问您能提供更多信息吗？例如城市、人数和预算。";
        }
        co_await memory_->UpdateContext(session.session_id, "SLOT_FILL", plan.slots);
        co_await memory_->AppendTurn(session.session_id,
            ConversationTurn{.role = "assistant", .content = result.response_text});
        co_return result;
    }

    // 7. Execute tool calls
    std::vector<RecommendationItem> items;
    spdlog::info("Executing {} tool calls for action={}", plan.tool_calls.size(), plan.next_state);
    for (const auto& call : plan.tool_calls) {
        auto tool = tools_->Get(call.tool_name);
        if (!tool) {
            spdlog::warn("Tool not found: {}", call.tool_name);
            continue;
        }
        spdlog::info("Calling tool {} with args: {}", call.tool_name, call.arguments_json);
        auto tool_result = co_await tool->Execute(call);
        spdlog::info("Tool {} result: success={}, result={}", call.tool_name, tool_result.success, tool_result.result_json);
        if (!tool_result.success) {
            spdlog::warn("Tool {} failed: {}", call.tool_name, tool_result.error_message);
            continue;
        }
        try {
            auto j = nlohmann::json::parse(tool_result.result_json);
            if (j.contains("items")) {
                for (const auto& item_json : j["items"]) {
                    RecommendationItem item;
                    item.item_id = item_json.value("item_id", "");
                    item.merchant_id = item_json.value("merchant_id", "");
                    item.title = item_json.value("title", "");
                    item.category = item_json.value("category", "");
                    item.price = item_json.value("price", 0.0);
                    item.original_price = item_json.value("original_price", 0.0);
                    item.score = item_json.value("score", 0.0);
                    item.reason = item_json.value("reason", "");
                    item.city = item_json.value("city", "");
                    if (item_json.contains("tags")) {
                        item.tags = item_json["tags"].get<std::vector<std::string>>();
                    }
                    items.push_back(std::move(item));
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("Failed to parse tool result: {}", e.what());
        }
    }

    // 8. Compose response
    auto composed = co_await composer_->Compose(req.user_message, plan.slots, items);
    result.response_text = composed.response_text;
    result.items = std::move(composed.items);
    result.is_clarifying = false;

    // 9. Output safety guard: mask PII / strip banned words before returning.
    if (guard_) {
        result.response_text = guard_->SanitizeOutputText(result.response_text);
        guard_->SanitizeItems(result.items);
    }

    // 10. Update context and store assistant turn
    co_await memory_->UpdateContext(session.session_id, "RESPOND", plan.slots);
    co_await memory_->AppendTurn(session.session_id,
        ConversationTurn{.role = "assistant", .content = result.response_text});

    co_return result;
}

} // namespace agent
