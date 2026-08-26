#pragma once

#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

struct sqlite3;  // forward declaration — header does not include sqlite3.h

namespace agent {

// Audit/observability store: one recommendation_logs row per request and one
// llm_calls row per LLM invocation, joined by trace_id. Lives in its own
// database file (default data/observability.db) so audit traffic never
// contends with session reads; the feedback table (in the sessions DB) is
// joined via ATTACH at aggregation time.
//
// Writes are fire-and-forget: failures are logged and never propagated to the
// recommendation path. Methods are plain sync calls (single connection +
// mutex), not coroutines — there is nothing to await.
class ObservabilityStore {
public:
    struct RecLogEntry {
        std::string trace_id;
        std::string session_id;
        std::string user_id;
        std::string request_text;
        std::string action;            // retrieve / clarify / respond / BLOCKED / FALLBACK / ERROR
        std::string slots_json;
        int item_count = 0;
        std::string ranked_items_json; // top-N [{item_id, score}]
        std::string response_text;
        int grounding_count = 0;
        std::string compose_mode;      // llm_stream / llm / template / short_circuit / none
        long latency_ms = 0;
        // Learning-to-rank audit (Phase 2.1); empty when no ranker ran.
        std::string candidates_json;   // ranker input candidates [{item_id, rule_score, model_score|null}] cap 50
        std::string experiment_group;  // control / treatment / "" (experiment off)
        std::string rank_mode;         // rule / model / rule_fallback / "" (no ranker this request)
    };

    struct LlmCallEntry {
        std::string trace_id;
        std::string session_id;
        std::string purpose;           // plan / compose
        std::string model;
        std::string status;            // success / parse_error / template_fallback / stream_fallback
        int prompt_tokens = 0;
        int completion_tokens = 0;
        int attempt = 0;
        long latency_ms = 0;
    };

    // Opens (or creates) the database file and initialises the schema.
    // Throws on failure.
    explicit ObservabilityStore(const std::string& db_path);
    ~ObservabilityStore();

    ObservabilityStore(const ObservabilityStore&) = delete;
    ObservabilityStore& operator=(const ObservabilityStore&) = delete;

    void LogRecommendation(const RecLogEntry& e);
    void LogLlmCall(const LlmCallEntry& e);

    // Aggregated metrics as JSON. sessions_db_path is ATTACHed to compute
    // feedback satisfaction; if the attach fails the "feedback" section is
    // simply omitted.
    nlohmann::json Aggregate(const std::string& sessions_db_path);

private:
    void InitSchema();

    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};

} // namespace agent
