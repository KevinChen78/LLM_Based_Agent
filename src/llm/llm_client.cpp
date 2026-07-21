#include "agent/llm_client.hpp"
#include "agent/prompt_builder.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

// --- Platform socket primitives (blocking; used for SSE streaming POST) ---
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using ag_sock_t = SOCKET;
  #define AG_SOCK_INVALID INVALID_SOCKET
  #define AG_SOCK_CLOSE  closesocket
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using ag_sock_t = int;
  #define AG_SOCK_INVALID (-1)
  #define AG_SOCK_CLOSE  ::close
#endif

namespace agent {

HttpLlmClient::HttpLlmClient(std::string base_url, std::string api_key)
    : base_url_(std::move(base_url))
    , api_key_(std::move(api_key)) {}

// Default streaming implementation: issue one non-streaming Chat() call.
// streamed stays false and on_delta is intentionally NOT invoked, signaling to
// callers (e.g. ResponseComposer) that no real token stream took place.
coro::Task<LlmClient::LlmStreamResult> LlmClient::ChatStream(
    const std::vector<LlmMessage>& messages,
    const Options& options,
    const DeltaCallback& /*on_delta*/) {
    auto resp = co_await Chat(messages, options);
    LlmStreamResult r;
    r.text = resp.raw_text;
    r.latency = resp.latency;
    r.streamed = false;
    co_return r;
}

// Parse one SSE event block (text between blank-line separators) and forward
// any `delta.content` to the callback / accumulator.
namespace {
void HandleSseEvent(const std::string& evt,
                    const LlmClient::DeltaCallback& on_delta,
                    std::string& accumulated) {
    std::istringstream iss(evt);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        static const std::string prefix = "data:";
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string payload = line.substr(prefix.size());
        while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\t')) {
            payload.erase(0, 1);
        }
        if (payload.empty() || payload == "[DONE]") continue;
        try {
            auto j = nlohmann::json::parse(payload);
            if (j.contains("choices") && !j["choices"].empty()) {
                auto& choice = j["choices"][0];
                if (choice.contains("delta") && choice["delta"].contains("content")) {
                    const auto& c = choice["delta"]["content"];
                    if (c.is_string()) {
                        std::string s = c.get<std::string>();
                        if (!s.empty()) {
                            accumulated += s;
                            if (on_delta) on_delta(s);
                        }
                    }
                }
            }
        } catch (const std::exception&) {
            // Ignore malformed chunk — streaming is best-effort.
        }
    }
}

#ifdef _WIN32
void EnsureWinsock() {
    // Ref-counted; safe to call alongside httplib's own initialization.
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
}
#endif

struct HttpUrl {
    bool ok = false;
    std::string host;
    int port = 80;
};

// Parse an "http://host[:port]" base URL. HTTPS is unsupported (we stream over
// plain TCP to the local gateway), so https URLs report ok=false and the caller
// falls back to non-streaming Chat().
HttpUrl ParseHttpUrl(const std::string& url) {
    HttpUrl u;
    std::string s = url;
    if (s.rfind("http://", 0) == 0) {
        s.erase(0, 7);
    } else if (s.rfind("https://", 0) == 0) {
        return u;   // TLS not supported here
    }
    auto slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    auto colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        u.host = hostport.substr(0, colon);
        try {
            u.port = std::stoi(hostport.substr(colon + 1));
        } catch (...) {
            return u;
        }
    } else {
        u.host = hostport;
        u.port = 80;
    }
    u.ok = !u.host.empty();
    return u;
}

