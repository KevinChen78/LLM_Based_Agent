#include "agent/sqlite_session_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>

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
