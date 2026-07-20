#include "agent/llm_client.hpp"
#include "agent/prompt_builder.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace agent {

HttpLlmClient::HttpLlmClient(std::string base_url, std::string api_key)
    : base_url_(std::move(base_url))
    , api_key_(std::move(api_key)) {}

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
                        "tool_name": "mock_retriever",
                        "arguments": {
                            "city": "上海",
                            "category": "海鲜",
                            "max_price": 300,
                            "people": 3,
                            "keywords": "",
                            "top_k": 20
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
