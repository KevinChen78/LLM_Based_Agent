#pragma once

#include "agent/common.hpp"
#include "agent/stream_emitter.hpp"

#include "coro/core/task.hpp"

#include <memory>
#include <string>
#include <vector>

namespace agent {

// Forward declarations
class TaskPlanner;
class ToolRegistry;
class SessionMemoryStore;
class LlmClient;
class ResponseComposer;
class SafetyGuard;
class ObservabilityStore;

class AgentOrchestrator {
public:
    struct Request {
        UserContext user_context;
        std::string user_message;
    };

    AgentOrchestrator(
        std::shared_ptr<TaskPlanner> planner,
        std::shared_ptr<ToolRegistry> tools,
        std::shared_ptr<SessionMemoryStore> memory,
        std::shared_ptr<LlmClient> llm,
        std::shared_ptr<ResponseComposer> composer,
        std::shared_ptr<SafetyGuard> guard = nullptr,
        std::shared_ptr<ObservabilityStore> obs = nullptr);

    // Non-streaming chat: returns the complete result.
    coro::Task<RecommendationResult> Chat(Request req);

    // Streaming chat: emits pipeline events through `emitter` and finally
    // returns the same result that the non-streaming path would produce.
    // Passing emitter=nullptr degrades to the non-streaming behaviour.
    coro::Task<RecommendationResult> ChatStream(
        Request req, std::shared_ptr<StreamEmitter> emitter = nullptr);

private:
    // Audit data collected while handling one request, written to the
    // observability store once at the single outer exit of ChatStream.
    struct RecAudit {
        std::vector<LlmCallInfo> llm_calls;  // planner attempts + composer call
        std::string slots_json;
    };

    coro::Task<RecommendationResult> ChatStreamInner(
        Request req, std::shared_ptr<StreamEmitter> emitter, RecAudit* audit);

    std::shared_ptr<TaskPlanner> planner_;
    std::shared_ptr<ToolRegistry> tools_;
    std::shared_ptr<SessionMemoryStore> memory_;
    std::shared_ptr<LlmClient> llm_;
    std::shared_ptr<ResponseComposer> composer_;
    std::shared_ptr<SafetyGuard> guard_;
    std::shared_ptr<ObservabilityStore> obs_;
};

} // namespace agent
