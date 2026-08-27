# 验收报告:Phase 4 阶段 A(非流式事实校验)+ B(流式 replace 纠偏)

- **结论**:✅ 通过
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`f2fa3bb`(Phase 5 收口);验收对象 = `7435f06`(A)+ `97ba002`(B)
- **对应 plan**:`phase4-output-fact-guard.md` 阶段 A、B(该 plan 自 Phase 5 挂起后接续)
- **涉及文件**:`safety_guard.{hpp,cpp}`、`response_composer.{hpp,cpp}`、`agent_orchestrator.cpp`、`web/app.js`、`tests/{test_safety_guard.cpp,test_composer.cpp}`

## 规划层核对

| 验收点 | 结果 | 证据 |
|---|---|---|
| A:货币数字与候选 {price, original_price} 精确比对 | ✅ | safety_guard.cpp MoneyAllowed:精确匹配 ±0.01 |
| A:派生数字白名单(人均/总价)不误杀 | ✅ 且优于 plan | 人均取 floor/ceil 双向 ±0.51 容差;总价按商品自身 [min_people,max_people] 区间(cap 20),比 plan 的裸 price×people 更精确;折扣 ±0.5 折容差 |
| A:违规 → 回退模板 compose,记 `template_guard_fallback` | ✅ | response_composer 非流式路径;原回复入审计不发出 |
| A:规则式零 LLM 调用 | ✅ | 纯正则/数值比对,无 LLM 依赖 |
| B:流式完成后校验,违规发 `replace` SSE 事件 | ✅ | 流式全文 FactCheck → 违规 Emit("replace", 模板兜底) + compose_mode=`llm_stream_guard_fallback` + spdlog::warn 带违规明细 |
| B:replace additive,旧前端不崩 | ✅ | web/app.js 新增 case 'replace' 整体替换气泡;旧前端走 default 静默忽略 |
| e2e 离线不破(stub 输出零误杀) | ✅(关键实证) | 67/67 全过——stub 确定性文本未触发任何 guard 回退 |

## 契约层核对

- SSE 协议:✅ 只新增 replace 事件类型(additive)
- compose_mode 枚举:新增两个 fallback 值,属审计字段扩展,观测消费方(evaluate.py 按值分布统计)不崩
- 降级链:✅ guard 失败方向是更安全的模板兜底,符合"降级向安全侧"哲学

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| build_windows.ps1 + CTest | ✅ **129/129**(含工作区已暂存的阶段 C 改动一并构建;阶段 B commit 自报口径 126/126) |
| e2e 离线 | ✅ **67/67**(零误杀实证) |
| 单测覆盖核对 | ✅ 编造价格拦截/真实价格放行/人均派生放行/流式违规→replace/流式诚实零 replace 等用例在列 |

## 差距清单

无。

## 下一阶段建议

阶段 C(guard_action/guard_detail 幂等加列 + evaluate.py Guard 段 + guard_rules.json 规则外置)已暂存在工作区,预计下轮可验收。验收重点:缺规则文件时行为与内置默认逐字节一致(降级链同款纪律)。
