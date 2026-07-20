#include "agent/sqlite_session_store.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

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
    static std::atomic<int> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "s-" + std::to_string(now) + "-" + std::to_string(++counter);
}

// Read column i as a string (empty if NULL).
std::string ColStr(sqlite3_stmt* stmt, int i) {
    const unsigned char* txt = sqlite3_column_text(stmt, i);
    return txt ? reinterpret_cast<const char*>(txt) : std::string{};
}

// RAII guard for a prepared statement.
struct StmtGuard {
    sqlite3_stmt* s = nullptr;
    explicit StmtGuard(sqlite3_stmt* stmt) : s(stmt) {}
    ~StmtGuard() { if (s) sqlite3_finalize(s); }
    StmtGuard(const StmtGuard&) = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;
};

} // namespace

SqliteSessionStore::SqliteSessionStore(const std::string& db_path) {
    // SQLite creates the file but not its parent directory.
    if (auto parent = std::filesystem::path(db_path).parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    if (sqlite3_open_v2(db_path.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        const std::string err = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SqliteSessionStore open failed: " + err);
    }
    InitSchema();
    spdlog::info("SqliteSessionStore opened: {}", db_path);
}

SqliteSessionStore::~SqliteSessionStore() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void SqliteSessionStore::InitSchema() {
    // Serialized writes within a single connection; foreign keys enforced.
    auto exec = [this](const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("SQLite schema error: " + msg);
        }
    };
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            session_id    TEXT PRIMARY KEY,
            user_id       TEXT,
            current_state TEXT NOT NULL DEFAULT 'SLOT_FILL',
            context       TEXT NOT NULL DEFAULT '{}',
            created_at    TEXT,
            updated_at    TEXT
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS turns (
            rowid      INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id TEXT NOT NULL,
            role       TEXT,
            content    TEXT,
            created_at TEXT,
            FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_turns_session ON turns(session_id, rowid);");
}

coro::Task<SessionMemoryStore::Session> SqliteSessionStore::GetOrCreateSession(
    const std::optional<std::string>& session_id,
    const UserContext& ctx) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string provided = session_id.value_or("");
    if (!provided.empty()) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT session_id, user_id, current_state, context, created_at, updated_at "
                "FROM sessions WHERE session_id=?1",
                -1, &raw, nullptr) == SQLITE_OK) {
            StmtGuard g(raw);
            sqlite3_bind_text(raw, 1, provided.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(raw) == SQLITE_ROW) {
                Session s;
                s.session_id = ColStr(raw, 0);
                s.user_id = ColStr(raw, 1);
                s.current_state = ColStr(raw, 2);
                s.context = nlohmann::json::parse(ColStr(raw, 3));
                s.created_at = ColStr(raw, 4);
                s.updated_at = ColStr(raw, 5);
                co_return s;
            }
        }
    }

    const std::string sid = GenerateSessionId();
    const std::string now = NowIso8601();
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO sessions (session_id, user_id, current_state, context, created_at, updated_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            -1, &raw, nullptr) != SQLITE_OK) {
        co_return Session{};
    }
    StmtGuard g(raw);
    sqlite3_bind_text(raw, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, ctx.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 3, "SLOT_FILL", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 4, "{}", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 5, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 6, now.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        spdlog::warn("SQLite insert session failed: {}", sqlite3_errmsg(db_));
    }

    Session s;
    s.session_id = sid;
    s.user_id = ctx.user_id;
    s.current_state = "SLOT_FILL";
    s.context = nlohmann::json::object();
    s.created_at = now;
    s.updated_at = now;
    co_return s;
}

coro::Task<Status> SqliteSessionStore::AppendTurn(
    const std::string& session_id,
    const ConversationTurn& turn) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO turns (session_id, role, content, created_at) VALUES (?1, ?2, ?3, ?4)",
            -1, &raw, nullptr) != SQLITE_OK) {
        co_return Status::Error(1, std::string("prepare failed: ") + sqlite3_errmsg(db_));
    }
    StmtGuard g(raw);
    sqlite3_bind_text(raw, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, turn.role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 3, turn.content.c_str(), -1, SQLITE_TRANSIENT);
    const std::string now = NowIso8601();
    sqlite3_bind_text(raw, 4, now.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        // FK violation => unknown session, or other error.
        co_return Status::Error(1, std::string("append turn failed: ") + sqlite3_errmsg(db_));
    }
    co_return Status::OK();
}

coro::Task<std::vector<ConversationTurn>> SqliteSessionStore::GetRecentTurns(
    const std::string& session_id,
    int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT role, content, created_at FROM turns "
            "WHERE session_id=?1 ORDER BY rowid DESC LIMIT ?2",
            -1, &raw, nullptr) != SQLITE_OK) {
        co_return std::vector<ConversationTurn>{};
    }
    StmtGuard g(raw);
    sqlite3_bind_text(raw, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(raw, 2, limit);

    std::vector<ConversationTurn> turns;
    while (sqlite3_step(raw) == SQLITE_ROW) {
        ConversationTurn t;
        t.role = ColStr(raw, 0);
        t.content = ColStr(raw, 1);
        t.created_at = ColStr(raw, 2);
        turns.push_back(std::move(t));
    }
    // Reverse to chronological order (oldest first).
    std::reverse(turns.begin(), turns.end());
    co_return turns;
}

coro::Task<Status> SqliteSessionStore::UpdateContext(
    const std::string& session_id,
    const std::string& state,
    const nlohmann::json& slots) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE sessions SET current_state=?1, context=?2, updated_at=?3 WHERE session_id=?4",
            -1, &raw, nullptr) != SQLITE_OK) {
        co_return Status::Error(1, std::string("prepare failed: ") + sqlite3_errmsg(db_));
    }
    StmtGuard g(raw);
    const std::string slots_str = slots.dump();
    const std::string now = NowIso8601();
    sqlite3_bind_text(raw, 1, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, slots_str.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 3, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 4, session_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        co_return Status::Error(1, std::string("update failed: ") + sqlite3_errmsg(db_));
    }
    if (sqlite3_changes(db_) == 0) {
        co_return Status::Error(1, "session not found: " + session_id);
    }
    co_return Status::OK();
}

} // namespace agent
