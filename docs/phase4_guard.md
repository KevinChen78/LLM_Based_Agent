# Phase 4 输出事实校验 Guard + Guard 可观测 — 收口文档

> 实施 plan:`phase4-output-fact-guard.md` v2(用户已确认)
> 提交:A `7435f06` / B `97ba002` / C `953c826` / D(本文档所在 commit)

## 目标与范围

补上合规闭环最后一环:LLM 输出后的**事实核验**。compose prompt 里「价格/
折扣必须与商品一致,禁止编造」此前只是软约束;Phase 4 起,回复中的货币
断言(¥xx / xx元)与折扣断言(xx折)在发出前与候选商品数据核验,违规
回复不到达用户。

规则式、确定性、零额外 LLM 调用(与 SafetyGuard 整体哲学一致);契约只增
不改(SSE `replace` 为 additive 事件,旧前端忽略不崩);所有 guard 动作
落库可复算。

## 实现

### 事实校验核心(`SafetyGuard::FactCheckReply`,阶段 A)

- 抽取:`¥\s*数字`、`数字\s*元`、`数字\s*折` 三类断言。
- 白名单(任一候选 item 命中即合法):
  - `price` / `original_price` 精确比对(±0.01);
  - 派生:人均 `price/p` 与总价 `price*p`,p 取 item 自身
    `[min_people, max_people]` 区间(cap 1..20;区间缺失则不派生),
    人均按 floor/ceil ±0.51 容差;
  - 折扣:`price/original_price*10` ±0.5 容差;`original_price=0` 时任何
    xx折 断言违规(无依据)。
- `RecommendationItem` 补 `min_people`/`max_people` 字段(additive),
  orchestrator 从 deal json 映射(检索服务/catalog 全程携带)。

### 接入点

| 路径 | 违规行为 | compose_mode |
|---|---|---|
| 非流式(`ResponseComposer::Compose` 无 emitter) | 原回复不发出,回退模板 | `template_guard_fallback` |
| 流式(delta 已发出) | 尾部发 `replace` 事件(content=模板回复)整体纠偏 | `llm_stream_guard_fallback` |

两种路径 LLM 调用本身仍记审计,`llm_calls.status=guard_fallback`。
`ResponseComposer` ctor 加可选 `shared_ptr<const SafetyGuard>`(nullptr=
不校验,旧调用点逐字节不变)。

前端 `web/app.js` 处理 `replace`:整体替换气泡文本;未知事件走 default
静默忽略(旧前端兼容)。

### 可观测(阶段 C)

- `recommendation_logs` 幂等加列 `guard_action`(空/refuse_input/sanitized/
  fact_violation)+ `guard_detail`(违规摘要,cap 400 字符)。
- 经既有观测回传通道:`RecommendationResult.guard_action/guard_detail`
  由 composer/orchestrator 填充,外层壳统一写库;store 指针不穿组件签名。
  - BLOCKED(输入拦截)→ `refuse_input`,detail = `risk_type: reason`;
  - 输出脱敏改变了回复文本 → `sanitized`(不掩盖更重的 fact_violation);
  - 事实校验违规 → `fact_violation`,detail = 逐条违规摘要。
- `evaluate.py` 加「Guard 动作」段:PRAGMA 检列旧库跳过;guard_action
  分布、fact_violation 率(>5% 告警)、最近 10 条违规明细。

### 规则外置(阶段 C)

`data/guard_rules.json`:`injection_patterns` / `banned_topics` /
`banned_output_words` / `max_input_chars`。SafetyGuard ctor 可选
`rules_path`(api_server 经 `GUARD_RULES_PATH` 接线,默认
`data/guard_rules.json`):

- 缺文件:静默用内置默认(降级链一致,离线/测试不受影响);
- 文件损坏:spdlog::warn + 回退内置默认;
- 文件存在:出现的键**整体替换**对应内置列表(缺键保留内置)。

内置默认与签入的 guard_rules.json 内容一致,行为逐字节同 Phase 0。
`.gitignore` 由 `data/` 改 `data/*` + `!data/guard_rules.json`(手工配置
入库,生成数据仍忽略)。

### 顺带 1:gRPC 回退可观测(Phase 5 差距#1)

`grpc_retrieval_client.cpp`:Health/RetrieveDeals/RetrieveKb 三处 gRPC 失败
fall-through 处加**限频** `spdlog::warn`(每 60s 一次,带 op 名 + status
code/message);channel 建连加 `GRPC_ARG_MAX_RECONNECT_BACKOFF_MS=2000`,
服务恢复后在一个请求 deadline 内被重新拉起(原默认退避可达分钟级)。
默认 OFF 构建不编译该 TU,行为不变。

## 违规场景实测记录

| 场景 | 方法 | 结果 |
|---|---|---|
| 编造价格(99 元,候选 288) | 单测 `FactViolationFallsBackToTemplate`(FakeLlmClient) | 模板兜底,reply 不含 99,compose_mode=template_guard_fallback,llm_calls status=guard_fallback,guard_action=fact_violation |
| 流式编造价格 | 单测 `StreamingFactViolationEmitsReplace`(FakeStreamingLlm) | delta 已发("只要 99 元!"),尾部 replace 事件带模板,compose_mode=llm_stream_guard_fallback |
| 真实价格/原价、人均/总价派生、折扣口径 | 单测 SafetyGuardFactCheck.*(7 例) | 全放行;区间外派生(五人价)拦截 |
| 注入/违禁 payload 端到端 | e2e S9(scripts/test_injection.json、test_banned.json 原文 POST) | BLOCKED + 礼貌拒绝 + 落库 guard_action=refuse_input、detail 含 risk_type |
| 旧库幂等迁移 | 复制 data/observability.db(旧 schema)→ 新二进制启动 | guard_action/guard_detail 两列 ALTER 成功;旧行 guard_action 读作 none |
| evaluate guard 段 | 迁移后副本跑 evaluate.py | 分布 none=228 / refuse_input=1,与 SQL 实查一致 |
| 规则文件缺失/损坏/覆盖 | 单测 SafetyGuardRules.*(3 例) | 缺失=内置;损坏=内置+warn;覆盖键整体替换且新增词生效 |

## Guard 审计复算方法

```sql
-- 某条回复为何被 guard 介入
SELECT guard_action, guard_detail, compose_mode, response_text
FROM recommendation_logs WHERE trace_id = '<trace_id>';

-- 事实违规率
SELECT COUNT(*) FILTER (WHERE guard_action='fact_violation') * 1.0 / COUNT(*)
FROM recommendation_logs;
```

或直接 `python -X utf8 scripts/evaluate.py` 看「Guard 动作」段。流式违规的
原始(被纠偏)文本在客户端 delta 里出现过,服务端只保留模板兜底文本;
违规摘要(detail)含被拦截的断言值,可对照候选集 candidates_json 复核。

## 回归证据

- 单测 129/129(阶段 A +11、B +2、C +3 例;1 SKIP live-PG)。
- e2e 离线 80/80(67 基线 + S9 13 项);stub 模板输出零误杀。
- gRPC 变体(build-grpc,ENABLE_GRPC=ON)增量构建通过。
