#include "agent/sqlite_session_store.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace agent;

namespace {

std::string TempDbPath() {
    static int n = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("llm_agent_test_" + std::to_string(++n) + ".db");
    std::error_code ec;
    std::filesystem::remove(p, ec);  // clean start
    return p.string();
}

// Read all feedback rows directly via the sqlite3 C API — the store
// deliberately has no feedback read API, so tests verify at the table level.
struct FeedbackRow {
    std::string session_id, trace_id, item_id, feedback_type, comment;
};

std::vector<FeedbackRow> ReadFeedback(const std::string& db_path) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
    std::vector<FeedbackRow> rows;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT session_id, trace_id, item_id, feedback_type, comment "
            "FROM feedback ORDER BY rowid",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            auto col = [&](int i) {
                const auto* p = sqlite3_column_text(st, i);
                return p ? reinterpret_cast<const char*>(p) : "";
            };
            rows.push_back(FeedbackRow{col(0), col(1), col(2), col(3), col(4)});
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rows;
}

} // namespace

TEST(SqliteSessionStore, CreatesNewSession) {
    SqliteSessionStore store(TempDbPath());
    UserContext ctx;
    ctx.user_id = "u1";
    auto s = store.GetOrCreateSession(std::nullopt, ctx).result();
    EXPECT_FALSE(s.session_id.empty());
    EXPECT_EQ(s.user_id, "u1");
    EXPECT_EQ(s.current_state, "SLOT_FILL");
}

TEST(SqliteSessionStore, AppendsAndReadsTurnsChronologically) {
    SqliteSessionStore store(TempDbPath());
    UserContext ctx;
    ctx.user_id = "u1";
    auto s = store.GetOrCreateSession(std::nullopt, ctx).result();

    EXPECT_TRUE(store.AppendTurn(s.session_id,
        ConversationTurn{.role = "user", .content = "hi"}).result().ok());
    EXPECT_TRUE(store.AppendTurn(s.session_id,
        ConversationTurn{.role = "assistant", .content = "hello"}).result().ok());

    auto turns = store.GetRecentTurns(s.session_id, 10).result();
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].role, "user");
    EXPECT_EQ(turns[1].role, "assistant");
}

TEST(SqliteSessionStore, UpdatesContextAndReadsBack) {
    SqliteSessionStore store(TempDbPath());
    UserContext ctx;
    ctx.user_id = "u1";
    auto s = store.GetOrCreateSession(std::nullopt, ctx).result();

    nlohmann::json slots = {{"city", "上海"}, {"budget", 300}};
    EXPECT_TRUE(store.UpdateContext(s.session_id, "RETRIEVE", slots).result().ok());

    auto s2 = store.GetOrCreateSession(s.session_id, ctx).result();
    EXPECT_EQ(s2.current_state, "RETRIEVE");
    EXPECT_EQ(s2.context["city"], "上海");
    EXPECT_EQ(s2.context["budget"], 300);
}

TEST(SqliteSessionStore, PersistsAcrossReopen) {
    const std::string path = TempDbPath();
    std::string sid;
    {
        SqliteSessionStore store(path);
        UserContext ctx;
        ctx.user_id = "u1";
        auto s = store.GetOrCreateSession(std::nullopt, ctx).result();
        sid = s.session_id;
        store.AppendTurn(sid, ConversationTurn{.role = "user", .content = "记住我"}).result();
        store.UpdateContext(sid, "RESPOND", nlohmann::json{{"city", "北京"}}).result();
    }
    // Reopen with a brand-new store instance against the same file.
    {
        SqliteSessionStore store(path);
        UserContext ctx;
        ctx.user_id = "u1";
        auto s = store.GetOrCreateSession(sid, ctx).result();
        EXPECT_EQ(s.session_id, sid);
        EXPECT_EQ(s.current_state, "RESPOND");
        EXPECT_EQ(s.context["city"], "北京");
        auto turns = store.GetRecentTurns(sid, 10).result();
        ASSERT_EQ(turns.size(), 1u);
        EXPECT_EQ(turns[0].content, "记住我");
    }
}

