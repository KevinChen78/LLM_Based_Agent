#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "coro/coro.hpp"

#include "agent/agent_orchestrator.hpp"
#include "agent/api_auth.hpp"
#include "agent/deal_catalog.hpp"
#include "agent/deal_tools.hpp"
#include "agent/grpc_retrieval_client.hpp"
#include "agent/llm_client.hpp"
#include "agent/observability_store.hpp"
#include "agent/rate_limiter.hpp"
#include "agent/response_composer.hpp"
#include "agent/retrieval_client.hpp"
#include "agent/safety_guard.hpp"
#include "agent/session_memory.hpp"
#include "agent/sse_stream_emitter.hpp"
#include "agent/sqlite_session_store.hpp"
#include "agent/task_planner.hpp"
#include "agent/tool_registry.hpp"
#include "agent/user_profile_store.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace coro;
using namespace coro::net::http;
using json = nlohmann::json;
using namespace agent;

namespace {

std::atomic<bool> g_running{true};
std::mutex g_mutex;
std::condition_variable g_cv;

#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        g_running = false;
        g_cv.notify_all();
    }
    return TRUE;
}
#endif

// MIME type by file extension for the static-file handler.
std::string MimeFor(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

// Serve a single file from the web directory. Guards against path traversal
// (".." or absolute paths) and falls back to 404. The coro router matches
// exact keys, so each served asset has its own .get() route (see main()).
void ServeStaticFile(Response& resp, const std::string& web_dir, const std::string& rel) {
    if (rel.empty() || rel.find("..") != std::string::npos ||
        rel.front() == '/' || rel.front() == '\\') {
        resp.status(coro::net::http::Status::NotFound).text("Not Found");
        return;
    }
    std::ifstream f(web_dir + "/" + rel, std::ios::binary);
    if (!f) {
        resp.status(coro::net::http::Status::NotFound).text("Not Found");
        return;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    resp.content_type(MimeFor(rel)).body(bytes);
}

} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

json ToJson(const RecommendationResult& r) {
    json j;
    j["session_id"] = r.session_id;
    j["trace_id"] = r.trace_id;
    j["reply"] = r.response_text;
    j["is_clarifying"] = r.is_clarifying;
    j["next_state"] = r.next_state;
    j["items"] = json::array();
    for (const auto& item : r.items) {
        json ji;
        ji["item_id"] = item.item_id;
        ji["title"] = item.title;
        ji["category"] = item.category;
        ji["city"] = item.city;
        ji["district"] = item.district;
        ji["price"] = item.price;
        ji["original_price"] = item.original_price;
        ji["sold_count"] = item.sold_count;
        ji["rating"] = item.rating;
        ji["score"] = item.score;
        ji["reason"] = item.reason;
        ji["tags"] = item.tags;
        j["items"].push_back(ji);
    }
    // RAG grounding passages (only present when kb_search ran).
    if (!r.grounding.empty()) {
        j["grounding"] = r.grounding;
    }
    return j;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    spdlog::set_level(spdlog::level::info);

    // Dependencies
    // Phase 0: connect to the local Python LLM Gateway by default.
    // Set LLM_BASE_URL= (empty) to fall back to the built-in deterministic stub.
    const char* env_url = std::getenv("LLM_BASE_URL");
    std::string llm_base_url = env_url ? env_url : "http://localhost:8000";
    auto llm = std::make_shared<HttpLlmClient>(llm_base_url, "");
    auto planner = std::make_shared<TaskPlanner>(llm);
    auto tools = std::make_shared<ToolRegistry>();
    // Retrieval / ranking: real catalog-backed backend (default and only wired
    // backend). MockRetriever / MockRanker remain in the codebase as a Phase-0
    // reference implementation but are not registered here.
    //   CATALOG_BACKEND  — json (default, hermetic) | postgres (libpq catalog;
    //                      falls back to the JSON file, then the built-in dataset)
    //   PG_DSN           — libpq conninfo, required when CATALOG_BACKEND=postgres
    //                      (same name as the retrieval service uses)
    //   DEALS_CATALOG_PATH — catalog JSON path (default data/deals.json); the
    //                      fallback when the PG load fails.
    const char* backend_env = std::getenv("CATALOG_BACKEND");
    std::string catalog_backend = backend_env ? backend_env : "json";
    const char* catalog_env = std::getenv("DEALS_CATALOG_PATH");
    std::string catalog_path = catalog_env ? catalog_env : "data/deals.json";
    std::string pg_dsn;
    if (catalog_backend == "postgres") {
        const char* dsn_env = std::getenv("PG_DSN");
        pg_dsn = dsn_env ? dsn_env : "";
        if (pg_dsn.empty()) {
            std::cout << "CATALOG_BACKEND=postgres but PG_DSN is empty; using JSON catalog" << std::endl;
        }
    }
    auto catalog = std::make_shared<DealCatalog>(catalog_path, pg_dsn);
    // Retrieval service (BM25 over deals + knowledge base). Enabled by setting
    //   RETRIEVAL_SERVICE_URL=http://localhost:8001
    // When set: DealRetriever delegates text ranking to BM25 and the kb_search
    // tool becomes available for knowledge-base RAG. When empty, both degrade
    // to the local substring retriever and no KB — offline behaviour unchanged.
    //   RETRIEVAL_PROTOCOL=http|grpc   (Phase 5-D pilot; default http)
    //   RETRIEVAL_GRPC_ADDR=127.0.0.1:8011   (gRPC front-end of the same service)
    // grpc mode requires a binary built with ENABLE_GRPC=ON; without it (or on
    // any gRPC failure) calls fall back to the HTTP client.
    const char* retr_env = std::getenv("RETRIEVAL_SERVICE_URL");
    std::string retr_url = retr_env ? retr_env : "";
    const char* proto_env = std::getenv("RETRIEVAL_PROTOCOL");
    std::string retr_protocol = proto_env ? proto_env : "http";
    const char* grpc_env = std::getenv("RETRIEVAL_GRPC_ADDR");
    std::string retr_grpc_addr = grpc_env ? grpc_env : "";
    std::shared_ptr<RetrievalClient> retrieval;
    if (retr_protocol == "grpc" && !retr_grpc_addr.empty()) {
        auto grpc_client = std::make_shared<GrpcRetrievalClient>(retr_url, retr_grpc_addr);
#ifdef AGENT_HAVE_GRPC
        std::cout << "Retrieval protocol: grpc (" << retr_grpc_addr
                  << ", HTTP fallback " << (retr_url.empty() ? "<none>" : retr_url) << ")"
                  << (grpc_client->GrpcActive() ? "" : " [grpc inactive — HTTP only]")
                  << std::endl;
#else
        std::cout << "Retrieval protocol: grpc requested but binary built without "
                     "ENABLE_GRPC; using HTTP" << std::endl;
#endif
        retrieval = grpc_client;
    } else {
        retrieval = std::make_shared<RetrievalClient>(retr_url);
    }
    tools->Register(std::make_shared<DealRetriever>(catalog, retrieval));
    // Learning-to-rank service (Phase 2.1). Enabled by setting
    //   RANKER_SERVICE_URL=http://localhost:8002
    //   RANKER_MODE=off|shadow|active   (default off — zero added latency)
    //   RANKER_TREATMENT_PCT=0..100     (active mode, default 50)
    // DealRanker always computes rule scores; any service failure or missing
    // model falls back to them, so offline behaviour is unchanged.
    const char* rank_env = std::getenv("RANKER_SERVICE_URL");
    std::string rank_url = rank_env ? rank_env : "";
    auto ranker_client = std::make_shared<RankerClient>(rank_url);
    auto experiment = ExperimentManager::FromEnv();
    tools->Register(std::make_shared<DealRanker>(ranker_client, experiment));
    std::cout << "Retrieval backend: Catalog (backend=" << catalog_backend
              << ", source=" << catalog->Source()
              << ", " << catalog->Size() << " deals)" << std::endl;
    if (retrieval->Enabled()) {
        tools->Register(std::make_shared<KnowledgeRetriever>(retrieval));
        bool up = retrieval->Healthy();
        std::cout << "Retrieval service: " << retr_url << " (BM25 deals + kb_search)"
                  << (up ? "" : " [unreachable — will degrade to local]") << std::endl;
    } else {
        std::cout << "Retrieval service: disabled (set RETRIEVAL_SERVICE_URL to enable BM25 + kb_search)" << std::endl;
    }
    {
        const char* mode_str = experiment.GetMode() == ExperimentManager::Mode::kActive ? "active"
            : experiment.GetMode() == ExperimentManager::Mode::kShadow ? "shadow" : "off";
        if (ranker_client->Enabled()) {
            bool up = ranker_client->Healthy();
            std::cout << "Ranking service: " << rank_url << " (mode=" << mode_str
                      << ", experiment=" << experiment.ExperimentName()
                      << ", treatment=" << experiment.TreatmentPct() << "%)"
                      << (up ? "" : " [unreachable — rule scores only]") << std::endl;
        } else {
            std::cout << "Ranking service: disabled (mode=" << mode_str
                      << "; set RANKER_SERVICE_URL to enable learning-to-rank)" << std::endl;
        }
    }
    // Session storage: SQLite (persistent) by default, InMemory fallback.
    //   SESSION_STORE=sqlite|memory   (default sqlite)
    //   SESSION_DB_PATH=data/sessions.db
    const char* store_env = std::getenv("SESSION_STORE");
    std::string store_kind = store_env ? store_env : "sqlite";
    std::shared_ptr<SessionMemoryStore> memory;
    std::string sessions_db_path;   // empty when SESSION_STORE=memory
    if (store_kind == "memory") {
        memory = std::make_shared<InMemorySessionStore>();
        std::cout << "Session store: InMemory" << std::endl;
    } else {
        const char* path_env = std::getenv("SESSION_DB_PATH");
        sessions_db_path = path_env ? path_env : "data/sessions.db";
        memory = std::make_shared<SqliteSessionStore>(sessions_db_path);
        std::cout << "Session store: SQLite (" << sessions_db_path << ")" << std::endl;
    }
    // Audit trail: one recommendation_logs row per request + one llm_calls row
    // per LLM call, in its own DB so it never contends with session traffic.
    //   OBS_DB_PATH — default data/observability.db
    const char* obs_env = std::getenv("OBS_DB_PATH");
    std::string obs_db_path = obs_env ? obs_env : "data/observability.db";
    auto obs = std::make_shared<ObservabilityStore>(obs_db_path);
    std::cout << "Observability: SQLite (" << obs_db_path << ")" << std::endl;
    // User profiles (Phase 2.2): cross-session preferences injected into the
    // planner prompt. Lives in the sessions DB on its own connection; only
    // available with the SQLite session store (memory mode => no profiles,
    // the planner prompt carries no profile section).
    std::shared_ptr<UserProfileStore> profiles;
    if (store_kind != "memory") {
        try {
            profiles = std::make_shared<SqliteUserProfileStore>(sessions_db_path);
            std::cout << "User profiles: SQLite (" << sessions_db_path
                      << ", user_profiles table)" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "User profiles: disabled (open failed: " << e.what() << ")" << std::endl;
        }
    } else {
        std::cout << "User profiles: disabled (SESSION_STORE=memory)" << std::endl;
    }
    // Phase 4-C: guard rules externalized to JSON; missing/malformed file
    // falls back to the built-in defaults (byte-identical behavior).
    const char* rules_env = std::getenv("GUARD_RULES_PATH");
    const std::string guard_rules =
        (rules_env && *rules_env) ? rules_env : "data/guard_rules.json";
    auto guard = std::make_shared<SafetyGuard>(guard_rules);
    auto composer = std::make_shared<ResponseComposer>(llm, guard);
    auto orchestrator = std::make_shared<AgentOrchestrator>(
        planner, tools, memory, llm, composer, guard, obs, profiles, catalog);

    // API key auth (Phase 5-A). AGENT_API_KEYS empty/unset => disabled and
    // every request passes, byte-identical to before. /v1/health and the
    // static assets are intentionally exempt (health-check convention).
    ApiAuth api_auth = ApiAuth::FromEnv();
    std::cout << "API auth: " << (api_auth.Enabled() ? "enabled (AGENT_API_KEYS)"
                                                     : "disabled (set AGENT_API_KEYS to enable)")
              << std::endl;
    // Per-user token-bucket rate limit (Phase 5-B). RATE_LIMIT_RPS/
    // RATE_LIMIT_BURST empty or 0 => unlimited, byte-identical to before.
    // Counted once per request at handler entry; SSE streams are not charged
    // by connection duration.
    RateLimiter rate_limiter = RateLimiter::FromEnv();
    if (rate_limiter.Enabled()) {
        std::cout << "Rate limit: " << rate_limiter.rps() << " rps, burst "
                  << rate_limiter.burst() << " per user" << std::endl;
    } else {
        std::cout << "Rate limit: disabled (set RATE_LIMIT_RPS/RATE_LIMIT_BURST to enable)" << std::endl;
    }
    std::atomic<long long> auth_rejected{0};
    std::atomic<long long> rate_limited{0};
    // Reject helper: shared 401 shape for all protected endpoints.
    auto require_auth = [&api_auth, &auth_rejected](const Request& req, Response& resp) -> bool {
        if (api_auth.Check(req.header("X-Api-Key"))) return true;
        ++auth_rejected;
        static std::atomic<int> auth_counter{0};
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        resp.status(coro::net::http::Status::Unauthorized)
            .json(json{{"error", "missing or invalid API key"},
                       {"trace_id", "t-" + std::to_string(now) + "-auth" +
                                        std::to_string(++auth_counter)}}.dump());
        return false;
    };
    // Rate-limit identity: user_id first, then API key, then a shared
    // fallback bucket (coro does not expose the peer address).
    auto require_quota = [&rate_limiter, &rate_limited](
            const std::string& user_id, const Request& req, Response& resp) -> bool {
        std::string key = !user_id.empty() ? "u:" + user_id
            : !req.header("X-Api-Key").empty() ? "k:" + req.header("X-Api-Key")
            : "anonymous";
        double retry = 0.0;
        if (rate_limiter.Allow(key, &retry)) return true;
        ++rate_limited;
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        resp.status(coro::net::http::Status::TooManyRequests)
            .header("Retry-After", std::to_string(static_cast<int>(std::ceil(retry))))
            .json(json{{"error", "rate limit exceeded"},
                       {"retry_after_seconds", retry},
                       {"trace_id", "t-" + std::to_string(now) + "-rl"}}.dump());
        return false;
    };

    // Web UI static assets. Served from WEB_DIR (default "web", relative to
    // cwd) on the same origin as the API, so no CORS is needed. The coro
    // router matches exact keys, so each file has its own GET route.
    const char* web_env = std::getenv("WEB_DIR");
    std::string web_dir = web_env ? web_env : "web";
    std::cout << "Web UI: " << web_dir << "/ (served at /)" << std::endl;

    ThreadPool pool(4);
    // AGENT_PORT (default 8080): lets a second instance run alongside the
    // main one (the e2e auth/rate-limit tiers use this).
    const char* port_env = std::getenv("AGENT_PORT");
    int port = port_env && *port_env ? std::atoi(port_env) : 8080;
    Server server(pool, static_cast<uint16_t>(port));

    server
        .post("/v1/chat", [orchestrator, require_auth, require_quota](const Request& req, Response& resp) -> Task<void> {
            if (!require_auth(req, resp)) co_return;
            try {
                auto body = json::parse(req.body());
                UserContext ctx;
                ctx.user_id = body.value("user_id", "");
                if (!require_quota(ctx.user_id, req, resp)) co_return;
                ctx.session_id = body.value("session_id", "");
                ctx.city = body.value("city", "");
                ctx.longitude = body.value("longitude", 0.0);
                ctx.latitude = body.value("latitude", 0.0);
                ctx.device_type = body.value("device_type", "");

                AgentOrchestrator::Request areq{
                    .user_context = ctx,
                    .user_message = body.value("message", "")
                };

                auto result = co_await orchestrator->Chat(areq);
                resp.json(ToJson(result).dump());
            } catch (const std::exception& e) {
                spdlog::error("Error handling /v1/chat: {}", e.what());
                resp.status(coro::net::http::Status::InternalServerError)
                    .json(json{{"error", e.what()}}.dump());
            }
            co_return;
        })
        .post("/v1/chat/stream", [orchestrator, require_auth, require_quota](const Request& req, Response& resp) -> Task<void> {
            if (!require_auth(req, resp)) co_return;
            try {
                auto body = json::parse(req.body());
                UserContext ctx;
                ctx.user_id = body.value("user_id", "");
                if (!require_quota(ctx.user_id, req, resp)) co_return;
                ctx.session_id = body.value("session_id", "");
                ctx.city = body.value("city", "");
                ctx.longitude = body.value("longitude", 0.0);
                ctx.latitude = body.value("latitude", 0.0);
                ctx.device_type = body.value("device_type", "");

                AgentOrchestrator::Request areq{
                    .user_context = ctx,
                    .user_message = body.value("message", "")
                };

                auto emitter = std::make_shared<agent::SseStreamEmitter>();
                resp.status(coro::net::http::Status::OK)
                    .content_type("text/event-stream; charset=utf-8")
                    .header("Cache-Control", "no-cache")
                    .stream(emitter->Writer());

                // Run the orchestrator in a detached thread so the handler can
                // return immediately and the server starts streaming headers.
                auto orch = orchestrator;
                std::thread([orch, areq, emitter]() mutable {
                    try {
                        auto task = orch->ChatStream(areq, emitter);
                        task.result();
                    } catch (const std::exception& e) {
                        spdlog::error("Streaming orchestrator thread error: {}", e.what());
                        emitter->Error(std::string("orchestrator error: ") + e.what());
                    }
                }).detach();
            } catch (const std::exception& e) {
                spdlog::error("Error handling /v1/chat/stream: {}", e.what());
                resp.status(coro::net::http::Status::InternalServerError)
                    .json(json{{"error", e.what()}}.dump());
            }
            co_return;
        })
        .post("/v1/feedback", [memory, require_auth, require_quota](const Request& req, Response& resp) -> Task<void> {
            if (!require_auth(req, resp)) co_return;
            try {
                auto body = json::parse(req.body());
                if (!require_quota(body.value("user_id", ""), req, resp)) co_return;
                SessionMemoryStore::FeedbackRecord rec;
                rec.session_id = body.value("session_id", "");
                rec.trace_id = body.value("trace_id", "");
                rec.item_id = body.value("item_id", "");
                rec.feedback_type = body.value("feedback_type", "");
                rec.comment = body.value("comment", "");

                if (rec.session_id.empty()) {
                    resp.status(coro::net::http::Status::BadRequest)
                        .json(json{{"success", false}, {"error", "session_id is required"}}.dump());
                    co_return;
                }
                if (rec.feedback_type != "like" && rec.feedback_type != "dislike") {
                    resp.status(coro::net::http::Status::BadRequest)
                        .json(json{{"success", false},
                                   {"error", "feedback_type must be like|dislike"}}.dump());
                    co_return;
                }

                auto st = co_await memory->AppendFeedback(rec);
                if (!st.ok()) {
                    resp.status(coro::net::http::Status::BadRequest)
                        .json(json{{"success", false}, {"error", st.message}}.dump());
                    co_return;
                }
                resp.json(json{{"success", true}}.dump());
            } catch (const std::exception& e) {
                spdlog::error("Error handling /v1/feedback: {}", e.what());
                resp.status(coro::net::http::Status::InternalServerError)
                    .json(json{{"success", false}, {"error", e.what()}}.dump());
            }
            co_return;
        })
        .get("/v1/health", [](const Request& req, Response& resp) -> Task<void> {
            resp.json(json{{"status", "ok"}}.dump());
            co_return;
        })
        .get("/v1/metrics", [obs, sessions_db_path, require_auth, require_quota,
                             &auth_rejected, &rate_limited](const Request& req, Response& resp) -> Task<void> {
            if (!require_auth(req, resp)) co_return;
            if (!require_quota("", req, resp)) co_return;
            try {
                auto m = obs->Aggregate(sessions_db_path);
                m["api_guard"] = {{"auth_rejected", auth_rejected.load()},
                                  {"rate_limited", rate_limited.load()}};
                resp.json(m.dump());
            } catch (const std::exception& e) {
                spdlog::error("Error handling /v1/metrics: {}", e.what());
                resp.status(coro::net::http::Status::InternalServerError)
                    .json(json{{"error", e.what()}}.dump());
            }
            co_return;
        })
        // Static web UI (flat web/ dir; each asset gets an exact GET route).
        .get("/", [&web_dir](const Request&, Response& r) -> Task<void> {
            ServeStaticFile(r, web_dir, "index.html");
            co_return;
        })
        .get("/index.html", [&web_dir](const Request&, Response& r) -> Task<void> {
            ServeStaticFile(r, web_dir, "index.html");
            co_return;
        })
        .get("/styles.css", [&web_dir](const Request&, Response& r) -> Task<void> {
            ServeStaticFile(r, web_dir, "styles.css");
            co_return;
        })
        .get("/app.js", [&web_dir](const Request&, Response& r) -> Task<void> {
            ServeStaticFile(r, web_dir, "app.js");
            co_return;
        });

    if (!server.bind("0.0.0.0")) {
        std::cerr << "Failed to bind to port " << port << std::endl;
        return 1;
    }

    std::cout << "LLM Agent API server starting on http://0.0.0.0:" << port << std::endl;
    std::cout << "Routes:" << std::endl;
    for (const auto& route : server.router().list_routes()) {
        std::cout << "  " << route << std::endl;
    }

    std::thread server_thread([&server]() {
        server.start();
    });

    // Block main thread until Ctrl+C
    std::cout << "Press Ctrl+C to stop..." << std::endl;

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#endif

    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv.wait(lock, [] { return !g_running.load(); });
    }

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    return 0;
}
