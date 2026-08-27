# 验收报告:Phase 3 召回缺口修复(阶段 A~D 整阶段)

- **结论**:✅ 通过
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`6f00f05`(Phase 2.3 收口);验收对象 = `8b60c2c`(A 词表)/ `9738184`(B 数据)/ `88de0c2`(C 放宽链)/ `3d17b97`(D 重放收口)
- **对应 plan**:`phase3-recall-gap-fix.md` 阶段 A~D
- **关键文件**:prompt_builder.cpp、deal_catalog.{hpp,cpp}、gen_{wuhan,city}_deals.py、retrieval_service/main.py、deal_tools.cpp、agent_orchestrator.cpp、tests/{test_category_vocab.cpp,test_kb.cpp}、docs/phase3_recall_fix.md

## 规划层核对

| 阶段 | 验收点 | 结果 |
|---|---|---|
| A | DistinctCategories 三源可用;prompt 注入白名单 + 规则收紧;空列表逐字节不变;画像段兼容;类目段单测 | ✅ 全部命中(巡检#12 代码审 + 单测 4 用例:test_category_vocab.cpp) |
| B | 生成器只增不改、幂等;PG 行数==deals.json;embedding 全覆盖;双后端矩阵;5320 硬编码同步 | ✅ 实证:连跑两次 md5 逐字节一致(dd461109…);矩阵自报 deals=5320 双侧;PG 实查 5320 行/embedding 5320 全覆盖;汉堡 446 行 |
| C | 三级放宽双后端单点共享;relaxed_level 只增不改;别名仅 0 命中后启用;向量通道同口径 | ✅ 实证:L0 响应无 relaxed_level 键(逐字节同前);L1 早茶→粤菜 15 行、汉堡→西餐 868 行;L2 不存在类目→去类目 5000 行;矩阵 24/24 |
| D | 5 条空推荐重放非空;evaluate 口径可复算;对照文档落盘 | ✅ 文档 5/5 非空表 + 四个重放暴露问题的修复记录;evaluate 实跑 24.1%(7/29) 与文档一致,分子 7 条确为修复前历史日志(缺口清单内容未变) |

**D 阶段重放暴露的 4 个问题修复质量复核**(clarify 死循环/null 参数崩溃/「没有要求」误识别/max_price=0 过滤一切):修法均在正确层——prompt 规则条件化在 prompt 层、null 容忍在工具边界、max_price 归一在 C++ 工具侧而**不动检索服务契约语义**,每层都有对位单测(NullArgsTolerated/ZeroMaxPriceMeansNoLimit/clarify 断言)。符合"单层失效仍有下层兜住"的设计原则。

## 契约层核对

- **检索端点形状**:✅ 只增不改,且比 plan 要求更严(level 0 响应不加键,逐字节同前)
- **降级链**:✅ 未触碰;e2e 51/51 全过
- **stdlib 策略**:✅ 生成器/e2e 无新依赖
- **prompt 兼容性**:✅ 无白名单/无画像逐字节同前(单测断言)

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| build_windows.ps1 + CTest | ✅ **103/103**(95→99→103,PG live 1 SKIP 按预期) |
| e2e 离线(RANKER_MODE=off) | ✅ **51/51** |
| 生成器幂等 | ✅ 两生成器连跑两次,data/deals.json md5 三次一致 |
| PG 实查 | ✅ 5320 行 == deals.json;embedding 5320/5320;汉堡 446 行 |
| test_pg_retrieval.py | ✅ **24/24 双后端一致**(含 relaxed_level/effective_category 跨后端比较) |
| 放宽链 live 实测(起 retrieval_service) | ✅ L0/L1/L2 三级全部按预期(见上) |
| evaluate.py 口径复算 | ✅ 真实 24.1%(7/29),与 docs/phase3_recall_fix.md §5 一致 |

## 差距清单

无阻塞差距。一条说明:真实口径空推荐率 24.1% 仍 >20% 阈值,缺口清单仍显示 7 条——**这口径正确而非回归**:分子全是修复前历史日志(不可回溯改写),新增 5 条重放全部非空只增大分母。该指标将随新真实流量继续下降,无需整改。

另注:执行窗口遗留的临时脚本 `scripts/_tmp_phase3d_replay.py` 未随阶段 D 提交(工作区曾见,当前已干净)——若后续需要复跑重放,建议要么纳入 scripts/ 要么明确删除,不留无主文件。

## 下一阶段建议(供规划窗口/用户裁决)

1. Phase 3 目标达成,真实流量召回缺口已闭环。按评估报告候选列表,下一里程碑首选 **PolicyEngine / InputGuard / OutputGuard**(项目规划 1.1 合规与兜底 + 2.3 policy 模块,唯一完全空缺的需求项)。
2. 召回修复后 feedback 积累通路已打通(空推荐不再阻断 like/dislike),Phase 2 的 ⏸数据不足 判定有了可持续的解除路径——可考虑把"真实数据积累"作为日常动作而非独立阶段。
