#include "agent/rate_limiter.hpp"

#include <algorithm>
#include <cstdlib>

namespace agent {

namespace {

double EnvDouble(const char* name, double dflt) {
    const char* env = std::getenv(name);
    if (!env || !*env) return dflt;
    char* end = nullptr;
    double v = std::strtod(env, &end);
    if (end == env || v < 0.0) return dflt;  // unparseable/negative => default
    return v;
}

} // namespace

RateLimiter RateLimiter::FromEnv() {
    const double rps = EnvDouble("RATE_LIMIT_RPS", 0.0);
    const double burst = EnvDouble("RATE_LIMIT_BURST",
                                   rps > 1.0 ? rps : 1.0);
    return RateLimiter(rps, burst);
}

RateLimiter::RateLimiter(double rps, double burst)
    : rps_(rps), burst_(burst > 0.0 ? burst : 1.0) {}

bool RateLimiter::Allow(const std::string& key, double* retry_after_seconds) {
    if (!Enabled()) return true;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mu_);
    Bucket& b = buckets_[key];
    if (b.last_refill == std::chrono::steady_clock::time_point{}) {
        b.tokens = burst_;
        b.last_refill = now;
    }
    // Refill proportional to elapsed time, capped at burst capacity.
    const double elapsed = std::chrono::duration<double>(now - b.last_refill).count();
    b.tokens = std::min(burst_, b.tokens + elapsed * rps_);
    b.last_refill = now;
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    if (retry_after_seconds) {
        *retry_after_seconds = (1.0 - b.tokens) / rps_;
    }
    return false;
}

} // namespace agent
