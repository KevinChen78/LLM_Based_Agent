#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace agent {

// Tolerantly extract a JSON object from raw LLM output. Models frequently
// wrap JSON in prose or markdown fences, so we try three forms in order:
//   1. clean JSON,
//   2. JSON wrapped in a ```json ... ``` markdown fence,
//   3. JSON embedded in prose (first '{' .. last '}').
// Returns nullopt when none of the forms parse to a JSON object.
//
// Shared by TaskPlanner (plan JSON) and ResponseComposer (reply JSON) so both
// degrade identically when the model is sloppy. Extracted from the composer's
// original TryParseComposition.
inline std::optional<nlohmann::json> ExtractJsonObject(const std::string& raw) {
    auto as_object = [](const std::string& text) -> std::optional<nlohmann::json> {
        try {
            auto j = nlohmann::json::parse(text);
            if (j.is_object()) return j;
        } catch (const std::exception&) {}
        return std::nullopt;
    };

    // 1. Direct parse.
    if (auto j = as_object(raw)) return j;

    // 2. Strip a markdown code fence and retry.
    auto fence = raw.find("```");
    if (fence != std::string::npos) {
        std::string t = raw.substr(fence + 3);
        if (t.rfind("json", 0) == 0) t.erase(0, 4);
        auto close = t.rfind("```");
        if (close != std::string::npos) {
            t.erase(close);
            while (!t.empty() && (t.front() == '\n' || t.front() == '\r' || t.front() == ' ')) {
                t.erase(0, 1);
            }
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) {
                t.pop_back();
            }
            if (auto j = as_object(t)) return j;
        }
    }

    // 3. First '{' .. last '}'.
    auto fb = raw.find('{');
    auto fe = raw.rfind('}');
    if (fb != std::string::npos && fe != std::string::npos && fe > fb) {
        if (auto j = as_object(raw.substr(fb, fe - fb + 1))) return j;
    }

    return std::nullopt;
}

} // namespace agent
