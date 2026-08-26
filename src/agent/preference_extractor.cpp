#include "agent/preference_extractor.hpp"

#include "agent/deal_catalog.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace agent {

namespace {

// Split a taboo string on the same separators the DealRanker's Tokenize uses.
std::vector<std::string> SplitTaboo(const std::string& text) {
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

// Top-n keys by score desc (tie: lexicographic for determinism), keeping
// only strictly positive scores.
std::vector<std::string> TopKeys(const std::map<std::string, int>& scores, size_t n) {
    std::vector<std::pair<std::string, int>> v(scores.begin(), scores.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::vector<std::string> out;
    for (const auto& [key, score] : v) {
        if (score <= 0 || out.size() >= n) break;
        if (!key.empty()) out.push_back(key);
    }
    return out;
}

} // namespace

UserProfile PreferenceExtractor::Extract(
    const std::string& user_id,
    const std::vector<FeedbackSignal>& feedback,
    const std::vector<SlotRecord>& slot_history,
    const DealCatalog& catalog) {
    UserProfile p;
    p.user_id = user_id;

    // item_id -> deal lookup for category/city/discount resolution.
    std::map<std::string, nlohmann::json> by_id;
    for (const auto& d : catalog.Deals()) {
        auto id = d.value("item_id", "");
        if (!id.empty()) by_id[id] = d;
    }

    std::map<std::string, int> cat_scores, city_scores;
    int liked = 0, liked_deep_discount = 0;
    for (const auto& f : feedback) {
        auto it = by_id.find(f.item_id);
        if (it == by_id.end()) continue;   // item left the catalog — skip
        const int w = f.feedback_type == "like" ? 1
                    : f.feedback_type == "dislike" ? -1 : 0;
        if (w == 0) continue;
        cat_scores[it->second.value("category", "")] += w;
        city_scores[it->second.value("city", "")] += w;
        if (w > 0) {
            ++liked;
            const double price = it->second.value("price", 0.0);
            const double original = it->second.value("original_price", 0.0);
            if (original > 0.0 && (original - price) / original > 0.3) {
                ++liked_deep_discount;
            }
        }
    }
    p.preferred_categories = TopKeys(cat_scores, 3);
    p.preferred_cities = TopKeys(city_scores, 3);
    p.price_sensitivity = liked > 0
        ? static_cast<double>(liked_deep_discount) / liked : 0.5;

    double budget_sum = 0.0;
    int budget_n = 0;
    std::set<std::string> taboo_terms;
    for (const auto& s : slot_history) {
        if (s.budget > 0.0) {
            budget_sum += s.budget;
            ++budget_n;
        }
        for (auto& t : SplitTaboo(s.taboo)) taboo_terms.insert(std::move(t));
    }
    p.avg_budget = budget_n > 0 ? budget_sum / budget_n : 0.0;
    p.dietary_tags.assign(taboo_terms.begin(), taboo_terms.end());

    return p;
}

} // namespace agent
