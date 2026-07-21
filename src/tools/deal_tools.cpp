#include "agent/deal_tools.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace agent {

namespace {

// Split a keyword / taboo string into normalized tokens. Splits on spaces,
// commas (both ASCII and Chinese)、，and treats each resulting piece as a token.
std::vector<std::string> Tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    };
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == ',' || c == ';' || c == '，' || c == '、' || c == '；') {
            flush();
        } else {
            cur += c;
        }
    }
    flush();
    return tokens;
}

// Concatenate the searchable text of a deal for keyword matching.
std::string SearchableText(const nlohmann::json& deal) {
    std::ostringstream oss;
    oss << deal.value("title", "") << " ";
    oss << deal.value("category", "") << " ";
    oss << deal.value("description", "") << " ";
    if (deal.contains("tags") && deal["tags"].is_array()) {
        for (const auto& t : deal["tags"]) {
            if (t.is_string()) oss << t.get<std::string>() << " ";
        }
    }
    return oss.str();
}

double DiscountOf(const nlohmann::json& deal) {
    double price = deal.value("price", 0.0);
    double original = deal.value("original_price", 0.0);
    if (original <= 0.0 || price <= 0.0) return 0.0;
    double d = (original - price) / original;   // 0..1
    return d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d);
}

// Compose a concise Chinese recommendation reason.
std::string BuildReason(const nlohmann::json& deal) {
    std::ostringstream oss;
    double rating = deal.value("rating", 0.0);
    long sold = deal.value("sold_count", 0);
    double price = deal.value("price", 0.0);
    double original = deal.value("original_price", 0.0);
    if (rating > 0.0) oss << "评分 " << std::to_string(rating).substr(0, 3) << " · ";
    if (sold > 0) oss << "销量 " << sold << " · ";
    if (original > price && price > 0.0) {
        double zhe = price / original * 10.0;
        oss << "约" << std::to_string(zhe).substr(0, 3) << " 折";
    }
    std::string s = oss.str();
    while (!s.empty() && (s.back() == ' ' || s.back() == '·')) s.pop_back();
    return s;
}

// Tokenize keyword list and count how many tokens match the deal's searchable text.
// Returns {matched, total}. If keywords empty, total==0 (neutral).
std::pair<int, int> KeywordMatch(const nlohmann::json& deal,
                                 const std::vector<std::string>& tokens) {
    if (tokens.empty()) return {0, 0};
    std::string hay = SearchableText(deal);
    int matched = 0;
    for (const auto& tk : tokens) {
        if (tk.empty()) continue;
        if (hay.find(tk) != std::string::npos) ++matched;
    }
    return {matched, static_cast<int>(tokens.size())};
}

} // namespace

// ---------------------------------------------------------------------------
// DealRetriever
// ---------------------------------------------------------------------------

DealRetriever::DealRetriever(std::shared_ptr<DealCatalog> catalog)
    : catalog_(std::move(catalog)) {}

std::string DealRetriever::SchemaJson() const {
    return R"({
        "name": "deal_retriever",
        "description": "从真实团购商品库召回候选商品，按城市/类目/区县/价格/人数过滤并按相关性打分排序",
        "parameters": {
            "type": "object",
            "required": ["city", "top_k"],
            "properties": {
                "city": {"type": "string", "description": "城市，如 上海、北京"},
                "category": {"type": "string", "description": "类目，如 海鲜、火锅、日料"},
                "district": {"type": "string", "description": "区县/商圈，可选"},
                "max_price": {"type": "number", "description": "套餐总价上限，默认不限"},
                "min_price": {"type": "number", "description": "套餐总价下限，默认 0"},
                "people": {"type": "integer", "description": "人数，用于匹配套餐人数区间"},
                "keywords": {"type": "string", "description": "关键词，空格或逗号分隔，如 龙虾 包间"},
                "top_k": {"type": "integer", "default": 20}
            }
        }
    })";
}

