#pragma once

#include <string>
#include <vector>

namespace agent {

// Prompt templates for TaskPlanner and ResponseComposer.
// In production these should be loaded from config files and versioned.
class PromptBuilder {
public:
    static std::string TaskPlanningPrompt(
        const std::string& history,
        const std::string& user_message,
        const std::string& current_slots_json,
        const std::string& user_profile_json = "",
        // Pre-joined ("、") list of categories that actually exist in the
        // catalog (Phase 3-A). When non-empty the prompt carries a category
        // whitelist section and the category slot rule tightens to it; when
        // empty (no catalog wired) the prompt is byte-identical to before.
        const std::string& category_list = "");

    static std::string ResponseCompositionPrompt(
        const std::string& user_request,
        const std::string& slots_json,
        const std::string& items_json,
        const std::string& grounding = "");

    // Streaming variant: asks the model to emit natural-language prose
    // directly (no JSON, no markdown fences) so tokens can be forwarded
    // to the client verbatim as deltas.
    static std::string ResponseCompositionStreamPrompt(
        const std::string& user_request,
        const std::string& slots_json,
        const std::string& items_json,
        const std::string& grounding = "");

    static std::string InputSafetyPrompt(const std::string& user_message);

    static std::string FallbackPrompt(
        const std::string& fallback_reason,
        const std::string& user_message);
};

} // namespace agent
