# LLM-based 团购推荐 Agent

## 目录说明

```text
LLM_Based_Agent/
├── apps/api_server/     # HTTP 服务入口
├── web/                 # Web 前端（Vanilla HTML/CSS/JS，api_server 托管）
├── include/agent/       # 公共头文件
├── src/                 # 核心实现
│   ├── agent/           # 编排层
│   ├── llm/             # LLM 客户端 / Prompt 构建
│   ├── tools/           # 工具实现（DealRetriever/DealRanker/KnowledgeRetriever + 客户端）
│   └── memory/          # 会话记忆
├── llm_gateway/         # Python LLM 网关（确定性 stub / OpenAI 兼容透传）
├── retrieval_service/   # Python 检索服务（PostgreSQL 存储 + SQL 过滤下推 + BM25/向量 RRF + gRPC 双协议）
├── ranking_service/     # Python 学习式排序服务（LightGBM，Phase 2.1）
├── sql/                 # PostgreSQL DDL 与初始化走查
├── tests/               # 单元测试
├── proto/               # gRPC/Protobuf 定义
├── configs/             # 配置文件
├── cmake/               # CMake 依赖管理
├── requirements.txt     # Python 依赖（psycopg+fastembed 仅检索服务;lightgbm 仅排序服务;grpcio 仅检索 gRPC 前端）
└── docs/                # 构建指南 + 各阶段收口文档 + 面试材料
```

## 依赖

- `coro`：本地协程库（`../coro`）
- `nlohmann/json`：FetchContent
- `spdlog`：FetchContent
- `cpp-httplib`：FetchContent（HTTP Client）
- `googletest`：FetchContent

## 构建

### Windows (Visual Studio 2022)

```powershell
cd LLM_Based_Agent
.\scripts\build_windows.ps1
```

### Linux / macOS / MSYS2

```bash
cd LLM_Based_Agent
./scripts/build_phase0.sh
```

（Windows 构建环境配置参考 [docs/Windows构建指南.md](docs/Windows构建指南.md)。）

## 运行

最小闭环只需控制平面（C++ `api_server`，:8080）一个进程——`LLM_BASE_URL`/
`RETRIEVAL_SERVICE_URL` 为空时自动降级为内置 stub + 本地目录检索，可离线演示。
完整形态是**四进程**:`api_server`(:8080)+ `llm_gateway`(:8000)+
`retrieval_service`(:8001)+ `ranking_service`(:8002,Phase 2.1,可选)。

### Windows（已验证）

```powershell
# 一键起全栈:retrieval_service(:8001) + llm_gateway(:8000)
#   + ranking_service(:8002) + api_server(:8080,Web UI 在 /)
.\start_all.bat

# 或分进程手动起:
python -u llm_gateway/main.py                          # 1. LLM 网关
python -u retrieval_service/main.py                    # 2. 检索服务
python -u ranking_service/main.py                      # 3. 排序服务(可选)
.\build\bin\Release\api_server.exe                     # 4. 控制平面
```

### Linux / macOS / MSYS2

```bash
python llm_gateway/main.py &
./build/bin/api_server   # 或 Windows 下 .\build\bin\api_server.exe
```

### 测试

中文请求需用 UTF-8 文件传入（Git Bash 内联 `-d` 会破坏中文编码）：

```bash
# 健康检查
curl -s http://localhost:8080/v1/health
curl -s http://localhost:8000/v1/health     # 网关模式

# 端到端对话（C++ -> Python Gateway）
curl -s -X POST http://localhost:8080/v1/chat \
  -H "Content-Type: application/json" \
  --data-binary @scripts/test_chat.json
```

多轮对话 e2e 回归（脚本自动起停 api_server，临时 SQLite 库，纯 stdlib）：

```bash
python scripts/e2e_multi_turn.py          # 离线确定性：内置 stub，80 项断言
python scripts/e2e_multi_turn.py --real   # 真实 LLM：需 gateway 配好 LLM_API_KEY，7 项语义检查
```

覆盖：追问→补全→推荐多轮主链路、反馈落库（👍/👎 → feedback 表 + FK 拒绝）、
SSE 事件序列、SQLite turns/context 直读校验、重启后同库续聊、会话内安全护栏、
观测落库（审计行数 + metrics 聚合）、API key 鉴权（S7 独立实例）、限流（S8 独立实例）、
输入护栏 payload 注入/违禁拦截（S9,含 guard_action 落库断言）；`--real` 额外验证
语义级槽位延续（只补预算时不再追问城市/人数）与知识问题 kb_search grounding。

## 接入真实 LLM

`llm_gateway` 默认是**确定性 stub**（零依赖即可跑）。要接入真实 LLM，把凭据写入
`llm_gateway/.env.local`（已 gitignore，模板见 `.env.example`），网关会以 OpenAI-compatible
方式透传到上游，并把模型返回的 JSON 计划原样回传：

```bash
# llm_gateway/.env.local（已 gitignore，不会提交）
LLM_API_KEY=sk-你的key
LLM_API_BASE=https://api.deepseek.com
LLM_MODEL=deepseek-v4-flash

python -u llm_gateway/main.py
# 启动日志显示 mode: REAL LLM passthrough，/v1/health 返回 mode: real-llm
```

