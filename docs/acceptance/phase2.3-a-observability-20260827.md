# 验收报告:Phase 2.3-A 观测口径修复(空推荐率拆口径 + 流式 compose usage)

- **结论**:✅ 通过(附 2 条非阻塞观察)
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`b50cd9a`(Phase 2.1/2.2 已验收收口);验收对象 = commit `a7a3bef`
- **对应 plan**:`C:\Users\Kevin Chen\.claude\plans\phase2.3-ranker-loop-observability.md`(草稿 v2)阶段 A
- **涉及文件**:`scripts/evaluate.py`、`llm_gateway/main.py`、`src/llm/llm_client.cpp`、`include/agent/llm_client.hpp`、`src/agent/response_composer.cpp`

## 规划层核对

| 验收点(plan 阶段 A) | 结果 | 证据 |
|---|---|---|
| A1 空推荐率按流量来源拆分 | ✅ 且超预期:拆三档(真实/测试stub/模拟 sim-),比 plan 的两档多一档,正好呼应阶段 B2 的 sim- 前缀硬约束;真实口径 >20% 时输出召回缺口清单(只暴露不修召回) | evaluate.py 实测输出:真实 100%(5/5)、测试 0%(0/7)、模拟 0%(0/150),缺口清单列出 [深圳/早茶]×3、[武汉/汉堡]×2 |
| A2 网关加 stream_options.include_usage | ✅ | llm_gateway/main.py:转发体含 `stream_options:{include_usage:true}`,注释明确上游不返则保持记 0 |
| A2 网关 stub 流式末块补 usage | ✅ | stub_stream 末块 `choices:[]` + usage(字符数估算,沿用非流式口径) |
| A2 C++ SSE 解析 usage 块入 LlmCallInfo | ✅ | HandleSseEvent 识别 usage 对象,经 LlmStreamResult 新字段 → LlmCallInfo;全链路实证见下 |
| 契约注意:retrieval/ranking 端点形状不动 | ✅ | diff 未触碰;usage 为可选字段,缺失时诚实记 0 |

## 契约层核对

- **降级链**:✅ 未触碰
- **诚实记录原则**:✅ 上游不返 usage → 记 0 而非伪造,代码与注释一致
- **stdlib 策略**:✅ evaluate.py / llm_gateway 新增代码纯 stdlib
- **JSON 契约形状**:✅ 只新增可选 usage 块,不改既有字段

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| `build_windows.ps1` | ✅ 构建成功,CTest **95/95** 通过(DealCatalogPg live 按预期 SKIP) |
| `RANKER_MODE=off python scripts/e2e_multi_turn.py` | ✅ **51/51 ALL PASS** |
| `python -X utf8 scripts/evaluate.py` | ✅ 空推荐率三档拆分输出 + 召回缺口清单 |
| 全链路 usage 实证:起 llm_gateway(stub)+ api_server,POST /v1/chat/stream 一轮,查 llm_calls | ✅ 新 compose 行 tokens=652+288(此前恒 0),历史行为 0 不受影响 |
| 端口/进程清场 | ✅ 验收前 8080/8002/8000 均空闲,验收后已杀净 |

## 差距清单

| # | 现象 | 位置 | 整改建议 | 严重度 |
|---|---|---|---|---|
| 1 | evaluate.py 页脚「流式 compose 行 tokens 记 0…」为无条件静态输出,新数据 tokens 已非 0,注释与现实矛盾会误导读报告的人 | scripts/evaluate.py | 改为条件输出:仅当 compose 行 tokens 全为 0 时打印该说明 | 低 |
| 2 | commit message 标「Phase 2.3-B」,而 plan v2 已将观测口径修复重排为阶段 A(B 是数据积累) | commit a7a3bef | 无需改历史 commit;后续 commit 对齐 plan v2 编号即可 | 低 |

另注:`scripts/simulate_feedback.py`(未跟踪)已存在且库中已有 150 行 sim- 数据——属阶段 B2 进行中,不在本次验收范围;evaluate.py 已正确将 sim 数据隔离出真实口径(实证:0/150 单列),"不污染默认口径"约束当前成立。

## 下一阶段建议

阶段 B(真实数据积累)依赖用户手工跑 20+ 轮会话,B2 模拟链路已在推进。建议规划窗口考虑:B 的"数据未攒够"合法出口若触发,是否直接转 shadow(C 阶段用规则分影子审计也有价值),避免流水线空转。
