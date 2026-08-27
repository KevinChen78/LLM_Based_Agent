# 规划 vs 最终实现对照表

> 对照基准:[项目规划_修订版.md](../项目规划_修订版.md)(§2.2 技术选型、§6 调用链、§7 排期)。
> 用途:面试「为什么这样取舍」的直接素材;每条结论都附代码/文档实证。
> 最后更新:2026-08-27(Phase 6 项目收尾,v1.0 基线)。

## 一、按规划落地的部分

| 规划项 | 最终实现 | 实证 |
|---|---|---|
| 控制面(C++)/数据面(Python)分离 | 四进程:api_server:8080(C++20 编排) + llm_gateway:8000 + retrieval_service:8001 + ranking_service:8002 | [CLAUDE.md](../CLAUDE.md) 架构节 |
| LLM 只做理解/规划/解释,推荐结果由检索+排序+规则决定 | TaskPlanner 出 JSON 计划;DealRetriever/DealRanker 确定性执行;taboo 剔除永远留在 C++ 侧、先于模型调用 | [src/agent/agent_orchestrator.cpp](../src/agent/agent_orchestrator.cpp)、[src/tools/deal_tools.cpp](../src/tools/deal_tools.cpp) |
| Agent 状态机 SLOT_FILL→RETRIEVE→RANK→EXPLAIN→RESPOND→FALLBACK | 按规划落地 | [include/agent/agent_orchestrator.hpp](../include/agent/agent_orchestrator.hpp) |
| 追问策略:缺 city/budget/people 即 SLOT_FILL | 按规划落地,e2e S1 守护 | [scripts/e2e_multi_turn.py](../scripts/e2e_multi_turn.py) |
| 召回硬过滤(城市/价格/人数不过 LLM) | 结构化过滤下推 PG SQL(索引列),BM25/向量只管存活集排序 | [retrieval_service/main.py](../retrieval_service/main.py) |
| LLM Gateway 统一接入+降级 | OpenAI 兼容透传 DeepSeek;无 key/上游失败回退确定性 stub | [llm_gateway/main.py](../llm_gateway/main.py) |
| nlohmann/json、spdlog | 按规划落地 | [CMakeLists.txt](../CMakeLists.txt) |
| 推荐理由价格必须来自商品表,禁止 LLM 编造 | **比规划更强**:prompt 软约束 + 输出 FactCheck 硬核验(¥xx/xx元/xx折 vs 候选集,派生白名单),违规不发出 | [docs/phase4_guard.md](phase4_guard.md) |
| 失败降级 | 全链路降级链:网关挂→C++ stub;检索挂→本地子串;PG 挂→json 后端;排序挂→规则分;gRPC 挂→逐调用回退 HTTP | [CLAUDE.md](../CLAUDE.md) 降级链节 |
| API 鉴权/限流/Trace | API key(恒定时间比较)+ 内存令牌桶(429+Retry-After)+ 全链路 trace_id | [docs/phase5_auth_grpc.md](phase5_auth_grpc.md) |
| A/B 实验 | ExperimentManager FNV-1a(user_id)%100 分桶,RANKER_MODE=off/shadow/active | [src/agent/experiment_manager.cpp](../src/agent/experiment_manager.cpp) |
| 反馈闭环(like/dislike 回流排序) | feedback 表 → build_features.py → train_ranker.py(LightGBM LambdaRank)→ ranking_service | [scripts/train_ranker.py](../scripts/train_ranker.py) |
| 可观测(recommendation_logs/llm_calls) | 按规划落地 + 三口径(真实/测试/模拟)拆分 | [scripts/evaluate.py](../scripts/evaluate.py) |

## 二、替代实现及理由

