#pragma once

#include "agent/common.hpp"
#include "agent/llm_client.hpp"
#include "agent/stream_emitter.hpp"
#include "coro/core/task.hpp"

#include <nlohmann/json.hpp>

#include <memory>

namespace agent {

class ResponseComposer {
public:
    explicit ResponseComposer(std::shared_ptr<LlmClient> llm);

    // Compose the final recommendation reply.
    //
    // When `emitter` is null (non-streaming endpoint): if the LLM is available
    // and returns a parseable reply, uses natural-language generation and may
    // enrich each item's `reason` from the model's per-item explanations;
    // otherwise degrades to a deterministic templated reply.
    //
    // When `emitter` is set (streaming endpoint): streams the reply token by
    // token as `delta` events via the LLM's ChatStream. If real streaming is
    // unavailable (offline / gateway down), emits the templated reply as a
    // sequence of deltas so the client still observes the streaming wire format.
    //
    // `grounding` is an optional block of knowledge-base passages (RAG) that is
    // injected into the LLM prompt (both streaming and non-streaming) so the
    // model answers factual questions from sourced text instead of guessing.
    // It is ignored by the deterministic template fallback. Empty = no RAG.
    coro::Task<RecommendationResult> Compose(
        const std::string& user_request,
        const nlohmann::json& slots,
        const std::vector<RecommendationItem>& items,
        std::shared_ptr<StreamEmitter> emitter = nullptr,
        const std::string& grounding = "");

private:
    std::shared_ptr<LlmClient> llm_;
};

} // namespace agent
