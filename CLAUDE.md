# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

基于 LLM 多轮会话的智能团购推荐 Agent(C++20 控制平面 + Python 数据平面)。
详细说明见 README.md;原始设计与目标 schema 见 `项目规划_修订版.md`。

## Build / Test / Run

```powershell
# 构建(VS2022 + CMake,自动定位 VS 自带的 cmake)
.\scripts\build_windows.ps1            # 加 -Rebuild 全量重配
# 产物:build\bin\Release\api_server.exe、build\bin\Release\test_agent.exe

# 单测(必须从项目根目录跑——部分用例相对路径读 data/deals.json)
.\build\bin\Release\test_agent.exe
.\build\bin\Release\test_agent.exe --gtest_filter=DealRetriever.*   # 跑单组

# e2e 回归(纯 stdlib,自启自停 api_server;端口 8080 必须空闲)
python scripts/e2e_multi_turn.py           # 离线 51 项确定性检查(C++ 内置 stub)
python scripts/e2e_multi_turn.py --real    # 7 项语义检查,需网关真模型 + 检索服务

# 检索双后端一致性(PostgreSQL vs JSON,20 例 diff 矩阵;无 PG 自动 SKIP)
python scripts/test_pg_retrieval.py

# 离线评估报告(读 data/observability.db + sessions.db)
python scripts/evaluate.py

# 一键起全栈:retrieval_service(:8001) + llm_gateway(:8000) + api_server(:8080,Web UI 在 /)
.\start_all.bat
```

数据变更流程:改/跑生成器(`scripts/gen_wuhan_deals.py`、`gen_city_deals.py`、
`gen_knowledge.py`,均确定性幂等)→ `python scripts/pg_seed.py` 同步到 PG →
`python scripts/pg_embed.py` 给新增行补向量 →
重启 retrieval_service(BM25 语料与向量通道状态都是启动时构建,不热加载)。

## 架构

**三进程 + 三存储**:

- `apps/api_server`(C++,:8080,coro::net::http)— 编排、工具、会话、SSE 流式、
  托管 `web/` 前端。唯一对外的 C++ 入口。
- `llm_gateway/main.py`(Python,:8000)— OpenAI 兼容网关 → DeepSeek;无 key 或
  上游失败时回退确定性 stub。**网关会用 `LLM_MODEL` 覆盖模型名**(C++ 发的是占位
  `gpt-4o-mini`);凭证在 gitignored 的 `llm_gateway/.env.local`。
- `retrieval_service/main.py`(Python,:8001)— 检索服务。**双存储后端**:
  `RETRIEVAL_BACKEND=postgres`(默认,结构化过滤下推 SQL,商品/知识在 PG)+
  `json`(文件兜底,PG 不可达自动降级)。BM25 排序始终在 Python,
  两后端结果逐字节一致(test_pg_retrieval.py 证明,矩阵内固定
  `RETRIEVAL_VECTOR=off`)。**向量召回**:`RETRIEVAL_VECTOR=on`(默认,仅 PG 后端)
  时查询向量与商品 embedding(pgvector,512 维,bge-small-zh-v1.5)做余弦 ANN,
  与 BM25 候选按 RRF(k=60)融合;缺 fastembed/缺向量自动降级纯 BM25,
  health 的 `vector`/`vector_model` 字段可观测。embedding 由
  `scripts/pg_embed.py` 离线生成(共享 `retrieval_service/dealtext.py` 的
  deal_text,与 BM25 语料同源)。
- 存储:PostgreSQL `groupbuy` 库(`groupbuy_items`/`merchants`/`kb_passages`,
  DDL `sql/001_schema.sql`,走查 `sql/README.md`);SQLite `data/sessions.db`
  (sessions/turns/feedback,WAL);SQLite `data/observability.db`
  (recommendation_logs/llm_calls,`/v1/metrics` 与 evaluate.py 读它)。

**C++ 侧分层**(include/agent + src/):`AgentOrchestrator` 状态机
SLOT_FILL→RETRIEVE→RANK→EXPLAIN→RESPOND→FALLBACK → `TaskPlanner`(LLM 规划,
JSON 容错解析 + 温度 0 重试)→ 工具注册表(`DealRetriever`/`DealRanker`/
`KnowledgeRetriever`)→ `ResponseComposer`(流式 LLM/模板/短路三种 compose_mode)。
协程是 `coro::Task<T>`(本地库 `../coro`),同步测试里用 `.result()` 驱动。

**关键设计模式**:

