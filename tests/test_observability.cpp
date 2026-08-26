#include "agent/observability_store.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>

using namespace agent;

namespace {

std::string TempDbPath(const char* tag) {
    static int n = 0;
    auto p = std::filesystem::temp_directory_path() /
             ("llm_agent_obs_" + std::string(tag) + "_" + std::to_string(++n) + ".db");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return p.string();
}

long long CountRows(const std::string& db_path, const char* table) {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
    long long n = -1;
    sqlite3_stmt* st = nullptr;
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return n;
}

ObservabilityStore::RecLogEntry MakeRec(const std::string& trace,
                                        const std::string& action,
                                        int items, long latency) {
    ObservabilityStore::RecLogEntry e;
    e.trace_id = trace;
    e.session_id = "s-1";
    e.user_id = "u1";
    e.request_text = "今晚三个人吃海鲜，预算300左右，上海";
    e.action = action;
    e.slots_json = R"({"city":"上海"})";
    e.item_count = items;
    e.ranked_items_json = R"([{"item_id":"gb-1","score":0.9}])";
    e.response_text = "为您推荐……";
    e.compose_mode = "llm";
    e.latency_ms = latency;
    return e;
}

} // namespace

TEST(ObservabilityStore, WritesAndReadsBack) {
    const std::string path = TempDbPath("rw");
    {
        ObservabilityStore store(path);
        store.LogRecommendation(MakeRec("t-1", "retrieve", 3, 120));
        store.LogRecommendation(MakeRec("t-2", "clarify", 0, 80));

        ObservabilityStore::LlmCallEntry le;
        le.trace_id = "t-1";
        le.session_id = "s-1";
        le.purpose = "plan";
        le.model = "deepseek-v4-flash";
        le.prompt_tokens = 500;
        le.completion_tokens = 120;
        le.latency_ms = 900;
        le.status = "success";
        store.LogLlmCall(le);
    }
    EXPECT_EQ(CountRows(path, "recommendation_logs"), 2);
    EXPECT_EQ(CountRows(path, "llm_calls"), 1);
}

TEST(ObservabilityStore, AggregateCountsAndRatios) {
    const std::string obs_path = TempDbPath("agg");
    const std::string sess_path = TempDbPath("sess");
    ObservabilityStore store(obs_path);
    store.LogRecommendation(MakeRec("t-1", "retrieve", 3, 100));
    store.LogRecommendation(MakeRec("t-2", "retrieve", 0, 200));   // empty retrieve
    store.LogRecommendation(MakeRec("t-3", "clarify", 0, 50));
    store.LogRecommendation(MakeRec("t-4", "FALLBACK", 0, 5000));

    ObservabilityStore::LlmCallEntry le;
    le.trace_id = "t-1";
    le.purpose = "plan";
    le.prompt_tokens = 400;
    le.completion_tokens = 100;
    le.status = "success";
    store.LogLlmCall(le);
    le.purpose = "compose";
    le.prompt_tokens = 300;
    store.LogLlmCall(le);

    // Sessions DB with a feedback table: 2 likes, 1 dislike.
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(sess_path.c_str(), &db), SQLITE_OK);
        char* err = nullptr;
        ASSERT_EQ(sqlite3_exec(db,
            "CREATE TABLE feedback (session_id TEXT, trace_id TEXT, item_id TEXT,"
            " feedback_type TEXT, comment TEXT, created_at TEXT);"
            "INSERT INTO feedback VALUES ('s-1','t-1','gb-1','like','','now');"
            "INSERT INTO feedback VALUES ('s-1','t-1','','like','','now');"
            "INSERT INTO feedback VALUES ('s-1','t-2','gb-2','dislike','','now');",
            nullptr, nullptr, &err), SQLITE_OK) << (err ? err : "");
        sqlite3_close(db);
    }

    auto j = store.Aggregate(sess_path);
    EXPECT_EQ(j["requests"]["total"], 4);
    EXPECT_EQ(j["requests"]["by_action"]["retrieve"], 2);
    EXPECT_EQ(j["requests"]["by_action"]["clarify"], 1);
    EXPECT_DOUBLE_EQ(j["requests"]["fallback_rate"], 0.25);
    EXPECT_DOUBLE_EQ(j["requests"]["empty_retrieve_rate"], 0.5);
    EXPECT_EQ(j["llm"]["total_calls"], 2);
    EXPECT_EQ(j["llm"]["prompt_tokens"], 700);
    ASSERT_TRUE(j.contains("feedback"));
    EXPECT_EQ(j["feedback"]["like"], 2);
    EXPECT_EQ(j["feedback"]["dislike"], 1);
    EXPECT_DOUBLE_EQ(j["feedback"]["satisfaction"], 2.0 / 3.0);
}

TEST(ObservabilityStore, AggregateWithoutSessionsDbOmitsFeedback) {
    const std::string obs_path = TempDbPath("nodb");
    ObservabilityStore store(obs_path);
    store.LogRecommendation(MakeRec("t-1", "respond", 0, 10));
    auto j = store.Aggregate("");   // no sessions DB -> no feedback section
    EXPECT_EQ(j["requests"]["total"], 1);
    EXPECT_FALSE(j.contains("feedback"));
}
