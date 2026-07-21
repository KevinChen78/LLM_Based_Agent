#include "agent/response_composer.hpp"
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

// Parse the model output into a reply. Tolerates three forms the model may emit:
//   1. clean JSON,
//   2. JSON wrapped in a ```json ... ``` markdown fence,
//   3. JSON embedded in prose (extract first '{' .. last '}').
std::optional<std::string> TryParseComposition(const std::string& raw,
                                               std::vector<RecommendationItem>& items) {
    // 1. Direct parse.
    try {
        if (auto r = ApplyComposition(nlohmann::json::parse(raw), items)) return r;
    } catch (const std::exception&) {}

    // 2. Strip a markdown code fence and retry.
    auto fence = raw.find("```");
    if (fence != std::string::npos) {
        std::string t = raw.substr(fence + 3);
        if (t.rfind("json", 0) == 0) t.erase(0, 4);
        auto close = t.rfind("```");
        if (close != std::string::npos) {
            t.erase(close);
            while (!t.empty() && (t.front() == '\n' || t.front() == '\r' || t.front() == ' ')) {
                t.erase(0, 1);
            }
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) {
                t.pop_back();
            }
            try {
                if (auto r = ApplyComposition(nlohmann::json::parse(t), items)) return r;
            } catch (const std::exception&) {}
        }
    }

    // 3. First '{' .. last '}'.
    auto fb = raw.find('{');
    auto fe = raw.rfind('}');
    if (fb != std::string::npos && fe != std::string::npos && fe > fb) {
        try {
            if (auto r = ApplyComposition(
                    nlohmann::json::parse(raw.substr(fb, fe - fb + 1)), items)) {
                return r;
            }
        } catch (const std::exception&) {}
    }

    return std::nullopt;
}

} // namespace

ResponseComposer::ResponseComposer(std::shared_ptr<LlmClient> llm)
    : llm_(std::move(llm)) {}

coro::Task<RecommendationResult> ResponseComposer::Compose(
    const std::string& user_request,
    const nlohmann::json& slots,
    const std::vector<RecommendationItem>& items,
    std::shared_ptr<StreamEmitter> emitter) {
    RecommendationResult result;
    result.items = items;   // copy; LLM may enrich per-item reasons in place

    // No candidates -> nothing for the LLM to write about; short-circuit.
    if (items.empty()) {
        result.response_text = "抱歉，暂时没有符合条件的团购。您可以换个城市或预算试试。";
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
                    user_request, slots_str, items_json);
                std::vector<LlmMessage> messages{
                    {"system", kSystem},
                    {"user", prompt}
                };
                auto options = LlmClient::Options{};
                options.temperature = 0.6;
                options.timeout = kComposeTimeout;
                auto sr = co_await llm_->ChatStream(
                    messages, options,
                    [&emitter](const std::string& d) { emitter->EmitDelta(d); });
                if (sr.streamed && !sr.text.empty()) {
                    result.response_text = std::move(sr.text);
                    co_return result;
                }
                spdlog::warn("ResponseComposer: streaming unavailable, emitting template as deltas.");
            } catch (const std::exception& e) {
                spdlog::warn("ResponseComposer: stream failed ({}), using template.", e.what());
            }
        }
        // Streaming fallback: emit the deterministic template as deltas.
        std::string t = BuildTemplateReply(items);
        StreamText(*emitter, t);
        result.response_text = std::move(t);
        co_return result;
    }

    // --- Non-streaming endpoint: JSON composition with per-item reasons. ---
    if (llm_ && llm_->Healthy()) {
        try {
            std::string prompt = PromptBuilder::ResponseCompositionPrompt(
                user_request, slots_str, items_json);
            std::vector<LlmMessage> messages{
                {"system", kSystem},
                {"user", prompt}
            };
            auto options = LlmClient::Options{};
            options.temperature = 0.6;
            options.timeout = kComposeTimeout;
            auto resp = co_await llm_->Chat(messages, options);

            auto reply = TryParseComposition(resp.raw_text, result.items);
            if (reply && !reply->empty()) {
                result.response_text = std::move(*reply);
                co_return result;
            }
            spdlog::warn("ResponseComposer: LLM reply unparseable, using template.");
        } catch (const std::exception& e) {
            spdlog::warn("ResponseComposer: LLM composition failed ({}), using template.", e.what());
        }
    }

    // Fallback: deterministic templated reply.
    result.response_text = BuildTemplateReply(items);
    co_return result;
}

} // namespace agent
