#include "agent/session_memory.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace agent {

namespace {

std::string NowIso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%dT%H:%M:%S");
    return ss.str();
}

std::string GenerateSessionId() {
    static int counter = 0;
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "s-" + std::to_string(now) + "-" + std::to_string(++counter);
}

} // namespace

coro::Task<SessionMemoryStore::Session> InMemorySessionStore::GetOrCreateSession(
    const std::optional<std::string>& session_id,
    const UserContext& ctx) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string sid = session_id.value_or("");
    if (sid.empty() || sessions_.find(sid) == sessions_.end()) {
        sid = GenerateSessionId();
        Session s;
        s.session_id = sid;
        s.user_id = ctx.user_id;
        s.current_state = "SLOT_FILL";
        s.context = nlohmann::json::object();
        s.created_at = NowIso8601();
        s.updated_at = s.created_at;
        sessions_[sid] = Data{.session = s, .turns = {}};
        co_return s;
    }
    co_return sessions_[sid].session;
}

coro::Task<Status> InMemorySessionStore::AppendTurn(
    const std::string& session_id,
    const ConversationTurn& turn) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        co_return Status::Error(1, "session not found: " + session_id);
    }
    it->second.turns.push_back(turn);
    co_return Status::OK();
}

coro::Task<std::vector<ConversationTurn>> InMemorySessionStore::GetRecentTurns(
    const std::string& session_id,
    int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        co_return std::vector<ConversationTurn>{};
    }
    const auto& turns = it->second.turns;
    int start = static_cast<int>(turns.size()) - limit;
    if (start < 0) start = 0;
    co_return std::vector<ConversationTurn>(turns.begin() + start, turns.end());
}

coro::Task<Status> InMemorySessionStore::UpdateContext(
    const std::string& session_id,
    const std::string& state,
    const nlohmann::json& slots) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        co_return Status::Error(1, "session not found: " + session_id);
    }
    it->second.session.current_state = state;
    it->second.session.context = slots;
    it->second.session.updated_at = NowIso8601();
    co_return Status::OK();
}

} // namespace agent