TEST(SqliteSessionStore, AppendTurnRejectsUnknownSession) {
    SqliteSessionStore store(TempDbPath());
    auto st = store.AppendTurn("does-not-exist",
        ConversationTurn{.role = "user", .content = "x"}).result();
    EXPECT_FALSE(st.ok());
}

TEST(SqliteSessionStore, GetRecentTurnsRespectsLimit) {
    SqliteSessionStore store(TempDbPath());
    UserContext ctx;
    ctx.user_id = "u1";
    auto s = store.GetOrCreateSession(std::nullopt, ctx).result();
    for (int i = 0; i < 5; ++i) {
        store.AppendTurn(s.session_id,
            ConversationTurn{.role = "user", .content = std::to_string(i)}).result();
    }
    auto turns = store.GetRecentTurns(s.session_id, 2).result();
    ASSERT_EQ(turns.size(), 2u);
    // Last two in chronological order: "3", "4".
    EXPECT_EQ(turns[0].content, "3");
    EXPECT_EQ(turns[1].content, "4");
}

// ---------------------------------------------------------------------------
// Feedback persistence (👍/👎 → feedback table, FK to sessions)
// ---------------------------------------------------------------------------

TEST(SqliteSessionStore, AppendFeedbackOk) {
    const std::string path = TempDbPath();
    SqliteSessionStore store(path);
    UserContext ctx;
    ctx.user_id = "u1";
    auto s = store.GetOrCreateSession(std::nullopt, ctx).result();

    SessionMemoryStore::FeedbackRecord rec;
    rec.session_id = s.session_id;
    rec.trace_id = "t-1";
    rec.item_id = "gb-20001";
    rec.feedback_type = "like";
    rec.comment = "";
    EXPECT_TRUE(store.AppendFeedback(rec).result().ok());

    // Whole-reply feedback: empty item_id.
    rec.item_id = "";
    rec.feedback_type = "dislike";
    EXPECT_TRUE(store.AppendFeedback(rec).result().ok());

    auto rows = ReadFeedback(path);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].session_id, s.session_id);
    EXPECT_EQ(rows[0].trace_id, "t-1");
    EXPECT_EQ(rows[0].item_id, "gb-20001");
    EXPECT_EQ(rows[0].feedback_type, "like");
    EXPECT_EQ(rows[1].item_id, "");
    EXPECT_EQ(rows[1].feedback_type, "dislike");
}

TEST(SqliteSessionStore, AppendFeedbackRejectsUnknownSession) {
    SqliteSessionStore store(TempDbPath());
    SessionMemoryStore::FeedbackRecord rec;
    rec.session_id = "does-not-exist";
    rec.feedback_type = "like";
    auto st = store.AppendFeedback(rec).result();
    EXPECT_FALSE(st.ok());
}

TEST(SqliteSessionStore, AppendFeedbackPersistsAcrossReopen) {
    const std::string path = TempDbPath();
    std::string sid;
    {
        SqliteSessionStore store(path);
        UserContext ctx;
        ctx.user_id = "u1";
        auto s = store.GetOrCreateSession(std::nullopt, ctx).result();
        sid = s.session_id;
        SessionMemoryStore::FeedbackRecord rec;
        rec.session_id = sid;
        rec.trace_id = "t-9";
        rec.item_id = "gb-20003";
        rec.feedback_type = "like";
        EXPECT_TRUE(store.AppendFeedback(rec).result().ok());
    }
    {   // Reopen: row must still be there.
        SqliteSessionStore store(path);
        auto rows = ReadFeedback(path);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0].session_id, sid);
        EXPECT_EQ(rows[0].item_id, "gb-20003");
    }
}

TEST(InMemorySessionStore, AppendFeedbackRejectsUnknownSession) {
    InMemorySessionStore store;
    SessionMemoryStore::FeedbackRecord rec;
    rec.session_id = "nope";
    rec.feedback_type = "like";
    EXPECT_FALSE(store.AppendFeedback(rec).result().ok());

    UserContext ctx;
    ctx.user_id = "u1";
    auto s = store.GetOrCreateSession(std::nullopt, ctx).result();
    rec.session_id = s.session_id;
    EXPECT_TRUE(store.AppendFeedback(rec).result().ok());
}
