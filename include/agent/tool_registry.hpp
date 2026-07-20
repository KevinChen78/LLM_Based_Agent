#pragma once

#include "agent/common.hpp"
#include "coro/core/task.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent {

class ITool {
public:
    virtual ~ITool() = default;
    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;
    virtual std::string SchemaJson() const = 0;
    virtual coro::Task<ToolResult> Execute(const ToolCall& call) = 0;
};

class ToolRegistry {
public:
    void Register(std::shared_ptr<ITool> tool);
    std::shared_ptr<ITool> Get(const std::string& name) const;
    std::vector<std::string> ListNames() const;

private:
    std::unordered_map<std::string, std::shared_ptr<ITool>> tools_;
};

} // namespace agent
