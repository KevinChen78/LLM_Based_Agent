#include "agent/deal_catalog.hpp"
#include "agent/deal_tools.hpp"
#include "agent/retrieval_client.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace agent;
using json = nlohmann::json;

namespace {

// Fake RetrievalClient that returns canned responses without any HTTP, so the
// tools can be tested offline. Overrides every virtual method.
class FakeRetrievalClient : public RetrievalClient {
public:
    FakeRetrievalClient() : RetrievalClient("http://fake") {}

    bool Enabled() const override { return enabled; }
    bool Healthy() override { return healthy; }

    std::optional<json> SearchDeals(const json& filters) override {
        ++deals_calls;
        last_filters = filters;
        if (!deals_response.is_null()) return deals_response;
        return std::nullopt;
    }

    std::optional<json> SearchKb(const std::string& query, int top_k) override {
        ++kb_calls;
        last_query = query;
        last_top_k = top_k;
        if (!kb_response.is_null()) return kb_response;
        return std::nullopt;
    }

    bool enabled = true;
    bool healthy = true;
    json deals_response;   // null -> simulate failure
    json kb_response;      // null -> simulate failure
    int deals_calls = 0;
    int kb_calls = 0;
    std::string last_query;
    int last_top_k = 0;
    json last_filters;
};

ToolCall MakeCall(const std::string& name, const json& args) {
    ToolCall c;
    c.tool_name = name;
    c.call_id = "tc-kb";
    c.arguments_json = args.dump();
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// KnowledgeRetriever (kb_search)
// ---------------------------------------------------------------------------

TEST(KnowledgeRetriever, ReturnsPassagesFromService) {
    auto client = std::make_shared<FakeRetrievalClient>();
    client->kb_response = json{
        {"passages", json::array({
            {{"id","kb-001"},{"category","发票"},{"title","发票"},
             {"content","所有团购均可开具电子发票。"},{"source","商家政策"},{"score",3.2}},
            {{"id","kb-007"},{"category","发票"},{"title","发票抬头"},
             {"content","支持个人与企业抬头。"},{"source","商家政策"},{"score",2.1}}
        })}
    };
    KnowledgeRetriever kb(client);

    auto result = kb.Execute(MakeCall("kb_search", {{"query","能不能开发票"},{"top_k",3}})).result();

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(client->kb_calls, 1);
    EXPECT_EQ(client->last_query, "能不能开发票");
    EXPECT_EQ(client->last_top_k, 3);

    auto out = json::parse(result.result_json);
    ASSERT_TRUE(out.contains("passages"));
    ASSERT_EQ(out["passages"].size(), 2u);
    EXPECT_EQ(out["passages"][0]["id"], "kb-001");
    EXPECT_NE(out["passages"][0]["content"].get<std::string>().find("电子发票"),
              std::string::npos);
}

TEST(KnowledgeRetriever, FailsWhenServiceNotConfigured) {
    auto client = std::make_shared<FakeRetrievalClient>();
    client->enabled = false;
    KnowledgeRetriever kb(client);

    auto result = kb.Execute(MakeCall("kb_search", {{"query","包间"}})).result();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(client->kb_calls, 0);   // short-circuits before calling the service
}

TEST(KnowledgeRetriever, FailsWhenServiceUnavailable) {
    auto client = std::make_shared<FakeRetrievalClient>();
    client->kb_response = json();   // null -> SearchKb returns nullopt
    KnowledgeRetriever kb(client);

    auto result = kb.Execute(MakeCall("kb_search", {{"query","停车"}})).result();

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("unavailable"), std::string::npos);
}

// ---------------------------------------------------------------------------
// DealRetriever with BM25 delegation
// ---------------------------------------------------------------------------

TEST(DealRetrieverBm25, DelegatesToServiceWhenHealthy) {
    auto catalog = std::make_shared<DealCatalog>("");   // built-in fallback
    auto client = std::make_shared<FakeRetrievalClient>();
    client->deals_response = json{
        {"total", 7},
        {"items", json::array({
            {{"item_id","gb-1"},{"title","麻辣小龙虾"},{"city","武汉"},
             {"price",128.0},{"original_price",258.0},{"rating",4.6},
             {"sold_count",800},{"score",7.19}}
        })}
    };
    DealRetriever retriever(catalog, client);

    auto result = retriever.Execute(MakeCall("deal_retriever", {
        {"city","武汉"},{"keywords","小龙虾"},{"max_price",200.0},{"top_k",5}
    })).result();

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(client->deals_calls, 1);
    // Structured filters forwarded to the service.
    EXPECT_EQ(client->last_filters["city"], "武汉");
    EXPECT_EQ(client->last_filters["query"], "小龙虾");
    EXPECT_DOUBLE_EQ(client->last_filters["max_price"].get<double>(), 200.0);

    auto out = json::parse(result.result_json);
    ASSERT_EQ(out["items"].size(), 1u);
    EXPECT_EQ(out["items"][0]["item_id"], "gb-1");
    EXPECT_EQ(out["total"], 7);
    // BM25 items carry no reason -> one is stamped locally.
    EXPECT_FALSE(out["items"][0]["reason"].get<std::string>().empty());
}

TEST(DealRetrieverBm25, FallsBackToLocalWhenServiceFails) {
    auto catalog = std::make_shared<DealCatalog>("");
    auto client = std::make_shared<FakeRetrievalClient>();
    client->deals_response = json();   // null -> SearchDeals returns nullopt
    DealRetriever retriever(catalog, client);

    auto result = retriever.Execute(MakeCall("deal_retriever", {
        {"city","上海"},{"category","海鲜"},{"top_k",5}
    })).result();

    ASSERT_TRUE(result.success) << result.error_message;
    EXPECT_EQ(client->deals_calls, 1);          // attempted the service...
    auto out = json::parse(result.result_json);
    EXPECT_FALSE(out["items"].empty());          // ...then fell back to local matches
    for (const auto& it : out["items"]) {
        EXPECT_EQ(it["city"], "上海");
    }
}

TEST(DealRetrieverBm25, RecallAuditForwardedWhenPresent) {
    auto catalog = std::make_shared<DealCatalog>("");
    auto client = std::make_shared<FakeRetrievalClient>();
    client->deals_response = json{
        {"total", 15},
        {"relaxed_level", 1},
        {"effective_category", "粤菜"},
        {"items", json::array({
            {{"item_id","gb-26031"},{"title","粤式早茶点心（2 人餐）"},{"city","深圳"},
             {"price",168.0},{"original_price",268.0},{"rating",4.5},
             {"sold_count",300},{"score",6.1}}
        })}
    };
    DealRetriever retriever(catalog, client);

    auto result = retriever.Execute(MakeCall("deal_retriever", {
        {"city","深圳"},{"category","早茶"},{"top_k",5}
    })).result();

    ASSERT_TRUE(result.success) << result.error_message;
    auto out = json::parse(result.result_json);
    // Phase 3-C: relaxation audit forwarded verbatim for the orchestrator.
    ASSERT_TRUE(out.contains("recall_audit"));
    EXPECT_EQ(out["recall_audit"]["relaxed_level"], 1);
    EXPECT_EQ(out["recall_audit"]["effective_category"], "粤菜");
    EXPECT_EQ(out["total"], 15);
}

TEST(DealRetrieverBm25, NoRecallAuditWhenAbsent) {
    auto catalog = std::make_shared<DealCatalog>("");
    auto client = std::make_shared<FakeRetrievalClient>();
    // Old-service shape: no relaxed_level key -> no recall_audit in output.
    client->deals_response = json{
        {"total", 3},
        {"items", json::array({
            {{"item_id","gb-1"},{"title","火锅"},{"city","武汉"},
             {"price",128.0},{"rating",4.6},{"sold_count",800},{"score",7.0}}
        })}
    };
    DealRetriever retriever(catalog, client);

    auto result = retriever.Execute(MakeCall("deal_retriever", {
        {"city","武汉"},{"category","火锅"},{"top_k",5}
    })).result();

    ASSERT_TRUE(result.success) << result.error_message;
    auto out = json::parse(result.result_json);
    EXPECT_FALSE(out.contains("recall_audit"));
}

TEST(DealRetrieverBm25, NullArgsTolerated) {
    // Phase 3-D replay finding: the LLM emits explicit nulls ("max_price":
    // null when the user states no budget). value<>() throws on null and the
    // whole tool call failed — nulls must read as absent.
    auto catalog = std::make_shared<DealCatalog>("");
    auto client = std::make_shared<FakeRetrievalClient>();
    client->deals_response = json{
        {"total", 2},
        {"items", json::array({
            {{"item_id","gb-1"},{"title","粤式早茶点心"},{"city","深圳"},
             {"price",168.0},{"rating",4.5},{"sold_count",300},{"score",6.1}}
        })}
    };
    DealRetriever retriever(catalog, client);

    auto result = retriever.Execute(MakeCall("deal_retriever", {
        {"city","深圳"},{"category",nullptr},{"keywords","早茶"},
        {"max_price",nullptr},{"min_price",nullptr},{"people",nullptr},
        {"top_k",nullptr}
    })).result();

    ASSERT_TRUE(result.success) << result.error_message;
    // Nulls became "absent": nothing but query/city reached the service.
    EXPECT_EQ(client->last_filters["city"], "深圳");
    EXPECT_EQ(client->last_filters["query"], "早茶");
    EXPECT_FALSE(client->last_filters.contains("max_price"));
    EXPECT_FALSE(client->last_filters.contains("category"));
    auto out = json::parse(result.result_json);
    ASSERT_EQ(out["items"].size(), 1u);
}

TEST(DealRetrieverBm25, ZeroMaxPriceMeansNoLimit) {
    // Phase 3-D replay finding: the planner translates "预算没有要求" into
    // budget=0 and forwards max_price=0. The retrieval service's max_price=0
    // semantics deliberately filter everything (price <= 0), so the tool must
    // normalize a non-positive ceiling to "no limit" before calling out.
    auto catalog = std::make_shared<DealCatalog>("");
    auto client = std::make_shared<FakeRetrievalClient>();
    client->deals_response = json{
        {"total", 2},
        {"items", json::array({
            {{"item_id","gb-1"},{"title","粤式早茶点心"},{"city","深圳"},
             {"price",168.0},{"rating",4.5},{"sold_count",300},{"score",6.1}}
        })}
    };
    DealRetriever retriever(catalog, client);

    auto result = retriever.Execute(MakeCall("deal_retriever", {
        {"city","深圳"},{"keywords","早茶"},{"max_price",0},{"people",2}
    })).result();

    ASSERT_TRUE(result.success) << result.error_message;
    // max_price=0 must NOT reach the service as a filter.
    EXPECT_FALSE(client->last_filters.contains("max_price"));
    auto out = json::parse(result.result_json);
    ASSERT_EQ(out["items"].size(), 1u);
}

TEST(DealRetrieverBm25, NoClientUsesLocalOnly) {
    auto catalog = std::make_shared<DealCatalog>("");
    DealRetriever retriever(catalog);   // no retrieval client injected

    auto result = retriever.Execute(MakeCall("deal_retriever", {
        {"city","上海"},{"category","海鲜"},{"keywords","龙虾"},{"top_k",50}
    })).result();

    ASSERT_TRUE(result.success) << result.error_message;
    auto out = json::parse(result.result_json);
    ASSERT_FALSE(out["items"].empty());
    // Local substring matcher: 龙虾 keyword still surfaces gb-20003 first.
    EXPECT_EQ(out["items"][0]["item_id"], "gb-20003");
}
