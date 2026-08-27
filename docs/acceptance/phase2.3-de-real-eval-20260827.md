# 验收报告:Phase 2.3-D 真模型验证 + 2.3-E 效果评估(暨 Phase 2.3 整阶段收口)

- **结论**:✅ 通过(Phase 2.3 全部五个阶段收口;Phase 2 效果判定维持 ⏸数据不足,依据充分)
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`ea72876`(2.3-C 已验收);验收对象 = commit `c0dd45c`(阶段 D)+ `6f00f05`(阶段 E)
- **对应 plan**:`phase2.3-ranker-loop-observability.md`(v2)阶段 D、E
- **涉及文件**:`include/agent/common.hpp`、`include/agent/observability_store.hpp`、`src/observability/observability_store.cpp`、`src/agent/{agent_orchestrator.cpp,response_composer.cpp,task_planner.cpp}`、`scripts/e2e_multi_turn.py`、`docs/phase2_evaluation.md`

## 规划层核对

| 验收点 | 结果 | 证据 |
|---|---|---|
| D:`--real` 7 项语义检查 | ✅(独立复跑) | 网关 real-llm(deepseek-v4-flash)+ 检索 PG/vector 起栈,**7/7 PASS** |
| D:画像注入对照实证落 llm_calls | ✅ | `llm_calls.raw_request` 幂等加列(`add_column_if_missing`,INSERT 10→11);6 行实存,含画像段恰好 1 行(老用户),其余 5 行无画像段;画像段位置在 `# 当前已填充槽位` 之前、含「当轮显式输入永远优先」——SQL 实查确认 |
| D:usage 可选字段契约 | ✅ | raw_request 仅新增审计列,retrieval/ranking 端点形状未动 |
| E:`docs/phase2_evaluation.md` 落盘 | ✅ | 98 行,含判定、口径说明、shadow 证据链、复算方法 |
| E:数据可复算 | ✅(逐项抽核) | rank_mode rule=166 ✅;llm_calls success=45/template_fallback=15 ✅;experiment_group control=6/treatment=6 ✅;真实空推荐率 5/5(巡检#4 已证)✅ |
| E:判定结论有明确依据 | ✅ | ⏸数据不足,缺口量化到行(6/100 样本、1/10 正样本、0/30 分组反馈);诚实标注 top-3 一致率"不构成效果证据"及原因——评审口径严谨,未粉饰 |
| E:Phase 3 候选方向 | ✅ | 4 项候选 + 数据积累建议,供用户裁决 |

## 契约层核对

- 降级链 ✅;幂等迁移模式扩展到 llm_calls(与 recommendation_logs 同一 `add_column_if_missing` 助手);stdlib 策略 ✅(e2e 修复只用 codecs)
- **附带确认**:2.3-BC 报告差距#1(e2e 一次性 NameError)已由 `c0dd45c` 修复——SSE 读取改增量 UTF-8 解码(多字节字符跨 4096 字节边界),本次 e2e 离线 + --real 两轮均零 flake

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| `build_windows.ps1` + CTest | ✅ 95/95(C++ 有改动,重跑了) |
| `RANKER_MODE=off` e2e 离线 | ✅ 51/51 |
| `RANKER_MODE=off` e2e `--real`(真网关 + PG 检索) | ✅ **7/7 PASS**(真实 DeepSeek 调用,22 行 deepseek-v4-flash 落库为证) |
| SQL 抽核评估报告数字 | ✅ 全部一致(见上表) |
| 清场 | ✅ 8000/8001/8002/8080 验收后均已释放 |

## 差距清单

无新增差距。前序报告的差距#1(e2e flake)已修复并实证;差距#2(commit 编号)在 D/E 两个 commit 已对齐「阶段D(v2)/阶段E(v2)」规范——**前序差距全部闭环**。

## Phase 2.3 整阶段收口意见

五阶段全部验收通过:A(观测口径)→ B(模拟链路,真实数据合法出口)→ C(shadow 灰度)→ D(真模型)→ E(评估报告)。Phase 2 的「优于规则基线 10%+」判定为 **⏸数据不足**,依据充分、缺口量化、解除路径明确(20+ 轮真实会话 + like/dislike)。

## 下一阶段建议(供规划窗口/用户裁决)

1. Phase 3 候选按评估报告第 6 节裁决;从验收视角看,**召回缺口修复**(真实空推荐率 100%,深圳/早茶、武汉/汉堡)是真实流量下最优先的效果瓶颈,PolicyEngine 是唯一完全空缺的需求项。
2. 若继续积累真实数据,每攒一批重跑 evaluate.py + train_ranker.py 即可,阈值机制已就绪。
