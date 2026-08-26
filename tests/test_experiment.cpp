#include "agent/experiment_manager.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace agent;

// Bucketing must be deterministic: same user + experiment => same bucket,
// across calls (and processes, since FNV-1a is implementation-stable).
TEST(ExperimentManager, BucketIsDeterministic) {
    ExperimentManager em(ExperimentManager::Mode::kActive, 50, "ranker_v1");
    EXPECT_EQ(em.Bucket("user-123"), em.Bucket("user-123"));
    EXPECT_GE(em.Bucket("user-123"), 0);
    EXPECT_LT(em.Bucket("user-123"), 100);
}

TEST(ExperimentManager, EmptyUserNeverExperiments) {
    ExperimentManager em(ExperimentManager::Mode::kActive, 100, "ranker_v1");
    EXPECT_EQ(em.Bucket(""), -1);
    EXPECT_EQ(em.Group(""), "control");
    EXPECT_FALSE(em.UseModel(""));
}

TEST(ExperimentManager, TreatmentPctBoundaries) {
    ExperimentManager all(ExperimentManager::Mode::kActive, 100, "exp");
    EXPECT_EQ(all.Group("anyone"), "treatment");
    EXPECT_TRUE(all.UseModel("anyone"));

    ExperimentManager none(ExperimentManager::Mode::kActive, 0, "exp");
    EXPECT_EQ(none.Group("anyone"), "control");
    EXPECT_FALSE(none.UseModel("anyone"));
}

TEST(ExperimentManager, DistributionRoughlyMatchesPct) {
    ExperimentManager em(ExperimentManager::Mode::kActive, 50, "ranker_v1");
    int treatment = 0;
    constexpr int kN = 1000;
    for (int i = 0; i < kN; ++i) {
        if (em.Group("user-" + std::to_string(i)) == "treatment") ++treatment;
    }
    // Expect ~500; allow a wide band — this catches a broken hash, not noise.
    EXPECT_GT(treatment, kN * 42 / 100);
    EXPECT_LT(treatment, kN * 58 / 100);
}

TEST(ExperimentManager, ModesGateModelAndShadow) {
    ExperimentManager off(ExperimentManager::Mode::kOff, 50, "exp");
    EXPECT_FALSE(off.UseModel("u"));
    EXPECT_FALSE(off.WantShadowScore("u"));

    ExperimentManager shadow(ExperimentManager::Mode::kShadow, 50, "exp");
    EXPECT_FALSE(shadow.UseModel("u"));
    EXPECT_TRUE(shadow.WantShadowScore("u"));

    ExperimentManager active(ExperimentManager::Mode::kActive, 100, "exp");
    EXPECT_TRUE(active.UseModel("u"));
    EXPECT_FALSE(active.WantShadowScore("u"));  // treatment: served, not shadowed

    ExperimentManager active0(ExperimentManager::Mode::kActive, 0, "exp");
    EXPECT_FALSE(active0.UseModel("u"));
    EXPECT_TRUE(active0.WantShadowScore("u"));  // control: shadow-scored
}

// Experiment name is part of the hash key, so re-running the same user under
// a new experiment re-randomizes (and Bucket stays in range).
TEST(ExperimentManager, ExperimentNameAffectsBucket) {
    ExperimentManager a(ExperimentManager::Mode::kActive, 50, "exp_a");
    ExperimentManager b(ExperimentManager::Mode::kActive, 50, "exp_b");
    // Not asserting they differ for every user (they could coincide), just
    // that both stay in range and the names are kept.
    EXPECT_EQ(a.ExperimentName(), "exp_a");
    EXPECT_EQ(b.ExperimentName(), "exp_b");
    EXPECT_GE(a.Bucket("u"), 0);
    EXPECT_GE(b.Bucket("u"), 0);
}
