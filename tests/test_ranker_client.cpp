#include "agent/deal_tools.hpp"
#include "agent/experiment_manager.hpp"
#include "agent/ranker_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace agent;
using json = nlohmann::json;

namespace {

// Fake RankerClient that returns canned responses without any HTTP, so the
// DealRanker can be tested offline (same pattern as test_kb.cpp's
// FakeRetrievalClient).
class FakeRankerClient : public RankerClient {
public:
    FakeRankerClient() : RankerClient("http://fake") {}

    bool Enabled() const override { return enabled; }
    bool Healthy() override { return healthy; }

    std::optional<json> Rank(const json& request) override {
        ++rank_calls;
        last_request = request;
        if (!rank_response.is_null()) return rank_response;
        return std::nullopt;
    }

    bool enabled = true;
    bool healthy = true;
    json rank_response;   // null -> simulate transport failure
    int rank_calls = 0;
    json last_request;
};

ToolCall MakeCall(const json& args) {
    ToolCall c;
    c.tool_name = "deal_ranker";
    c.call_id = "tc-rank";
    c.arguments_json = args.dump();
    return c;
}

// Two candidates where rule and model scores disagree:
//   a — strong by rules (rating 5, best seller, 50% off, in budget)
//   b — weak by rules, but the fake model prefers it
json Candidates() {
    return json::array({
        {{"item_id","a"},{"title","A"},{"price",100.0},{"original_price",200.0},
         {"sold_count",1000},{"rating",5.0},{"category","火锅"},{"city","武汉"}},
        {{"item_id","b"},{"title","B"},{"price",100.0},{"original_price",100.0},
         {"sold_count",1},{"rating",3.0},{"category","烧烤"},{"city","武汉"}}
    });
}

json ModelResponse() {
    return json{
        {"model_loaded", true},
        {"model_version", "test-v1"},
        {"items", json::array({
            {{"item_id","b"},{"model_score",0.99}},
            {{"item_id","a"},{"model_score",0.01}}
        })}
    };
}

json RunRanker(DealRanker& ranker, const json& args) {
    auto result = ranker.Execute(MakeCall(args)).result();
    EXPECT_TRUE(result.success) << result.error_message;
    return json::parse(result.result_json);
}

} // namespace

// Active mode + treatment bucket: model scores drive the ordering AND the
// served score field.
TEST(RankerClientIntegration, ModelScoresServeTreatment) {
    auto client = std::make_shared<FakeRankerClient>();
    client->rank_response = ModelResponse();
    DealRanker ranker(client, ExperimentManager(
        ExperimentManager::Mode::kActive, 100, "exp"));

    auto out = RunRanker(ranker, {
        {"candidates", Candidates()}, {"budget", 300.0}, {"top_n", 2},
        {"user_id", "u-treatment"}
    });
    EXPECT_EQ(client->rank_calls, 1);
    ASSERT_EQ(out["items"].size(), 2u);
    EXPECT_EQ(out["items"][0]["item_id"], "b");          // model prefers b
    EXPECT_DOUBLE_EQ(out["items"][0]["score"].get<double>(), 0.99);
    EXPECT_EQ(out["rank_audit"]["rank_mode"], "model");
    EXPECT_EQ(out["rank_audit"]["experiment_group"], "treatment");
    EXPECT_EQ(out["rank_audit"]["model_version"], "test-v1");
    // Both scores audited per candidate.
    ASSERT_EQ(out["rank_audit"]["candidates"].size(), 2u);
    EXPECT_TRUE(out["rank_audit"]["candidates"][0].contains("rule_score"));
    EXPECT_TRUE(out["rank_audit"]["candidates"][0].contains("model_score"));
}

// Shadow mode: rule scores serve; the model is still called and its scores
// land in rank_audit only.
TEST(RankerClientIntegration, ShadowServesRulesButAuditsModel) {
    auto client = std::make_shared<FakeRankerClient>();
    client->rank_response = ModelResponse();
    DealRanker ranker(client, ExperimentManager(
        ExperimentManager::Mode::kShadow, 50, "exp"));

    auto out = RunRanker(ranker, {
        {"candidates", Candidates()}, {"budget", 300.0}, {"top_n", 2},
        {"user_id", "u-shadow"}
    });
    EXPECT_EQ(client->rank_calls, 1);
    EXPECT_TRUE(client->last_request.value("shadow", false));
    ASSERT_EQ(out["items"].size(), 2u);
    EXPECT_EQ(out["items"][0]["item_id"], "a");          // rules prefer a
    EXPECT_EQ(out["rank_audit"]["rank_mode"], "rule");
    // Model score present in audit even though it did not serve.
    bool saw_model_score = false;
    for (const auto& c : out["rank_audit"]["candidates"]) {
        if (!c["model_score"].is_null()) saw_model_score = true;
    }
    EXPECT_TRUE(saw_model_score);
}

