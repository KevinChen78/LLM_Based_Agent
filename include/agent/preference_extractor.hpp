#pragma once

#include "agent/user_profile_store.hpp"

#include <string>
#include <vector>

namespace agent {

class DealCatalog;

// Deterministic, rule-based preference extraction (Phase 2.2). Deliberately
// NOT LLM-based: it must be cheap, reproducible, and unit-testable.
//
// Rules (see preference_extractor.cpp for details):
//   preferred_categories / preferred_cities — liked items' category/city,
//     looked up in the catalog, like=+1 / dislike=-1 weighted, top 3 with a
//     positive score.
//   avg_budget        — mean of budget slots across the user's sessions.
//   dietary_tags      — deduplicated taboo terms from slot history.
//   price_sensitivity — share of liked items whose discount exceeds 30%.
class PreferenceExtractor {
public:
    static UserProfile Extract(const std::string& user_id,
                               const std::vector<FeedbackSignal>& feedback,
                               const std::vector<SlotRecord>& slot_history,
                               const DealCatalog& catalog);
};

} // namespace agent