bool SendAll(ag_sock_t sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(sock, data.data() + sent,
                        static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Read the HTTP response body (delimited by Connection: close), parsing SSE
// events and forwarding delta tokens via on_delta. Returns false on transport
// error or non-200 status.
bool ReadSseStream(ag_sock_t sock,
                   const LlmClient::DeltaCallback& on_delta,
                   std::string& accumulated) {
    std::string raw;
    std::string sse_buf;
    bool headers_done = false;
    char buf[8192];
    for (;;) {
        int n = ::recv(sock, buf, sizeof(buf), 0);
        if (n < 0) return false;     // error or read timeout
        if (n == 0) break;           // EOF — Connection: close
        if (!headers_done) {
            raw.append(buf, n);
            auto sep = raw.find("\r\n\r\n");
            if (sep == std::string::npos) continue;
            if (raw.substr(0, sep).find(" 200 ") == std::string::npos) {
                return false;        // non-200 upstream
            }
            headers_done = true;
            sse_buf = raw.substr(sep + 4);
            raw.clear();
        } else {
            sse_buf.append(buf, static_cast<size_t>(n));
        }
        size_t pos;
        while ((pos = sse_buf.find("\n\n")) != std::string::npos) {
            std::string evt = sse_buf.substr(0, pos);
            sse_buf.erase(0, pos + 2);
            HandleSseEvent(evt, on_delta, accumulated);
        }
    }
    if (!sse_buf.empty()) HandleSseEvent(sse_buf, on_delta, accumulated);
    return true;
}

// Open a blocking TCP connection, send a raw HTTP request, and stream the SSE
// response. Returns true on a completed (200) exchange.
bool StreamHttpSse(const HttpUrl& url, const std::string& request,
                   long recv_timeout_ms,
                   const LlmClient::DeltaCallback& on_delta,
                   std::string& accumulated) {
#ifdef _WIN32
    EnsureWinsock();
#endif
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* ai = nullptr;
    const std::string portstr = std::to_string(url.port);
    if (getaddrinfo(url.host.c_str(), portstr.c_str(), &hints, &ai) != 0 || !ai) {
        return false;
    }

    bool ok = false;
    // Iterate all resolved addresses (e.g. ::1 then 127.0.0.1 for localhost) so
    // we succeed even when the gateway listens on IPv4 only.
    for (struct addrinfo* p = ai; p != nullptr && !ok; p = p->ai_next) {
        ag_sock_t sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == AG_SOCK_INVALID) continue;

#ifdef _WIN32
        DWORD rto = static_cast<DWORD>(recv_timeout_ms);
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rto), sizeof(rto));
#else
        struct timeval tv;
        tv.tv_sec = recv_timeout_ms / 1000;
        tv.tv_usec = (recv_timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

        if (::connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
            if (SendAll(sock, request)) {
                ok = ReadSseStream(sock, on_delta, accumulated);
            }
        }
        AG_SOCK_CLOSE(sock);
    }
    freeaddrinfo(ai);
    return ok;
}
} // namespace

coro::Task<LlmClient::LlmStreamResult> HttpLlmClient::ChatStream(
    const std::vector<LlmMessage>& messages,
    const Options& options,
    const DeltaCallback& on_delta) {
    LlmStreamResult out;
    auto start = std::chrono::steady_clock::now();

    // Offline stub: no upstream to stream from.
    if (base_url_.empty()) {
        co_return out;   // streamed=false
    }

    auto url = ParseHttpUrl(base_url_);
    if (!url.ok) {
        // HTTPS or unparseable base URL -> cannot stream; caller falls back.
        co_return out;
    }

    nlohmann::json req_json;
    req_json["model"] = options.model;
    req_json["messages"] = nlohmann::json::array();
    for (const auto& msg : messages) {
        req_json["messages"].push_back({{"role", msg.role}, {"content", msg.content}});
    }
    req_json["max_tokens"] = options.max_tokens;
    req_json["temperature"] = options.temperature;
    req_json["stream"] = true;
    const std::string body = req_json.dump();

    std::ostringstream reqss;
    reqss << "POST /v1/chat/completions HTTP/1.1\r\n"
          << "Host: " << url.host << ":" << url.port << "\r\n"
          << "Content-Type: application/json\r\n"
          << "Accept: text/event-stream\r\n"
          << "Connection: close\r\n";
    if (!api_key_.empty()) {
        reqss << "Authorization: Bearer " << api_key_ << "\r\n";
    }
    reqss << "Content-Length: " << body.size() << "\r\n\r\n" << body;

    std::string accumulated;
    bool ok = StreamHttpSse(url, reqss.str(),
                            120000,   // recv timeout (ms); a stream may be long
                            on_delta, accumulated);
    if (!ok) {
        spdlog::warn("LLM stream request failed (host={}:{})", url.host, url.port);
        healthy_ = false;
        co_return out;   // streamed=false
    }

    healthy_ = true;
    out.text = std::move(accumulated);
    out.streamed = true;
    out.latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    co_return out;
}

