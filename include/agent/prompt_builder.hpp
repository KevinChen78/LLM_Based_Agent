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
        const std::string& current_slots_json);

    static std::string ResponseCompositionPrompt(
        const std::string& user_request,
        const std::string& slots_json,
        const std::string& items_json);

    // Streaming variant: asks the model to emit natural-language prose
    // directly (no JSON, no markdown fences) so tokens can be forwarded
    // to the client verbatim as deltas.
    static std::string ResponseCompositionStreamPrompt(
        const std::string& user_request,
        const std::string& slots_json,
        const std::string& items_json);

    static std::string InputSafetyPrompt(const std::string& user_message);

    static std::string FallbackPrompt(
        const std::string& fallback_reason,
        const std::string& user_message);
};

} // namespace agent
