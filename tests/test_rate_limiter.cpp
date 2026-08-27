// Phase 5-B: token-bucket rate limiter — disabled-by-default contract,
// burst/reject/refill behaviour, per-key independence, Retry-After value.

#include "agent/rate_limiter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace agent;

TEST(RateLimiter, DisabledAllowsEverything) {
    RateLimiter rl(0.0, 0.0);
    EXPECT_FALSE(rl.Enabled());
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(rl.Allow("u:1"));
    }
}

TEST(RateLimiter, BurstThenReject) {
    RateLimiter rl(1.0, 2.0);   // 1 rps, capacity 2
    EXPECT_TRUE(rl.Enabled());
    EXPECT_TRUE(rl.Allow("u:1"));
    EXPECT_TRUE(rl.Allow("u:1"));
    double retry = -1.0;
    EXPECT_FALSE(rl.Allow("u:1", &retry));
    EXPECT_GT(retry, 0.0);
    EXPECT_LE(retry, 1.0);      // one token takes 1/rps = 1s
}

TEST(RateLimiter, RefillsOverTime) {
    RateLimiter rl(20.0, 1.0);  // one token per 50ms
    EXPECT_TRUE(rl.Allow("u:1"));
    EXPECT_FALSE(rl.Allow("u:1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_TRUE(rl.Allow("u:1"));
}

TEST(RateLimiter, KeysAreIndependent) {
    RateLimiter rl(0.5, 1.0);
    EXPECT_TRUE(rl.Allow("u:a"));
    EXPECT_FALSE(rl.Allow("u:a"));
    EXPECT_TRUE(rl.Allow("u:b"));   // separate bucket
    EXPECT_TRUE(rl.Allow("u:c"));
}

TEST(RateLimiter, BurstCappedAtCapacity) {
    RateLimiter rl(1000.0, 2.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // would be 50 tokens uncapped
    EXPECT_TRUE(rl.Allow("u:1"));
    EXPECT_TRUE(rl.Allow("u:1"));
    EXPECT_FALSE(rl.Allow("u:1"));  // capped at burst=2
}
