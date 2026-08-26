#include "agent/experiment_manager.hpp"

#include <cstdint>
#include <cstdlib>

namespace agent {

namespace {

uint32_t Fnv1a32(const std::string& s) {
    uint32_t h = 2166136261u;  // FNV offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;        // FNV prime
    }
    return h;
}

} // namespace

ExperimentManager ExperimentManager::FromEnv() {
    const char* mode_env = std::getenv("RANKER_MODE");
    const std::string mode_str = mode_env ? mode_env : "off";
    Mode mode = Mode::kOff;
    if (mode_str == "shadow") mode = Mode::kShadow;
    else if (mode_str == "active") mode = Mode::kActive;

    int pct = 50;
    if (const char* pct_env = std::getenv("RANKER_TREATMENT_PCT")) {
        try {
            pct = std::stoi(pct_env);
        } catch (...) {
            pct = 50;
        }
    }

    const char* name_env = std::getenv("RANKER_EXPERIMENT");
    return ExperimentManager(mode, pct, name_env ? name_env : "ranker_v1");
}

int ExperimentManager::Bucket(const std::string& user_id) const {
    if (user_id.empty()) return -1;
    return static_cast<int>(Fnv1a32(experiment_ + ":" + user_id) % 100u);
}

std::string ExperimentManager::Group(const std::string& user_id) const {
    const int b = Bucket(user_id);
    if (b < 0) return "control";
    return b < treatment_pct_ ? "treatment" : "control";
}

} // namespace agent
