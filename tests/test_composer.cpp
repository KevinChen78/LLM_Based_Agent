#include "agent/llm_client.hpp"
#include "agent/response_composer.hpp"
#include "agent/stream_emitter.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <mutex>
#include <vector>

using namespace agent;
using json = nlohmann::json;

namespace {

// Fake LLM client that returns a canned raw_text and counts calls.
class FakeLlmClient : public LlmClient {
public:
    coro::Task<LlmResponse> Chat(const std::vector<LlmMessage>& msgs,
                                 const Options& /*opts*/) override {
        ++call_count;
        for (const auto& m : msgs) {
            if (m.role == "user") last_user_message = m.content;
        }
        LlmResponse r;
        r.raw_text = raw_text;
        co_return r;
    }
    bool Healthy() const override { return healthy; }

    std::string raw_text;
    std::string last_user_message;
    int call_count = 0;
    bool healthy = true;
};

// Fake LLM client that simulates real token streaming in ChatStream.
class FakeStreamingLlm : public LlmClient {
public:
    coro::Task<LlmResponse> Chat(const std::vector<LlmMessage>&,
                                 const Options&) override {
        ++chat_count;
        co_return LlmResponse{};
    }
    coro::Task<LlmStreamResult> ChatStream(const std::vector<LlmMessage>& msgs,
                                           const Options&,
                                           const DeltaCallback& on_delta) override {
        ++stream_count;
        for (const auto& m : msgs) {
            if (m.role == "user") last_user_message = m.content;
        }
        for (const auto& s : chunks) {
            if (on_delta) on_delta(s);
        }
        LlmStreamResult r;
        r.text = joined;
        r.streamed = true;
        co_return r;
    }
    bool Healthy() const override { return true; }

    std::vector<std::string> chunks = {"我", "为您", "推荐"};
    std::string joined = "我为您推荐";
    std::string last_user_message;
    int stream_count = 0;
    int chat_count = 0;
};

// Records emitted events (incl. deltas) for assertions.
class RecordingEmitter : public StreamEmitter {
public:
    void Emit(const std::string& t, const nlohmann::json& p) override {
        std::lock_guard<std::mutex> l(m_);
        events_.push_back({t, p});
    }
    void EmitDelta(const std::string& d) override {
        std::lock_guard<std::mutex> l(m_);
        deltas_ += d;
        delta_count_++;
    }
    void Finish(const RecommendationResult&) override {}
    void Error(const std::string&) override {}
    bool IsClosed() const override { return false; }

    std::string deltas() const {
        std::lock_guard<std::mutex> l(m_);
        return deltas_;
    }
    int delta_count() const {
        std::lock_guard<std::mutex> l(m_);
        return delta_count_;
    }

private:
    mutable std::mutex m_;
    std::vector<std::pair<std::string, nlohmann::json>> events_;
    std::string deltas_;
    int delta_count_ = 0;
};

RecommendationItem MakeItem(const std::string& id,
                            const std::string& title,
                            double price) {
    RecommendationItem it;
    it.item_id = id;
    it.title = title;
    it.price = price;
    it.original_price = price * 2;
    it.reason = "模板理由";
    it.city = "武汉";
    it.category = "小龙虾";
    it.tags = {"小龙虾", "蒜蓉"};
    it.rating = 4.6;
    it.sold_count = 500;
    return it;
}

} // namespace

TEST(ResponseComposer, LlmGeneratesNaturalReply) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"为您精选了人气小龙虾店，麻辣鲜香，约上朋友撸串吧！","item_reasons":{"gb-1":"招牌蒜蓉味，人气最旺"}})";
    ResponseComposer composer(llm);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉吃小龙虾", json{{"city", "武汉"}}, items).result();

    EXPECT_EQ(llm->call_count, 1);
    EXPECT_EQ(result.response_text, "为您精选了人气小龙虾店，麻辣鲜香，约上朋友撸串吧！");
    // Per-item reason should be enriched from item_reasons.
    EXPECT_EQ(result.items[0].reason, "招牌蒜蓉味，人气最旺");
}

TEST(ResponseComposer, FallsBackToTemplateOnGarbage) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = "这不是 JSON，模型放飞自我了";
    ResponseComposer composer(llm);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉吃小龙虾", json::object(), items).result();

    EXPECT_EQ(llm->call_count, 1);
    EXPECT_NE(result.response_text.find("为您推荐以下团购"), std::string::npos);
    EXPECT_NE(result.response_text.find("蒜蓉小龙虾"), std::string::npos);
}

