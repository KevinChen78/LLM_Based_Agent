#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace agent {

// Phase 5-B: in-memory per-user token-bucket rate limiter.
//
// Configured via RATE_LIMIT_RPS / RATE_LIMIT_BURST; empty or 0 => disabled
// and every request passes (default behaviour byte-identical to before).
// Buckets are keyed by a caller-chosen identity string (user_id, then API
// key, then a constant fallback). SSE streaming requests count once per
// request at handler entry — never by connection duration.
class RateLimiter {
public:
    // RATE_LIMIT_RPS: sustained tokens/second per key; RATE_LIMIT_BURST:
    // bucket capacity (defaults to max(1, rps) when unset).
    static RateLimiter FromEnv();

    // rps <= 0 => disabled.
    RateLimiter(double rps, double burst);

    bool Enabled() const { return rps_ > 0.0; }

    // True when the request may proceed. When rejected, retry_after_seconds
    // (if non-null) receives the seconds until one token is available.
    bool Allow(const std::string& key, double* retry_after_seconds = nullptr);

    double rps() const { return rps_; }
    double burst() const { return burst_; }

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point last_refill;
    };

    double rps_ = 0.0;
    double burst_ = 0.0;
    std::mutex mu_;
    std::unordered_map<std::string, Bucket> buckets_;
};

} // namespace agent