coro::Task<ToolResult> DealRetriever::Execute(const ToolCall& call) {
    ToolResult result;
    result.call_id = call.call_id;

    try {
        auto args = nlohmann::json::parse(call.arguments_json);
        std::string city = args.value("city", "");
        std::string category = args.value("category", "");
        std::string district = args.value("district", "");
        double max_price = args.value("max_price", 1e9);
        double min_price = args.value("min_price", 0.0);
        int people = args.value("people", 0);
        std::string keywords = args.value("keywords", "");
        int top_k = args.value("top_k", 20);
        if (top_k <= 0) top_k = 20;

        auto tokens = Tokenize(keywords);

        struct ScoredDeal {
            nlohmann::json deal;
            double score;
        };
        std::vector<ScoredDeal> survivors;

        for (const auto& deal : catalog_->Deals()) {
            if (!city.empty() && deal.value("city", "") != city) continue;
            if (!category.empty() && deal.value("category", "") != category) continue;
            if (!district.empty() && deal.value("district", "") != district) continue;
            double price = deal.value("price", 0.0);
            if (price > max_price) continue;
            if (price < min_price) continue;
            if (people > 0) {
                int min_p = deal.value("min_people", 0);
                int max_p = deal.value("max_people", 0);
                // Match when the party size falls in the deal's serving range.
                // Deals without a range are kept.
                if (min_p > 0 && max_p > 0 && (people < min_p || people > max_p)) continue;
            }

            double rating_norm = deal.value("rating", 0.0) / 5.0;
            double discount = DiscountOf(deal);
            double score;
            if (tokens.empty()) {
                // No keyword signal: rank by rating + discount.
                score = 0.6 * rating_norm + 0.4 * discount;
            } else {
                auto [matched, total] = KeywordMatch(deal, tokens);
                double relevance = total > 0 ? static_cast<double>(matched) / total : 0.0;
                score = 0.5 * relevance + 0.3 * rating_norm + 0.2 * discount;
            }
            survivors.push_back({deal, score});
        }

        std::sort(survivors.begin(), survivors.end(),
                  [](const ScoredDeal& a, const ScoredDeal& b) { return a.score > b.score; });

        nlohmann::json out_items = nlohmann::json::array();
        for (size_t i = 0; i < survivors.size() && static_cast<int>(i) < top_k; ++i) {
            nlohmann::json item = survivors[i].deal;
            item["score"] = survivors[i].score;
            if (!item.contains("reason") || item["reason"].get<std::string>().empty()) {
                item["reason"] = BuildReason(survivors[i].deal);
            }
            out_items.push_back(item);
        }

        result.success = true;
        nlohmann::json out;
        out["items"] = out_items;
        out["total"] = survivors.size();
        result.result_json = out.dump();
        spdlog::info("deal_retriever: city={}, category={}, keywords='{}' -> {}/{} matched",
                     city, category, keywords, out_items.size(), survivors.size());
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("deal_retriever error: ") + e.what();
    }

    co_return result;
}

// ---------------------------------------------------------------------------
// DealRanker
// ---------------------------------------------------------------------------

std::string DealRanker::SchemaJson() const {
    return R"({
        "name": "deal_ranker",
        "description": "对召回候选做多因子重排（评分/销量/价格契合/折扣），可按禁忌词过滤，取 top_n",
        "parameters": {
            "type": "object",
            "required": ["candidates"],
            "properties": {
                "candidates": {"type": "array", "description": "召回结果数组；为空时由编排器自动注入上一步召回结果"},
                "budget": {"type": "number", "description": "预算，用于价格契合度评分（超预算大幅扣分）"},
                "people": {"type": "integer", "description": "人数，可选"},
                "taboo": {"type": "string", "description": "禁忌/不喜欢，如 不吃辣、海鲜过敏；命中则剔除"},
                "top_n": {"type": "integer", "default": 3}
            }
        }
    })";
}

coro::Task<ToolResult> DealRanker::Execute(const ToolCall& call) {
    ToolResult result;
    result.call_id = call.call_id;

    try {
        auto args = nlohmann::json::parse(call.arguments_json);
        auto candidates = args.value("candidates", nlohmann::json::array());
        double budget = args.value("budget", 0.0);
        int top_n = args.value("top_n", 3);
        if (top_n <= 0) top_n = 3;
        std::string taboo = args.value("taboo", "");
        auto taboo_tokens = Tokenize(taboo);

        // Optional taboo filtering.
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& c : candidates) {
            if (!taboo_tokens.empty()) {
                auto [matched, total] = KeywordMatch(c, taboo_tokens);
                if (matched > 0) continue;   // contains a taboo term -> drop
            }
            filtered.push_back(c);
        }

        // Popularity normalization denominator.
        long max_sold = 1;
        for (const auto& c : filtered) {
            long s = c.value("sold_count", 0L);
            if (s > max_sold) max_sold = s;
        }

        struct Scored { nlohmann::json deal; double score; };
        std::vector<Scored> scored;
        for (const auto& c : filtered) {
            double rating_norm = c.value("rating", 0.0) / 5.0;
            double popularity = static_cast<double>(c.value("sold_count", 0L)) / max_sold;
            double price = c.value("price", 0.0);
            double price_fit = 1.0;
            if (budget > 0.0) {
                price_fit = (price <= budget)
                    ? 1.0
                    : std::max(0.0, 1.0 - (price - budget) / budget);
            }
            double discount = DiscountOf(c);
            double score = 0.35 * rating_norm
                         + 0.25 * popularity
                         + 0.25 * price_fit
                         + 0.15 * discount;
            scored.push_back({c, score});
        }

        std::sort(scored.begin(), scored.end(),
                  [](const Scored& a, const Scored& b) { return a.score > b.score; });

        nlohmann::json out_items = nlohmann::json::array();
        for (size_t i = 0; i < scored.size() && static_cast<int>(i) < top_n; ++i) {
            nlohmann::json item = scored[i].deal;
            item["score"] = scored[i].score;
            if (!item.contains("reason") || item["reason"].get<std::string>().empty()) {
                item["reason"] = BuildReason(scored[i].deal);
            }
            out_items.push_back(item);
        }

        result.success = true;
        nlohmann::json out;
        out["items"] = out_items;
        out["total"] = scored.size();
        result.result_json = out.dump();
        spdlog::info("deal_ranker: budget={}, taboo='{}' -> {}/{} kept",
                     budget, taboo, out_items.size(), candidates.size());
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("deal_ranker error: ") + e.what();
    }

    co_return result;
}

} // namespace agent