- **降级链**:每一层外部依赖都有本地兜底——网关挂→C++ 内置 stub;
  `RETRIEVAL_SERVICE_URL` 空或检索服务挂→DealCatalog 本地子串匹配、不注册
  kb_search;PG 挂→检索服务降级 json 后端;`CATALOG_BACKEND=postgres` 但
  PG 不可达→DealCatalog 回退 JSON 文件→内置 8 条。改任何一层都不能破坏降级链。
- **DealCatalog 三源回退**:ctor `DealCatalog(json_path, pg_dsn="")`,
  pg_dsn 非空先经 libpq 直连 `groupbuy_items`(`AGENT_HAVE_LIBPQ` 编译开关,
  CMake `ENABLE_PG_CATALOG` 自动探测 `C:/Program Files/PostgreSQL/17` 并
  POST_BUILD 暂存 6 个运行时 DLL 到 api_server/test_agent 输出目录);
  `Source()` 返回 "postgres"|"file:<path>"|"builtin"。默认参数保持
  全部旧调用点行为不变。
- **RetrievalClient 全 virtual**(include/agent/retrieval_client.hpp),
  单测用 fake 注入,不碰真实 HTTP。
- **观测回传通道**:不在组件签名里穿 store 指针;`Plan.llm_calls`、
  `RecommendationResult.llm_calls/compose_mode` 把审计数据带回编排器统一写库
  (Orchestrator 外层 ChatStream 计时+写库,内层 ChatStreamInner 纯业务)。
- **契约稳定面**:retrieval_service 三个端点(/v1/health、/v1/retrieve/deals、
  /v1/retrieve/kb)的 JSON 形状是 C++↔Python 的边界,改响应形状=改 C++。
  `/v1/retrieve/deals` 的 `total` 是 top_k 截断前的存活数;无文本命中时回退
  评分排序(score=rating/5)。

**Python 依赖策略**:除检索服务(requirements.txt 的 psycopg + fastembed)外
全部纯标准库——网关、e2e、生成器、评估脚本都不许引入 pip 依赖。
fastembed 首次加载模型需下载 ~100MB,国内网络要
`HF_ENDPOINT=https://hf-mirror.com` + `HF_HUB_DISABLE_XET=1`。

**测试注意**:`tests/test_deals_tools.cpp` 读真实 `data/deals.json`——计数断言
从 catalog 动态推导(FiltersWuhanFromCatalog),重新生成数据不会破坏它;
`tests/test_deal_catalog_pg.cpp` 硬编码 5320 条,改数据规模需同步。
CTest 已固定 `WORKING_DIRECTORY=项目根`(tests/CMakeLists.txt),
`RUN_TESTS`/直接跑 test_agent.exe 行为一致。

## 环境变量(常用)

`LLM_BASE_URL`(空=C++ 内置 stub 离线模式)、`RETRIEVAL_SERVICE_URL`、
`RETRIEVAL_BACKEND` / `RETRIEVAL_VECTOR` / `PG_DSN`(检索服务,见
retrieval_service/.env.example)、`CATALOG_BACKEND`(api_server,默认 json,
postgres 时复用 `PG_DSN`)、`PG_TEST_DSN`(设置后 DealCatalog live-PG 用例不 SKIP)、
`DEALS_CATALOG_PATH`、`OBS_DB_PATH`、`WEB_DIR`。
`.env` 加载是 setdefault 语义且 .env 先于 .env.local——同名键 .env 赢,每个键只定义一处。

## Windows 环境坑(已踩过)

- **Git Bash 每次调用 cwd 重置为 D:\Working**;跑项目命令必须在同一条命令里
  `cd /d/Working/LLM_Based_Agent`(或用子shell/绝对路径)。Python `subprocess`
  里传 `/d/...` 形式的 MSYS 路径给 Windows python 会打不开文件,要用 `D:/...`。
- 中文 POST body 不要用 bash 内联 `curl -d`(编码会毁),用 Python urllib 或
  `scripts/*.json` 现成 payload。
- 重编译前确认 api_server.exe 没在跑,否则 LNK1104(tasklist 找 PID 后
  `taskkill //PID <pid> //F`)。
- MSVC `long` 是 32 位,sqlite3_int64 相关代码用 `long long`。
- Python 管道输出注意块缓冲:看服务启动 banner 要 `python -u`。
- **sudo 在此机被禁用**;需要管理员写 Program Files 时(如 pgvector install)
  用 `powershell -Command "Start-Process ... -Verb RunAs"`,且参数要写成 .ps1/.bat
  文件再执行——经 Git Bash→PowerShell 内联引号会 mangling(路径含空格 "Kevin Chen" 必中招)。
- 控制台 GBK 毁中文输出:Python 脚本里 `sys.stdout.reconfigure(encoding='utf-8')`
  或 `python -X utf8`。
