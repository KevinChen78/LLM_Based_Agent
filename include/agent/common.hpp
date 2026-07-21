#pragma once

#include <string>
#include <vector>
#include <optional>

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
    std::string reason;
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
