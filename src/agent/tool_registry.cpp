#include "agent/tool_registry.hpp"

namespace agent {

void ToolRegistry::Register(std::shared_ptr<ITool> tool) {
    if (tool) {
        tools_[tool->Name()] = std::move(tool);
    }
}

std::shared_ptr<ITool> ToolRegistry::Get(const std::string& name) const {
    auto it = tools_.find(name);
    if (it != tools_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> ToolRegistry::ListNames() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& [name, _] : tools_) {
        names.push_back(name);
    }
    return names;
}

} // namespace agent
