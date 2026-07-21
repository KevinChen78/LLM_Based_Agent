#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "coro/coro.hpp"

#include "agent/agent_orchestrator.hpp"
#include "agent/deal_catalog.hpp"
#include "agent/deal_tools.hpp"
#include "agent/llm_client.hpp"
#include "agent/response_composer.hpp"
#include "agent/safety_guard.hpp"
#include "agent/session_memory.hpp"
#include "agent/sse_stream_emitter.hpp"
#include "agent/sqlite_session_store.hpp"
#include "agent/task_planner.hpp"
#include "agent/tool_registry.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
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

} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string GenerateTraceId() {
    static int counter = 0;
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "t-" + std::to_string(now) + "-" + std::to_string(++counter);
}

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
    //   DEALS_CATALOG_PATH — override the catalog JSON path (default data/deals.json).
    const char* catalog_env = std::getenv("DEALS_CATALOG_PATH");
    std::string catalog_path = catalog_env ? catalog_env : "data/deals.json";
    auto catalog = std::make_shared<DealCatalog>(catalog_path);
    tools->Register(std::make_shared<DealRetriever>(catalog));
    tools->Register(std::make_shared<DealRanker>());
    std::cout << "Retrieval backend: Catalog (" << catalog_path
              << ", " << catalog->Size() << " deals)" << std::endl;
    // Session storage: SQLite (persistent) by default, InMemory fallback.
    //   SESSION_STORE=sqlite|memory   (default sqlite)
    //   SESSION_DB_PATH=data/sessions.db
    const char* store_env = std::getenv("SESSION_STORE");
    std::string store_kind = store_env ? store_env : "sqlite";
    std::shared_ptr<SessionMemoryStore> memory;
    if (store_kind == "memory") {
        memory = std::make_shared<InMemorySessionStore>();
        std::cout << "Session store: InMemory" << std::endl;
    } else {
        const char* path_env = std::getenv("SESSION_DB_PATH");
        std::string db_path = path_env ? path_env : "data/sessions.db";
        memory = std::make_shared<SqliteSessionStore>(db_path);
        std::cout << "Session store: SQLite (" << db_path << ")" << std::endl;
    }
    auto composer = std::make_shared<ResponseComposer>(llm);
    auto guard = std::make_shared<SafetyGuard>();
    auto orchestrator = std::make_shared<AgentOrchestrator>(
        planner, tools, memory, llm, composer, guard);

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
        .post("/v1/feedback", [](const Request& req, Response& resp) -> Task<void> {
            resp.json(json{{"success", true}}.dump());
            co_return;
        })
        .get("/v1/health", [](const Request& req, Response& resp) -> Task<void> {
            resp.json(json{{"status", "ok"}}.dump());
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
