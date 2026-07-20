@echo off
REM Quick start for Phase 0 on Windows

REM 1. Start LLM Gateway in a new window
start "LLM Gateway" python llm_gateway\main.py

REM 2. Wait a moment
 timeout /t 2 /nobreak > nul

REM 3. Run the C++ API server (must be built first)
.\build\bin\api_server.exe
