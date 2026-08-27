#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace agent {

struct UserContext {
    std::string user_id;
    std::string session_id;
    std::string city;
    std::string language = "zh";
    std::string device_type;
    double longitude = 0.0;
    double latitude = 0.0;
};

struct SlotValue {
    std::string slot_name;
    std::string value;
    double confidence = 1.0;
};

struct ConversationTurn {
    int turn_id = 0;
    std::string role;      // user / assistant / tool
    std::string content;
    std::string created_at;
};

struct ClarificationQuestion {
    std::string question;
    std::vector<std::string> candidate_options;
};

struct RecommendationItem {
    std::string item_id;
    std::string merchant_id;
    std::string title;
    std::string category;
    double price = 0.0;
    double original_price = 0.0;
    std::string city;
    std::string district;             // 区县/商圈，用于就近推荐
    std::vector<std::string> tags;
    double score = 0.0;               // 排序得分（归一化到 0~1）
    long sold_count = 0;              // 销量，排序因子之一
    double rating = 0.0;              // 评分（0~5），排序因子之一
    int min_people = 0;               // 套餐适用人数下限（0 = 未知）
    int max_people = 0;               // 上限（0 = 未知）；事实校验派生白名单用
    std::string reason;
};

// Metadata for one LLM call, returned by planner/composer so the orchestrator
// can persist an audit row (llm_calls) keyed by trace_id. Streaming compose
// calls fill tokens from the trailing SSE usage chunk when the upstream sends
// one (the gateway requests stream_options.include_usage); otherwise tokens
// stay 0, which is honest.
struct LlmCallInfo {
    std::string purpose;          // plan / compose
    std::string model;
    std::string status;           // success / parse_error / template_fallback / stream_fallback
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int attempt = 0;              // planner retry index (0 = first)
    std::chrono::milliseconds latency{0};
    // The user-turn prompt as sent (the planning prompt carries the
    // user-profile section). Persisted for audit/contrast checks (Phase 2.3-D).
    std::string raw_request;
};

struct RecommendationResult {
    std::string session_id;
    std::string trace_id;
    std::string response_text;
    std::vector<RecommendationItem> items;
    bool is_clarifying = false;
    std::string next_state;
    // Knowledge-base passages (kb_search) that grounded the reply, rendered as
    // readable snippets ("【title】content（来源：source）"). Empty when no RAG
    // retrieval ran, so non-RAG responses are unchanged.
    std::vector<std::string> grounding;
    // --- observability (never serialized to clients) ---
    // How the reply was produced: llm_stream / llm / template / short_circuit.
    std::string compose_mode;
    // Guard audit (Phase 4-C): which guard intervened, if any:
    // "" / refuse_input / sanitized / fact_violation. Detail carries the
    // violation summary (capped) for fact_violation, the risk type for
    // refuse_input.
    std::string guard_action;
    std::string guard_detail;
    // LLM calls made while composing (planner calls are carried on Plan).
    std::vector<LlmCallInfo> llm_calls;
};

struct ToolCall {
    std::string tool_name;
    std::string call_id;
    std::string arguments_json;
};

struct ToolResult {
    std::string call_id;
    bool success = false;
    std::string result_json;
    std::string error_message;
};

struct Status {
    int code = 0;
    std::string message;
    bool ok() const { return code == 0; }
    static Status OK() { return Status{}; }
    static Status Error(int c, std::string msg) {
        Status s;
        s.code = c;
        s.message = std::move(msg);
        return s;
    }
};

} // namespace agent