| 规划选型 | 实际替代 | 理由 | 实证 |
|---|---|---|---|
| Elasticsearch/OpenSearch + Milvus | **PostgreSQL + pgvector** 单库:结构化过滤下推 SQL + BM25(Python 侧,字符 bigram)+ 向量 ANN 按 RRF(k=60) 融合 | 5320 条商品、22 条知识的体量不需要 ES/Milvus 两个重型组件;PG 一个进程覆盖结构化+向量;运维面最小 | [retrieval_service/main.py](../retrieval_service/main.py)、[sql/002_vector.sql](../sql/002_vector.sql) |
| gRPC 全量服务间通信 | **HTTP JSON 为主,gRPC 在 retrieval_service 双协议试点**(ENABLE_GRPC 默认 OFF;任何 gRPC 失败逐调用回退 HTTP) | HTTP JSON 契约先行冻结保证可调试性;gRPC 以试点方式验证收益再推广;工具链成本高(首构 29min,踩坑 4 条) | [docs/phase5_auth_grpc.md](phase5_auth_grpc.md) |
| libcurl 同步 HTTP Client | **cpp-httplib**(header-only)+ 流式路径裸 socket 增量解析 SSE | cpp-httplib 与 FetchContent 集成更顺;其 `Post` 无 ContentReceiver 重载,流式才落裸 socket | [src/llm/llm_client.cpp](../src/llm/llm_client.cpp) |
| Redis(实时特征/会话)+ ClickHouse/Hive(离线) | **SQLite 三库 WAL**:sessions.db(会话/反馈/画像)、observability.db(审计)、ranking_features.db(特征) | 单机演示体量下 SQLite WAL 足够;零额外进程;evaluate.py 直接 ATTACH 跨库复算 | [src/memory/sqlite_session_store.cpp](../src/memory/sqlite_session_store.cpp) |
| Flink/Spark 离线画像更新 | **规则式 PreferenceExtractor,请求时惰性计算 + 5 分钟缓存** | 无流式数据规模;规则式确定性可单测;画像缺失时 prompt 无画像段(逐字节同前) | [src/agent/preference_extractor.cpp](../src/agent/preference_extractor.cpp) |
| OpenTelemetry + Prometheus + Grafana + ELK | **observability.db + GET /v1/metrics + evaluate.py** 离线报告 | 观测回传通道(组件填审计字段,编排器统一写库)替代外部栈;所有指标 SQL 可复算 | [src/observability/observability_store.cpp](../src/observability/observability_store.cpp) |
| 配置中心 Apollo/Nacos + Vault | **env 文件(.env/.env.local,setdefault 语义)+ data/guard_rules.json 规则外置** | 单机单部署;规则外置保留"改规则不改代码"能力,缺文件回退内置默认 | [data/guard_rules.json](../data/guard_rules.json) |

## 三、规划有、本轮未做(封存项)

| 规划项 | 状态 | 说明 |
|---|---|---|
| ExternalToolClient(地图/营业/券实时信息) | 未做 | 非核心闭环;planner 工具集可扩展 |
| p99 < 1.5s SLO 告警化 | 部分 | metrics/evaluate 有 avg/p50/p95 观测,未接告警 |
| 排序「优于规则基线 10%+」成功标准 | **⏸ 挂起** | 真实 feedback 数据不足(train_ranker 样本<100/正样本<10 拒绝产模型是刻意设计);解除路径:积累 → build_features → train_ranker → evaluate |
| sessions/feedback/observability 三库迁 PG | 未做 | DDL 终态已有(003_user_profiles.sql 先例),多实例部署时再做 |
| LLM 级二次安全审查(InputSafetyPrompt) | 未做 | 规则式 Guard 已覆盖演示需求;成本高 |

## 四、超出规划的部分

- **召回三级放宽链**(Phase 3):类目放宽/价格放宽逐层回退 + `_relaxed_level` 审计,空推荐率 29.2%→24.1%([docs/phase3_recall_fix.md](phase3_recall_fix.md))。
- **SSE 流式 token 级输出 + `replace` 纠偏事件**(规划只写了 REST)。
- **三窗口协作流程**(执行/规划/验收分窗,验收报告 12 份入库 [docs/acceptance/](acceptance/))——工程实践本身成为项目资产。
