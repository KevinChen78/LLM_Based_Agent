# 验收报告:Phase 2.3-B 模拟反馈链路 + 2.3-C shadow 灰度

- **结论**:✅ 通过(阶段 B 按 plan 合法出口"真实数据未攒够"收口;附 2 条观察)
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`a7a3bef`(Phase 2.3-A 已验收);验收对象 = commit `0b63be7`(模拟链路 + sim 隔离)+ `ea72876`(shadow checklist)
- **对应 plan**:`phase2.3-ranker-loop-observability.md`(v2)阶段 B、C
- **涉及文件**:`scripts/{simulate_feedback.py,build_features.py,train_ranker.py,evaluate.py}`、`docs/ranker_shadow_checklist.md`

## 规划层核对

| 验收点 | 结果 | 证据 |
|---|---|---|
| B2 模拟脚本纯 stdlib、确定性、写 sessions/observability 两库 | ✅ | simulate_feedback.py 176 行,仅 stdlib import;库中 150 请求/302 反馈 sim- 行实存 |
| B2 硬约束:sim 前缀 + 双向默认排除 + --include-sim 显式开关 | ✅ | build_features/train_ranker 均有 `--include-sim`;evaluate.py 三口径分组(real/test/sim) |
| B3 真实数据不足 → 拒绝产模型(合法出口) | ✅ | `train_ranker.py` 默认跑:samples=0,**exit=2**,输出明确 "no model written",model.txt mtime 前后不变(03:01 未覆盖) |
| B 验收点:模拟数据不污染默认口径(三组对照) | ✅ | 默认 build_features → item_features **7 行**(commit 报告 --include-sim 时 805);默认 train 拒绝;evaluate sim 单列 0/150 不进真实指标 |
| B 验收点:health feature_rows 与 item_features 一致 | ✅ | 实测 health `feature_rows=7` == DB 实查 7 |
| C1 checklist 落盘且可复现 | ✅ | docs/ranker_shadow_checklist.md 89 行:起栈步骤、审计字段核对、双分数对比、**分级回滚**(第 4 节,off 完全关闭) |
| C2 shadow 跑栈审计(执行窗口自证) | ✅(采信+旁证) | commit 报告 experiment_group control=6/treatment=6、rank_mode 全 rule、candidates_json 双分数 12/12;旁证:health model_loaded=true(sim 冒烟模型,meta includes_sim=true,仅验链路) |
| C3 shadow 无 ranking_service 降级:e2e 51/51 | ✅(独立复跑) | `RANKER_MODE=shadow`、8002 无监听,**51/51 ALL PASS** |

## 契约层核对

- 降级链 ✅(C3 实证:shadow 模式下服务缺席不破坏任何 e2e 检查);检索端点形状未动;stdlib 策略 ✅(本轮无新 pip 依赖);诚实记录原则 ✅(数据不足拒产模型而非强产)

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| `RANKER_MODE=shadow` e2e(无 ranking_service) | ✅ 51/51(首次运行出现一次性 NameError,见差距清单#1) |
| `train_ranker.py`(默认,真实数据) | ✅ exit=2 拒绝,model.txt 未被覆盖 |
| `PRAGMA`/count 实查 ranking_features.db | ✅ item_features=7(默认口径) |
| 起 ranking_service 实测 /v1/health | ✅ feature_rows=7 与 DB 一致,model_loaded=true(sim 冒烟模型) |
| C++ 构建/单测 | 未重跑:两 commit 仅触 Python 脚本与文档,无 C++ 改动,沿用 a7a3bef 验收时的 95/95 |

## 差距清单

| # | 现象 | 整改建议 | 严重度 |
|---|---|---|---|
| 1 | C3 首次 e2e 运行在 S2 段报 `error: NameError("name 'codecs' is not defined")` 后退出,随后两次重跑均 51/51 通过。e2e 脚本第 29 行已有 `import codecs`,疑似错误来自子进程 stderr 转播或某条罕触发路径,**根因未定** | 执行窗口定位 NameError 真实来源(一次性失败不可复现≠不存在);若属启动竞态,错误处理路径需修 | 中( flake 会侵蚀 e2e 作为回归门的可信度) |
| 2 | commit 编号再次错位:`0b63be7` 标「Phase 2.3-C」但内容是 v2 的阶段 B(模拟链路);`ea72876` 已改标「阶段C(v2)」 | 后续 commit 统一对齐 plan v2 编号(同 phase2.3-a 报告差距#2,延续观察) | 低 |

## 下一阶段建议

1. 阶段 B 真实数据出口已触发"数据未攒够"——20+ 轮真实会话的用户手工动作是解除阻塞的唯一途径,建议用户安排或明确以 sim 链路证据收尾 Phase 2.3。
2. 阶段 D(--real e2e)与 E(Phase 2 效果评估)待推进;E 的「优于规则基线 10%+」判定在真实数据缺席下大概率落 ⏸数据不足,建议规划窗口预先准备该结论的措辞与后续选项。
