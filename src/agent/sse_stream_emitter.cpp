#include "agent/sse_stream_emitter.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <sstream>

namespace agent {

namespace {

std::string Chunk(const std::string& data) {
    std::ostringstream oss;
    oss << std::hex << data.size() << "\r\n" << data << "\r\n";
    return oss.str();
}

} // namespace

SseStreamEmitter::SseStreamEmitter() = default;
SseStreamEmitter::~SseStreamEmitter() = default;

void SseStreamEmitter::Emit(const std::string& event_type, const nlohmann::json& payload) {
    PushEvent(event_type, payload);
}

void SseStreamEmitter::EmitDelta(const std::string& text_delta) {
    PushEvent("delta", nlohmann::json{{"content", text_delta}});
}

void SseStreamEmitter::Finish(const RecommendationResult& result) {
    nlohmann::json j;
    j["session_id"] = result.session_id;
    j["trace_id"] = result.trace_id;
    j["reply"] = result.response_text;
    j["is_clarifying"] = result.is_clarifying;
    j["next_state"] = result.next_state;
    j["items"] = nlohmann::json::array();
    for (const auto& item : result.items) {
        nlohmann::json ji;
        ji["item_id"] = item.item_id;
        ji["title"] = item.title;
        ji["category"] = item.category;
        ji["price"] = item.price;
        ji["original_price"] = item.original_price;
        ji["score"] = item.score;
        ji["reason"] = item.reason;
        ji["tags"] = item.tags;
        j["items"].push_back(ji);
    }
    // RAG grounding passages (only present when kb_search ran).
    if (!result.grounding.empty()) {
        j["grounding"] = result.grounding;
    }
    PushEvent("final", j);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

void SseStreamEmitter::Error(const std::string& message) {
    PushEvent("error", nlohmann::json{{"message", message}});
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        error_ = true;
    }
    cv_.notify_all();
}

bool SseStreamEmitter::IsClosed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::function<coro::Task<void>(coro::io::TcpStream&)> SseStreamEmitter::Writer() {
    auto self = shared_from_this();
    return [self](coro::io::TcpStream& stream) -> coro::Task<void> {
        while (true) {
            std::unique_lock<std::mutex> lock(self->mutex_);
            self->cv_.wait(lock, [&self] {
                return self->closed_ || !self->queue_.empty();
            });

            if (self->queue_.empty() && self->closed_) {
                co_return;
            }

            // Drain all queued events under the lock.
            std::vector<QueuedEvent> batch;
            batch.swap(self->queue_);
            lock.unlock();

            for (const auto& ev : batch) {
                const std::string sse = FormatSse(ev.event_type, ev.payload);
                const std::string chunk = Chunk(sse);
                bool ok = co_await stream.write_all(chunk);
                if (!ok) {
                    spdlog::warn("SSE stream write failed; closing emitter");
                    {
                        std::lock_guard<std::mutex> lk(self->mutex_);
                        self->closed_ = true;
                    }
                    co_return;
                }
            }
        }
    };
}

void SseStreamEmitter::PushEvent(const std::string& event_type, nlohmann::json payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        queue_.push_back(QueuedEvent{event_type, std::move(payload)});
    }
    cv_.notify_one();
}

std::string SseStreamEmitter::FormatSse(const std::string& event_type,
                                        const nlohmann::json& payload) {
    nlohmann::json envelope;
    envelope["event"] = event_type;
    envelope["data"] = payload;
    const std::string line = "data: " + envelope.dump(-1, ' ', false,
                                                       nlohmann::json::error_handler_t::replace);
    return line + "\n\n";
}

} // namespace agent
