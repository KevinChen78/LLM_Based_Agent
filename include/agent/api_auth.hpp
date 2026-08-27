#pragma once

#include <string>
#include <vector>

namespace agent {

// Phase 5-A: API-key authentication for the public HTTP endpoints.
//
// Rule-based and dependency-free. Configured via the AGENT_API_KEYS env var
// (comma-separated keys); when unset or empty the guard is DISABLED and every
// request passes — default behaviour is byte-identical to before.
//
// Comparison is constant-time per key and does not early-exit across keys,
// so response timing does not leak which prefix of a key matched.
class ApiAuth {
public:
    // Reads AGENT_API_KEYS (comma-separated, whitespace-trimmed, empties
    // dropped). Empty/unset => disabled.
    static ApiAuth FromEnv();

    explicit ApiAuth(std::vector<std::string> keys = {});

    // When disabled, Check() always returns true.
    bool Enabled() const { return !keys_.empty(); }

    // True when disabled, or when `presented` matches any configured key.
    bool Check(const std::string& presented) const;

private:
    std::vector<std::string> keys_;
};

} // namespace agent
