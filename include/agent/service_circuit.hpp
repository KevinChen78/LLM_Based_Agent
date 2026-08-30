#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace agent {

// Phase 8-C: health-check TTL cache + failure circuit breaker, shared by the
// HTTP service clients (RetrievalClient / RankerClient).
//
// Motivation (Phase 8-B measured data, docs/phase9_latency_report.md §3):
// on this machine a refused localhost connect costs ~2.05s (WinError 10061 is
// NOT instant on Windows), and every tool call re-paid it while the services
// were down (deal_retriever 4.08s/call, deal_ranker 2.03s/call, ~6.1s per
// retrieve turn of pure waste). Two mechanisms, both thread-safe:
//
//   1. TTL health cache — Healthy() verdicts (success AND failure) are cached
//      for health_ttl_ms, so a known-dead service costs zero network calls
//      instead of one refused connect per turn.
//   2. Circuit breaker — after breaker_threshold consecutive transport
//      failures the circuit opens for breaker_cooldown_ms: AllowRequest()
//      returns false and callers fail fast into their local fallback without
//      touching the network. After the cooldown requests are allowed again
//      (technically every concurrent caller passes until the first Report
//      lands — at this scale that burst IS the recovery probe); its verdict
//      re-closes (success) or re-opens (failure) via ReportSuccess /
//      ReportFailure.
//
// Purely additive to the degradation chain: every fast-fail path lands in the
// same local fallback the slow failure used to reach.
class ServiceCircuit {
public:
    struct Config {
        int health_ttl_ms = 30000;        // success + failure health cache
        int breaker_threshold = 2;        // consecutive failures before open
        int breaker_cooldown_ms = 30000;  // open duration before half-open trial
    };

    explicit ServiceCircuit(Config cfg = {}) : cfg_(cfg) {}

    // True when a request may hit the network right now. False while the
    // circuit is open — the caller must fail fast into its local fallback.
    bool AllowRequest() const {
        return NowMs() >= open_until_ms_.load(std::memory_order_relaxed);
    }

    // If a fresh-enough health verdict is cached, stores it in `ok` and
    // returns true — the caller must NOT hit the network.
    bool CachedHealth(bool& ok) const {
        const int64_t last = health_at_ms_.load(std::memory_order_relaxed);
        if (last < 0 || NowMs() - last > cfg_.health_ttl_ms) return false;
        ok = health_ok_.load(std::memory_order_relaxed);
        return true;
    }

    void ReportSuccess() {
        failures_.store(0, std::memory_order_relaxed);
        open_until_ms_.store(0, std::memory_order_relaxed);
        health_ok_.store(true, std::memory_order_relaxed);
        health_at_ms_.store(NowMs(), std::memory_order_relaxed);
    }

    void ReportFailure() {
        health_ok_.store(false, std::memory_order_relaxed);
        health_at_ms_.store(NowMs(), std::memory_order_relaxed);
        if (failures_.fetch_add(1, std::memory_order_relaxed) + 1 >=
            cfg_.breaker_threshold) {
            open_until_ms_.store(NowMs() + cfg_.breaker_cooldown_ms,
                                 std::memory_order_relaxed);
        }
    }

    // Test introspection.
    bool CircuitOpen() const { return !AllowRequest(); }

private:
    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    Config cfg_;
    std::atomic<int64_t> health_at_ms_{-1};  // -1 = never checked
    std::atomic<bool> health_ok_{false};
    std::atomic<int> failures_{0};
    std::atomic<int64_t> open_until_ms_{0};
};

} // namespace agent
