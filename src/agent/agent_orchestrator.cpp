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
#include <string_view>

namespace agent {

namespace {

std::string GenerateTraceId() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(100000, 999999);
    return "t-" + std::to_string(dist(rng));
}

void EmitIf(const std::shared_ptr<StreamEmitter>& emitter,
            const std::string& event_type,
            const nlohmann::json& payload) {
    if (emitter) {
        emitter->Emit(event_type, payload);
    }
}

void FinishIf(const std::shared_ptr<StreamEmitter>& emitter,
              const RecommendationResult& result) {
    if (emitter) {
        emitter->Finish(result);
    }
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
    // Non-streaming path: use the streaming implementation with a null emitter.
    co_return co_await ChatStream(std::move(req), nullptr);
}

coro::Task<RecommendationResult> AgentOrchestrator::ChatStream(
    Request req, std::shared_ptr<StreamEmitter> emitter) {
    RecommendationResult result;
    result.trace_id = GenerateTraceId();

    try {
        // 1. Get or create session
        auto session = co_await memory_->GetOrCreateSession(
            req.user_context.session_id.empty() ? std::optional<std::string>{} : req.user_context.session_id,
            req.user_context);
        result.session_id = session.session_id;
        EmitIf(emitter, "started", nlohmann::json{
            {"session_id", session.session_id},
            {"state", session.current_state}
        });

        // 2. Input safety guard (before planning). Optional: skipped if no guard.
        if (guard_) {
            EmitIf(emitter, "input_guard", nlohmann::json{{"status", "checking"}});
            auto guard_result = guard_->CheckInput(req.user_message);
            if (!guard_result.is_safe) {
                spdlog::warn("Input blocked: risk_type={}, reason={}",
                             guard_result.risk_type, guard_result.reason);
                result.response_text = guard_result.refusal_reply;
                result.next_state = "BLOCKED";
                result.is_clarifying = false;
                EmitIf(emitter, "input_guard", nlohmann::json{
                    {"status", "blocked"},
                    {"risk_type", guard_result.risk_type},
                    {"reason", guard_result.reason}
                });
                // Record both turns so the transcript stays coherent.
                co_await memory_->AppendTurn(session.session_id,
                    ConversationTurn{.role = "user", .content = req.user_message});
                co_await memory_->AppendTurn(session.session_id,
                    ConversationTurn{.role = "assistant", .content = result.response_text});
                FinishIf(emitter, result);
                co_return result;
            }
            EmitIf(emitter, "input_guard", nlohmann::json{{"status", "safe"}});
        }

        // 3. Append user turn
        ConversationTurn user_turn;
        user_turn.role = "user";
        user_turn.content = req.user_message;
        co_await memory_->AppendTurn(session.session_id, user_turn);

        // 4. Retrieve history
        auto history = co_await memory_->GetRecentTurns(session.session_id, 10);

        // 5. Plan next step
        EmitIf(emitter, "planning", nlohmann::json{{"detail", "deciding next step"}});
        auto plan = co_await planner_->PlanNextStep(
            req.user_context, history, req.user_message, session.context);

        result.next_state = plan.next_state;
        EmitIf(emitter, "plan", nlohmann::json{
            {"next_state", plan.next_state},
            {"missing_slots", plan.missing_slots},
            {"tool_count", plan.tool_calls.size()}
        });

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
            FinishIf(emitter, result);
            co_return result;
        }

        // 7. Execute tool calls
        // Tool results carry full JSON items; we accumulate them and only map
        // to RecommendationItem at the end. This lets a ranker refine the
        // candidate set (replace) rather than duplicating retriever output.
        auto is_ranker = [](const std::string& name) {
            constexpr std::string_view kSuffix = "_ranker";
            return name.size() >= kSuffix.size() &&
                   name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
        };

        nlohmann::json accumulated_items = nlohmann::json::array();
        spdlog::info("Executing {} tool calls for action={}", plan.tool_calls.size(), plan.next_state);
        for (const auto& call : plan.tool_calls) {
            auto tool = tools_->Get(call.tool_name);
            if (!tool) {
                spdlog::warn("Tool not found: {}", call.tool_name);
                EmitIf(emitter, "tool_error", nlohmann::json{
                    {"tool_name", call.tool_name},
                    {"reason", "not_found"}
                });
                continue;
            }

            // Deterministic retrieve→rank chaining: if a ranker is invoked
            // without candidates, feed it the items accumulated so far. This
            // avoids depending on the LLM to forward retriever output.
            ToolCall effective = call;
            bool injected = false;
            if (is_ranker(call.tool_name)) {
                try {
                    auto args = nlohmann::json::parse(call.arguments_json);
                    if (!args.contains("candidates") || args["candidates"].empty()) {
                        args["candidates"] = accumulated_items;
                        effective.arguments_json = args.dump();
                        injected = !accumulated_items.empty();
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to inspect ranker args for chaining: {}", e.what());
                }
            }

            spdlog::info("Calling tool {} with args: {}", call.tool_name, effective.arguments_json);
            EmitIf(emitter, "tool_call", nlohmann::json{
                {"tool_name", call.tool_name},
                {"call_id", call.call_id},
                {"chained", injected}
            });
            auto tool_result = co_await tool->Execute(effective);
            spdlog::info("Tool {} result: success={}, result={}", call.tool_name, tool_result.success, tool_result.result_json);
            EmitIf(emitter, "tool_result", nlohmann::json{
                {"tool_name", call.tool_name},
                {"call_id", call.call_id},
                {"success", tool_result.success},
                {"error_message", tool_result.error_message}
            });
            if (!tool_result.success) {
                spdlog::warn("Tool {} failed: {}", call.tool_name, tool_result.error_message);
                continue;
            }
            try {
                auto j = nlohmann::json::parse(tool_result.result_json);
                if (!j.contains("items")) continue;
                if (is_ranker(call.tool_name)) {
                    // A ranker refines the candidate set: its output replaces.
                    accumulated_items = j["items"];
                } else {
                    // A retriever (or other producer) adds candidates.
                    for (const auto& it : j["items"]) accumulated_items.push_back(it);
                }
            } catch (const std::exception& e) {
                spdlog::error("Failed to parse tool result: {}", e.what());
            }
        }

        // Map the accumulated JSON items into the output model, preserving the
        // richer fields (sold_count / rating / district) for ranking signal.
        std::vector<RecommendationItem> items;
        for (const auto& item_json : accumulated_items) {
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
            item.district = item_json.value("district", "");
            item.sold_count = item_json.value("sold_count", 0);
            item.rating = item_json.value("rating", 0.0);
            if (item_json.contains("tags")) {
                item.tags = item_json["tags"].get<std::vector<std::string>>();
            }
            items.push_back(std::move(item));
        }

        // 8. Compose response
        EmitIf(emitter, "composing", nlohmann::json{{"detail", "generating reply"}});
        auto composed = co_await composer_->Compose(req.user_message, plan.slots, items, emitter);
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

        FinishIf(emitter, result);
        co_return result;
    } catch (const std::exception& e) {
        spdlog::error("AgentOrchestrator::ChatStream error: {}", e.what());
        if (emitter) {
            emitter->Error(std::string("internal error: ") + e.what());
        }
        result.response_text = "抱歉，处理您的请求时出了点问题，请稍后再试。";
        result.next_state = "ERROR";
        result.is_clarifying = false;
        co_return result;
    }
}

} // namespace agent
