#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "coro/coro.hpp"

#include "agent/agent_orchestrator.hpp"
#include "agent/deal_catalog.hpp"
#include "agent/deal_tools.hpp"
#include "agent/llm_client.hpp"
#include "agent/observability_store.hpp"
#include "agent/response_composer.hpp"
#include "agent/retrieval_client.hpp"
#include "agent/safety_guard.hpp"
#include "agent/session_memory.hpp"
#include "agent/sse_stream_emitter.hpp"
#include "agent/sqlite_session_store.hpp"
#include "agent/task_planner.hpp"
#include "agent/tool_registry.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
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
    const char* retr_env = std::getenv("RETRIEVAL_SERVICE_URL");
    std::string retr_url = retr_env ? retr_env : "";
    auto retrieval = std::make_shared<RetrievalClient>(retr_url);
    tools->Register(std::make_shared<DealRetriever>(catalog, retrieval));
    tools->Register(std::make_shared<DealRanker>());
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
    auto composer = std::make_shared<ResponseComposer>(llm);
    auto guard = std::make_shared<SafetyGuard>();
    auto orchestrator = std::make_shared<AgentOrchestrator>(
        planner, tools, memory, llm, composer, guard, obs);

    // Web UI static assets. Served from WEB_DIR (default "web", relative to
    // cwd) on the same origin as the API, so no CORS is needed. The coro
    // router matches exact keys, so each file has its own GET route.
    const char* web_env = std::getenv("WEB_DIR");
    std::string web_dir = web_env ? web_env : "web";
    std::cout << "Web UI: " << web_dir << "/ (served at /)" << std::endl;

    ThreadPool pool(4);
    Server server(pool, 8080);

    server
        .post("/v1/chat", [orchestrator](const Request& req, Response& resp) -> Task<void> {
            try {
                auto body = json::parse(req.body());
                UserContext ctx;
                ctx.user_id = body.value("user_id", "");
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
        .post("/v1/chat/stream", [orchestrator](const Request& req, Response& resp) -> Task<void> {
            try {
                auto body = json::parse(req.body());
                UserContext ctx;
                ctx.user_id = body.value("user_id", "");
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
        .post("/v1/feedback", [memory](const Request& req, Response& resp) -> Task<void> {
            try {
                auto body = json::parse(req.body());
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
        .get("/v1/metrics", [obs, sessions_db_path](const Request& req, Response& resp) -> Task<void> {
            try {
                resp.json(obs->Aggregate(sessions_db_path).dump());
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
        std::cerr << "Failed to bind to port 8080" << std::endl;
        return 1;
    }

    std::cout << "LLM Agent API server starting on http://0.0.0.0:8080" << std::endl;
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