优先级：真实环境变量 > `.env.local` > `.env`。

**已验证**：DeepSeek `deepseek-v4-flash`（base `https://api.deepseek.com`）端到端跑通。
网关对后端模型名是权威——C++ 端默认发 `model="gpt-4o-mini"` 占位名，网关会用配置的
`LLM_MODEL` 覆盖，避免上游收到未知模型名而 400。

> 验证真模型（而非 stub 兜底）的方法：发一条 stub 无法匹配的查询，如「火锅/北京/4人/500」。
> stub 对含「吃/想」的输入只会 clarify；真模型会正确解析为 `deal_retriever(category=火锅,city=北京,...)`。

上游不可用时网关自动回退到确定性 stub，不会让整条链路失败。

## Web 前端界面

`api_server` 内置了一个**零依赖的 Web 聊天界面**（Vanilla HTML/CSS/JS，无构建、无 node_modules），
与 API 同源托管，浏览器打开即用：

```powershell
# 照常起栈（web/ 由 api_server 自动托管，可用 WEB_DIR 覆盖）;一键方式见 start_all.bat
python retrieval_service/main.py
python llm_gateway/main.py
$env:RETRIEVAL_SERVICE_URL="http://localhost:8001"
.\build\bin\Release\api_server.exe
# 启动日志显示 Web UI: web/ (served at /)
```

浏览器访问 **http://localhost:8080/** 即可。功能：

- **对话优先**的聊天界面（类 ChatGPT/微信），用户/助手气泡、流式打字效果。
- **逐 token 流式**：通过 `fetch + ReadableStream` 手动解析 `/v1/chat/stream` 的 SSE
  （原生 `EventSource` 只支持 GET，本端点是 POST，故前端自行解析 `data: {...}\n\n` 帧）。
- **思考过程**：可折叠面板实时展示 `商品召回 / 智能排序 / 知识检索` 各工具的调用与完成状态。
- **商品卡片**：内嵌展示价格/原价/折扣、⭐评分、🔥销量、📍区县、tags、推荐理由。
- **回答依据**：kb_search 命中时展示知识库 grounding 段落（折叠面板）。
- **追问芯片**：clarify 时按 `missing_slots` 给出城市/人数/预算/类目快捷填充。
- **示例问题**一键发送、会话持久（`session_id` 存 localStorage，复用后端 SQLite 记忆）、
  在线状态点（轮询 `/v1/health`）、中途停止生成（AbortController）、
  流式失败自动降级为 `/v1/chat` 一次性回复。

实现要点：

- 静态托管在 `apps/api_server/main.cpp`：`ServeStaticFile()` 读 `web/` 下文件按扩展名设 MIME，
  含**路径穿越防护**（`..`、绝对路径 → 404）；coro 路由是精确键匹配，故每个资源一条 `.get()` 路由
  （`/`、`/index.html`、`/styles.css`、`/app.js`）。`WEB_DIR` 环境变量可覆盖目录（默认 `web`）。
- 前端三文件：[web/index.html](web/index.html)、[web/styles.css](web/styles.css)、[web/app.js](web/app.js)。
- 流式 `final.items` 已补全 `city/district/sold_count/rating`（对齐 `/v1/chat` 的 12 字段），
  卡片数据完整。

### 让 C++ 端用本地 stub（不连网关）

```powershell
$env:LLM_BASE_URL=""          # Windows
./build/bin/Release/api_server.exe
```

## 进程边界与降级链

- `HttpLlmClient`：`base_url` 非空时走 HTTP 调用 LLM Gateway（默认 `http://localhost:8000`）；为空则降级为内置确定性 Stub。默认超时 45s（适配推理模型）。
- `llm_gateway`：未配置 `LLM_API_KEY` 时为确定性 stub；配置后透传到真实 OpenAI-compatible LLM，并对后端模型名做权威覆盖。
- 召回/排序默认使用**真实目录后端**（`DealRetriever` + `DealRanker`，读 `data/deals.json`，缺失时用内置兜底数据集）；设 `RETRIEVAL_SERVICE_URL` 后文本相关性升级为检索服务的 BM25+向量 RRF（见下文 RAG 章节）；设 `RANKER_SERVICE_URL` + `RANKER_MODE` 后排序可升级为 LightGBM 模型分（挂/无模型自动回退规则分）。
- 会话记忆默认为 **SQLite** 持久化实现（`SqliteSessionStore`），支持跨进程重启保持会话；可用 `SESSION_STORE=memory` 切回内存实现。
- gRPC/Protobuf:retrieval_service 已试点双协议并存（Phase 5，见下文专节），其余进程间仍走 HTTP JSON。

## 会话持久化（SQLite）

默认启动即启用 SQLite，会话写入 `data/sessions.db`：

```powershell
# Windows：默认 SQLite
.\build\bin\Release\api_server.exe

# 自定义 DB 路径
$env:SESSION_DB_PATH="data/my_sessions.db"
.\build\bin\Release\api_server.exe

# 切回内存存储
$env:SESSION_STORE="memory"
.\build\bin\Release\api_server.exe
```

