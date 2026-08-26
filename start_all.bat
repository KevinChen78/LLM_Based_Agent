@echo off
rem ============================================================
rem  LLM Group-Buying Agent - one-click startup
rem  Starts: retrieval_service(:8001) + llm_gateway(:8000)
rem          + ranking_service(:8002, Phase 2.1 learning-to-rank)
rem          + api_server(:8080, serves Web UI at /)
rem  Each service runs in its own window (logs visible).
rem  Close a window or press Ctrl+C in it to stop that service.
rem ============================================================
setlocal
cd /d "%~dp0"

rem api_server picks these up via environment (inherited by start)
set RETRIEVAL_SERVICE_URL=http://localhost:8001

rem Phase 2.1 learning-to-rank: shadow mode = model scores logged for
rem comparison but rule scores still serve. Set RANKER_MODE=active to
rem actually serve model scores to the treatment bucket (RANKER_TREATMENT_PCT).
set RANKER_SERVICE_URL=http://localhost:8002
set RANKER_MODE=shadow
set RANKER_TREATMENT_PCT=50

rem Catalog: load deals from PostgreSQL (falls back to data/deals.json if PG
rem is unreachable). Fill in the password from retrieval_service/.env.local.
set CATALOG_BACKEND=postgres
set PG_DSN=host=127.0.0.1 port=5432 dbname=groupbuy user=agent password=agent_dev_2026

echo [1/4] Starting retrieval_service on :8001 ...
start "retrieval_service :8001" python retrieval_service/main.py

echo [2/4] Starting llm_gateway on :8000 ...
start "llm_gateway :8000" python llm_gateway/main.py

echo [3/4] Starting ranking_service on :8002 (Phase 2.1) ...
start "ranking_service :8002" python ranking_service/main.py

echo [4/4] Starting api_server on :8080 (Web UI at /) ...
start "api_server :8080" build\bin\Release\api_server.exe

echo.
echo Waiting for services to come up ...
timeout /t 5 /nobreak >nul

echo Opening http://127.0.0.1:8080/ in your browser ...
start "" "http://127.0.0.1:8080/"

echo.
echo All started. Keep the 4 service windows open while using the app.
endlocal
