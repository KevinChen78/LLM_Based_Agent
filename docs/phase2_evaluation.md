# Phase 2 效果评估与收尾报告(Phase 2.3-E)

> 数据快照:2026-08-27,`data/observability.db` + `data/sessions.db`。
> 所有数字可用文末「复算方法」的命令重新算出。
> 上游标准:`项目规划_修订版.md` Phase 2「学习式排序」——排序指标优于规则基线 10%+。

## 1. 判定结论

**⏸ 数据不足,无法判定 10%+ 标准。**

差距是明确的、可量化的:

| 缺口 | 现状 | 判定所需 | 差 |
|---|---|---|---|
| 真实标注训练样本(trace 回连 feedback 的候选行,非 sim) | **6 行** | ≥100 行(train_ranker `--min-samples`) | **94 行** |
| 真实正样本(like) | 1 行 | ≥10 行(`--min-positives`) | 9 行 |
| 带实验分组的反馈(control/treatment 各) | **0 行** | 各 ≥30 行(evaluate.py 置信线) | 各 30 行 |
| 带 candidates_json 的非 sim 请求 | 16 条 | — (曝光足够,缺的是标注) | — |

首版真实模型训练已按既定逻辑**拒绝产模型**(exit 2,"below gates"),
ranking_service 保持 `model_loaded=false` 时 C++ 自动回退规则分 ——
降级链按设计工作,这不是失败,是诚实的"还没攒够"。

## 2. 数据口径(Phase 2.3-A 修复后)

evaluate.py 将流量拆为三口径,本报告所有结论均按口径标注:

- **真实**:浏览器/手工会话(user_id 为 localStorage UUID);
- **测试/stub**:e2e(user_id=e2e)及一切经 stub 模型(gpt-4o-mini)的 trace;
- **模拟(sim-)**:simulate_feedback.py 写入,默认被训练/评估排除,
  只用于验证链路通畅,**不参与任何效果判定**。

修复前「空推荐率 41.7%」的混合口径数字已废弃。拆分后:

- 真实口径空推荐率 **100%(5/5)** —— 全部为召回缺口,不是排序问题:
  深圳/早茶 ×3(商品库无深圳数据)、武汉/汉堡 ×2(无汉堡类目)。
  本阶段只暴露不修召回(见 Phase 3 候选 4)。
- 测试/sim 口径空推荐率 0%。

## 3. 排序 / 实验对比(shadow 证据链)

- **rank_mode 分布**:rule=166,rule_fallback=0,无 model(shadow 模式输出
  恒为规则分,符合设计)。
- **shadow 双分数**:12/12 请求 candidates_json 的 rule_score + model_score
  齐全;experiment_group 分布 control=6 / treatment=6(FNV-1a 分桶正常)。
- **rule vs model top-3 顺序一致率 12/12(100%)**。
  注意此数字**不构成效果证据**:① shadow 轮次集中于上海/海鲜,本地候选仅
  2 条,无可区分空间;② 当前服务内模型为 `--include-sim` 管道冒烟模型
  (meta.json `includes_sim=true`),模拟偏好信号与规则公式同构
  (都偏好低价/高评分/高折扣),顺序一致是预期结果。
- **位置敏感度**(⚠️ 该段数据以 sim 口径为主,仅证明埋点可算):
  第 1/2/3 位 like 占比 93.3% / 95.0% / 94.1%,无明显位置衰减。
- **分组满意率**:暂无带实验分组的反馈(shadow 轮次未配 feedback 动作;
  active 实验未开启)。

## 4. 画像注入对照(Phase 2.3-D 实证)

同一 query(上海海鲜 300元 3人):

- 老用户(有 like 历史)第二轮 plan prompt **含**用户画像段,位置在
  `# 当前已填充槽位` 之前,含「当轮显式输入永远优先」规则;
  user_profiles 实证 `["上海"]/["海鲜"]/avg_budget=300/price_sensitivity=1.0`。
- 新用户(无历史)prompt **不含**画像段(逐字节同 Phase 2.2 之前)。
- 证据持久化于 `llm_calls.raw_request`(本阶段新增审计列)。

## 5. 观测口径修复收益(Phase 2.3-A)

- 流式 compose tokens 不再恒 0:网关 stub 全链路行 547+256;
  真实上游经 `stream_options.include_usage` 返回 usage 时如实记录。
- llm_calls 状态分布:success=45 / template_fallback=15
  (template_fallback 均为 stub 模式下 compose 走模板,属预期)。

## 6. Phase 3 候选方向(供用户裁决,未纳入本轮)

1. **user_profiles → PG 迁移**(sql/003 终态参考;与 KB/商品数据向 PG
   收敛一起做才有意义)。
2. **PolicyEngine / InputGuard / OutputGuard** —— 项目规划 1.1「合规与兜底」
   与 2.3 policy 模块至今未建,是唯一完全空缺的需求项。
3. **RANKER_MODE=active 真实验** —— 需要真实流量;演示项目 shadow
   证据链已足够。
4. **召回缺口修复** —— 真实口径空推荐率 100%(深圳/早茶、武汉/汉堡),
   若继续积累真实流量,这是比排序更优先的效果瓶颈。

**积累真实数据的建议**(衔接判定缺口):手工跑 20+ 轮真实多轮会话并
like/dislike(覆盖 ≥3 类目、含新老用户),然后
`python scripts/build_features.py && python scripts/train_ranker.py`;
每攒一批重跑一次本报告第五节即可看到口径变化。

## 7. 复算方法

```powershell
python -X utf8 scripts/evaluate.py                 # 全部口径拆分 + 排序实验段
python scripts/train_ranker.py                     # 真实数据训练(门槛拒绝=数据未够)
python scripts/train_ranker.py --include-sim       # 管道冒烟(产物 meta.json includes_sim=true)
python scripts/simulate_feedback.py --requests 150 # 重新生成 sim 数据(确定性幂等)
```

shadow 双分数对比 SQL 见 `docs/ranker_shadow_checklist.md` 第 2/3 节。
