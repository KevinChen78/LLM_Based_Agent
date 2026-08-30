#include "agent/ranker_client.hpp"

#include <httplib.h>
#include <spdlog/spdlog.h>

namespace agent {

RankerClient::RankerClient(std::string base_url)
    : base_url_(std::move(base_url)) {}

bool RankerClient::Enabled() const {
    return !base_url_.empty();
}

bool RankerClient::Healthy() {
    if (!Enabled()) {
        healthy_ = false;
        return false;
    }
    // Phase 8-C: fail fast while the breaker is open, then serve the TTL
    // cache (success and failure alike) before paying a network probe —
    // a refused connect costs ~2.05s on Windows (Phase 8-B measurement).
    if (!circuit_.AllowRequest()) {
        healthy_ = false;
        return false;
    }
    bool cached = false;
    if (circuit_.CachedHealth(cached)) {
        healthy_ = cached;
        return cached;
    }
    try {
        httplib::Client cli(base_url_);
        cli.set_connection_timeout(2.0);
        cli.set_read_timeout(2.0);
        auto res = cli.Get("/v1/health");
        healthy_ = res && res->status == 200;
    } catch (const std::exception& e) {
        spdlog::warn("RankerClient health check failed: {}", e.what());
        healthy_ = false;
    }
    if (healthy_.load()) {
        circuit_.ReportSuccess();
    } else {
        circuit_.ReportFailure();
    }
    return healthy_.load();
}

std::optional<nlohmann::json> RankerClient::Post(const std::string& path,
                                                 const std::string& body) {
    if (!Enabled()) return std::nullopt;
    if (!circuit_.AllowRequest()) {
        spdlog::debug("RankerClient POST {} skipped: circuit open", path);
        return std::nullopt;
    }
    try {
        httplib::Client cli(base_url_);
        cli.set_connection_timeout(2.0);
        cli.set_read_timeout(2.0);
        cli.set_write_timeout(2.0);

        auto res = cli.Post(path, body, "application/json");
        if (!res) {
            spdlog::warn("RankerClient POST {}: no response", path);
            healthy_ = false;
            circuit_.ReportFailure();
            return std::nullopt;
        }
        if (res->status != 200) {
            spdlog::warn("RankerClient POST {}: status={}", path, res->status);
            healthy_ = false;
            circuit_.ReportFailure();
            return std::nullopt;
        }
        healthy_ = true;
        circuit_.ReportSuccess();
        return nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        spdlog::warn("RankerClient POST {} exception: {}", path, e.what());
        healthy_ = false;
        circuit_.ReportFailure();
        return std::nullopt;
    }
}

std::optional<nlohmann::json> RankerClient::Rank(const nlohmann::json& request) {
    return Post("/v1/rank", request.dump());
}

} // namespace agent