- `SESSION_STORE=sqlite|memory`（默认 `sqlite`）
- `SESSION_DB_PATH`（默认 `data/sessions.db`）

该实现使用官方 SQLite amalgamation，不依赖系统 SQLite，也不依赖 GitHub（amalgamation 从 `sqlite.org` 下载；若网络慢，可把 `sqlite-amalgamation-*.zip` 放到项目根目录，CMake 会自动识别并使用）。

已验证：停止 `api_server` 后重启，同一 `session_id` 的对话历史仍然保留。

## 安全护栏（I/O Guard）

`SafetyGuard` 以**规则式、确定性、零额外 LLM 调用**的方式接入 `AgentOrchestrator`：

- **输入拦截**（规划前）：检测 prompt 注入（中英文越狱话术）、违禁话题、超长输入；命中则短路返回礼貌拒绝，`next_state="BLOCKED"`。
- **输出脱敏**（返回前）：掩码手机号 / 邮箱 / 身份证 / 16–19 位数字串，过滤违禁词（`***`），作用于回复正文与商品 title/reason/tags。
- **输出事实校验**（Phase 4，compose 后）：抽取回复中的货币断言（`¥xx`/`xx元`）与折扣断言（`xx折`），与候选商品的 `price`/`original_price` 精确比对；人均（price/人数）与总价（price×人数，人数取商品自身 [min_people,max_people] 区间）为合法派生不误杀；折扣按 price/original×10 ±0.5 容差。违规时编造回复**不发出**：非流式回退模板（`compose_mode=template_guard_fallback`），流式在尾部发 additive 的 `replace` SSE 事件整体纠偏（`compose_mode=llm_stream_guard_fallback`，旧前端忽略不崩）。
- **规则外置**（Phase 4-C）：违禁词/注入模式/违禁话题/输入长度阈值在 [data/guard_rules.json](data/guard_rules.json)，缺文件或损坏时回退内置默认（与内置逐字节一致）；`GUARD_RULES_PATH` 可改路径。
- **Guard 可观测**（Phase 4-C）：`recommendation_logs` 幂等加列 `guard_action`（refuse_input/sanitized/fact_violation）+ `guard_detail`（违规摘要），`evaluate.py` 有「Guard 动作」段（分布、fact_violation 率、明细）。

构造 `AgentOrchestrator` 时传 `SafetyGuard` 即启用，传 `nullptr` 则跳过（默认参数，不破坏现有调用）。规则集见 [include/agent/safety_guard.hpp](include/agent/safety_guard.hpp) 与 [src/agent/safety_guard.cpp](src/agent/safety_guard.cpp)；单测见 [tests/test_safety_guard.cpp](tests/test_safety_guard.cpp)。

## API 鉴权与限流(Phase 5,默认关闭)

- **API key 鉴权**:`AGENT_API_KEYS`(逗号分隔)设置后,`/v1/chat`、
  `/v1/chat/stream`、`/v1/feedback`、`/v1/metrics` 要求 `X-Api-Key` header,
  失败 401;`/v1/health` 与静态资源豁免。空 = 关闭,行为与之前逐字节一致。
  比较恒定时间防时序侧信道。
- **限流**:`RATE_LIMIT_RPS` / `RATE_LIMIT_BURST` 设置后启用每用户内存令牌桶
  (key:user_id → API key → anonymous;SSE 按请求计 1 次),超限 429 +
  `Retry-After`。空/0 = 不限。`/v1/metrics` 响应含
  `api_guard{auth_rejected, rate_limited}` 计数器。
- **多实例**:`AGENT_PORT`(默认 8080)。

## 服务间 gRPC 试点(Phase 5,retrieval_service)

HTTP JSON 契约冻结不变,gRPC 作为**并存试点**:`-DENABLE_GRPC=ON`(默认 OFF)
源码构建工具链;retrieval_service 设 `GRPC_PORT=8011` 后同进程双协议(共用
handler);api_server 设 `RETRIEVAL_PROTOCOL=grpc` +
`RETRIEVAL_GRPC_ADDR=127.0.0.1:8011` 走 gRPC,**任何 gRPC 失败逐调用回退
HTTP**。契约 `proto/retrieval.proto` 逐字段镜像 HTTP 形状;一致性由
`scripts/test_pg_retrieval.py` 的协议×后端矩阵守护。配置/踩坑/回滚详见
[docs/phase5_auth_grpc.md](docs/phase5_auth_grpc.md)。


## 观测与评估（recommendation_logs / llm_calls / metrics / evaluate.py）

每次请求落一行推荐审计、每次 LLM 调用落一行调用审计（独立库 `data/observability.db`，
`OBS_DB_PATH` 可改），`trace_id` 串联两表：

- `recommendation_logs`：trace_id / session_id / 请求文本 / action / 槽位 / Top5 商品+分数 /
  回复 / grounding 数 / compose_mode（llm_stream·llm·template·short_circuit·none，
  guard 介入时另有 template_guard_fallback·llm_stream_guard_fallback）/ 端到端延迟 /
  guard_action·guard_detail（Phase 4-C）。
