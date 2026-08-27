#include "agent/api_auth.hpp"

#include <cstdlib>

namespace agent {

namespace {

// Constant-time equality: folds the length difference into the accumulator
// and scans max(a,b) bytes, so timing depends only on the input lengths.
bool KeysEqual(const std::string& a, const std::string& b) {
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

std::string Trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace

ApiAuth ApiAuth::FromEnv() {
    const char* env = std::getenv("AGENT_API_KEYS");
    std::vector<std::string> keys;
    if (env) {
        std::string rest = env;
        size_t pos;
        while ((pos = rest.find(',')) != std::string::npos) {
            std::string k = Trim(rest.substr(0, pos));
            if (!k.empty()) keys.push_back(k);
            rest.erase(0, pos + 1);
        }
        std::string k = Trim(rest);
        if (!k.empty()) keys.push_back(k);
    }
    return ApiAuth(std::move(keys));
}

ApiAuth::ApiAuth(std::vector<std::string> keys) : keys_(std::move(keys)) {}

bool ApiAuth::Check(const std::string& presented) const {
    if (keys_.empty()) return true;  // auth disabled
    bool match = false;
    for (const auto& k : keys_) {
        // No early exit: scanning every key keeps timing independent of
        // which key (or key prefix) matched.
        match = KeysEqual(presented, k) || match;
    }
    return match;
}

} // namespace agent
