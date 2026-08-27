#include "agent/response_composer.hpp"
#include "agent/json_extract.hpp"
#include "agent/llm_client.hpp"
#include "agent/prompt_builder.hpp"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace agent {

namespace {

// Cap how many items are sent to the LLM for composition. The pipeline
// already ranks before composing, so the head of the list is what matters;
// capping keeps the prompt small and the reply focused.
constexpr size_t kMaxComposeItems = 10;

// Latency ceiling for the composition call. Composition has a fast template
// fallback, so we fail over quickly rather than blocking on a slow model.
constexpr auto kComposeTimeout = std::chrono::milliseconds(20000);

nlohmann::json ItemsToJson(const std::vector<RecommendationItem>& items) {
    auto arr = nlohmann::json::array();
    for (size_t i = 0; i < items.size() && i < kMaxComposeItems; ++i) {
        const auto& it = items[i];
        arr.push_back({
            {"item_id", it.item_id},
            {"title", it.title},
            {"category", it.category},
            {"city", it.city},
            {"district", it.district},
            {"price", it.price},
            {"original_price", it.original_price},
            {"rating", it.rating},
            {"sold_count", it.sold_count},
            {"tags", it.tags},
            {"reason", it.reason},
        });
    }
    return arr;
}

// Phase 0 templated reply — used when the LLM is unavailable or its output
// can't be parsed. Kept deterministic and dependency-free.
std::string BuildTemplateReply(const std::vector<RecommendationItem>& items) {
    std::string reply = "为您推荐以下团购：\n";
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        reply += std::to_string(i + 1) + ". **" + item.title + "**";
        reply += "：现价 " + std::to_string(static_cast<int>(item.price)) + " 元";
        if (item.original_price > 0) {
            reply += "，原价 " + std::to_string(static_cast<int>(item.original_price)) + " 元";
        }
        if (!item.reason.empty()) {
            reply += "。" + item.reason;
        }
        reply += "\n";
    }
    return reply;
}

// Push already-rendered text to the emitter in line-sized deltas. Used for the
// template fallback so a streaming client still observes progressive `delta`
// events even when no real LLM stream is available.
void StreamText(StreamEmitter& emitter, const std::string& text) {
    if (text.empty()) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t end = (nl == std::string::npos) ? text.size() : nl + 1;
        emitter.EmitDelta(text.substr(pos, end - pos));
        pos = end;
    }
}

// Apply a parsed composition object: extract `reply` and merge `item_reasons`.
// Returns nullopt if there is no usable reply.
std::optional<std::string> ApplyComposition(const nlohmann::json& j,
                                            std::vector<RecommendationItem>& items) {
    if (!j.is_object() || !j.contains("reply") || !j["reply"].is_string()) {
        return std::nullopt;
    }
    std::string reply = j["reply"].get<std::string>();
    if (reply.empty()) return std::nullopt;

    if (j.contains("item_reasons") && j["item_reasons"].is_object()) {
        for (auto& item : items) {
            const auto& reasons = j["item_reasons"];
            if (reasons.contains(item.item_id) && reasons[item.item_id].is_string()) {
                std::string r = reasons[item.item_id].get<std::string>();
                if (!r.empty()) item.reason = r;
            }
        }
    }
    return reply;
}

// Parse the model output into a reply. Uses the shared tolerant extractor
// (clean JSON / markdown fence / embedded-in-prose) then merges item_reasons.
std::optional<std::string> TryParseComposition(const std::string& raw,
                                               std::vector<RecommendationItem>& items) {
    auto j = ExtractJsonObject(raw);
    if (!j) return std::nullopt;
    return ApplyComposition(*j, items);
}

// Cap for the guard_detail audit string (violations are short, but a
// pathological reply could produce many — keep the DB row small).
constexpr size_t kGuardDetailCap = 400;

// Join fact-check violations into the audit detail string (capped).
std::string GuardDetail(const FactCheckResult& fc) {
    std::string detail;
    for (const auto& v : fc.violations) {
        if (!detail.empty()) detail += "; ";
        detail += v;
        if (detail.size() >= kGuardDetailCap) {
            detail.resize(kGuardDetailCap);
            break;
        }
    }
    return detail;
}

} // namespace

ResponseComposer::ResponseComposer(std::shared_ptr<LlmClient> llm,
                                   std::shared_ptr<const SafetyGuard> guard)
    : llm_(std::move(llm)), guard_(std::move(guard)) {}

