#pragma once

#include "agent/common.hpp"
#include "coro/core/task.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

class SessionMemoryStore {
public:
    virtual ~SessionMemoryStore() = default;

    struct Session {
        std::string session_id;
        std::string user_id;
        std::string current_state = "SLOT_FILL";
        nlohmann::json context = nlohmann::json::object();
        std::string created_at;
        std::string updated_at;
    };

    virtual coro::Task<Session> GetOrCreateSession(
        const std::optional<std::string>& session_id,
        const UserContext& ctx) = 0;

    virtual coro::Task<Status> AppendTurn(
        const std::string& session_id,
        const ConversationTurn& turn) = 0;

    virtual coro::Task<std::vector<ConversationTurn>> GetRecentTurns(
        const std::string& session_id,
        int limit = 10) = 0;

    virtual coro::Task<Status> UpdateContext(
        const std::string& session_id,
        const std::string& state,
        const nlohmann::json& slots) = 0;

    // One piece of user feedback on a reply (item_id empty) or a single deal
    // card. FK semantics mirror AppendTurn: unknown session_id is an error.
    struct FeedbackRecord {
        std::string session_id;
        std::string trace_id;
        std::string item_id;        // empty = whole-reply feedback
        std::string feedback_type;  // like / dislike
        std::string comment;        // optional
    };

    virtual coro::Task<Status> AppendFeedback(const FeedbackRecord& rec) = 0;
};

// In-memory implementation for Phase 0
class InMemorySessionStore : public SessionMemoryStore {
public:
    coro::Task<Session> GetOrCreateSession(
        const std::optional<std::string>& session_id,
        const UserContext& ctx) override;

    coro::Task<Status> AppendTurn(
        const std::string& session_id,
        const ConversationTurn& turn) override;

    coro::Task<std::vector<ConversationTurn>> GetRecentTurns(
        const std::string& session_id,
        int limit = 10) override;

    coro::Task<Status> UpdateContext(
        const std::string& session_id,
        const std::string& state,
        const nlohmann::json& slots) override;

    coro::Task<Status> AppendFeedback(const FeedbackRecord& rec) override;

private:
    struct Data {
        Session session;
        std::vector<ConversationTurn> turns;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, Data> sessions_;
    std::vector<FeedbackRecord> feedback_;
    int next_turn_id_ = 0;
};

} // namespace agent