- `llm_calls`：trace_id / purpose（plan·compose）/ model / prompt+completion tokens / 延迟 /
  status / attempt（planner 重试序号）。流式 compose 行的 tokens 来自尾部 SSE
  usage chunk（网关已请求 `stream_options.include_usage`，Phase 2.3-A 起按真实
  usage 落库；上游不给 usage 时如实记 0）。

聚合指标：`GET /v1/metrics`（请求分布、FALLBACK 率、空推荐率、avg/p95 延迟、LLM 调用与
token 汇总、ATTACH 会话库算反馈满意率）。

离线评估报告：

```bash
python scripts/evaluate.py     # 读 data/observability.db + data/sessions.db
```

输出 action 分布、FALLBACK/空推荐率、追问后续答率、延迟 avg/p50/p95、LLM token 消耗、
grounding 触发率、反馈满意率与被 👎 最多商品 Top5。实现见
[include/agent/observability_store.hpp](include/agent/observability_store.hpp)；
单测见 [tests/test_observability.cpp](tests/test_observability.cpp)。

## 反馈落库（👍/👎 → feedback 表）

每条助手回复底部和每张商品卡片上都有 👍/👎 按钮，点击后 POST `/v1/feedback`，
写入会话库（`data/sessions.db`）的 `feedback` 表：

```
rowid | session_id(FK→sessions) | trace_id | item_id(空=整条回复) | feedback_type(like/dislike) | comment | created_at
```

- 请求体：`{session_id(必填), feedback_type("like"|"dislike"), trace_id?, item_id?, comment?}`；
  成功 `{"success":true}`；未知 session（FK 拒绝）或非法 type → 400 `{"success":false,"error":...}`。
- 存储接口：`SessionMemoryStore::AppendFeedback`（SQLite / InMemory 双实现，未知 session 语义一致）。
- 前端：`web/app.js` 的 `sendFeedback()`，乐观置灰、失败恢复可点。

## 真实召回 / 排序后端

默认后端是**真实目录驱动的召回 + 多因子排序**，替代了 Phase 0 的写死 Mock 数据：

- `DealCatalog`（[include/agent/deal_catalog.hpp](include/agent/deal_catalog.hpp)）：从 `data/deals.json` 加载团购商品（5320 条，覆盖 8 城——武汉 5000 条 + 深圳/北京/上海各 100 条生成数据，由 `scripts/gen_wuhan_deals.py` 与 `scripts/gen_city_deals.py` 确定性生成、幂等重跑），文件缺失/不可读时自动回退到内置兜底数据集（8 条），保证离线和单测环境也能跑。
- `DealRetriever`（工具名 `deal_retriever`）：按 `city / category / district / max_price / min_price / people / keywords / top_k` 过滤，再按**相关性打分**（关键词命中 + 评分 + 折扣）排序并截断 `top_k`；`people` 会匹配套餐的 `min_people/max_people` 区间。
- `DealRanker`（工具名 `deal_ranker`）：对召回候选做多因子重排 `0.35·评分 + 0.25·销量归一 + 0.25·价格契合 + 0.15·折扣`，可选 `taboo` 禁忌词过滤、`budget` 对超预算项大幅扣分，取 `top_n`。

### retrieve → rank 自动链接

编排器对工具调用做**确定性链接**：当 `*_ranker` 被调用但 `candidates` 为空时，自动注入上一步召回累积的商品作为候选；ranker 的输出**替换**（而非追加）召回结果。这样不依赖 LLM 把召回结果转发给排序器。流式事件里 `tool_call` 会带 `"chained": true` 标识本次注入。

### 配置

```powershell
# 默认：真实目录后端（读 data/deals.json，缺失则用内置兜底数据集）
.\build\bin\Release\api_server.exe

# 自定义商品目录路径
$env:DEALS_CATALOG_PATH="data/my_deals.json"
.\build\bin\Release\api_server.exe
```

> 旧的 `MockRetriever / MockRanker`（写死 4 条数据）作为 Phase-0 参考实现保留在源码中，但默认不再注册；当前唯一接线的是真实目录后端。

商品目录 JSON 格式见 [data/deals.json](data/deals.json)，每条含 `item_id / merchant_id / title / category / city / district / price / original_price / sold_count / rating / min_people / max_people / tags / description`。

### 字段透传

`RecommendationItem` 扩展了 `sold_count / rating / district / score / reason`，编排器与 `/v1/chat`、`/v1/chat/stream` 都会返回这些字段，前端可直接用于展示评分、销量、折扣理由。

单测见 [tests/test_deals_tools.cpp](tests/test_deals_tools.cpp)（catalog 兜底、过滤、关键词加权、人数区间、top_k、多因子重排、禁忌过滤、预算惩罚）。

## 自然语言回复生成

召回排序完成后，`ResponseComposer`（[src/agent/response_composer.cpp](src/agent/response_composer.cpp)）会用 **LLM 把排好序的商品拼成一段自然、有吸引力的中文推荐语**，而不是固定的模板列表：