// Transport failure on the model path: rule fallback, flagged in rank_mode.
TEST(RankerClientIntegration, ServiceDownFallsBackToRules) {
    auto client = std::make_shared<FakeRankerClient>();
    client->rank_response = json();   // null -> Rank returns nullopt
    DealRanker ranker(client, ExperimentManager(
        ExperimentManager::Mode::kActive, 100, "exp"));

    auto out = RunRanker(ranker, {
        {"candidates", Candidates()}, {"budget", 300.0}, {"top_n", 2},
        {"user_id", "u-treatment"}
    });
    ASSERT_EQ(out["items"].size(), 2u);
    EXPECT_EQ(out["items"][0]["item_id"], "a");
    EXPECT_EQ(out["rank_audit"]["rank_mode"], "rule_fallback");
}

// Service answers but has no trained model: also a rule fallback.
TEST(RankerClientIntegration, ModelNotLoadedFallsBackToRules) {
    auto client = std::make_shared<FakeRankerClient>();
    client->rank_response = json{{"model_loaded", false}, {"items", json::array()}};
    DealRanker ranker(client, ExperimentManager(
        ExperimentManager::Mode::kActive, 100, "exp"));

    auto out = RunRanker(ranker, {
        {"candidates", Candidates()}, {"budget", 300.0}, {"top_n", 2},
        {"user_id", "u-treatment"}
    });
    ASSERT_EQ(out["items"].size(), 2u);
    EXPECT_EQ(out["items"][0]["item_id"], "a");
    EXPECT_EQ(out["rank_audit"]["rank_mode"], "rule_fallback");
}

// Off mode: the service is never called; audit carries no experiment group.
TEST(RankerClientIntegration, OffModeSkipsService) {
    auto client = std::make_shared<FakeRankerClient>();
    client->rank_response = ModelResponse();
    DealRanker ranker(client, ExperimentManager(
        ExperimentManager::Mode::kOff, 100, "exp"));

    auto out = RunRanker(ranker, {
        {"candidates", Candidates()}, {"budget", 300.0}, {"top_n", 2},
        {"user_id", "u-treatment"}
    });
    EXPECT_EQ(client->rank_calls, 0);
    EXPECT_EQ(out["items"][0]["item_id"], "a");
    EXPECT_EQ(out["rank_audit"]["rank_mode"], "rule");
    EXPECT_EQ(out["rank_audit"]["experiment_group"], "");
}

// Taboo filtering is local and runs BEFORE the model call: taboo items must
// not appear in the request sent to the ranking service.
TEST(RankerClientIntegration, TabooFilteredBeforeModelCall) {
    auto client = std::make_shared<FakeRankerClient>();
    client->rank_response = ModelResponse();
    DealRanker ranker(client, ExperimentManager(
        ExperimentManager::Mode::kActive, 100, "exp"));

    auto cands = Candidates();
    cands[1]["title"] = "麻辣烧烤";   // taboo will hit "麻辣"
    auto out = RunRanker(ranker, {
        {"candidates", cands}, {"budget", 300.0}, {"top_n", 3},
        {"taboo", "麻辣"}, {"user_id", "u-treatment"}
    });
    ASSERT_EQ(out["items"].size(), 1u);
    EXPECT_EQ(out["items"][0]["item_id"], "a");
    ASSERT_EQ(client->rank_calls, 1);
    for (const auto& c : client->last_request["candidates"]) {
        EXPECT_NE(c["item_id"], "b");
    }
}

// No client at all (offline): pure rule path, no crash.
TEST(RankerClientIntegration, NoClientIsRuleOnly) {
    DealRanker ranker;   // default: nullptr client + FromEnv (off in tests)
    auto out = RunRanker(ranker, {
        {"candidates", Candidates()}, {"budget", 300.0}, {"top_n", 2}
    });
    ASSERT_EQ(out["items"].size(), 2u);
    EXPECT_EQ(out["items"][0]["item_id"], "a");
    EXPECT_EQ(out["rank_audit"]["rank_mode"], "rule");
}
