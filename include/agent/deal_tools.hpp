#pragma once

#include "agent/common.hpp"
#include "agent/deal_catalog.hpp"
#include "agent/tool_registry.hpp"

#include <memory>

namespace agent {

// Real catalog-backed retriever. Filters the DealCatalog by city / category /
// district / price range / party size, then ranks the survivors by a relevance
// score (keyword match + rating + discount) and returns the top_k.
class DealRetriever : public ITool {
public:
    explicit DealRetriever(std::shared_ptr<DealCatalog> catalog);

    std::string Name() const override { return "deal_retriever"; }
    std::string Description() const override {
        return "从真实团购商品库召回候选（按城市/类目/预算/人数过滤 + 相关性打分）";
    }
    std::string SchemaJson() const override;
    coro::Task<ToolResult> Execute(const ToolCall& call) override;

private:
    std::shared_ptr<DealCatalog> catalog_;
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

} // namespace agent
