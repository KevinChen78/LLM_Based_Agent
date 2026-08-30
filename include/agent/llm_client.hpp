#pragma once

#include "agent/common.hpp"
#include "agent/service_circuit.hpp"
#include "coro/core/task.hpp"

#include <atomic>
#include <chrono>
#include <functional>
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
    std::string model;            // echo of the upstream model field (may be empty)
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

    // Streaming variant. `on_delta` is invoked for each incremental text chunk
    // as the model emits it. The default implementation falls back to a single
    // Chat() call with streamed=false (so clients without real streaming
    // support inherit a sane, non-streaming behavior).
    using DeltaCallback = std::function<void(const std::string&)>;

    struct LlmStreamResult {
        std::string text;                       // accumulated full text
        bool streamed = false;                  // true only if real token streaming occurred
        std::chrono::milliseconds latency{0};
        // Token usage from the trailing `usage` SSE chunk (gateway requests
        // stream_options.include_usage; stub emits an estimate). 0 when the
        // upstream did not send usage — recorded honestly.
        int prompt_tokens = 0;
        int completion_tokens = 0;
    };

    virtual coro::Task<LlmStreamResult> ChatStream(
        const std::vector<LlmMessage>& messages,
        const Options& options,
        const DeltaCallback& on_delta);

    virtual bool Healthy() const = 0;
};

// Simple HTTP-based LLM client implementation (Phase 0)
class HttpLlmClient : public LlmClient {
public:
    HttpLlmClient(std::string base_url, std::string api_key);

    coro::Task<LlmResponse> Chat(
        const std::vector<LlmMessage>& messages,
        const Options& options) override;

    // Real streaming via HTTP chunked SSE. When base_url is empty (offline
    // stub), returns streamed=false immediately so callers can fall back.
    coro::Task<LlmStreamResult> ChatStream(
        const std::vector<LlmMessage>& messages,
        const Options& options,
        const DeltaCallback& on_delta) override;

    bool Healthy() const override;

private:
    std::string base_url_;
    std::string api_key_;
    std::atomic<bool> healthy_{true};
    // Phase 9-A0: gateway-absence circuit breaker (shared implementation with
    // the retrieval/ranker clients). A dead gateway costs ~2.05s per refused
    // connect on Windows (×2 for localhost dual-stack); after breaker
    // threshold consecutive transport failures, Chat/ChatStream fail fast
    // into the existing fallback chain (planner default action / template
    // reply) instead of re-paying the refusal tax on every turn.
    ServiceCircuit circuit_;
};

} // namespace agent
