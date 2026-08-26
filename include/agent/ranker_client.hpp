#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <optional>
#include <string>

namespace agent {

// HTTP client for the Python learning-to-rank service (ranking_service,
// Phase 2.1). Mirrors RetrievalClient's degrade philosophy: an empty base_url
// disables the client (offline mode), and every method returns std::nullopt
// on transport failure so the DealRanker falls back to its rule scoring.
//
// The service contract (see ranking_service/main.py):
//   GET  /v1/health -> {"status":"ok", "model_loaded":bool,
//                       "model_version":"...", "feature_rows":N,
//                       "profiles_available":bool}
//   POST /v1/rank   <- {"candidates":[<deal>], "context":{...}, "top_n":N,
//                       "shadow":bool}
//                   -> {"model_loaded":bool, "model_version":"...",
//                       "items":[{"item_id":"...","model_score":0.83}, ...]}
//                   model unavailable -> {"model_loaded":false, "items":[]}
//
// Methods are virtual so tests can substitute a fake without a live service.
class RankerClient {
public:
    explicit RankerClient(std::string base_url);
    virtual ~RankerClient() = default;

    // True when a base_url is configured (the service is expected to run).
    virtual bool Enabled() const;

    // True when the service answers GET /v1/health. Updates the cached flag.
    virtual bool Healthy();

    // POST /v1/rank with the given request body; returns the parsed response
    // or nullopt on failure. Timeouts are tighter than RetrievalClient's
    // (ranking is a local in-process model and sits on the hot path,
    // including shadow mode).
    virtual std::optional<nlohmann::json> Rank(const nlohmann::json& request);

protected:
    std::optional<nlohmann::json> Post(const std::string& path, const std::string& body);

    std::string base_url_;
    std::atomic<bool> healthy_{true};
};

} // namespace agent