- 把 top 商品（最多 10 条，已按得分排序）连同价格/评分/销量/标签喂给 `ResponseCompositionPrompt`（[src/llm/prompt_builder.cpp](src/llm/prompt_builder.cpp)），让模型输出 `{"reply": "...", "item_reasons": {item_id: 一句话理由}}`。
- 解析容错：依次尝试**直接 JSON → 去除 ```` ```json ```` 围栏 → 截取首尾 `{}`**，并按 `item_id` 把 `item_reasons` 合并回每条商品的 `reason` 字段。
- **降级策略**：LLM 客户端不可用（`Healthy()==false`，例如上一步规划调用已探测到网关宕机）、为空指针、抛异常或输出不可解析时，**自动回退到确定性模板回复**（"为您推荐以下团购：1. ..."），保证链路不因生成失败而中断。

实测（DeepSeek `deepseek-v4-flash`，上海/海鲜/3人/300）：

```text
我为您推荐2家适合今晚三人聚餐的海鲜团购，都在上海且预算300左右：
首先评分4.8的波士顿龙虾三人餐仅298元，几乎对折；还有好评如潮的海鲜大咖套餐
3-4人仅288元，性价比极高。
```

离线（`LLM_BASE_URL=""`，确定性 stub）时 stub 无法产出合法回复 → 自动走模板，回复仍为结构化列表，整条链路正常。单测见 [tests/test_composer.cpp](tests/test_composer.cpp)（LLM 正常/垃圾输出/Markdown 围栏/散文嵌套 JSON/空项跳过/空指针/不健康降级）。

## 流式回复 `/v1/chat/stream`

除了一次性返回完整结果的 `POST /v1/chat` 外，控制平面还提供 **Server-Sent Events** 流式端点：

```bash
# 请求体与 /v1/chat 相同；使用 -N 让 curl 实时输出收到的每个事件
curl -s -N -X POST http://localhost:8080/v1/chat/stream \
  -H "Content-Type: application/json" \
  --data-binary @scripts/test_chat.json
```

返回格式为 SSE，每条约 `data: {"event": ..., "data": ...}\n\n`：

```text
data: {"event":"started","data":{"session_id":"...","state":"SLOT_FILL"}}
data: {"event":"input_guard","data":{"status":"safe"}}
data: {"event":"planning","data":{"detail":"deciding next step"}}
data: {"event":"plan","data":{"next_state":"retrieve","tool_count":2}}
data: {"event":"tool_call","data":{"tool_name":"deal_retriever","chained":false}}
data: {"event":"tool_result","data":{"tool_name":"deal_retriever","success":true}}
data: {"event":"tool_call","data":{"tool_name":"deal_ranker","chained":true}}
data: {"event":"tool_result","data":{"tool_name":"deal_ranker","success":true}}
data: {"event":"composing","data":{"detail":"generating reply"}}
data: {"event":"delta","data":{"content":"今晚"}}
data: {"event":"delta","data":{"content":"三个人"}}
data: {"event":"delta","data":{"content":"吃海鲜"}}
...（逐 token 增量）...
data: {"event":"final","data":{"session_id":"...","reply":"...","items":[...]}}
```

事件说明：

| event | 含义 |
|-------|------|
| `started` | 会话已加载 |
| `input_guard` | 输入安检中 / 安全 / 被拦截 |
| `planning` | 正在规划下一步 |
| `plan` | 规划完成，携带 `next_state`、`missing_slots`、`tool_count` |
| `tool_call` | 开始调用工具（`chained` 表示编排器是否自动注入了上一步结果）|
| `tool_result` | 工具调用结束 |
| `grounding` | 知识库命中（`passage_count`），仅 kb_search 有结果时出现 |
| `composing` | 正在生成回复 |
| `delta` | **token 级增量**，`data.content` 为一段文本片段，拼起来即完整回复 |
| `replace` | （Phase 4-B，additive）事实校验在流式输出完成后发现违规，`data.content` 为模板兜底回复，整体替换此前 delta 拼出的文本；旧前端忽略不崩 |
| `final` | 完整 `RecommendationResult`（与 `/v1/chat` 返回一致）|
| `error` | 处理异常 |

实现要点：

- 传输层基于 HTTP `Transfer-Encoding: chunked`，`Content-Type: text/event-stream; charset=utf-8`。
- `composing` 与 `final` 之间的 `delta` 事件是**真实 LLM token 流**：`ResponseComposer` 用 `LlmClient::ChatStream` 逐 token 生成自然语言回复，每个片段通过 `StreamEmitter::EmitDelta()` 即时推送。
- 流式链路：`AgentOrchestrator` 把 emitter 传给 composer → composer 调 `ChatStream` → `HttpLlmClient` 用**裸 socket** 向网关 `POST stream=true` 并增量解析上游 SSE（cpp-httplib 的 `Post` 无 `ContentReceiver` 重载，故直接用阻塞 socket）→ 网关 `stream_real_llm` 把上游 SSE 原样透传给客户端。
- **降级**：离线 stub（`LLM_BASE_URL=""`）或网关不可用时，`ChatStream` 返回 `streamed=false`，composer 改为把**确定性模板**按行切成多个 `delta` 推送，客户端仍能看到流式线序，链路不中断。
- `delta` 携带的是 LLM 原始输出；`final.reply` 是经输出安全脱敏后的最终文本（若 LLM 文本不含敏感信息，二者一致）。
- 已实测（DeepSeek `deepseek-v4-flash`）：单次回复约 200+ 个 `delta`、流为合法 UTF-8、所有 `delta` 拼接与 `final.reply` 逐字节一致。

### 与 `/v1/chat` 的差异

| 特性 | `/v1/chat` | `/v1/chat/stream` |
|------|------------|-------------------|
| 返回格式 | 单次 JSON | SSE 事件流 |
| 进度可见 | 无 | 有（planning / tool / composing 等）|
| 连接 | 请求-响应 | 长连接，流结束后关闭 |
| 适用场景 | 简单集成 | 需要实时反馈 / 后续 token 流 |

## RAG：BM25 + 向量 RRF 商品语义召回 + 知识库问答

在真实目录后端之上，可选地接入一个检索服务
（[retrieval_service/main.py](retrieval_service/main.py)，:8001），提供两层 RAG 能力：

1. **商品语义召回**：`DealRetriever` 的文本相关性从子串匹配升级为 **BM25
   （k1=1.5，b=0.75，中文按字符 bigram 分词，无需 jieba）+ pgvector 向量召回的
   RRF 融合**（k=60；向量由 fastembed + bge-small-zh-v1.5 生成，512 维，见下文
   "向量召回"小节），同义词/口语表达（"情侣约会""吃点辣的"）召回率显著提升。
   结构化过滤（城市/类目/价格/人数）规则与 C++ 端一致，先过滤再对存活集融合排序；
   query 为空或过滤集为空时回退到评分排序。
2. **知识库 RAG**：新增工具 `kb_search`（`KnowledgeRetriever`），检索
   [data/knowledge.json](data/knowledge.json)（22 条 FAQ/政策/菜品知识：
   发票/预约/退款/包间/停车/忌口/营业时间/核销等）。编排器把命中的段落收集为
   **grounding**，注入 composer 的 LLM prompt（`# 参考知识` 块），让事实性回答
   有据可依而非编造；`/v1/chat` 响应与流式 `final` 事件都会带上 `grounding` 字段。

