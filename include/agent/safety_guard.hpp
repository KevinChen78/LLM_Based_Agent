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
    SafetyGuard();

    // Synchronous input check. Runs before the TaskPlanner.
    InputGuardResult CheckInput(const std::string& user_message) const;

    // Mask PII (phone / email / ID card / long digit runs) in arbitrary text.
    static std::string MaskPii(const std::string& text);

    // Strip banned words (*** ) from reply text.
    static std::string StripBannedWords(const std::string& text);

    // Sanitize the final reply text (PII mask + banned-word strip).
    std::string SanitizeOutputText(const std::string& text) const;

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
};

} // namespace agent
