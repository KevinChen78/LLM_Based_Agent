#include "agent/observability_store.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <functional>
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

std::string ColStr(sqlite3_stmt* stmt, int i) {
    const unsigned char* txt = sqlite3_column_text(stmt, i);
    return txt ? reinterpret_cast<const char*>(txt) : std::string{};
}

struct StmtGuard {
    sqlite3_stmt* s = nullptr;
    explicit StmtGuard(sqlite3_stmt* stmt) : s(stmt) {}
    ~StmtGuard() { if (s) sqlite3_finalize(s); }
    StmtGuard(const StmtGuard&) = delete;
    StmtGuard& operator=(const StmtGuard&) = delete;
};

} // namespace

ObservabilityStore::ObservabilityStore(const std::string& db_path) {
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
        throw std::runtime_error("ObservabilityStore open failed: " + err);
    }
    InitSchema();
    spdlog::info("ObservabilityStore opened: {}", db_path);
}

ObservabilityStore::~ObservabilityStore() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void ObservabilityStore::InitSchema() {
    auto exec = [this](const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("ObservabilityStore schema init failed: " + msg);
        }
    };
    // Idempotent migration: ALTER TABLE ADD COLUMN only when the column is
    // absent, so existing database files upgrade in place.
    auto add_column_if_missing = [this, &exec](const char* table, const char* col,
                                               const char* ddl_type) {
        sqlite3_stmt* raw = nullptr;
        const std::string pragma = std::string("PRAGMA table_info(") + table + ")";
        if (sqlite3_prepare_v2(db_, pragma.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
            throw std::runtime_error("ObservabilityStore schema init failed: " +
                                     std::string(sqlite3_errmsg(db_)));
        }
        bool found = false;
        {
            StmtGuard g(raw);
            while (sqlite3_step(raw) == SQLITE_ROW) {
                if (ColStr(raw, 1) == col) { found = true; break; }
            }
        }
        if (!found) {
            const std::string alter = std::string("ALTER TABLE ") + table +
                                      " ADD COLUMN " + col + " " + ddl_type;
            exec(alter.c_str());
        }
    };
    // WAL so the api_server connection and offline evaluators can coexist;
    // busy_timeout smooths over concurrent single-writer conflicts.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA busy_timeout=2000;");
    exec(R"(
        CREATE TABLE IF NOT EXISTS recommendation_logs (
            rowid           INTEGER PRIMARY KEY AUTOINCREMENT,
            trace_id        TEXT NOT NULL,
            session_id      TEXT,
            user_id         TEXT,
            request_text    TEXT,
            action          TEXT,
            slots_json      TEXT,
            item_count      INTEGER DEFAULT 0,
            ranked_items    TEXT,
            response_text   TEXT,
            grounding_count INTEGER DEFAULT 0,
            compose_mode    TEXT,
            latency_ms      INTEGER,
            created_at      TEXT
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_rec_logs_trace ON recommendation_logs(trace_id);");
    exec("CREATE INDEX IF NOT EXISTS idx_rec_logs_session ON recommendation_logs(session_id, rowid);");
    // Phase 2.1 learning-to-rank audit columns (added after the base table so
    // existing databases pick them up via ALTER).
    add_column_if_missing("recommendation_logs", "candidates_json", "TEXT");
    add_column_if_missing("recommendation_logs", "experiment_group", "TEXT");
    add_column_if_missing("recommendation_logs", "rank_mode", "TEXT");
    // Phase 4-C guard audit columns.
    add_column_if_missing("recommendation_logs", "guard_action", "TEXT");
    add_column_if_missing("recommendation_logs", "guard_detail", "TEXT");
    exec(R"(
        CREATE TABLE IF NOT EXISTS llm_calls (
            rowid             INTEGER PRIMARY KEY AUTOINCREMENT,
            trace_id          TEXT NOT NULL,
            session_id        TEXT,
            purpose           TEXT,
            model             TEXT,
            prompt_tokens     INTEGER DEFAULT 0,
            completion_tokens INTEGER DEFAULT 0,
            latency_ms        INTEGER,
            status            TEXT,
            attempt           INTEGER DEFAULT 0,
            created_at        TEXT
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_llm_calls_trace ON llm_calls(trace_id);");
    // Phase 2.3-D: user-turn prompt audit (profile-injection contrast checks).
    add_column_if_missing("llm_calls", "raw_request", "TEXT");
}

void ObservabilityStore::LogRecommendation(const RecLogEntry& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO recommendation_logs "
            "(trace_id, session_id, user_id, request_text, action, slots_json, "
            " item_count, ranked_items, response_text, grounding_count, compose_mode, "
            " latency_ms, created_at, candidates_json, experiment_group, rank_mode, "
            " guard_action, guard_detail) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18)",
            -1, &raw, nullptr) != SQLITE_OK) {
        spdlog::warn("LogRecommendation prepare failed: {}", sqlite3_errmsg(db_));
        return;
    }
    StmtGuard g(raw);
    const std::string now = NowIso8601();
    sqlite3_bind_text(raw, 1, e.trace_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, e.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 3, e.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 4, e.request_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 5, e.action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 6, e.slots_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(raw, 7, e.item_count);
    sqlite3_bind_text(raw, 8, e.ranked_items_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 9, e.response_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(raw, 10, e.grounding_count);
    sqlite3_bind_text(raw, 11, e.compose_mode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(raw, 12, e.latency_ms);
    sqlite3_bind_text(raw, 13, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 14, e.candidates_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 15, e.experiment_group.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 16, e.rank_mode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 17, e.guard_action.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 18, e.guard_detail.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        // Audit writes must never break the recommendation path.
        spdlog::warn("LogRecommendation insert failed: {}", sqlite3_errmsg(db_));
    }
}

void ObservabilityStore::LogLlmCall(const LlmCallEntry& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO llm_calls "
            "(trace_id, session_id, purpose, model, prompt_tokens, completion_tokens, "
            " latency_ms, status, attempt, created_at, raw_request) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
            -1, &raw, nullptr) != SQLITE_OK) {
        spdlog::warn("LogLlmCall prepare failed: {}", sqlite3_errmsg(db_));
        return;
    }
    StmtGuard g(raw);
    const std::string now = NowIso8601();
    sqlite3_bind_text(raw, 1, e.trace_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, e.session_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 3, e.purpose.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 4, e.model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(raw, 5, e.prompt_tokens);
    sqlite3_bind_int(raw, 6, e.completion_tokens);
    sqlite3_bind_int64(raw, 7, e.latency_ms);
    sqlite3_bind_text(raw, 8, e.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(raw, 9, e.attempt);
    sqlite3_bind_text(raw, 10, now.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 11, e.raw_request.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        spdlog::warn("LogLlmCall insert failed: {}", sqlite3_errmsg(db_));
    }
}

nlohmann::json ObservabilityStore::Aggregate(const std::string& sessions_db_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json out;

    // Run a query and visit each row; returns false on prepare error.
    auto query = [this](const char* sql, const std::function<void(sqlite3_stmt*)>& row) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK) {
            spdlog::warn("Aggregate query failed: {}", sqlite3_errmsg(db_));
            return false;
        }
        StmtGuard g(raw);
        while (sqlite3_step(raw) == SQLITE_ROW) row(raw);
        return true;
    };

    // --- requests ---
    nlohmann::json by_action = nlohmann::json::object();
    long long total = 0;
    query("SELECT COALESCE(action,''), COUNT(*), AVG(latency_ms) FROM recommendation_logs GROUP BY action",
          [&](sqlite3_stmt* st) {
              by_action[ColStr(st, 0)] = sqlite3_column_int64(st, 1);
              total += sqlite3_column_int64(st, 1);
          });
    double avg_latency = 0.0;
    query("SELECT AVG(latency_ms) FROM recommendation_logs",
          [&](sqlite3_stmt* st) { avg_latency = sqlite3_column_double(st, 0); });
    long long p95_latency = 0;
    query("SELECT MAX(latency_ms) FROM ("
          "  SELECT latency_ms FROM recommendation_logs ORDER BY latency_ms "
          "  LIMIT 1 OFFSET (SELECT MAX(CAST(COUNT(*)*0.95 AS INT)-1, 0) FROM recommendation_logs)"
          ")",
          [&](sqlite3_stmt* st) { p95_latency = sqlite3_column_int64(st, 0); });
    long long fallback = 0, retrieve_total = 0, retrieve_empty = 0, grounded = 0;
    query("SELECT COUNT(*) FROM recommendation_logs WHERE action IN ('FALLBACK','ERROR')",
          [&](sqlite3_stmt* st) { fallback = sqlite3_column_int64(st, 0); });
    query("SELECT COUNT(*), SUM(CASE WHEN item_count=0 AND grounding_count=0 THEN 1 ELSE 0 END) "
          "FROM recommendation_logs WHERE action='retrieve'",
          [&](sqlite3_stmt* st) {
              retrieve_total = sqlite3_column_int64(st, 0);
              retrieve_empty = sqlite3_column_type(st, 1) == SQLITE_NULL
                                   ? 0 : sqlite3_column_int64(st, 1);
          });
    query("SELECT COUNT(*) FROM recommendation_logs WHERE grounding_count>0",
          [&](sqlite3_stmt* st) { grounded = sqlite3_column_int64(st, 0); });

    out["requests"] = {
        {"total", total},
        {"by_action", by_action},
        {"fallback_rate", total ? double(fallback) / double(total) : 0.0},
        {"empty_retrieve_rate", retrieve_total ? double(retrieve_empty) / double(retrieve_total) : 0.0},
        {"grounded_requests", grounded},
        {"avg_latency_ms", avg_latency},
        {"p95_latency_ms", p95_latency},
    };

    // --- llm calls ---
    nlohmann::json by_purpose = nlohmann::json::object();
    long long llm_total = 0, prompt_sum = 0, completion_sum = 0;
    query("SELECT COALESCE(purpose,''), COUNT(*), AVG(latency_ms), "
          "SUM(prompt_tokens), SUM(completion_tokens) FROM llm_calls GROUP BY purpose",
          [&](sqlite3_stmt* st) {
              by_purpose[ColStr(st, 0)] = {
                  {"calls", sqlite3_column_int64(st, 1)},
                  {"avg_latency_ms", sqlite3_column_double(st, 2)},
                  {"prompt_tokens", sqlite3_column_type(st, 3) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, 3)},
                  {"completion_tokens", sqlite3_column_type(st, 4) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, 4)},
              };
              llm_total += sqlite3_column_int64(st, 1);
          });
    query("SELECT SUM(prompt_tokens), SUM(completion_tokens) FROM llm_calls",
          [&](sqlite3_stmt* st) {
              prompt_sum = sqlite3_column_type(st, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, 0);
              completion_sum = sqlite3_column_type(st, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, 1);
          });
    out["llm"] = {
        {"total_calls", llm_total},
        {"by_purpose", by_purpose},
        {"prompt_tokens", prompt_sum},
        {"completion_tokens", completion_sum},
        {"note", "streaming compose calls report tokens=0 (upstream sends no usage)"},
    };

    // --- feedback (sessions DB via ATTACH) ---
    if (!sessions_db_path.empty()) {
        sqlite3_stmt* raw = nullptr;
        const std::string attach = "ATTACH DATABASE ? AS sessions_db";
        if (sqlite3_prepare_v2(db_, attach.c_str(), -1, &raw, nullptr) == SQLITE_OK) {
            {
                StmtGuard g(raw);
                sqlite3_bind_text(raw, 1, sessions_db_path.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(raw) == SQLITE_DONE) {
                    long long likes = 0, dislikes = 0;
                    query("SELECT feedback_type, COUNT(*) FROM sessions_db.feedback GROUP BY feedback_type",
                          [&](sqlite3_stmt* st) {
                              if (ColStr(st, 0) == "like") likes = sqlite3_column_int64(st, 1);
                              if (ColStr(st, 0) == "dislike") dislikes = sqlite3_column_int64(st, 1);
                          });
                    out["feedback"] = {
                        {"like", likes},
                        {"dislike", dislikes},
                        {"satisfaction", (likes + dislikes) ? double(likes) / double(likes + dislikes) : 0.0},
                    };
                }
            }
            // Detach best-effort (only meaningful if attach succeeded).
            char* err = nullptr;
            sqlite3_exec(db_, "DETACH DATABASE sessions_db", nullptr, nullptr, &err);
            if (err) sqlite3_free(err);
        }
    }

    return out;
}

} // namespace agent
