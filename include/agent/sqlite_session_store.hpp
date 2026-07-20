#pragma once

#include "agent/session_memory.hpp"

#include <mutex>
#include <string>

struct sqlite3;  // forward declaration — header does not include sqlite3.h

namespace agent {

// SQLite-backed session store. Persists to a single .db file so sessions
// survive process restarts. The schema is plain SQL and ports directly to
// PostgreSQL. Drop-in replacement for InMemorySessionStore.
class SqliteSessionStore : public SessionMemoryStore {
public:
    // Opens (or creates) the database file. Parent directories are created
    // if missing. Throws on failure to open or initialise the schema.
    explicit SqliteSessionStore(const std::string& db_path);
    ~SqliteSessionStore() override;

    SqliteSessionStore(const SqliteSessionStore&) = delete;
    SqliteSessionStore& operator=(const SqliteSessionStore&) = delete;

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

private:
    void InitSchema();

    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};

} // namespace agent