coro::Task<LlmResponse> HttpLlmClient::Chat(
    const std::vector<LlmMessage>& messages,
    const Options& options) {
    LlmResponse resp;
    auto start = std::chrono::steady_clock::now();

    // Phase 0 fallback: if no base_url is configured, behave as a deterministic stub.
    if (base_url_.empty()) {
        std::string last_user_msg;
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->role == "user") {
                last_user_msg = it->content;
                break;
            }
        }

        // Extract the real user message from the planning prompt template.
        // PromptBuilder puts the user input under "# 用户当前输入".
        auto extract_user_input = [](const std::string& prompt) -> std::string {
            const std::string marker = "# 用户当前输入";
            auto pos = prompt.find(marker);
            if (pos == std::string::npos) return prompt;
            pos += marker.size();
            while (pos < prompt.size() && (prompt[pos] == '\n' || prompt[pos] == '\r')) ++pos;
            auto end = prompt.find("#", pos);
            std::string input = (end == std::string::npos)
                ? prompt.substr(pos)
                : prompt.substr(pos, end - pos);
            // trim trailing whitespace/newlines
            while (!input.empty() && (input.back() == '\n' || input.back() == '\r' || input.back() == ' ')) {
                input.pop_back();
            }
            return input;
        };

        std::string user_input = extract_user_input(last_user_msg);

        if (user_input.find("海鲜") != std::string::npos &&
            user_input.find("上海") != std::string::npos) {
            resp.raw_text = R"({
                "action": "retrieve",
                "slots": {
                    "city": "上海",
                    "category": "海鲜",
                    "budget": 300,
                    "people": 3,
                    "time": "今晚",
                    "preference": "",
                    "taboo": ""
                },
                "missing_slots": [],
                "clarification_question": "",
                "tool_calls": [
                    {
                        "tool_name": "deal_retriever",
                        "arguments": {
                            "city": "上海",
                            "category": "海鲜",
                            "max_price": 300,
                            "people": 3,
                            "keywords": "",
                            "top_k": 20
                        }
                    },
                    {
                        "tool_name": "deal_ranker",
                        "arguments": {
                            "candidates": [],
                            "budget": 300,
                            "people": 3,
                            "taboo": "",
                            "top_n": 3
                        }
                    }
                ]
            })";
        } else if (user_input.find("吃") != std::string::npos ||
                   user_input.find("想") != std::string::npos) {
            resp.raw_text = R"({
                "action": "clarify",
                "slots": {
                    "city": "",
                    "category": "海鲜",
                    "budget": 0,
                    "people": 0,
                    "time": "",
                    "preference": "",
                    "taboo": ""
                },
                "missing_slots": ["city", "budget", "people"],
                "clarification_question": "您想在哪个城市吃海鲜？预算和人数大概是多少呢？",
                "tool_calls": []
            })";
        } else {
            resp.raw_text = R"({
                "action": "respond",
                "slots": {},
                "missing_slots": [],
                "clarification_question": "",
                "tool_calls": [],
                "response": "您好，我可以帮您推荐团购套餐，请告诉我城市、人数和预算。"
            })";
        }

        auto end = std::chrono::steady_clock::now();
        resp.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        co_return resp;
    }

    // Build OpenAI-compatible chat completion request
    nlohmann::json req_json;
    req_json["model"] = options.model;
    req_json["messages"] = nlohmann::json::array();
    for (const auto& msg : messages) {
        req_json["messages"].push_back({{"role", msg.role}, {"content", msg.content}});
    }
    req_json["max_tokens"] = options.max_tokens;
    req_json["temperature"] = options.temperature;

    std::string request_body = req_json.dump();

    try {
        httplib::Client cli(base_url_);
        cli.set_connection_timeout(options.timeout.count() / 1000.0);
        cli.set_read_timeout(options.timeout.count() / 1000.0);
        cli.set_write_timeout(options.timeout.count() / 1000.0);

        httplib::Headers headers{
            {"Content-Type", "application/json"}
        };
        if (!api_key_.empty()) {
            headers.emplace("Authorization", "Bearer " + api_key_);
        }

        auto res = cli.Post("/v1/chat/completions", headers, request_body, "application/json");

        if (!res) {
            spdlog::error("LLM HTTP request failed: no response");
            healthy_ = false;
            resp.raw_text = R"({"action":"FALLBACK"})";
            co_return resp;
        }

        if (res->status != 200) {
            spdlog::error("LLM HTTP error: status={}, body={}", res->status, res->body);
            healthy_ = false;
            resp.raw_text = R"({"action":"FALLBACK"})";
            co_return resp;
        }

        auto j = nlohmann::json::parse(res->body);
        if (j.contains("choices") && !j["choices"].empty()) {
            resp.raw_text = j["choices"][0]["message"]["content"].get<std::string>();
        } else {
            resp.raw_text = res->body;
        }
        resp.prompt_tokens = j.value("usage", nlohmann::json::object()).value("prompt_tokens", 0);
        resp.completion_tokens = j.value("usage", nlohmann::json::object()).value("completion_tokens", 0);
        healthy_ = true;
    } catch (const std::exception& e) {
        spdlog::error("LLM HTTP exception: {}", e.what());
        healthy_ = false;
        resp.raw_text = R"({"action":"FALLBACK"})";
    }

    auto end = std::chrono::steady_clock::now();
    resp.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    co_return resp;
}

bool HttpLlmClient::Healthy() const {
    return healthy_.load();
}

} // namespace agent
