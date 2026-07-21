#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <optional>
#include <string>

namespace agent {

// HTTP client for the Python retrieval service (BM25 over the deals catalog and
// the knowledge corpus). Mirrors HttpLlmClient's degrade philosophy: an empty
// base_url disables the client (offline mode), and every method returns
// std::nullopt on transport failure so callers fall back to local logic.
//
// The service contract (see retrieval_service/main.py):
//   GET  /v1/health          -> {"status":"ok", "deal_count":N, "kb_count":M}
//   POST /v1/retrieve/deals  -> {"items":[<full deal>+score], "total":N}
//   POST /v1/retrieve/kb     -> {"passages":[{id,category,title,content,source,score}]}
//
// Methods are virtual so tests can substitute a fake without a live service.
class RetrievalClient {
public:
    explicit RetrievalClient(std::string base_url);
    virtual ~RetrievalClient() = default;

    // True when a base_url is configured (the service is expected to run).
    virtual bool Enabled() const;

    // True when the service answers GET /v1/health. Updates the cached flag.
    virtual bool Healthy();

    // POST /v1/retrieve/deals with the given filter body; returns the parsed
    // response or nullopt on failure.
    virtual std::optional<nlohmann::json> SearchDeals(const nlohmann::json& filters);

    // POST /v1/retrieve/kb; returns the parsed response or nullopt on failure.
    virtual std::optional<nlohmann::json> SearchKb(const std::string& query, int top_k);

protected:
    // Raw POST returning the parsed JSON body, or nullopt on any failure.
    std::optional<nlohmann::json> Post(const std::string& path, const std::string& body);

    std::string base_url_;
    std::atomic<bool> healthy_{true};
};

} // namespace agent