TEST(ResponseComposer, MarkdownFencedJsonStillParses) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = "好的，推荐如下：\n```json\n{\"reply\":\"围栏内的回复\"}\n```\n";
    ResponseComposer composer(llm);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉", json::object(), items).result();

    EXPECT_EQ(result.response_text, "围栏内的回复");
}

TEST(ResponseComposer, EmbeddedJsonInProseStillParses) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = "当然可以！ {\"reply\":\"嵌入式回复\"} 希望你喜欢～";
    ResponseComposer composer(llm);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉", json::object(), items).result();

    EXPECT_EQ(result.response_text, "嵌入式回复");
}

TEST(ResponseComposer, EmptyItemsSkipsLlm) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"不应被使用"})";
    ResponseComposer composer(llm);

    auto result = composer.Compose("随便", json::object(), {}).result();

    EXPECT_EQ(llm->call_count, 0);
    EXPECT_NE(result.response_text.find("暂时没有"), std::string::npos);
}

TEST(ResponseComposer, NullLlmUsesTemplate) {
    ResponseComposer composer(nullptr);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉", json::object(), items).result();

    EXPECT_NE(result.response_text.find("为您推荐以下团购"), std::string::npos);
}

TEST(ResponseComposer, UnhealthyLlmUsesTemplate) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->healthy = false;
    llm->raw_text = R"({"reply":"不应被使用"})";
    ResponseComposer composer(llm);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉", json::object(), items).result();

    EXPECT_EQ(llm->call_count, 0);   // skipped because client reports unhealthy
    EXPECT_NE(result.response_text.find("为您推荐以下团购"), std::string::npos);
}

TEST(ResponseComposer, StreamsTokenDeltasWhenEmitterPresent) {
    auto llm = std::make_shared<FakeStreamingLlm>();
    ResponseComposer composer(llm);
    auto emitter = std::make_shared<RecordingEmitter>();

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉吃小龙虾", json::object(), items, emitter).result();

    EXPECT_EQ(llm->stream_count, 1);
    EXPECT_EQ(result.response_text, "我为您推荐");
    // Each token chunk arrived as its own delta; concatenated they reconstruct the reply.
    EXPECT_EQ(emitter->delta_count(), 3);
    EXPECT_EQ(emitter->deltas(), "我为您推荐");
}

TEST(ResponseComposer, StreamingFallsBackToTemplateDeltas) {
    // FakeLlmClient uses the default ChatStream (streamed=false) -> composer
    // must emit the template as deltas rather than real tokens.
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"无关"})";
    ResponseComposer composer(llm);
    auto emitter = std::make_shared<RecordingEmitter>();

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉", json::object(), items, emitter).result();

    EXPECT_NE(result.response_text.find("为您推荐以下团购"), std::string::npos);
    EXPECT_GE(emitter->delta_count(), 1);
    EXPECT_NE(emitter->deltas().find("蒜蓉小龙虾"), std::string::npos);
}

TEST(ResponseComposer, EmptyItemsEmitsDeltaAndShortCircuits) {
    auto llm = std::make_shared<FakeStreamingLlm>();
    ResponseComposer composer(llm);
    auto emitter = std::make_shared<RecordingEmitter>();

    auto result = composer.Compose("随便", json::object(), {}, emitter).result();

    EXPECT_EQ(llm->stream_count, 0);   // nothing to compose
    EXPECT_NE(result.response_text.find("暂时没有"), std::string::npos);
    EXPECT_GE(emitter->delta_count(), 1);
    EXPECT_NE(emitter->deltas().find("暂时没有"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Grounding (RAG knowledge passages)
// ---------------------------------------------------------------------------

TEST(ResponseComposer, GroundingIsInjectedIntoPrompt) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"可以开发票的。"})";
    ResponseComposer composer(llm);

    std::string grounding = "1. 【发票】所有团购均可开具电子发票，下单后联系商家。（来源：商家政策）\n";
    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("能开发票吗", json::object(), items,
                                   nullptr, grounding).result();

    EXPECT_EQ(llm->call_count, 1);
    EXPECT_NE(llm->last_user_message.find("# 参考知识"), std::string::npos);
    EXPECT_NE(llm->last_user_message.find("开具电子发票"), std::string::npos);
    EXPECT_EQ(result.response_text, "可以开发票的。");
}

