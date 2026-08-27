#include "agent/grpc_retrieval_client.hpp"

// The whole gRPC path compiles only with ENABLE_GRPC=ON (AGENT_HAVE_GRPC is
// defined by CMake and retrieval_proto/grpc++ are on the link line). Without
// it this TU is a thin pass-through to the HTTP base class, so the GLOB-built
// OFF binary stays byte-identical in behaviour.

#ifdef AGENT_HAVE_GRPC

#include "retrieval.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <sstream>

namespace agent {

namespace {

using groupbuy::retrieval::v1::HealthRequest;
using groupbuy::retrieval::v1::HealthResponse;
using groupbuy::retrieval::v1::RetrieveDealsRequest;
using groupbuy::retrieval::v1::RetrieveDealsResponse;
using groupbuy::retrieval::v1::RetrieveKbRequest;
using groupbuy::retrieval::v1::RetrieveKbResponse;
using groupbuy::retrieval::v1::RetrievalService;

// Same generous-but-bounded budget as the HTTP path: retrieval is local, so
// 5s is already far beyond p99. (ClientContext is non-copyable/non-movable,
// hence the unique_ptr.)
constexpr int kGrpcDeadlineMs = 5000;

std::unique_ptr<grpc::ClientContext> MakeContext() {
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->set_deadline(std::chrono::system_clock::now() +
                      std::chrono::milliseconds(kGrpcDeadlineMs));
    return ctx;
}

nlohmann::json DealItemToJson(const groupbuy::retrieval::v1::DealItem& it) {
    nlohmann::json j;
    j["item_id"] = it.item_id();
    j["title"] = it.title();
    j["category"] = it.category();
    j["city"] = it.city();
    j["district"] = it.district();
    j["price"] = it.price();
    j["original_price"] = it.original_price();
    j["rating"] = it.rating();
    j["sold_count"] = it.sold_count();
    j["score"] = it.score();
    j["description"] = it.description();
    j["merchant_id"] = it.merchant_id();
    j["min_people"] = it.min_people();
    j["max_people"] = it.max_people();
    j["tags"] = nlohmann::json::array();
    for (const auto& t : it.tags()) j["tags"].push_back(t);
    return j;
}

} // namespace

struct GrpcRetrievalClient::Impl {
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<RetrievalService::Stub> stub;
};

GrpcRetrievalClient::GrpcRetrievalClient(std::string http_base_url,
                                         std::string grpc_addr)
    : RetrievalClient(std::move(http_base_url)),
      grpc_addr_(std::move(grpc_addr)) {
    if (!grpc_addr_.empty()) {
        impl_ = std::make_unique<Impl>();
        impl_->channel = grpc::CreateChannel(grpc_addr_,
                                             grpc::InsecureChannelCredentials());
        impl_->stub = RetrievalService::NewStub(impl_->channel);
    }
}

GrpcRetrievalClient::~GrpcRetrievalClient() = default;

bool GrpcRetrievalClient::GrpcActive() const { return impl_ != nullptr; }

bool GrpcRetrievalClient::Enabled() const {
    return GrpcActive() || RetrievalClient::Enabled();
}

bool GrpcRetrievalClient::Healthy() {
    if (GrpcActive()) {
        HealthRequest req;
        HealthResponse resp;
        auto ctx = MakeContext();
        if (impl_->stub->Health(ctx.get(), req, &resp).ok() &&
            resp.status() == "ok") {
            healthy_ = true;
            return true;
        }
        // gRPC down: report the HTTP fallback's health instead.
    }
    return RetrievalClient::Healthy();
}

std::optional<nlohmann::json> GrpcRetrievalClient::SearchDeals(
    const nlohmann::json& filters) {
    if (GrpcActive()) {
        RetrieveDealsRequest req;
        req.set_city(filters.value("city", ""));
        req.set_district(filters.value("district", ""));
        req.set_category(filters.value("category", ""));
        req.set_query(filters.value("query", ""));
        // Presence mirrors the HTTP contract: absent = no filter, 0 = real.
        if (filters.contains("max_price") && filters["max_price"].is_number())
            req.set_max_price(filters["max_price"].get<double>());
        if (filters.contains("min_price") && filters["min_price"].is_number())
            req.set_min_price(filters["min_price"].get<double>());
        if (filters.contains("people") && filters["people"].is_number())
            req.set_people(filters["people"].get<int>());
        if (filters.contains("top_k") && filters["top_k"].is_number())
            req.set_top_k(filters["top_k"].get<int>());

        RetrieveDealsResponse resp;
        auto ctx = MakeContext();
        if (impl_->stub->RetrieveDeals(ctx.get(), req, &resp).ok()) {
            nlohmann::json out;
            out["total"] = resp.total();
            out["items"] = nlohmann::json::array();
            for (const auto& it : resp.items()) {
                out["items"].push_back(DealItemToJson(it));
            }
            // Phase 3-C additive audit fields, same rule as HTTP.
            if (resp.has_relaxed_level()) {
                out["relaxed_level"] = resp.relaxed_level();
                out["effective_category"] = resp.effective_category();
            }
            return out;
        }
        // fall through to HTTP
    }
    return RetrievalClient::SearchDeals(filters);
}

std::optional<nlohmann::json> GrpcRetrievalClient::SearchKb(
    const std::string& query, int top_k) {
    if (GrpcActive()) {
        RetrieveKbRequest req;
        req.set_query(query);
        req.set_top_k(top_k);
        RetrieveKbResponse resp;
        auto ctx = MakeContext();
        if (impl_->stub->RetrieveKb(ctx.get(), req, &resp).ok()) {
            nlohmann::json out;
            out["passages"] = nlohmann::json::array();
            for (const auto& p : resp.passages()) {
                nlohmann::json pj;
                pj["id"] = p.id();
                pj["category"] = p.category();
                pj["title"] = p.title();
                pj["content"] = p.content();
                pj["source"] = p.source();
                pj["score"] = p.score();
                pj["tags"] = nlohmann::json::array();
                for (const auto& t : p.tags()) pj["tags"].push_back(t);
                out["passages"].push_back(pj);
            }
            return out;
        }
    }
    return RetrievalClient::SearchKb(query, top_k);
}

} // namespace agent

#else  // !AGENT_HAVE_GRPC — HTTP pass-through

namespace agent {

struct GrpcRetrievalClient::Impl {};

GrpcRetrievalClient::GrpcRetrievalClient(std::string http_base_url,
                                         std::string grpc_addr)
    : RetrievalClient(std::move(http_base_url)),
      grpc_addr_(std::move(grpc_addr)) {}

GrpcRetrievalClient::~GrpcRetrievalClient() = default;

bool GrpcRetrievalClient::GrpcActive() const { return false; }

bool GrpcRetrievalClient::Enabled() const { return RetrievalClient::Enabled(); }

bool GrpcRetrievalClient::Healthy() { return RetrievalClient::Healthy(); }

std::optional<nlohmann::json> GrpcRetrievalClient::SearchDeals(
    const nlohmann::json& filters) {
    return RetrievalClient::SearchDeals(filters);
}

std::optional<nlohmann::json> GrpcRetrievalClient::SearchKb(
    const std::string& query, int top_k) {
    return RetrievalClient::SearchKb(query, top_k);
}

} // namespace agent

#endif  // AGENT_HAVE_GRPC
