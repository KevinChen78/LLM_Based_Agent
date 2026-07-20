#include "agent/mock_tools.hpp"

#include <algorithm>

namespace agent {

const nlohmann::json& MockRetriever::MockItems() {
    static nlohmann::json items = nlohmann::json::array({
        {
            {"item_id", "gb-10001"},
            {"merchant_id", "m-20001"},
            {"title", "海鲜大咖套餐（3-4 人）"},
            {"category", "海鲜"},
            {"city", "上海"},
            {"district", "黄浦"},
            {"price", 288.0},
            {"original_price", 598.0},
            {"sold_count", 1200},
            {"rating", 4.7},
            {"tags", nlohmann::json::array({"大虾", "生蚝", "扇贝", "鱿鱼"})},
            {"reason", "含 8 种海鲜，分量足，人均不到 100 元"}
        },
        {
            {"item_id", "gb-10002"},
            {"merchant_id", "m-20002"},
            {"title", "精品海鲜双人餐"},
            {"category", "海鲜"},
            {"city", "上海"},
            {"district", "静安"},
            {"price", 198.0},
            {"original_price", 398.0},
            {"sold_count", 800},
            {"rating", 4.5},
            {"tags", nlohmann::json::array({"清蒸鲈鱼", "蒜蓉扇贝"})},
            {"reason", "主打清蒸海鲜，口味清淡"}
        },
        {
            {"item_id", "gb-10003"},
            {"merchant_id", "m-20003"},
            {"title", "波士顿龙虾三人餐"},
            {"category", "海鲜"},
            {"city", "上海"},
            {"district", "浦东"},
            {"price", 298.0},
            {"original_price", 688.0},
            {"sold_count", 500},
            {"rating", 4.8},
            {"tags", nlohmann::json::array({"波士顿龙虾", "海鲜拼盘"})},
            {"reason", "龙虾新鲜，适合三人聚餐"}
        },
        {
            {"item_id", "gb-10004"},
            {"merchant_id", "m-20004"},
            {"title", "老北京铜锅涮肉 4 人餐"},
            {"category", "火锅"},
            {"city", "北京"},
            {"district", "朝阳"},
            {"price", 268.0},
            {"original_price", 528.0},
            {"sold_count", 900},
            {"rating", 4.6},
            {"tags", nlohmann::json::array({"羊肉", "铜锅"})},
            {"reason", "地道老北京铜锅，适合冬季聚餐"}
        }
    });
    return items;
}

std::string MockRetriever::SchemaJson() const {
    return R"({
        "name": "mock_retriever",
        "description": "从 Mock 团购商品库中召回候选商品",
        "parameters": {
            "type": "object",
            "required": ["city", "top_k"],
            "properties": {
                "city": {"type": "string"},
                "category": {"type": "string"},
                "max_price": {"type": "number"},
                "people": {"type": "integer"},
                "keywords": {"type": "string"},
                "top_k": {"type": "integer", "default": 20}
            }
        }
    })";
}

coro::Task<ToolResult> MockRetriever::Execute(const ToolCall& call) {
    ToolResult result;
    result.call_id = call.call_id;
    result.success = true;

    try {
        auto args = nlohmann::json::parse(call.arguments_json);
        std::string city = args.value("city", "");
        std::string category = args.value("category", "");
        double max_price = args.value("max_price", 1e9);

        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& item : MockItems()) {
            if (item.value("city", "") != city) continue;
            if (!category.empty() && item.value("category", "") != category) continue;
            if (item.value("price", 0.0) > max_price) continue;
            filtered.push_back(item);
        }

        nlohmann::json out;
        out["items"] = filtered;
        result.result_json = out.dump();
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
    }

    co_return result;
}

std::string MockRanker::SchemaJson() const {
    return R"({
        "name": "mock_ranker",
        "description": "对召回候选进行排序（Phase 0 为规则排序）",
        "parameters": {
            "type": "object",
            "required": ["candidates"],
            "properties": {
                "candidates": {"type": "array"},
                "budget": {"type": "number"},
                "people": {"type": "integer"},
                "top_n": {"type": "integer", "default": 3}
            }
        }
    })";
}

coro::Task<ToolResult> MockRanker::Execute(const ToolCall& call) {
    ToolResult result;
    result.call_id = call.call_id;
    result.success = true;

    try {
        auto args = nlohmann::json::parse(call.arguments_json);
        auto candidates = args.value("candidates", nlohmann::json::array());
        int top_n = args.value("top_n", 3);

        // Simple sort: higher sold_count first
        std::sort(candidates.begin(), candidates.end(),
            [](const nlohmann::json& a, const nlohmann::json& b) {
                return a.value("sold_count", 0) > b.value("sold_count", 0);
            });

        nlohmann::json ranked = nlohmann::json::array();
        for (size_t i = 0; i < candidates.size() && i < static_cast<size_t>(top_n); ++i) {
            auto item = candidates[i];
            item["score"] = 0.9 - i * 0.05;
            ranked.push_back(item);
        }

        nlohmann::json out;
        out["items"] = ranked;
        result.result_json = out.dump();
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = e.what();
    }

    co_return result;
}

} // namespace agent
