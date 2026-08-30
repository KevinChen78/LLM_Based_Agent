#pragma once

#include "agent/common.hpp"

#include <string>
#include <vector>

namespace agent {

// Result of the synchronous, rule-based input check.
struct InputGuardResult {
    bool is_safe = true;
    // "none" | "prompt_injection" | "banned_topic" | "too_long"
    std::string risk_type = "none";
    std::string reason;
    // Polite refusal shown to the user when is_safe == false.
    std::string refusal_reply;
};

// Result of the output fact check (Phase 4-A).
struct FactCheckResult {
    bool ok = true;
    // One entry per unmatched money/discount claim, e.g.
    // "99元 not in {price/original_price/derived per-person,total}".
    std::vector<std::string> violations;
};

// Phase 0 I/O safety guard.
//
// Rule-based and deterministic on purpose: no extra LLM call, no latency,
// no dependency on the gateway being up. The LLM-based InputSafetyPrompt
// (see PromptBuilder) remains available as a future deeper-check layer.
//
// Two responsibilities:
//   1. CheckInput  — runs before planning; blocks prompt-injection, banned
//                    topics and pathologically long inputs.
//   2. Sanitize*   — runs before responding; masks PII and strips banned
//                    words from the final reply and item reasons.
class SafetyGuard {
public:
    // Phase 4-C: `rules_path` optionally points at a JSON file
    // (data/guard_rules.json) overriding the built-in rule lists. A missing
    // or malformed file keeps the built-in defaults byte-identical to before
    // (degradation-chain consistent: the guard never fails to construct).
    explicit SafetyGuard(const std::string& rules_path = "");

    // Synchronous input check. Runs before the TaskPlanner.
    InputGuardResult CheckInput(const std::string& user_message) const;

    // Mask PII (phone / email / ID card / long digit runs) in arbitrary text.
    static std::string MaskPii(const std::string& text);

    // Strip banned words (*** ) from reply text using the built-in default
    // list. Kept static for compatibility; SanitizeOutputText uses the
    // (possibly file-overridden) member list instead.
    static std::string StripBannedWords(const std::string& text);

    // Sanitize the final reply text (PII mask + banned-word strip).
    std::string SanitizeOutputText(const std::string& text) const;

    // Phase 4-A: fact-check a composed reply against the candidate items.
    // Extracts money/discount claims (¥xx, xx元, xx折) and verifies each
    // against the items' price/original_price, plus a derived whitelist:
    // per-person (price/people) and total (price*people) for people in the
    // item's own [min_people, max_people] range, and zhe-level discounts
    // (price/original_price*10). Rule-based, zero LLM calls.
    //
    // Phase 8-C: when `whitelist_user_amounts_` is enabled (rules-file key
    // `fact_check_whitelist_user_amounts`), money numbers that already appear
    // in `user_text` (current input + session slots, passed by the composer)
    // are additionally allowed — the LLM echoing the user's own budget
    // ("您 500 元的预算…") is not a fabricated price. Phase 8-B measured this
    // false-positive pattern at a 33% guard-fallback rate on real traffic.
    FactCheckResult FactCheckReply(
        const std::string& reply,
        const std::vector<RecommendationItem>& items,
        const std::string& user_text = "") const;

    // Sanitize each item's reason / title in place.
    void SanitizeItems(std::vector<RecommendationItem>& items) const;

    // Tunable limits (public so tests can construct edge cases).
    std::size_t max_input_chars = 500;

private:
    // Lower-cased ASCII jailbreak substrings.
    std::vector<std::string> injection_patterns_;
    // Banned-topic substrings (any language, matched verbatim).
    std::vector<std::string> banned_topics_;
    // Banned words to redact from output.
    std::vector<std::string> banned_output_words_;
    // Phase 8-C: allow money numbers echoed from user input / session slots
    // (rules-file key `fact_check_whitelist_user_amounts`; default off keeps
    // the built-in behavior byte-identical to Phase 4 when no file exists).
    bool whitelist_user_amounts_ = false;
};

} // namespace agent
