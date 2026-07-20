#pragma once

#include "agent/common.hpp"
#include "coro/core/task.hpp"

#include <nlohmann/json.hpp>

#include <memory>

namespace agent {

class LlmClient;

class ResponseComposer {
public:
    explicit ResponseComposer(std::shared_ptr<LlmClient> llm);

    coro::Task<RecommendationResult> Compose(
        const std::string& user_request,
        const nlohmann::json& slots,
        const std::vector<RecommendationItem>& items);

private:
    std::shared_ptr<LlmClient> llm_;
};

} // namespace agent