**存储后端**（`RETRIEVAL_BACKEND`，默认 `postgres`）：

- `postgres`：商品/知识库存 PostgreSQL（`groupbuy_items`/`merchants`/`kb_passages`，
  DDL 见 [sql/001_schema.sql](sql/001_schema.sql)，初始化走查见 [sql/README.md](sql/README.md)）。
  **结构化过滤下推为索引 SQL**（city/category/district/价格/人数），BM25 排序仍在
  Python——启动时从 PG 全量加载构建语料，排序行为与 JSON 后端逐字节一致
  （`scripts/test_pg_retrieval.py` 双后端 20 例 diff 矩阵证明）。
  依赖 psycopg + fastembed（`pip install -r requirements.txt`，项目仅有的两个
  第三方 Python 依赖）；数据播种 `python scripts/pg_seed.py`（JSON 生成器仍是
  数据源，幂等同步），向量生成 `python scripts/pg_embed.py`（增量补 NULL 行）。
- `json`：原文件后端，纯 Python 标准库零依赖。postgres 后端启动失败（PG 未起/
  未播种/缺 psycopg）时自动降级为该模式并打印 WARNING，`/v1/health` 的
  `backend` 字段指示当前实际后端。

**向量召回**（`RETRIEVAL_VECTOR`，默认 `on`，仅 postgres 后端生效）：
fastembed（ONNX 运行时，无 torch）加载 bge-small-zh-v1.5，查询向量与商品向量在
PG 内做余弦 ANN（pgvector `<=>` 算子 + HNSW 索引），向量候选与 BM25 候选按
RRF 融合（k=60，分数归一化到 0..1）；缺模型/缺向量列/embedding 未生成时任一失败
自动降级纯 BM25 并 WARNING，健康检查的 `vector`/`vector_model` 字段可观测。
初始化与验证步骤见 [sql/README.md](sql/README.md) §6（含国内网络 HF 镜像配置）；
语义断言与对比证明见 `scripts/test_pg_vector.py`。
注意行为变化：向量通道开启后非空查询几乎总有结果（余弦最近邻永远存在），
"无文本命中 → 评分排序兜底"只在结构化过滤集为空时触发。
知识库 kb_passages 保持纯 BM25（22 条，无需向量）。

### 启用方式

```powershell
# 0. 一次性：PostgreSQL 初始化（建库/建表/播种），见 sql/README.md
pip install -r requirements.txt

# 1. 启动检索服务（:8001，默认 backend=postgres）
python retrieval_service/main.py

# 2. 启动网关（:8000）与 api_server（:8080），并指向检索服务
python llm_gateway/main.py
$env:RETRIEVAL_SERVICE_URL="http://localhost:8001"
.\build\bin\Release\api_server.exe
# 启动日志：Retrieval service: http://localhost:8001 (BM25 deals + kb_search)
```

`RETRIEVAL_SERVICE_URL` 为空（默认）= **完全离线降级**：商品检索退回本地子串匹配、
不注册 `kb_search`、无 grounding，原有行为不变。服务中途宕机时单次调用自动回退本地逻辑。