coro::Task<RecommendationResult> ResponseComposer::Compose(
    const std::string& user_request,
    const nlohmann::json& slots,
    const std::vector<RecommendationItem>& items,
    std::shared_ptr<StreamEmitter> emitter,
    const std::string& grounding) {
    RecommendationResult result;
    result.items = items;   // copy; LLM may enrich per-item reasons in place

    // No candidates AND no grounding -> nothing for the LLM to write about;
    // short-circuit. When grounding passages exist (kb_search answered a
    // factual question), fall through so the LLM can answer from the knowledge
    // even though there are no deals to recommend.
    if (items.empty() && grounding.empty()) {
        result.response_text = "抱歉，暂时没有符合条件的团购。您可以换个城市或预算试试。";
        result.compose_mode = "short_circuit";
        if (emitter) StreamText(*emitter, result.response_text);
        co_return result;
    }

    const std::string slots_str = slots.is_null() ? "{}" : slots.dump();
    const std::string items_json = ItemsToJson(items).dump();
    constexpr const char* kSystem =
        "你是团购推荐助手。请根据已筛选并排序好的团购商品生成中文推荐回复。"
        "价格/折扣/商家等数据必须与给定商品一致，禁止编造。";

    // --- Streaming endpoint: stream the reply as token deltas. ---
    if (emitter) {
        if (llm_ && llm_->Healthy()) {
            try {
                std::string prompt = PromptBuilder::ResponseCompositionStreamPrompt(
                    user_request, slots_str, items_json, grounding);
                std::vector<LlmMessage> messages{
                    {"system", kSystem},
                    {"user", prompt}
                };
                auto options = LlmClient::Options{};
                options.temperature = 0.6;
                options.timeout = kComposeTimeout;
                // Reasoning models burn tokens on reasoning before prose; give
                // the reply ample room (the 1024 default truncated outputs).
                options.max_tokens = 2048;
                auto sr = co_await llm_->ChatStream(
                    messages, options,
                    [&emitter](const std::string& d) { emitter->EmitDelta(d); });
                // Token usage arrives in the trailing SSE usage chunk when the
                // gateway gets it from the upstream (or the stub's estimate);
                // it stays 0 when the upstream sends none — recorded honestly.
                LlmCallInfo info;
                info.purpose = "compose";
                info.model = options.model;
                info.latency = sr.latency;
                info.prompt_tokens = sr.prompt_tokens;
                info.completion_tokens = sr.completion_tokens;
                info.raw_request = prompt;
                if (sr.streamed && !sr.text.empty()) {
                    // Phase 4-B: the stream already went out token by token,
                    // so a fact violation is corrected after the fact with a
                    // trailing `replace` SSE event carrying the template
                    // reply (additive event; old frontends ignore it).
                    if (guard_) {
                        const auto fc = guard_->FactCheckReply(sr.text, result.items);
                        if (!fc.ok) {
                            info.status = "guard_fallback";
                            result.llm_calls.push_back(std::move(info));
                            const std::string detail = GuardDetail(fc);
                            spdlog::warn("ResponseComposer: streamed reply failed "
                                         "fact check ({}), emitting replace.", detail);
                            std::string t = BuildTemplateReply(result.items);
                            emitter->Emit("replace", {{"content", t}});
                            result.compose_mode = "llm_stream_guard_fallback";
                            result.response_text = std::move(t);
                            result.guard_action = "fact_violation";
                            result.guard_detail = detail;
                            co_return result;
                        }
                    }
                    info.status = "success";
                    result.llm_calls.push_back(std::move(info));
                    result.compose_mode = "llm_stream";
                    result.response_text = std::move(sr.text);
                    co_return result;
                }
                info.status = "stream_fallback";
                result.llm_calls.push_back(std::move(info));
                spdlog::warn("ResponseComposer: streaming unavailable, emitting template as deltas.");
            } catch (const std::exception& e) {
                spdlog::warn("ResponseComposer: stream failed ({}), using template.", e.what());
            }
        }
        // Streaming fallback: emit the deterministic template as deltas.
        std::string t = BuildTemplateReply(items);
        StreamText(*emitter, t);
        result.compose_mode = "template";
        result.response_text = std::move(t);
        co_return result;
    }

    // --- Non-streaming endpoint: JSON composition with per-item reasons. ---
    if (llm_ && llm_->Healthy()) {
        try {
            std::string prompt = PromptBuilder::ResponseCompositionPrompt(
                user_request, slots_str, items_json, grounding);
            std::vector<LlmMessage> messages{
                {"system", kSystem},
                {"user", prompt}
            };
            auto options = LlmClient::Options{};
            options.temperature = 0.6;
            options.timeout = kComposeTimeout;
            options.max_tokens = 2048;   // see streaming path note
            auto resp = co_await llm_->Chat(messages, options);

            LlmCallInfo info;
            info.purpose = "compose";
            info.model = resp.model.empty() ? options.model : resp.model;
            info.prompt_tokens = resp.prompt_tokens;
            info.completion_tokens = resp.completion_tokens;
            info.latency = resp.latency;
            info.raw_request = prompt;
            auto reply = TryParseComposition(resp.raw_text, result.items);
            if (reply && !reply->empty()) {
                // Phase 4-A: fact-check money/discount claims against the
                // candidate items before the reply goes out. A violation never
                // reaches the user — the deterministic template takes over and
                // the compose_mode records why (the LLM call itself is still
                // audited as a success above the guard layer).
                if (guard_) {
                    const auto fc = guard_->FactCheckReply(*reply, result.items);
                    if (!fc.ok) {
                        info.status = "guard_fallback";
                        result.llm_calls.push_back(std::move(info));
                        const std::string detail = GuardDetail(fc);
                        spdlog::warn("ResponseComposer: fact check failed ({}), "
                                     "using template.", detail);
                        result.compose_mode = "template_guard_fallback";
                        result.response_text = BuildTemplateReply(result.items);
                        result.guard_action = "fact_violation";
                        result.guard_detail = detail;
                        co_return result;
                    }
                }
                info.status = "success";
                result.llm_calls.push_back(std::move(info));
                result.compose_mode = "llm";
                result.response_text = std::move(*reply);
                co_return result;
            }
            info.status = "template_fallback";
            result.llm_calls.push_back(std::move(info));
            spdlog::warn("ResponseComposer: LLM reply unparseable, using template.");
        } catch (const std::exception& e) {
            spdlog::warn("ResponseComposer: LLM composition failed ({}), using template.", e.what());
        }
    }

    // Fallback: deterministic templated reply.
    result.compose_mode = "template";
    result.response_text = BuildTemplateReply(items);
    co_return result;
}

} // namespace agent
