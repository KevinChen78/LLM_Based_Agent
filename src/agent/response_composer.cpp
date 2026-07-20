#include "agent/response_composer.hpp"
#include "agent/llm_client.hpp"

#include <nlohmann/json.hpp>

namespace agent {

ResponseComposer::ResponseComposer(std::shared_ptr<LlmClient> llm)
    : llm_(std::move(llm)) {}

coro::Task<RecommendationResult> ResponseComposer::Compose(
    const std::string& user_request,
    const nlohmann::json& slots,
    const std::vector<RecommendationItem>& items) {
    RecommendationResult result;
    result.items = items;

    if (items.empty()) {
        result.response_text = "抱歉，暂时没有符合条件的团购。您可以换个城市或预算试试。";
        co_return result;
    }

    // Phase 0: simple templated reply
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

    result.response_text = reply;
    co_return result;
}

} // namespace agent
