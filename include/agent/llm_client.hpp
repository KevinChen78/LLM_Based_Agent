#pragma once

#include "agent/common.hpp"
#include "coro/core/task.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace agent {

enum class LlmModelTier {
    PRIMARY,
    FALLBACK,
    FAST
};

struct LlmMessage {
    std::string role;
    std::string content;
};

struct LlmResponse {
    std::string raw_text;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    std::chrono::milliseconds latency{0};
};

class LlmClient {
public:
    struct Options {
        std::string model = "gpt-4o-mini";
        int max_tokens = 1024;
        double temperature = 0.3;
        // Reasoning models (e.g. deepseek-v4-flash) can take many seconds;
        // must exceed the gateway's upstream timeout so the client waits
        // long enough for a response or graceful fallback.
        std::chrono::milliseconds timeout{45000};
        int max_retries = 1;
    };

    virtual ~LlmClient() = default;

    virtual coro::Task<LlmResponse> Chat(
        const std::vector<LlmMessage>& messages,
        const Options& options) = 0;

    virtual bool Healthy() const = 0;
};

// Simple HTTP-based LLM client implementation (Phase 0)
class HttpLlmClient : public LlmClient {
public:
    HttpLlmClient(std::string base_url, std::string api_key);

    coro::Task<LlmResponse> Chat(
        const std::vector<LlmMessage>& messages,
        const Options& options) override;

    bool Healthy() const override;

private:
    std::string base_url_;
    std::string api_key_;
    std::atomic<bool> healthy_{true};
};

} // namespace agent
