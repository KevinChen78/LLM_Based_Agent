# Phase 6 项目收尾 — 收口文档(v1.0 基线)

> 实施 plan:`phase6-project-wrapup.md`(用户确认执行)
> 提交:A `0714ea9` / B `180304c`+`e803d69` / C(本文档所在 commit)
> PM 阶段验收:A ✅(`phase6-a-docs-20260827.md`,无差距)

## 全量验证矩阵(2026-08-27,v1.0 tag 基线)

| 验证项 | 命令 | 结果 |
|---|---|---|
| 构建 + 单测 | `.\scripts\build_windows.ps1` → `test_agent.exe` | ✅ **129/129**(1 SKIP:DealCatalogPg.LivePostgresMatchesFile,无 PG_TEST_DSN) |
| e2e 离线 | `python scripts/e2e_multi_turn.py` | ✅ **80/80**(S1~S9 含注入/违禁/鉴权/限流) |
| e2e 真模型 | `python scripts/e2e_multi_turn.py --real` | ✅ **7/7**(DeepSeek,语义槽位延续 + kb grounding) |
| 检索一致性矩阵 | `python scripts/test_pg_retrieval.py` | ✅ 后端 24/24(postgres≡json)+ 协议 24/24(grpc≡http) |
| 离线评估 | `python -X utf8 scripts/evaluate.py` | ✅ 三口径 + Guard 段正常(见下) |

工作区干净,tag `v1.0` 打在绿基线上。

## evaluate.py 快照(挂起判定的口径依据)

- 请求总量 229;反馈满意率:真实 👍1/👎0,测试 👍6/👎0,模拟 👍284/👎18(94.0%,sim 隔离不污染真实口径)。
- 真实流量空推荐率仍 >20% 告警:**分子全是 Phase 3 修复前的历史日志**(docs/phase3_recall_fix.md 已证 29.2%→24.1%,修复后无新增)。
- Guard:guard_action 分布 none=228 / refuse_input=1,fact_violation 0.0%。
- 排序:treatment 组 👍1(样本<30,仅供参考)。

## ⏸ 数据不足判定(Phase 2 成功标准)挂起说明

学习式排序「优于规则基线 10%+」是项目**唯一悬而未决的效果类目标**。
当前状态:链路全栈就绪(埋点/特征/训练/服务/灰度全部验收),但真实
feedback 积累未达训练门槛——`train_ranker.py` 样本 <100 或正样本 <10
**拒绝产模型**是刻意设计(没有数据就没有模型,链路自动回退规则分)。

**解除路径**(日常动作,非新阶段):真实会话持续积累 →
`python scripts/build_features.py` → `python scripts/train_ranker.py`
→ 重启 ranking_service → RANKER_MODE=shadow/active →
`python -X utf8 scripts/evaluate.py` 看「排序/实验对比」段分组满意率。

## 封存的可选深化项(不在 v1.0 范围)

- LLM 级二次安全审查(Phase0 文档 §5.5 InputSafetyPrompt;规则式 Guard 已覆盖演示需求);
- user_profiles / sessions / feedback / observability 迁 PG(终态 DDL 先例:sql/003_user_profiles.sql);
- RANKER_MODE=active 真实验(待数据门槛);
- gRPC 推广到 llm_gateway / ranking_service(gateway 涉 SSE 需单独评估);
- p99 < 1.5s SLO 告警化(当前仅 /v1/metrics + evaluate.py 观测)。

## 面试/复盘材料索引

- [docs/project_story.md](project_story.md) — 演进讲述稿 + 可复算证据数字
- [docs/architecture_vs_plan.md](architecture_vs_plan.md) — 规划 vs 实现取舍对照
- [docs/面试题库_全模块.md](面试题库_全模块.md) — 19 模块 190 问
- [docs/面试项目介绍 + 可能的提问.md](面试项目介绍%20+%20可能的提问.md) — 2 分钟稿 + 25 压力题
- [docs/acceptance/](acceptance/) — 12 份验收报告(三窗口协作实证)
