# 项目讲述稿:LLM 团购推荐 Agent 的架构演进

> 用途:面试/复盘时的完整故事线。所有数字可复算(出处随文标注:单测=test_agent.exe,e2e=scripts/e2e_multi_turn.py,验收=docs/acceptance/ 12 份报告)。
> 最后更新:2026-08-27(v1.0 基线)。

## 一句话版本

一句话自然语言 → 追问补全 → 召回 → 排序 → 生成推荐理由 → 反馈回流学习的完整闭环;C++20 协程控制面 + Python 数据面四进程,每一层外部依赖都有本地兜底,每一次 LLM 调用和 guard 介入都可审计复算。

## 演进故事线(每阶段解决什么问题)

### Phase 0:Agent 闭环——"先让它能聊起来"
C++ 编排器(显式状态机 SLOT_FILL→RETRIEVE→RANK→EXPLAIN→RESPOND→FALLBACK)+ TaskPlanner(LLM 出 JSON 计划)+ Mock 工具 + SQLite 会话。第一课:**LLM 输出不可信**——容错 JSON 提取(干净 JSON→围栏→首尾花括号)+ 温度 0 重试,把 DeepSeek 推理模型的规划截断率从 ~1/3 压到 15/15。

### Phase 1:真实召回——"从 Mock 到真数据"
5320 条确定性生成数据;检索服务 BM25(中文 bigram,零依赖)起步,数据扩量后加 pgvector 向量通道、RRF(k=60) 融合而非替换——**BM25 保精确与可解释,向量补语义泛化**。结构化过滤下推 PG SQL,双后端(PG/JSON)结果逐字节一致,用 diff 矩阵证明而不是宣称。

### Phase 2:学习式排序 + 画像——"让反馈流动起来"
反馈表 → build_features → train_ranker(LightGBM LambdaRank)→ ranking_service(:8002)。三个关键决策:
- **候选集埋点先行**(candidates_json 加列)——没有输入侧候选集的日志,学到的只是"幸存者排序";
- **样本门槛拒绝产模型**(样本<100 或正样本<10)——没有数据就没有模型,链路自动回退规则分;
- **taboo 剔除永远留在 C++ 侧**——模型只能重排幸存者,安全规则不可被学习绕过。

画像走规则式 PreferenceExtractor(零 LLM、纯函数可单测),惰性计算+5 分钟缓存;空画像时 prompt 逐字节同前——**行为不变性靠测试断言,不靠小心**。

### Phase 2.3/3:观测口径与召回放宽——"先量准,再修好"
空推荐率告警先是假动作:模拟反馈数据污染了真实口径。先拆三口径(真实/测试/模拟,sim 隔离不污染满意率),再修召回:三级放宽链(relaxed_level 0→2)additive 回传审计。空推荐率 29.2%→24.1%,且如实标注分子全是修复前历史日志(docs/phase3_recall_fix.md)。**诚实记录原则**:流式 tokens 上游不给 usage 就记 0,不估算冒充。

### Phase 4:输出事实校验——"把红线从提示词变成机制"
「价格必须来自数据库」原来只是 prompt 里一句话。FactCheckReply 规则式核验每个 ¥xx/xx元/xx折 断言(派生白名单放行人均/总价),违规回复不发出:非流式换模板,流式发 additive `replace` 事件整体纠偏(旧前端忽略不崩)。e2e 80 项零误杀实证。规则外置 data/guard_rules.json,缺文件逐字节回退内置。

### Phase 5:鉴权限流 + gRPC 试点——"补上生产横切"
API key(恒定时间比较)+ 内存令牌桶(429+Retry-After),默认全关、逐字节兼容;e2e 用独立端口实例验证启用路径。gRPC 不全量替换,在 retrieval_service 双协议试点:proto 逐字段镜像 HTTP,任何 gRPC 失败逐调用回退 HTTP,一致性由 24 例 × 双后端 × 双协议矩阵守护。工具链首构 29 分钟、踩坑 4 条全记录(docs/phase5_auth_grpc.md)。

## 关键技术决策与权衡(贯穿全程)

| 决策 | 权衡 | 一句话理由 |
|---|---|---|
| 降级链贯穿每一层 | 每层多写兜底代码 | 演示/断网/依赖挂都能跑;降级方向永远朝安全侧 |
| 契约冻结(retrieval 三端点/SSE) | 改契约成本高 | C++↔Python 边界稳定,内部实现随便换 |
| 观测回传通道 | 组件多带审计字段 | store 指针不穿签名,组件纯逻辑可单测,写库单一出口 |
| 规则式 Guard(零 LLM) | 覆盖面不如 LLM 审查 | 确定性、零延迟、零成本、可单测;LLM 级审查封存为可选项 |
| SQLite 替代 Redis/ES/ClickHouse | 多实例时要迁 | 按当前规模选型,按目标形态设计(DDL 已备好) |
| 诚实记录(⏸ 判定/tokens 记 0) | 指标不好看 | 假数据比没数据更糟;数据不足做成系统行为而非口头解释 |

## 可复算证据数字

| 数字 | 复算方式 |
|---|---|
| 单测 129/129(1 SKIP live-PG) | `.\build\bin\Release\test_agent.exe`(项目根) |
| e2e 离线 80/80 | `python scripts/e2e_multi_turn.py` |
| e2e 真模型 7/7 | `python scripts/e2e_multi_turn.py --real` |
| 检索一致性矩阵 24 例×双后端×双协议 | `python scripts/test_pg_retrieval.py` |
| 空推荐率 29.2%→24.1% | [docs/phase3_recall_fix.md](phase3_recall_fix.md) |
| gRPC 首构 29min / 踩坑 4 条 | [docs/phase5_auth_grpc.md](phase5_auth_grpc.md) |
| ⏸ 数据不足判定(排序模型未上线) | `python scripts/train_ranker.py`(样本门槛拒绝)+ [docs/acceptance/phase4-cd-guard-observability-20260827.md](acceptance/phase4-cd-guard-observability-20260827.md) |
| 验收报告 12 份 | [docs/acceptance/](acceptance/) |

## 三窗口协作流程(工程实践本身就是素材)

项目后半程用三个 AI 窗口分工:**规划窗口**出阶段 plan(用户确认后生效)、**执行窗口**只按 plan 实现+报证据、**验收窗口**(PM)三层验收(规划核对/契约核对/独立实跑)。产出物:[docs/collaboration/](collaboration/) 的窗口职责通告 + [docs/acceptance/](acceptance/) 12 份验收报告。价值:① 职责隔离防止"自己验收自己";② 每份报告的差距清单驱动下一轮 plan(Phase 5 差距#1 → Phase 4 顺带修复,闭环);③ 所有结论可复算,验收不是签字是实证。