TEST(ResponseComposer, EmptyGroundingLeavesPromptUnchanged) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"推荐如下。"})";
    ResponseComposer composer(llm);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    composer.Compose("武汉吃小龙虾", json::object(), items, nullptr, "").result();

    EXPECT_EQ(llm->call_count, 1);
    // The "# 参考知识" section header appears only when grounding is injected
    // (the rules alone merely mention the phrase in passing).
    EXPECT_EQ(llm->last_user_message.find("# 参考知识"), std::string::npos);
}

TEST(ResponseComposer, StreamingGroundingIsInjectedIntoPrompt) {
    auto llm = std::make_shared<FakeStreamingLlm>();
    ResponseComposer composer(llm);
    auto emitter = std::make_shared<RecordingEmitter>();

    std::string grounding = "1. 【包间】本店提供包间，需提前预约。（来源：门店信息）\n";
    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    composer.Compose("有包间吗", json::object(), items, emitter, grounding).result();

    EXPECT_EQ(llm->stream_count, 1);
    EXPECT_NE(llm->last_user_message.find("# 参考知识"), std::string::npos);
    EXPECT_NE(llm->last_user_message.find("提前预约"), std::string::npos);
}

TEST(ResponseComposer, EmptyItemsWithGroundingStillQueriesLlm) {
    // Pure knowledge Q&A (no deals matched): the composer must NOT short-circuit
    // to the "no deals" template when grounding passages are available — the
    // LLM answers from the knowledge instead.
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"可以开发票，下单后联系商家即可。"})";
    ResponseComposer composer(llm);

    std::string grounding = "1. 【发票】所有团购均可开具电子发票。（来源：商家政策）\n";
    auto result = composer.Compose("能开发票吗", json::object(), {},
                                   nullptr, grounding).result();

    EXPECT_EQ(llm->call_count, 1);
    EXPECT_EQ(result.response_text, "可以开发票，下单后联系商家即可。");
}

// ---------------------------------------------------------------------------
// Fact-check guard (Phase 4-A)
// ---------------------------------------------------------------------------

TEST(ResponseComposer, FactViolationFallsBackToTemplate) {
    auto llm = std::make_shared<FakeLlmClient>();
    // The model fabricates a 99 元 price that is not in the candidate set.
    llm->raw_text = R"({"reply":"超划算！蒜蓉小龙虾只要 99 元，快抢！"})";
    auto guard = std::make_shared<SafetyGuard>();
    ResponseComposer composer(llm, guard);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉吃小龙虾", json::object(), items).result();

    EXPECT_EQ(llm->call_count, 1);
    // The fabricated reply must NOT reach the user; the template takes over.
    EXPECT_EQ(result.compose_mode, "template_guard_fallback");
    EXPECT_EQ(result.response_text.find("99"), std::string::npos);
    EXPECT_NE(result.response_text.find("为您推荐以下团购"), std::string::npos);
    // The LLM call is audited with the guard-fallback status.
    ASSERT_EQ(result.llm_calls.size(), 1u);
    EXPECT_EQ(result.llm_calls[0].status, "guard_fallback");
}

TEST(ResponseComposer, HonestPricePassesGuard) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"蒜蓉小龙虾现价 268 元，相当于 5 折。"})";
    auto guard = std::make_shared<SafetyGuard>();
    ResponseComposer composer(llm, guard);

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉吃小龙虾", json::object(), items).result();

    EXPECT_EQ(result.compose_mode, "llm");
    EXPECT_EQ(result.response_text, "蒜蓉小龙虾现价 268 元，相当于 5 折。");
    ASSERT_EQ(result.llm_calls.size(), 1u);
    EXPECT_EQ(result.llm_calls[0].status, "success");
}

TEST(ResponseComposer, NullGuardKeepsOldBehavior) {
    auto llm = std::make_shared<FakeLlmClient>();
    llm->raw_text = R"({"reply":"只要 1 元！纯编造但无 guard 不拦截。"})";
    ResponseComposer composer(llm);   // no guard -> byte-identical to before

    std::vector<RecommendationItem> items{MakeItem("gb-1", "蒜蓉小龙虾（3 人餐）", 268.0)};
    auto result = composer.Compose("武汉", json::object(), items).result();

    EXPECT_EQ(result.compose_mode, "llm");
    EXPECT_EQ(result.response_text, "只要 1 元！纯编造但无 guard 不拦截。");
}
