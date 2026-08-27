// Phase 5-C link smoke: construct a generated message, round-trip through
// Serialize/Parse, print the field. Exits non-zero on any mismatch so CI can
// gate on it. No server, no channel — business wiring is Stage D.

#include "retrieval.pb.h"

#include <cstdio>
#include <string>

int main() {
    groupbuy::retrieval::v1::RetrieveDealsRequest req;
    req.set_city("武汉");
    req.set_query("汉堡");
    req.set_max_price(300.0);
    req.set_top_k(20);

    std::string bytes = req.SerializeAsString();

    groupbuy::retrieval::v1::RetrieveDealsRequest back;
    if (!back.ParseFromString(bytes)) {
        std::fprintf(stderr, "parse failed\n");
        return 1;
    }
    if (back.city() != "武汉" || !back.has_max_price() ||
        back.max_price() != 300.0 || back.top_k() != 20 ||
        back.has_min_price()) {
        std::fprintf(stderr, "round-trip mismatch\n");
        return 1;
    }
    std::printf("grpc_proto_smoke OK (%zu bytes)\n", bytes.size());
    return 0;
}