### 服务契约

```text
GET  /v1/health          -> {"status":"ok","deal_count":5320,"kb_count":22,"backend":"postgres|json",
                             "vector":"on|off","vector_model":"BAAI/bge-small-zh-v1.5"}
POST /v1/retrieve/deals  body {"query","city","category","district","max_price","min_price","people","top_k"}
                     -> {"items":[<完整 deal>+score], "total":N}
POST /v1/retrieve/kb     body {"query","top_k"}
                     -> {"passages":[{"id","category","title","content","source","score"}]}
```

C++ 侧由 `RetrievalClient`（[include/agent/retrieval_client.hpp](include/agent/retrieval_client.hpp)，
cpp-httplib，非流式 POST）封装；`DealRetriever` 注入 client 后健康则委托 BM25，
`KnowledgeRetriever` 只在 `RETRIEVAL_SERVICE_URL` 非空时注册。

### 实测（DeepSeek deepseek-v4-flash + 检索服务）

问「武汉3人想吃小龙虾，预算400，请问能开发票吗？有没有包间？」：
规划器同时发出 `deal_retriever`(BM25) + `deal_ranker` + `kb_search` 三个调用，
回复既推荐商品又依据知识段落回答「所有套餐都支持开具发票…包间预订时请在备注中说明」，
`grounding` 字段携带发票与包间两条知识片段。流式端点事件序列为
`tool_call(deal_retriever/deal_ranker/kb_search) → grounding → delta×196 → final(含 grounding)`。

### 升级路径

语义召回升级（BM25 → BM25 + 向量 RRF）已完成（见上文"向量召回"小节，
端点契约不变，C++ 侧零改动）。下一步可在 kb_passages 上引入向量召回，
或在更大商品量级下评估 embedding 模型升级（bge-m3 等）。

单测见 [tests/test_kb.cpp](tests/test_kb.cpp)（kb_search 契约/未配置/服务宕机、
BM25 委托/回退/无 client 不回归）与 [tests/test_composer.cpp](tests/test_composer.cpp)
（grounding 注入 prompt、空 items + 有 grounding 仍走 LLM 回答知识问题）。

## 路线图：Phase 2 增量规划

核心推荐闭环、反馈闭环、观测评估闭环均已闭合。以下是从"可演示 Agent"走向
"生产级推荐系统"的增量项，按优先级排序（对应《项目规划_修订版.md》Phase 2）：

### 2.1 学习式排序（反馈数据 → 排序优化）—— **已完成（首版）**

反馈表已开始积累 like/dislike 信号，这是排序学习的数据基础。本迭代落地：

- **候选集埋点**：`recommendation_logs` 幂等加列 `candidates_json`（ranker
  输入侧全候选 + 规则分/模型分，cap 50）/ `experiment_group` / `rank_mode`，
  由 `DealRanker` 经 `rank_audit` 顶层字段回传（不改 ITool 接口）。
- **特征存储**：`scripts/build_features.py`（纯 stdlib，幂等）聚合
  `candidates_json` 曝光 + `feedback` 反馈 → `data/ranking_features.db` 的
  `item_features` 表（impressions/likes/dislikes/category_hot）。
- **排序模型服务**：`ranking_service/main.py`（:8002，LightGBM，requirements.txt
  新增 `lightgbm>=4.0,<5` 仅限此服务与训练脚本）。`GET /v1/health` 报
  `model_loaded`/`feature_rows`/`profiles_available`；`POST /v1/rank` 收
  candidates+context 回 `{item_id, model_score}`。特征组装单点共享
  `ranking_service/features.py`（训练/推理双端 import，meta.json 存
  feature_names 校验，杜绝漂移）。`model_loaded=false` 是合法健康态（冷启动），
  C++ 回退规则分。
- **训练**：`scripts/train_ranker.py` —— LambdaRank（group=trace_id），
  label：like=1/dislike=0/无反馈忽略（`--implicit-negatives` 可选）；
  样本 <100 或正样本 <10 拒绝产模型（链路自动回退规则分）；
  `--synthetic` 合成数据冒烟。产物 `ranking_service/model.txt` + `meta.json`
  （已 gitignore）。
- **C++ 接线**：`RankerClient`（全 virtual，仿 RetrievalClient，超时收紧至
  2s）注入 `DealRanker`；taboo 剔除始终留在 C++ 侧、先于任何模型调用。
  `rank_mode` 观测三种生效路径：`model` / `rule` / `rule_fallback`。
- **A/B 实验**：`ExperimentManager`（FNV-1a(user_id) % 100 分桶，空 user_id
  恒 control）。`RANKER_MODE=off|shadow|active` + `RANKER_TREATMENT_PCT` +
  `RANKER_EXPERIMENT`；shadow 模式规则分服务、模型分只入审计。
- **评估**：`evaluate.py` 新增「排序/实验对比」段——分组满意率
  （feedback 按 trace_id 回连，样本 <30 标注）、rank_mode 分布与模型回退率、
  位置敏感度（like 率 by 推荐位置）。
- **成功标准**（修订版）：点击率/转化率优于规则基线 10%+ —— 待数据积累后
  用实验对比段验证。
