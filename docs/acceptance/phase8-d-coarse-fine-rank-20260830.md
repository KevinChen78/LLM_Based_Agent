# 验收报告:Phase 8-D 粗排/精排显式拆分

- **结论**:✅ 通过(附 1 项用户裁决待办——plan 明示的行为变化点)
- **验收日期**:2026-08-30
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`ae94181`(Phase 8-A 已验收);验收对象 = commit `72e1546`「Phase 8-D: 粗排/精排显式拆分」
- **对应 plan**:`phase8-latency-coarse-fine-rank.md` 阶段 D
- **涉及文件**:`src/tools/deal_tools.cpp`、`src/llm/prompt_builder.cpp`、`ranking_service/features.py`、CLAUDE.md、docs/architecture_vs_plan.md、面试题库

## 规划层核对(plan 阶段 D 验收点逐项)

| 验收点 | 结果 | 证据 |
|---|---|---|
| top_k 20→100:schema 默认值 + C++ 调用点 + prompt 示例三处同步 | ✅ | diff 三处一致(deal_tools SchemaJson/Execute 默认与兜底、prompt_builder 示例);服务端 pool_n=max(top_k*4,40) 随动 |
| 检索端点响应形状不变 | ✅ | 矩阵实测 24+24 全同(见实证层);top_k 是请求参数非响应契约 |
| 粗排层:taboo 剔除位置不变(永远 C++ 侧最先)→ 规则分排序 → 截 kCoarseTopK=50 | ✅ | diff 顺序正确;stable_sort 保持稳定性 |
| 精排候选 = 粗排序前 50;rank_in_rules 语义=粗排名次,特征值不变免重训 | ✅ | model_candidates 改自 scored(截断后 ≤50);features.py 仅注释对齐,FEATURE_NAMES 未动(meta.json 防漂移校验不受触) |
| off/挂/无模型 → 粗排名次截 top_n,与旧规则分兜底语义等价 | ✅ | 同候选集内路径逐字节一致:e2e 离线 80 项确定性断言全过(stub top_k=20 未动,候选集不变);代码层 fallback 分支不再重排,直接用粗排顺 |
| shadow 日志 candidates_json ≤50 | ✅(代码级结构性保证) | 截断(resize 50)先于 candidates 组装,上界硬保证;shadow 通道本身在 Phase 2.3-B 已实证 |
| **行为变化点:off 模式真实流量 top_n 可能变化 → diff 清单 + 用户裁决** | ⏳ **清单已产出(commit message 载明 4 查询实测:深圳火锅/深圳甜品/武汉小龙虾变化,上海海鲜一致),用户裁决待进行** | 这是本阶段唯一允许的行为变化,plan 要求用户拍板 |
| e2e/单测同步后全过;矩阵零回归 | ✅(实证) | 见实证层 |
| 文档:CLAUDE.md 两级排序段 / architecture_vs_plan / 题库条目 | ✅ | CLAUDE.md L86-89 两级排序段在;题库 14.8/14.9 在(197→199 问);architecture_vs_plan +1 行 |

## 契约层核对

- **降级链**:✅ off→粗排截 top_n(等价旧兜底);服务挂→同路径;精排不触发即无模型调用;taboo 剔除铁律位置未动
- **检索端点形状**:✅ 未动(矩阵逐字节证明)
- **特征契约**:✅ FEATURE_NAMES 零改动,仅注释;免重训声明成立
- **内置 stub top_k=20 不动**:✅ 离线 e2e 确定性保护到位(commit 明示,80/80 反证)

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| build_windows.ps1 + test_agent.exe(含 live-PG) | ✅ **129/129** |
| e2e_multi_turn.py 离线 | ✅ **80/80**(首次 67/80 为端口竞争污染,清栈后全绿,见差距清单#1) |
| test_pg_retrieval.py | ✅ 后端 **24/24**(deals=9713)+ 协议(gRPC≡HTTP)**24/24** |
| 文档同步抽查 | ✅ CLAUDE.md/题库条目均在 |

## 差距清单

| # | 现象 | 整改建议 | 严重度 |
|---|---|---|---|
| 1 | 验收中 8001 有两个监听进程(SO_REUSEADDR 双绑);且**本窗口误将用户正在使用的全栈当作执行窗口遗留进程 taskkill**,造成 8080 竞争与一次 67/80 污染跑(清栈复跑 80/80,非代码问题)——用户 2026-08-30 指正:被杀的是其在使用中的服务 | PM 窗口流程整改(见本报告备注);重复监听现象本身仍建议启动前查端口 | **中(PM 流程错误,已认领)** |
| 2 | plan 阶段 D 唯一行为变化点(召回池 20→100 下 off 模式 top_n 变化)diff 清单已在 commit message/完成汇报给出,**用户裁决尚未发生** | 用户在执行窗口汇报中确认接受与否;接受则阶段 D 完全关闭 | 待办(用户侧) |

## 下一阶段建议

1. 阶段 B(实测归因)所需埋点(8-A)与排序架构(8-D)均已就位,建议用户按正常演示节奏跑 10~20 轮真实会话后,执行窗口出分阶段占比 + Top3 归因报告。
2. 阶段 C 优化项动手术前,先关闭差距#2 的用户裁决,避免行为基线在优化前后二次漂移。
