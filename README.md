# LLM-based 团购推荐 Agent —— Phase 0 工程

## 目录说明

```text
LLM_Based_Agent/
├── apps/api_server/     # HTTP 服务入口
├── web/                 # Web 前端（Vanilla HTML/CSS/JS，api_server 托管）
├── include/agent/       # 公共头文件
├── src/                 # 核心实现
│   ├── agent/           # 编排层
│   ├── llm/             # LLM 客户端 / Prompt 构建
│   ├── tools/           # Mock 工具
│   └── memory/          # 会话记忆
├── llm_gateway/         # Python Mock LLM Gateway
├── retrieval_service/   # Python BM25 检索服务（商品语义召回 + 知识库 RAG）
├── tests/               # 单元测试
├── proto/               # gRPC/Protobuf 定义
├── configs/             # 配置文件
├── cmake/               # CMake 依赖管理
└── docs/                # 构建指南
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

> 当前环境缺少 C++ 编译器和 CMake，请先参考 [docs/Windows构建指南.md](docs/Windows构建指南.md)。

## 运行

控制平面（C++ `api_server`，:8080）通过 HTTP 调用数据平面（Python `llm_gateway`，:8000）。
**两个进程都要启动**，否则 C++ 端会降级为 `FALLBACK`。

### Windows（已验证）

```powershell
# 1. 启动 Python LLM Gateway（数据平面）
python llm_gateway/main.py
# 或带 -u 实时查看日志：
python -u llm_gateway/main.py

# 2. 另开终端启动 C++ API Server（控制平面）
.\build\bin\Release\api_server.exe
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
> stub 对含「吃/想」的输入只会 clarify；真模型会正确解析为 `mock_retriever(category=火锅,city=北京,...)`。

上游不可用时网关自动回退到确定性 stub，不会让整条链路失败。

## Web 前端界面

`api_server` 内置了一个**零依赖的 Web 聊天界面**（Vanilla HTML/CSS/JS，无构建、无 node_modules），
与 API 同源托管，浏览器打开即用：

```powershell
# 照常起三进程（web/ 由 api_server 自动托管，可用 WEB_DIR 覆盖）
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

## Phase 0 边界

- `HttpLlmClient`：`base_url` 非空时走 HTTP 调用 LLM Gateway（默认 `http://localhost:8000`）；为空则降级为内置确定性 Stub。默认超时 45s（适配推理模型）。
- `llm_gateway`：未配置 `LLM_API_KEY` 时为确定性 stub；配置后透传到真实 OpenAI-compatible LLM，并对后端模型名做权威覆盖。
- 召回/排序默认使用**真实目录后端**（`DealRetriever` + `DealRanker`，读 `data/deals.json`，缺失时用内置兜底数据集）；设 `RETRIEVAL_SERVICE_URL` 后文本相关性升级为检索服务的 BM25（见下文 RAG 章节）。
- 会话记忆默认为 **SQLite** 持久化实现（`SqliteSessionStore`），支持跨进程重启保持会话；可用 `SESSION_STORE=memory` 切回内存实现。
- gRPC/Protobuf 仅保留定义文件，Phase 0 先通过 HTTP JSON 通信。

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

构造 `AgentOrchestrator` 时传 `SafetyGuard` 即启用，传 `nullptr` 则跳过（默认参数，不破坏现有调用）。规则集见 [include/agent/safety_guard.hpp](include/agent/safety_guard.hpp) 与 [src/agent/safety_guard.cpp](src/agent/safety_guard.cpp)；单测见 [tests/test_safety_guard.cpp](tests/test_safety_guard.cpp)。

## 真实召回 / 排序后端

默认后端是**真实目录驱动的召回 + 多因子排序**，替代了 Phase 0 的写死 Mock 数据：

- `DealCatalog`（[include/agent/deal_catalog.hpp](include/agent/deal_catalog.hpp)）：从 `data/deals.json` 加载团购商品（20 条，覆盖 6 城 7 类目），文件缺失/不可读时自动回退到内置兜底数据集（8 条），保证离线和单测环境也能跑。
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

## RAG：BM25 商品语义召回 + 知识库问答

在真实目录后端之上，可选地接入一个**纯 Python 标准库**的检索服务
（[retrieval_service/main.py](retrieval_service/main.py)，:8001），提供两层 RAG 能力：

1. **商品语义召回**：`DealRetriever` 的文本相关性从子串匹配升级为 **BM25**
   （中文按字符 bigram 分词，无需 jieba；英文按单词；k1=1.5，b=0.75），
   同义词/口语表达召回率更高。结构化过滤（城市/类目/价格/人数）规则与 C++ 端一致，
   先过滤再对存活集做 BM25；query 为空或无文本命中时回退到评分排序。
2. **知识库 RAG**：新增工具 `kb_search`（`KnowledgeRetriever`），检索
   [data/knowledge.json](data/knowledge.json)（22 条 FAQ/政策/菜品知识：
   发票/预约/退款/包间/停车/忌口/营业时间/核销等）。编排器把命中的段落收集为
   **grounding**，注入 composer 的 LLM prompt（`# 参考知识` 块），让事实性回答
   有据可依而非编造；`/v1/chat` 响应与流式 `final` 事件都会带上 `grounding` 字段。

### 启用方式

```powershell
# 1. 启动检索服务（:8001）
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
GET  /v1/health          -> {"status":"ok","deal_count":120,"kb_count":22}
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

BM25 → embedding 时只需在 `retrieval_service` 内把 `BM25Index` 换成
sentence-transformers（如 `paraphrase-multilingual-MiniLM-L12-v2`）+ 余弦检索，
端点契约不变，C++ 侧零改动。

单测见 [tests/test_kb.cpp](tests/test_kb.cpp)（kb_search 契约/未配置/服务宕机、
BM25 委托/回退/无 client 不回归）与 [tests/test_composer.cpp](tests/test_composer.cpp)
（grounding 注入 prompt、空 items + 有 grounding 仍走 LLM 回答知识问题）。

## 相关文件

- 流式抽象：`include/agent/stream_emitter.hpp`
- SSE 实现：`include/agent/sse_stream_emitter.hpp`、`src/agent/sse_stream_emitter.cpp`
- 编排器事件注入：`src/agent/agent_orchestrator.cpp` 中的 `ChatStream()`
- HTTP 入口：`apps/api_server/main.cpp` 的 `/v1/chat/stream` 路由
- 底层 chunked 支持：`../coro/include/coro/net/http/response.hpp`、`../coro/include/coro/net/http/server.hpp`
- 单测：`tests/test_streaming.cpp`
#   L L M _ B a s e d _ A g e n t  
 