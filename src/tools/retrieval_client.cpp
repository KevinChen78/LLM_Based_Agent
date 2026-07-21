#include "agent/retrieval_client.hpp"

#include <httplib.h>
#include <spdlog/spdlog.h>

namespace agent {

RetrievalClient::RetrievalClient(std::string base_url)
    : base_url_(std::move(base_url)) {}

bool RetrievalClient::Enabled() const {
    return !base_url_.empty();
}

bool RetrievalClient::Healthy() {
    if (!Enabled()) {
        healthy_ = false;
        return false;
    }
    try {
        httplib::Client cli(base_url_);
        cli.set_connection_timeout(2.0);
        cli.set_read_timeout(2.0);
        auto res = cli.Get("/v1/health");
        healthy_ = res && res->status == 200;
    } catch (const std::exception& e) {
        spdlog::warn("RetrievalClient health check failed: {}", e.what());
        healthy_ = false;
    }
    return healthy_.load();
}

std::optional<nlohmann::json> RetrievalClient::Post(const std::string& path,
                                                    const std::string& body) {
    if (!Enabled()) return std::nullopt;
    try {
        httplib::Client cli(base_url_);
        cli.set_connection_timeout(5.0);
        cli.set_read_timeout(10.0);
        cli.set_write_timeout(5.0);

        auto res = cli.Post(path, body, "application/json");
        if (!res) {
            spdlog::warn("RetrievalClient POST {}: no response", path);
            healthy_ = false;
            return std::nullopt;
        }
        if (res->status != 200) {
            spdlog::warn("RetrievalClient POST {}: status={}", path, res->status);
            healthy_ = false;
            return std::nullopt;
        }
        healthy_ = true;
        return nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        spdlog::warn("RetrievalClient POST {} exception: {}", path, e.what());
        healthy_ = false;
        return std::nullopt;
    }
}

std::optional<nlohmann::json> RetrievalClient::SearchDeals(const nlohmann::json& filters) {
    return Post("/v1/retrieve/deals", filters.dump());
}

std::optional<nlohmann::json> RetrievalClient::SearchKb(const std::string& query, int top_k) {
    nlohmann::json body{{"query", query}, {"top_k", top_k}};
    return Post("/v1/retrieve/kb", body.dump());
}

} // namespace agent
