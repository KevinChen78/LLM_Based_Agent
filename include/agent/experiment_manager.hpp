#pragma once

#include <string>

namespace agent {

// A/B experiment bucketing for the learning-to-rank rollout (Phase 2.1).
//
// Bucketing is deterministic: FNV-1a 32-bit hash of (experiment + user_id)
// mod 100 decides the bucket, so the same user always lands in the same
// group across requests and processes. FNV-1a is implemented locally rather
// than using std::hash, whose output is not guaranteed stable across
// implementations (and we want tests to assert exact buckets).
//
// Config (read once at construction via FromEnv):
//   RANKER_MODE          off (default) | shadow | active
//   RANKER_TREATMENT_PCT 0..100, default 50 — share of users on model scores
//   RANKER_EXPERIMENT    experiment name, default "ranker_v1"
//
// Modes:
//   off    — never call the ranking service (zero added latency).
//   shadow — rule scores serve; model scores are still computed and logged
//            to recommendation_logs for offline comparison.
//   active — treatment bucket serves model scores; control bucket serves
//            rule scores while still logging shadow model scores.
//
// Empty user_id always maps to control (anonymous traffic never experiments).
class ExperimentManager {
public:
    enum class Mode { kOff, kShadow, kActive };

    ExperimentManager() = default;  // kOff
    ExperimentManager(Mode mode, int treatment_pct, std::string experiment)
        : mode_(mode)
        , treatment_pct_(treatment_pct < 0 ? 0 : (treatment_pct > 100 ? 100 : treatment_pct))
        , experiment_(std::move(experiment)) {}

    static ExperimentManager FromEnv();

    Mode GetMode() const { return mode_; }
    const std::string& ExperimentName() const { return experiment_; }
    int TreatmentPct() const { return treatment_pct_; }

    // 0..99 bucket; -1 for empty user_id (never experiments).
    int Bucket(const std::string& user_id) const;

    // "control" | "treatment"; empty user_id or off mode => "control".
    std::string Group(const std::string& user_id) const;

    // Serve model scores to this user?
    bool UseModel(const std::string& user_id) const {
        return mode_ == Mode::kActive && Group(user_id) == "treatment";
    }

    // Compute model scores for audit only (serving stays on rule scores)?
    bool WantShadowScore(const std::string& user_id) const {
        return mode_ == Mode::kShadow ||
               (mode_ == Mode::kActive && Group(user_id) == "control");
    }

private:
    Mode mode_ = Mode::kOff;
    int treatment_pct_ = 50;
    std::string experiment_ = "ranker_v1";
};

} // namespace agent
