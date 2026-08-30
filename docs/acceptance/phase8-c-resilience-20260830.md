# 验收报告:Phase 8-C 服务韧性 + shadow 异步化 + guard 金额豁免

- **结论**:✅ 通过(附 2 条低危观察项)
- **验收日期**:2026-08-30
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`da6f5da`(Phase 8-B 已验收);验收对象 = commit `e508dc3`「Phase 8-C」(+`403a45c` 验收报告入库,纯 docs)
- **对应 plan**:`phase8-latency-coarse-fine-rank.md` 阶段 C;修复项范围 = 8-B 报告 §5 裁决的 4 项(健康检查负缓存、shadow 异步、死服务快速失败、guard 金额豁免),commit message 载明"用户裁决锁定 4 项"——立项链完整
- **涉及文件**:`service_circuit.hpp`(新)、retrieval/ranker 双客户端、`deal_tools.cpp`、`safety_guard.{hpp,cpp}`、`response_composer.cpp`、`guard_rules.json`、测试 ×4 文件、文档 ×3

## 规划层核对(plan 阶段 C 验收点逐项)

| 验收点 | 结果 | 证据 |
|---|---|---|
| 每个修复项前后对比数据(同口径) | ✅ | commit 载明 前=retriever 4086ms+ranker 2027ms → 后=首轮 4053ms/次轮 35+0ms;本窗口独立复测(见实证层)方向一致且更优 |
| 降级链逐条不破 | ✅(实证) | 双服务全挂下 retrieve 动作成功,本地 catalog 兜底出真实商品(gb-20001/20003);熔断快速失败落入与原慢失败**同一**兜底路径 |
| e2e 80/80 + 单测全过 + 24+24 矩阵零回归 | ✅(本窗口独立实跑) | 单测 **137/137**(+8:ServiceCircuit×6、guard 白名单×2、shadow 测试重写异步语义;含 live-PG);e2e **80/80**;矩阵 **24/24 + 24/24**(deals=9713) |
| 健康检查负缓存(8-B 裁决#1,成败同缓存) | ✅ | `ServiceCircuit` TTL 30s 成败同缓存;实证:启动期一次性探测后,热路径零网络探测 |
| shadow fire-and-forget(8-B 裁决#3) | ✅(实证,铁证) | detached 线程;日志时序:22:29:22.708 响应已发出 → 22:29:24.709 shadow 的 2.05s 拒绝才在后台落地——**2s 拒绝税完全移出热路径**;`rank_audit.shadow_async=true` 诚实标注 model_score 不再产出(8-B 已声明的代价) |
| 死服务快速失败(新增#6,已立项) | ✅ | 连败 2 次熔断 30s + 半开恢复;AllowRequest=false 时零网络直入兜底 |
| guard 金额豁免(新增#7,已立项) | ✅ | `fact_check_whitelist_user_amounts` 规则文件门控;先查候选集再查白名单(纯放宽不误放);单测 ×2 覆盖 |

## 契约层核对

- **降级链**:✅ 熔断/负缓存只省网络税,兜底语义逐字节不变(实证:死服务下本地 catalog 出单)
- **无文件行为不变**:✅ 白名单默认 off;`FactCheckReply` 新参 `user_text` 带默认值,旧调用点零改动
- **审计语义诚实**:✅ shadow 异步的 model_score 缺失用 `shadow_async=true` 显式标注,非静默
- **检索端点形状 / user_id 不可见 / 空画像 prompt**:✅ 均未触碰
- **8-B 差距#1(文档措辞)**:✅ 执行窗口已修订("两天 11 例,剔除 p7-smoke/context 预算"),闭环

## 实证层(本窗口独立实跑)

| 项目 | 结果 |
|---|---|
| build + test_agent.exe(含 live-PG) | ✅ 137/137 |
| e2e 离线 | ✅ 80/80 |
| test_pg_retrieval.py | ✅ 24/24 + 24/24 |
| **死服务场景复测**(stub 模式,8001/8002 空置) | ✅ retrieve 轮 121ms 全程:retriever 34ms(本地 catalog 出真实商品)+ ranker 0ms;shadow 拒绝税 2.05s 在响应发出后由后台线程承担(日志时序铁证) |
| 拒绝成本平台事实复测 | ✅ 裸 socket 实测 127.0.0.1=2.05s/次、localhost=4.1s(双栈两段)——B 报告数字可复现,8-C 动机成立 |

**附带发现(非差距)**:8-A 验收时我观察到的"planner_llm=4106ms"实为 `LLM_BASE_URL` **unset→默认指向 :8000 死网关**的 2 次拒绝税(2×2.05s),不是 stub 耗时——stub 模式必须显式 `LLM_BASE_URL=`(空串,main.cpp:152 unset≠空)。本机实测 clarify 轮 stub 仅 99ms。此发现反向证明 8-C 熔断对 LLM 客户端之外的路径价值;LLM 客户端同款死服务税是否在 Phase 9 范围,建议规划窗口知悉。

## 差距清单

| # | 现象 | 整改建议 | 严重度 |
|---|---|---|---|
| 1 | `guard_rules.json` 的 `_comment` 仍称"缺文件时内置默认内容与本文件一致(行为逐字节不变)",但白名单键使文件/内置**不再一致**(文件 on/内置 off)——hpp 注释正确,json 注释漏改 | 执行窗口顺手修订该句(如"白名单键例外:缺文件时默认关") | 低(文档) |
| 2 | `service_circuit.hpp` 注释"a single half-open trial",实现实为 cooldown 后放行全部并发请求(直到首个 Report 落地) | 注释改实话;本规模无实际影响 | 低(注释) |

## 下一阶段建议

1. Phase 8 仅剩阶段 E(收口:全量回归 + phase9_summary + 题库条目),内容已基本就绪,可与 Phase 9 合并收口以省一轮仪式。
2. **Phase 9(planner 瘦身)已具最高优先级数据支撑**:8-B 实测 planner 占 56.8%,且本次发现 LLM 客户端在网关缺席时也付 2×2.05s 拒绝税——phase9-planner-latency.md(草稿)阶段 C(模型/参数调优)之外,建议把"网关缺席时 planner 快速失败/熔断"纳入勘察。
