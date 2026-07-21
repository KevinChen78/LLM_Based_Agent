#pragma once

#include "agent/stream_emitter.hpp"
#include "coro/core/task.hpp"
#include "coro/io/tcp.hpp"

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

// Server-Sent Events implementation of StreamEmitter.
// Events are queued and drained by the coroutine returned from Writer().
class SseStreamEmitter : public StreamEmitter,
                         public std::enable_shared_from_this<SseStreamEmitter> {
public:
    SseStreamEmitter();
    ~SseStreamEmitter() override;

    // StreamEmitter interface
    void Emit(const std::string& event_type, const nlohmann::json& payload) override;
    void EmitDelta(const std::string& text_delta) override;
    void Finish(const RecommendationResult& result) override;
    void Error(const std::string& message) override;
    bool IsClosed() const override;

    // Returns the coroutine that the HTTP server invokes to write the stream.
    // The callback captures a shared_ptr to keep this emitter alive.
    std::function<coro::Task<void>(coro::io::TcpStream&)> Writer();

private:
    struct QueuedEvent {
        std::string event_type;
        nlohmann::json payload;
    };

    void PushEvent(const std::string& event_type, nlohmann::json payload);
    static std::string FormatSse(const std::string& event_type, const nlohmann::json& payload);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<QueuedEvent> queue_;
    bool closed_ = false;
    bool error_ = false;
};

} // namespace agent
