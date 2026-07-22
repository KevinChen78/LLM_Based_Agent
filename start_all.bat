@echo off
rem ============================================================
rem  LLM Group-Buying Agent - one-click startup
rem  Starts: retrieval_service(:8001) + llm_gateway(:8000)
rem          + api_server(:8080, serves Web UI at /)
rem  Each service runs in its own window (logs visible).
rem  Close a window or press Ctrl+C in it to stop that service.
rem ============================================================
setlocal
cd /d "%~dp0"

rem api_server picks this up via environment (inherited by start)
set RETRIEVAL_SERVICE_URL=http://localhost:8001

echo [1/3] Starting retrieval_service on :8001 ...
start "retrieval_service :8001" python retrieval_service/main.py

echo [2/3] Starting llm_gateway on :8000 ...
start "llm_gateway :8000" python llm_gateway/main.py

echo [3/3] Starting api_server on :8080 (Web UI at /) ...
start "api_server :8080" build\bin\Release\api_server.exe

echo.
echo Waiting for services to come up ...
timeout /t 5 /nobreak >nul

echo Opening http://127.0.0.1:8080/ in your browser ...
start "" "http://127.0.0.1:8080/"

echo.
echo All started. Keep the 3 service windows open while using the app.
endlocal
