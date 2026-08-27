# 验收报告:Phase 4 阶段 C(Guard 可观测 + 规则外置)+ D(e2e 扩展 + 收口)— 暨 Phase 4 整阶段收口

- **结论**:✅ 通过
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`97ba002`(阶段 B 已验收);验收对象 = `953c826`(C)+ `5f6df4a`(D)
- **对应 plan**:`phase4-output-fact-guard.md` 阶段 C、D
- **涉及文件**:`safety_guard.{hpp,cpp}`、`observability_store.{hpp,cpp}`、`data/guard_rules.json`、`scripts/evaluate.py`、`scripts/e2e_multi_turn.py`、`src/tools/grpc_retrieval_client.cpp`、`docs/phase4_guard.md`

## 规划层核对

| 验收点 | 结果 | 证据 |
|---|---|---|
| C:`guard_action`/`guard_detail` 幂等加列 | ✅ | 沿用 add_column_if_missing;**旧库实证**:启动前无两列,boot api_server 后自动获得 |
| C:观测回传通道模式 | ✅ | orchestrator 统一写库,store 指针未穿组件签名 |
| C:evaluate.py Guard 段 | ✅ | 实跑输出「guard_action 分布: none=228、fact_violation 0.0%」;PRAGMA 检列旧库跳过 |
| C:规则外置缺文件 = 内置默认逐字节 | ✅(实证) | 移走 guard_rules.json 起服务,注入 payload 仍被内置规则 BLOCKED 礼貌拒绝;文件已还原 |
| C:rules 文件增词生效 | ✅(单测覆盖) | test_safety_guard.cpp +57 含规则加载用例 |
| D:e2e 纳入注入/违禁 payload | ✅ | S9 段 13 项新检查(含 guard_action/guard_detail 落库断言),**80/80 全过**(67→80) |
| D:README 口径修正 | ✅ | 流式 tokens 口径同步 Phase 2.3-A 现状 |
| D:docs/phase4_guard.md 收口 | ✅ | 113 行:违规场景实测、审计复算方法 |

**附带闭环**:Phase 5 D/E 报告差距#1(gRPC 回退静默)→ `5f6df4a` 中 grpc_retrieval_client.cpp +39 行实现限频 warn,我提的整改建议已落实。

## 契约层核对

- 降级链:✅ 规则外置缺文件回退内置默认,与全项目降级哲学一致
- SSE/检索/排行契约:✅ 未触碰
- 观测加列:✅ 幂等模式第三轮实证(2.1-A、2.3-D、4-C)

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| build_windows.ps1 + CTest | ✅ 129/129 |
| e2e 离线 | ✅ **80/80**(注入/违禁新检查全过) |
| 旧库迁移 | ✅ guard 两列 boot 后自动获得 |
| 缺规则文件行为 | ✅ 注入仍被内置默认拦截 |
| evaluate.py Guard 段 | ✅ 输出与库实查一致 |

## 差距清单

无。

## Phase 4 整阶段收口意见

A(非流式 FactCheck)→ B(流式 replace)→ C(可观测 + 规则外置)→ D(e2e + 收口)全部通过。「价格/折扣必须来自数据库」交易红线闭环:compose 软约束 + 输出硬核验 + 全量审计可复算。项目规划 1.1「合规与兜底」的输出侧空缺已填补(输入侧 SafetyGuard 为 Phase 0 存量)。

## 下一阶段建议

1. 规划层面仅剩:PolicyEngine 深度化(LLM 级二次审查,plan 评估为演示项目非必需)、user_profiles→PG 迁移、RANKER_MODE=active 真实验——三者均为"可选深化"而非"空缺",项目主体需求已全覆盖。
2. Phase 2 的 ⏸数据不足判定仍待真实 feedback 积累解除,这是唯一悬而未决的效果类目标。
