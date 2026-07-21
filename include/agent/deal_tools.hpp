#pragma once

#include "agent/common.hpp"
#include "agent/deal_catalog.hpp"
#include "agent/retrieval_client.hpp"
#include "agent/tool_registry.hpp"

#include <memory>

namespace agent {

// Real catalog-backed retriever. Filters the DealCatalog by city / category /
// district / price range / party size, then ranks the survivors by a relevance
// score (keyword match + rating + discount) and returns the top_k.
//
// When a RetrievalClient is injected and the retrieval service is reachable,
// the text-relevance step is delegated to the service's BM25 ranker (better
// synonym / colloquial matching); otherwise the local substring matcher runs,
// so offline behaviour is unchanged.
class DealRetriever : public ITool {
public:
    explicit DealRetriever(std::shared_ptr<DealCatalog> catalog,
                           std::shared_ptr<RetrievalClient> retrieval = nullptr);

    std::string Name() const override { return "deal_retriever"; }
    std::string Description() const override {
        return "从真实团购商品库召回候选（按城市/类目/预算/人数过滤 + 相关性打分）";
    }
    std::string SchemaJson() const override;
    coro::Task<ToolResult> Execute(const ToolCall& call) override;

private:
    std::shared_ptr<DealCatalog> catalog_;
    std::shared_ptr<RetrievalClient> retrieval_;
};

// Real multi-factor ranker. Re-orders a candidate list (typically the
// retriever's output) using rating + popularity + price-fit + discount, with
// optional taboo filtering. When `candidates` is empty the orchestrator injects
// the accumulated retriever results, enabling deterministic retrieve→rank
// chaining without relying on the LLM to forward candidates.
class DealRanker : public ITool {
public:
    std::string Name() const override { return "deal_ranker"; }
    std::string Description() const override {
        return "对召回候选做多因子重排（评分/销量/价格契合/折扣）并取 top_n";
    }
    std::string SchemaJson() const override;
    coro::Task<ToolResult> Execute(const ToolCall& call) override;
};

// Knowledge-base RAG retriever (tool name `kb_search`). Forwards a natural-
// language query to the retrieval service's BM25 knowledge corpus and returns
// matching passages (FAQ / policy / dish knowledge: 发票/预约/退款/包间/停车/
// 忌口/营业时间…). The orchestrator collects these passages as grounding that
// the composer injects into the LLM prompt, so factual answers are sourced
// rather than hallucinated. Registered only when RETRIEVAL_SERVICE_URL is set.
class KnowledgeRetriever : public ITool {
public:
    explicit KnowledgeRetriever(std::shared_ptr<RetrievalClient> retrieval);

    std::string Name() const override { return "kb_search"; }
    std::string Description() const override {
        return "检索团购知识库（发票/预约/退款/包间/停车/忌口/营业时间等），返回相关段落作为回答依据";
    }
    std::string SchemaJson() const override;
    coro::Task<ToolResult> Execute(const ToolCall& call) override;

private:
    std::shared_ptr<RetrievalClient> retrieval_;
};

} // namespace agent
