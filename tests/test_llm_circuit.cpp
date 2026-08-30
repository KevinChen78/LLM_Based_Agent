#include "agent/llm_client.hpp"

#include <gtest/gtest.h>

#include <chrono>

using namespace agent;

// Phase 9-A0: HttpLlmClient circuit breaker. Uses a real (almost certainly
// closed) port so the transport genuinely fails — no mocking of the failure
// path we actually care about.

namespace {

HttpLlmClient::Options FastOptions() {
    HttpLlmClient::Options o;
    o.timeout = std::chrono::milliseconds(3000);
    return o;
}

std::vector<LlmMessage> DummyMessages() {
    return {{"user", "ping"}};
}

} // namespace

TEST(LlmClientCircuit, OpensAfterConsecutiveFailuresThenFailsFast) {
    // Port 1 is privileged and never a listener in dev/CI.
    HttpLlmClient client("http://127.0.0.1:1", "");

    auto r1 = client.Chat(DummyMessages(), FastOptions()).result();
    EXPECT_NE(r1.raw_text.find("FALLBACK"), std::string::npos);
    EXPECT_FALSE(client.Healthy());
    // One failure: below the breaker threshold, requests still go out.
    auto t0 = std::chrono::steady_clock::now();
    auto r2 = client.Chat(DummyMessages(), FastOptions()).result();
    (void)r2;
    // Second consecutive failure opens the circuit.

    // From now on Chat must fail fast — no refused-connect tax (measured at
    // ~2.05s/attempt on Windows). Generous bound for slow CI.
    auto r3 = client.Chat(DummyMessages(), FastOptions()).result();
    const auto fast_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_NE(r3.raw_text.find("FALLBACK"), std::string::npos);
    EXPECT_LT(fast_ms, 3000);   // r2 (real attempt) + r3 (fast fail)
    EXPECT_FALSE(client.Healthy());
}

TEST(LlmClientCircuit, StreamFailsFastWhenCircuitOpen) {
    HttpLlmClient client("http://127.0.0.1:1", "");
    (void)client.Chat(DummyMessages(), FastOptions()).result();
    (void)client.Chat(DummyMessages(), FastOptions()).result();
    // Circuit now open.

    bool delta_called = false;
    auto t0 = std::chrono::steady_clock::now();
    auto sr = client.ChatStream(DummyMessages(), FastOptions(),
                                [&delta_called](const std::string&) {
                                    delta_called = true;
                                }).result();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_FALSE(sr.streamed);
    EXPECT_FALSE(delta_called);
    EXPECT_LT(ms, 1000);
}
