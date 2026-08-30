# 验收报告:Phase 8-A 分阶段延迟埋点

- **结论**:✅ 通过
- **验收日期**:2026-08-30
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`c8560c1`(gitignore chore);验收对象 = commit `ae94181`「Phase 8-A: 分阶段延迟埋点」
- **对应 plan**:`phase8-latency-coarse-fine-rank.md`(v1.1,用户 2026-08-28 确认)阶段 A
- **涉及文件**:`include/agent/agent_orchestrator.hpp`、`include/agent/observability_store.hpp`、`src/agent/agent_orchestrator.cpp`、`src/observability/observability_store.cpp`、`scripts/evaluate.py`
- **工作区另有未提交的 Phase 8-D 预备改动**(top_k 100/粗排截 50/features.py 注释),本次不在验收范围,已做代码级初审(见差距清单外说明)

## 规划层核对(plan 阶段 A 验收点逐项)

| 验收点 | 结果 | 证据 |
|---|---|---|
| 新列幂等(重跑升级不炸) | ✅(实证) | 在**既有旧 schema** data/observability.db 上启动 api_server:列自动补齐、会话正常;同一已迁移库**二次启动** health 200 无异常 |
| 一轮真实会话后 stage_ms_json 字段齐全 | ✅(实证) | 实测一行:session_load/input_guard/history_load/profile_resolve/planner_llm/compose_total/guard_post/session_save 全在;planner_attempts=1 |
| 总和≈端到端(误差<5%) | ✅(实证) | stage 合计 4206ms vs latency_ms 4208ms,**覆盖率 100.0%** |
| evaluate.py 分阶段表 | ✅(实证) | 「分阶段延迟」段按 compose_mode 分组,各 stage p50/p95/max + planner_attempts 次数 + 埋点覆盖率行(avg 100.0%) |
| e2e/单测零回归(纯 additive) | ✅(实证) | 单测 **129/129**(含 live-PG 4/4,DSN 隐式加载);e2e 离线 **80/80** |
| 计时范围:plan 列举阶段全覆盖 | ✅ | 工具循环逐工具 {name,ms};compose_first_token 流式首 token(模板/短路模式字段诚实缺席,commit 与注释均声明);异常路径也 flush |

## 契约层核对

- **观测回传通道模式**:✅ `RecAudit.stage_ms_json` 回传、外层壳统一写库,组件签名零改动,与 Plan.llm_calls 先例一致
- **审计写不破坏推荐路径**:✅ insert 失败仅 warn(既有纪律未动);列数 18→19 与 bind 序号同步正确
- **additive 语义**:✅ 旧行空值,头文件注释明示;PRAGMA 检列与 guard_action 先例同模式
- 检索端点形状/user_id 不可见/空画像 prompt:✅ 均未触碰
- StageTimer 用 steady_clock 本地读数,探针开销 ns 级;FirstTokenProbe 对 emitter 逐字节透传

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| build_windows.ps1 + test_agent.exe | ✅ 129/129(128 + live-PG 4/4) |
| e2e_multi_turn.py(离线) | ✅ 80/80 |
| 旧库迁移 + 真实会话打点 | ✅ 列自动补齐;字段齐全;覆盖率 100.0% |
| 已迁移库二次启动 | ✅ health 200,幂等 |
| evaluate.py 分阶段段 | ✅ 输出如上表 |

## 差距清单

| # | 现象 | 整改建议 | 严重度 |
|---|---|---|---|
| 1 | 短路模式实测 planner_llm=4106ms(stub 规划器?)占端到端 97.6%——这正是阶段 B 要归因的延迟大头,埋点首战即立功 | 非缺陷;阶段 B 报告应解释该 4.1s 构成 | 观察项 |

**范围外说明(Phase 8-D 预备,未 commit)**:工作区 diff(top_k 20→100 三处同步、kCoarseTopK=50 粗排截断、features.py rank_in_rules 注释对齐)代码级初审**符合 plan 阶段 D**;`out["total"]=pre_coarse` 语义变化有注释、taboo 剔除仍在最前、模型候选集 = 粗排截断后 ≤50 均与 plan 一致。待 commit 后按阶段 D 验收点实跑(重点:RANKER_MODE=off 逐字节对比或 diff 清单裁决)。

## 下一阶段建议

1. 阶段 A 埋点数据通道已就绪,可按 plan 进入阶段 B:用户正常演示节奏跑 10~20 轮真实会话,执行窗口出分阶段占比 + Top3 归因报告。
2. 观察项#1 提示:即使是 stub/短链路,planner 环节 4.1s 已是体验瓶颈候选,阶段 B 应优先量化其构成(LLM 本身 vs 健康检查 vs 其他串行项)。
