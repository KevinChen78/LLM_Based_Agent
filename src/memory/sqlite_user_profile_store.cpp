#include "agent/user_profile_store.hpp"

#include <sqlite3.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

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

SqliteUserProfileStore::SqliteUserProfileStore(const std::string& db_path) {
    if (auto parent = std::filesystem::path(db_path).parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    if (sqlite3_open_v2(db_path.c_str(), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
        const std::string err = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SqliteUserProfileStore open failed: " + err);
    }
    InitSchema();
    spdlog::info("SqliteUserProfileStore opened: {}", db_path);
}

SqliteUserProfileStore::~SqliteUserProfileStore() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void SqliteUserProfileStore::InitSchema() {
    auto exec = [this](const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("SqliteUserProfileStore schema error: " + msg);
        }
    };
    // WAL so this connection coexists with SqliteSessionStore's on the same file.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA busy_timeout=2000;");
    exec(R"(
        CREATE TABLE IF NOT EXISTS user_profiles (
            user_id              TEXT PRIMARY KEY,
            preferred_cities     TEXT NOT NULL DEFAULT '[]',
            preferred_categories TEXT NOT NULL DEFAULT '[]',
            price_sensitivity    REAL NOT NULL DEFAULT 0.5,
            dietary_tags         TEXT NOT NULL DEFAULT '[]',
            avg_budget           REAL NOT NULL DEFAULT 0,
            updated_at           TEXT
        );
    )");
}

std::optional<UserProfile> SqliteUserProfileStore::Get(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT preferred_cities, preferred_categories, price_sensitivity,"
            " dietary_tags, avg_budget, updated_at"
            " FROM user_profiles WHERE user_id = ?1",
            -1, &raw, nullptr) != SQLITE_OK) {
        spdlog::warn("UserProfileStore::Get prepare failed: {}", sqlite3_errmsg(db_));
        return std::nullopt;
    }
    StmtGuard g(raw);
    sqlite3_bind_text(raw, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_ROW) return std::nullopt;

    UserProfile p;
    p.user_id = user_id;
    auto parse_arr = [](const std::string& s) {
        std::vector<std::string> out;
        try {
            auto j = nlohmann::json::parse(s);
            if (j.is_array()) {
                for (const auto& v : j) {
                    if (v.is_string()) out.push_back(v.get<std::string>());
                }
            }
        } catch (const std::exception&) {}
        return out;
    };
    p.preferred_cities = parse_arr(ColStr(raw, 0));
    p.preferred_categories = parse_arr(ColStr(raw, 1));
    p.price_sensitivity = sqlite3_column_double(raw, 2);
    p.dietary_tags = parse_arr(ColStr(raw, 3));
    p.avg_budget = sqlite3_column_double(raw, 4);
    p.updated_at = ColStr(raw, 5);
    return p;
}

bool SqliteUserProfileStore::Upsert(const UserProfile& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO user_profiles "
            "(user_id, preferred_cities, preferred_categories, price_sensitivity,"
            " dietary_tags, avg_budget, updated_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
            "ON CONFLICT(user_id) DO UPDATE SET "
            " preferred_cities=excluded.preferred_cities,"
            " preferred_categories=excluded.preferred_categories,"
            " price_sensitivity=excluded.price_sensitivity,"
            " dietary_tags=excluded.dietary_tags,"
            " avg_budget=excluded.avg_budget,"
            " updated_at=excluded.updated_at",
            -1, &raw, nullptr) != SQLITE_OK) {
        spdlog::warn("UserProfileStore::Upsert prepare failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    StmtGuard g(raw);
    auto dump_arr = [](const std::vector<std::string>& v) {
        nlohmann::json j = v;
        return j.dump();
    };
    const std::string cities = dump_arr(p.preferred_cities);
    const std::string cats = dump_arr(p.preferred_categories);
    const std::string tags = dump_arr(p.dietary_tags);
    const std::string now = NowIso8601();
    sqlite3_bind_text(raw, 1, p.user_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 2, cities.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(raw, 3, cats.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(raw, 4, p.price_sensitivity);
    sqlite3_bind_text(raw, 5, tags.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(raw, 6, p.avg_budget);
    sqlite3_bind_text(raw, 7, now.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_DONE) {
        spdlog::warn("UserProfileStore::Upsert failed: {}", sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

std::vector<FeedbackSignal> SqliteUserProfileStore::LoadFeedbackSignals(
    const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FeedbackSignal> out;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT f.item_id, f.feedback_type FROM feedback f"
            " JOIN sessions s ON s.session_id = f.session_id"
            " WHERE s.user_id = ?1 AND f.item_id IS NOT NULL AND f.item_id != ''",
            -1, &raw, nullptr) != SQLITE_OK) {
        spdlog::warn("LoadFeedbackSignals prepare failed: {}", sqlite3_errmsg(db_));
        return out;
    }
    StmtGuard g(raw);
    sqlite3_bind_text(raw, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(raw) == SQLITE_ROW) {
        out.push_back({ColStr(raw, 0), ColStr(raw, 1)});
    }
    return out;
}

std::vector<SlotRecord> SqliteUserProfileStore::LoadSlotHistory(
    const std::string& user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SlotRecord> out;
    sqlite3_stmt* raw = nullptr;
    // Sessions only keep the LATEST slots per session — good enough for
    // budget/taboo aggregates (older intermediate values are approximations
    // anyway as the LLM refines slots within a session).
    if (sqlite3_prepare_v2(db_,
            "SELECT context FROM sessions WHERE user_id = ?1",
            -1, &raw, nullptr) != SQLITE_OK) {
        spdlog::warn("LoadSlotHistory prepare failed: {}", sqlite3_errmsg(db_));
        return out;
    }
    StmtGuard g(raw);
    sqlite3_bind_text(raw, 1, user_id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(raw) == SQLITE_ROW) {
        try {
            auto ctx = nlohmann::json::parse(ColStr(raw, 0));
            SlotRecord rec;
            rec.budget = ctx.value("budget", 0.0);
            rec.taboo = ctx.value("taboo", "");
            if (rec.budget > 0.0 || !rec.taboo.empty()) out.push_back(std::move(rec));
        } catch (const std::exception&) {}
    }
    return out;
}

} // namespace agent
