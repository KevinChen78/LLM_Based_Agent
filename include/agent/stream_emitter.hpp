#pragma once

#include "agent/common.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace agent {

// Abstract sink for streaming events emitted by the agent pipeline.
// Implementations are responsible for transport formatting (e.g. SSE).
class StreamEmitter {
public:
    virtual ~StreamEmitter() = default;

    // Emit a typed event. `payload` is the event-specific data object.
    virtual void Emit(const std::string& event_type, const nlohmann::json& payload) = 0;

    // Emit a text delta. Reserved for future token-level LLM streaming.
    virtual void EmitDelta(const std::string& text_delta) = 0;

    // Signal completion and optionally attach the final result.
    virtual void Finish(const RecommendationResult& result) = 0;

    // Signal an error and close the stream.
    virtual void Error(const std::string& message) = 0;

    // Returns true if the consumer side has disconnected or the emitter was closed.
    virtual bool IsClosed() const = 0;
};

} // namespace agent
