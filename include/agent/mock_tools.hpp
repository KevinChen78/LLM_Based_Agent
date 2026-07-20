#pragma once

#include "agent/common.hpp"
#include "agent/tool_registry.hpp"

#include <nlohmann/json.hpp>

#include <memory>

namespace agent {

class MockRetriever : public ITool {
public:
    std::string Name() const override { return "mock_retriever"; }
    std::string Description() const override { return "Mock retriever for Phase 0"; }
    std::string SchemaJson() const override;
    coro::Task<ToolResult> Execute(const ToolCall& call) override;

private:
    static const nlohmann::json& MockItems();
};

class MockRanker : public ITool {
public:
    std::string Name() const override { return "mock_ranker"; }
    std::string Description() const override { return "Mock ranker for Phase 0"; }
    std::string SchemaJson() const override;
    coro::Task<ToolResult> Execute(const ToolCall& call) override;
};

} // namespace agent
