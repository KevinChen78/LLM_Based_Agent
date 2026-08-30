#include "agent/service_circuit.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace agent;

// Phase 8-C: health TTL cache + failure circuit breaker (see
// docs/phase9_latency_report.md §3 for the motivating measurements).

TEST(ServiceCircuit, InitiallyNoCacheAndClosed) {
    ServiceCircuit c;
    EXPECT_TRUE(c.AllowRequest());
    bool ok = true;
    EXPECT_FALSE(c.CachedHealth(ok));   // never checked -> no cache
}

TEST(ServiceCircuit, FailureIsNegativelyCached) {
    ServiceCircuit c;
    c.ReportFailure();
    bool ok = true;
    ASSERT_TRUE(c.CachedHealth(ok));
    EXPECT_FALSE(ok);                   // failure cached -> no re-probe
    EXPECT_TRUE(c.AllowRequest());      // 1st failure: below breaker threshold
}

TEST(ServiceCircuit, SuccessIsCached) {
    ServiceCircuit c;
    c.ReportSuccess();
    bool ok = false;
    ASSERT_TRUE(c.CachedHealth(ok));
    EXPECT_TRUE(ok);
    EXPECT_FALSE(c.CircuitOpen());
}

TEST(ServiceCircuit, BreakerOpensAtThresholdAndRecoversAfterCooldown) {
    ServiceCircuit c({.health_ttl_ms = 50,
                      .breaker_threshold = 2,
                      .breaker_cooldown_ms = 60});
    c.ReportFailure();
    EXPECT_TRUE(c.AllowRequest());      // 1 < threshold
    c.ReportFailure();
    EXPECT_FALSE(c.AllowRequest());     // 2 >= threshold -> open
    EXPECT_TRUE(c.CircuitOpen());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_TRUE(c.AllowRequest());      // cooldown elapsed -> half-open trial
}

TEST(ServiceCircuit, SuccessResetsFailuresAndClosesCircuit) {
    ServiceCircuit c({.health_ttl_ms = 50,
                      .breaker_threshold = 2,
                      .breaker_cooldown_ms = 60000});
    c.ReportFailure();
    c.ReportFailure();
    EXPECT_TRUE(c.CircuitOpen());
    c.ReportSuccess();                  // half-open trial succeeded
    EXPECT_FALSE(c.CircuitOpen());
    bool ok = false;
    ASSERT_TRUE(c.CachedHealth(ok));
    EXPECT_TRUE(ok);
}

TEST(ServiceCircuit, HealthCacheExpires) {
    ServiceCircuit c({.health_ttl_ms = 40,
                      .breaker_threshold = 2,
                      .breaker_cooldown_ms = 60000});
    c.ReportSuccess();
    bool ok = false;
    ASSERT_TRUE(c.CachedHealth(ok));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_FALSE(c.CachedHealth(ok));   // TTL expired -> caller re-probes
}
