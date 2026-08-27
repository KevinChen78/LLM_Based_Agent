#pragma once

#include "agent/retrieval_client.hpp"

#include <memory>
#include <string>

namespace agent {

// Phase 5-D: gRPC pilot client for the retrieval service.
//
// Runs the proto/retrieval.proto contract against retrieval_service's gRPC
// front-end (GRPC_PORT, default off). Degrade chain extension:
//   gRPC failure / unavailable  ->  HTTP client (base class methods)
//   HTTP failure                ->  caller's local fallback (unchanged)
//
// When the binary is built without ENABLE_GRPC, every method degrades
// straight to the HTTP implementation — constructing this class is harmless.
class GrpcRetrievalClient : public RetrievalClient {
public:
    // http_base_url: fallback HTTP endpoint (may be empty => fully offline).
    // grpc_addr: "host:port" of the gRPC front-end; empty => HTTP only.
    GrpcRetrievalClient(std::string http_base_url, std::string grpc_addr);
    ~GrpcRetrievalClient() override;

    bool Enabled() const override;
    bool Healthy() override;

    std::optional<nlohmann::json> SearchDeals(const nlohmann::json& filters) override;
    std::optional<nlohmann::json> SearchKb(const std::string& query, int top_k) override;

    // True when the gRPC channel was actually created (i.e. built with
    // ENABLE_GRPC and a non-empty address). Exposed for the startup banner.
    bool GrpcActive() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string grpc_addr_;
};

} // namespace agent
