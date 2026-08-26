#include "agent/agent_orchestrator.hpp"
#include "agent/deal_catalog.hpp"
#include "agent/preference_extractor.hpp"
#include "agent/task_planner.hpp"
#include "agent/tool_registry.hpp"
#include "agent/session_memory.hpp"
#include "agent/llm_client.hpp"
#include "agent/observability_store.hpp"
#include "agent/response_composer.hpp"
#include "agent/safety_guard.hpp"
#include "agent/user_profile_store.hpp"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace agent {

namespace {

std::string GenerateTraceId() {
    static std::atomic<int> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "t-" + std::to_string(now) + "-" + std::to_string(++counter);
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

// ISO-8601 local timestamp for (now - minutes). The stores write updated_at
// with the same fixed-width format, so lexicographic == chronological.
std::string IsoMinutesAgo(int minutes) {
    auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() - std::chrono::minutes(minutes));
    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

// Resolve the cross-session user profile for the planner prompt (Phase 2.2):
// read the cached row; if missing or stale (>5 min), re-extract from
// feedback/slot signals and upsert. Any failure or empty profile yields an
// empty object, which leaves the prompt byte-identical to no-profile runs.
nlohmann::json ResolveUserProfile(UserProfileStore* profiles,
                                  DealCatalog* catalog,
                                  const std::string& user_id) {
    const auto empty = nlohmann::json::object();
    if (!profiles || !catalog || user_id.empty()) return empty;
    try {
        auto prof = profiles->Get(user_id);
        const bool fresh = prof && !prof->updated_at.empty() &&
                           prof->updated_at >= IsoMinutesAgo(5);
        if (!fresh) {
            UserProfile p = PreferenceExtractor::Extract(
                user_id, profiles->LoadFeedbackSignals(user_id),
                profiles->LoadSlotHistory(user_id), *catalog);
            // Only cache profiles that carry real signals. Caching an
            // all-defaults row would hide the user's first feedback for the
            // whole TTL; skipping the upsert costs two cheap reads per
            // request for signal-less users instead.
            const bool has_signals = !p.preferred_categories.empty() ||
                !p.preferred_cities.empty() || p.avg_budget > 0.0 ||
                !p.dietary_tags.empty();
            if (has_signals) {
                if (profiles->Upsert(p)) prof = p;
            }
        }
        if (!prof) return empty;
        // No signals yet => no prompt section (avoid teaching the planner to
        // lean on an all-defaults profile).
        if (prof->preferred_categories.empty() && prof->preferred_cities.empty() &&
            prof->avg_budget <= 0.0 && prof->dietary_tags.empty()) {
            return empty;
        }
        return nlohmann::json{
            {"preferred_cities", prof->preferred_cities},
            {"preferred_categories", prof->preferred_categories},
            {"avg_budget", prof->avg_budget},
            {"dietary_tags", prof->dietary_tags},
            {"price_sensitivity", prof->price_sensitivity},
        };
    } catch (const std::exception& e) {
        spdlog::warn("user profile resolve failed ({}); continuing without", e.what());
        return empty;
    }
}

// Render one knowledge passage as a compact readable snippet.
std::string FormatPassage(const nlohmann::json& p) {    std::string title = p.value("title", "");
    std::string content = p.value("content", "");
    std::string source = p.value("source", "");
    std::string s;
    if (!title.empty()) s += "【" + title + "】";
    s += content;
    if (!source.empty()) s += "（来源：" + source + "）";
    return s;
}

} // namespace

AgentOrchestrator::AgentOrchestrator(
    std::shared_ptr<TaskPlanner> planner,
    std::shared_ptr<ToolRegistry> tools,
    std::shared_ptr<SessionMemoryStore> memory,
    std::shared_ptr<LlmClient> llm,
    std::shared_ptr<ResponseComposer> composer,
    std::shared_ptr<SafetyGuard> guard,
    std::shared_ptr<ObservabilityStore> obs,
    std::shared_ptr<UserProfileStore> profiles,
    std::shared_ptr<DealCatalog> catalog)
    : planner_(std::move(planner))
    , tools_(std::move(tools))
    , memory_(std::move(memory))
    , llm_(std::move(llm))
    , composer_(std::move(composer))
    , guard_(std::move(guard))
    , obs_(std::move(obs))
    , profiles_(std::move(profiles))
    , catalog_(std::move(catalog)) {}

coro::Task<RecommendationResult> AgentOrchestrator::Chat(Request req) {
    // Non-streaming path: use the streaming implementation with a null emitter.
    co_return co_await ChatStream(std::move(req), nullptr);
}

coro::Task<RecommendationResult> AgentOrchestrator::ChatStream(
    Request req, std::shared_ptr<StreamEmitter> emitter) {
    // Outer shell: time the whole request and persist one recommendation_logs
    // row plus one llm_calls row per LLM call, no matter which of the inner
    // paths (blocked / clarify / respond / retrieve / error) produced it.
    const auto start = std::chrono::steady_clock::now();
    const std::string user_id = req.user_context.user_id;
    const std::string user_message = req.user_message;

    RecAudit audit;
    auto result = co_await ChatStreamInner(
        std::move(req), std::move(emitter), obs_ ? &audit : nullptr);

    if (obs_) {
        ObservabilityStore::RecLogEntry e;
        e.trace_id = result.trace_id;
        e.session_id = result.session_id;
        e.user_id = user_id;
        e.request_text = user_message;
        e.action = result.next_state;
        e.slots_json = audit.slots_json.empty() ? "{}" : audit.slots_json;
        e.item_count = static_cast<int>(result.items.size());
        auto ranked = nlohmann::json::array();
        for (size_t i = 0; i < result.items.size() && i < 5; ++i) {
            ranked.push_back({{"item_id", result.items[i].item_id},
                              {"score", result.items[i].score}});
        }
        e.ranked_items_json = ranked.dump();
        e.response_text = result.response_text;
        e.grounding_count = static_cast<int>(result.grounding.size());
        e.compose_mode = result.compose_mode.empty() ? "none" : result.compose_mode;
        e.latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        // Learning-to-rank audit fields (Phase 2.1). Fire-and-forget: any
        // parse failure leaves the columns empty rather than failing the log.
        if (!audit.rank_audit_json.empty()) {
            try {
                auto ra = nlohmann::json::parse(audit.rank_audit_json);
                e.experiment_group = ra.value("experiment_group", "");
                e.rank_mode = ra.value("rank_mode", "");
                if (ra.contains("candidates") && ra["candidates"].is_array()) {
                    e.candidates_json = ra["candidates"].dump();
                }
            } catch (const std::exception& ex) {
                spdlog::warn("Failed to parse rank_audit: {}", ex.what());
            }
        }
        obs_->LogRecommendation(e);

        for (const auto& c : audit.llm_calls) {
            ObservabilityStore::LlmCallEntry le;
            le.trace_id = result.trace_id;
            le.session_id = result.session_id;
            le.purpose = c.purpose;
            le.model = c.model;
            le.status = c.status;
            le.prompt_tokens = c.prompt_tokens;
            le.completion_tokens = c.completion_tokens;
            le.attempt = c.attempt;
            le.latency_ms = c.latency.count();
            le.raw_request = c.raw_request;
            obs_->LogLlmCall(le);
        }
    }

    co_return result;
}

coro::Task<RecommendationResult> AgentOrchestrator::ChatStreamInner(
    Request req, std::shared_ptr<StreamEmitter> emitter, RecAudit* audit) {
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

        // 5. Plan next step (with the cross-session user profile, Phase 2.2 —
        // empty object when profiles are unwired/anonymous/no signals).
        EmitIf(emitter, "planning", nlohmann::json{{"detail", "deciding next step"}});
        const nlohmann::json user_profile = ResolveUserProfile(
            profiles_.get(), catalog_.get(), req.user_context.user_id);
        auto plan = co_await planner_->PlanNextStep(
            req.user_context, history, req.user_message, session.context,
            user_profile);
        if (audit) {
            audit->slots_json = plan.slots.dump();
            for (auto& c : plan.llm_calls) audit->llm_calls.push_back(std::move(c));
        }

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

        // 6.5 Handle direct respond (chitchat / thanks): the planner's canned
        // reply is the answer — running the recommendation pipeline here would
        // compose over zero items and mislead with "no matching deals".
        if (plan.next_state == "respond") {
            result.response_text = plan.direct_response.empty()
                ? "您好，我可以帮您推荐团购套餐，请告诉我城市、人数和预算。"
                : plan.direct_response;
            co_await memory_->UpdateContext(session.session_id, "RESPOND", plan.slots);
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
        // Knowledge passages returned by kb_search, collected separately from
        // deal candidates. These ground the composed reply (RAG).
        nlohmann::json grounding_passages = nlohmann::json::array();
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
                        injected = !accumulated_items.empty();
                    }
                    // user_id is injected by the orchestrator (not part of the
                    // LLM-visible tool schema) so the ranker can bucket
                    // experiments and the ranking service can read profiles.
                    args["user_id"] = req.user_context.user_id;
                    // City/category feed the model's context-cross features;
                    // they come from the planned slots, not the LLM's args.
                    if (plan.slots.is_object()) {
                        if (plan.slots.contains("city")) args["city"] = plan.slots["city"];
                        if (plan.slots.contains("category")) args["category"] = plan.slots["category"];
                    }
                    effective.arguments_json = args.dump();
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
                // kb_search returns {"passages":[...]} — collect as grounding.
                if (j.contains("passages") && j["passages"].is_array()) {
                    for (const auto& p : j["passages"]) grounding_passages.push_back(p);
                }
                if (!j.contains("items")) continue;
                if (is_ranker(call.tool_name)) {
                    // A ranker refines the candidate set: its output replaces.
                    accumulated_items = j["items"];
                    // Learning-to-rank audit (Phase 2.1): the ranker reports
                    // experiment group / rank mode / per-candidate scores; the
                    // outer shell persists them into recommendation_logs.
                    if (audit && j.contains("rank_audit") && j["rank_audit"].is_object()) {
                        audit->rank_audit_json = j["rank_audit"].dump();
                    }
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
        // Build the grounding block (readable snippets + a numbered text block
        // for the LLM prompt) from any kb_search passages.
        std::string grounding_text;
        for (const auto& p : grounding_passages) {
            std::string snippet = FormatPassage(p);
            if (snippet.empty()) continue;
            result.grounding.push_back(snippet);
            grounding_text += std::to_string(result.grounding.size()) + ". " + snippet + "\n";
        }
        if (!result.grounding.empty()) {
            spdlog::info("Grounding reply with {} knowledge passage(s)", result.grounding.size());
            EmitIf(emitter, "grounding", nlohmann::json{{"passage_count", result.grounding.size()}});
        }

        EmitIf(emitter, "composing", nlohmann::json{{"detail", "generating reply"}});
        auto composed = co_await composer_->Compose(
            req.user_message, plan.slots, items, emitter, grounding_text);
        result.response_text = composed.response_text;
        result.items = std::move(composed.items);
        result.compose_mode = composed.compose_mode;
        if (audit) {
            for (auto& c : composed.llm_calls) audit->llm_calls.push_back(std::move(c));
        }
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