- 训练流程：`python scripts/build_features.py` → `python scripts/train_ranker.py`
  → 重启 ranking_service（模型/特征启动时加载，不热加载）。

### 2.2 用户画像与长期记忆 —— **已完成（首版）**

- **UserProfileStore**（`include/agent/user_profile_store.hpp` +
  `src/memory/sqlite_user_profile_store.cpp`）：`user_profiles` 表与
  sessions/feedback 同库（`data/sessions.db`）、独立连接（WAL + busy_timeout），
  不污染 SessionMemoryStore 接口；PG 终态 DDL 见 `sql/003_user_profiles.sql`
  （本轮不迁移）。
- **PreferenceExtractor**（规则式、确定性、纯函数可单测，不用 LLM）：like 的
  item 反查 catalog 聚合 category/city top-3（like +1 / dislike -1）、budget
  均值、taboo 去重入 dietary_tags、price_sensitivity = like 中 discount>0.3
  占比。请求时惰性计算 + 5 分钟缓存（`updated_at` 字符串比较）。
- **planner prompt 注入**：`PromptBuilder::TaskPlanningPrompt` 第 4 参
  `user_profile_json`（默认空）；画像段插在 `# 当前已填充槽位` 之前，规则明示
  「当轮显式输入永远优先于画像」；空画像时段落完全不出现（prompt 逐字节同前）。
- **前端**：`web/app.js` 的 user_id 从硬编码 `web-user` 改为 localStorage
  持久 UUID（`crypto.randomUUID`，兜底时间戳随机串），画像与 A/B 分桶才真正
  分用户。
- 会话记忆从 SQLite 迁 Redis（短期 TTL）+ PostgreSQL（持久化），支撑多实例部署——
  待 2.4 多实例需求出现时再做。

### 2.3 数据层升级 —— **已完成**

- **商品库迁 PostgreSQL（检索路径）**：`groupbuy_items`/`merchants`/
  `kb_passages` 三表（[sql/001_schema.sql](sql/001_schema.sql)），检索服务默认
  `backend=postgres`，结构化过滤下推 SQL + BM25 排序留 Python；
  `scripts/pg_seed.py` 幂等播种、`scripts/test_pg_retrieval.py` 双后端一致性矩阵。
- **`DealCatalog` DB 后端**：C++ 侧经 libpq 直连 `groupbuy_items`
  （`CATALOG_BACKEND=postgres` + `PG_DSN`，默认 json 保持离线确定性），
  回退链 PG → JSON 文件 → 内置 8 条；CMake 自动探测 libpq 并暂存运行时 DLL。
- **语义召回升级**：BM25 + pgvector 向量 **RRF 融合**（fastembed
  bge-small-zh-v1.5 512 维，`sql/002_vector.sql` + `scripts/pg_embed.py` 离线生成，
  查询时向量 ANN 与 BM25 各取候选池按倒数排名融合，同义词/口语查询召回显著提升；
  `RETRIEVAL_VECTOR` 开关，缺模型/缺向量自动降级纯 BM25；
  `scripts/test_pg_vector.py` 语义断言 + 对比证明）。
- `sql/` 目录已有检索库 DDL；后续待办：sessions/feedback/observability 三库
  从 SQLite 迁 PG 的正式 DDL 与迁移脚本。

### 2.4 服务化与生产横切

- **gRPC**：retrieval_service 已试点双协议并存（见上文 Phase 5 节）；
  待办：推广到 llm_gateway / ranking_service(gateway 涉 SSE 流式需单独评估)、
  对外 `proto/agent_service.proto` 启用。
- **API 中间件**：鉴权（API key 已落地,JWT 未做）、限流（令牌桶已落地）、
  Trace 注入（对接已有 trace_id,401/429 已带 trace_id）。
- **外部工具**：`ExternalToolClient`（地图/营业时间/优惠券实时信息），扩展
  planner 的工具集。
- **管理后台**：`apps/admin_console`——配置热更（Prompt 版本/模型名/超参）、
  日志查询（直接读 observability.db）、实验管理界面。
- **SLO 落地**：p99 < 1.5s 目标接入 `/v1/metrics` 告警口径；LLM 多模型
  `ModelFallbackManager`（主模型超时自动切备用模型，`LlmModelTier` 已预留）。

### 明确不做（当前规模下）

K8s/服务网格、ClickHouse 离线数仓、Prometheus 外部监控栈——单机 SQLite +
`/v1/metrics` + `evaluate.py` 已覆盖当前体量的观测需求，待多实例部署时再引入。

## 相关文件

- 流式抽象：`include/agent/stream_emitter.hpp`
- SSE 实现：`include/agent/sse_stream_emitter.hpp`、`src/agent/sse_stream_emitter.cpp`
- 编排器事件注入：`src/agent/agent_orchestrator.cpp` 中的 `ChatStream()`
- HTTP 入口：`apps/api_server/main.cpp` 的 `/v1/chat/stream` 路由
- 底层 chunked 支持：`../coro/include/coro/net/http/response.hpp`、`../coro/include/coro/net/http/server.hpp`
- 单测：`tests/test_streaming.cpp`
