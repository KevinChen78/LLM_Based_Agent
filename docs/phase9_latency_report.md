# Phase 8-B 延迟实测量化报告

- **日期**:2026-08-30
- **数据窗口**:2026-08-30 19:46–20:20,`data/observability.db` 真实用户流量 15 轮
  (user `4865fbed…`,7 个会话;已剔除 `e2e`/`pm_accept_p8a`/`u3`/`u4` 测试流量)
- **埋点**:Phase 8-A `stage_ms_json`(commit ae94181)+ `llm_calls` 交叉验证
- **结论一句话**:延迟三大头 = 上游 LLM 规划(56.8%)、上游 LLM 流式compose(25.5%)、
  **检索/排序服务未运行时的连接拒绝开销(17.2%,纯浪费)**;本地代码开销可忽略(0.4%)。

## 1. 端到端与分阶段占比

15 轮端到端:p50 = **22574ms**,p95 = 33258ms,max = 41623ms。

| 阶段 | 耗时占比 | p50 | 说明 |
|---|---|---|---|
| planner_llm | **56.8%** | 13895ms | 非流式规划调用 |
| compose_total | **25.5%** | 6728ms | 流式输出全程;首 token p50 = 4352ms |
| tools(检索+排序+kb) | **17.2%** | ~6.1s/检索轮 | 全部为服务不在线的连接拒绝(见 §3) |
| 其余(session/profile/guard/history) | 0.4% | <80ms 各项 | 本地逻辑实测可忽略 |

**口径校验**:`planner_llm` 与 `llm_calls` 表同 trace 的 plan 调用耗时差恒为 **6~7ms**
(15/15 行)——planner 阶段的 99.95% 就是上游 LLM 调用本身,C++ 侧无任何可压缩的串行开销
(健康检查、画像解析等均不在此列)。PM 在 8-A 验收时观察到的「planner_llm=4106ms 占 97.6%」
是 stub 链路口径;真实模型口径下结论一致且更强。

## 2. Top1:planner_llm(上游 LLM 规划,13.9s p50)

- 证据:llm_calls 显示 plan 调用 prompt ~2–3.6KB、completion 150–1600 token,
  延迟 5.7s–22.5s 全由上游(DeepSeek)生成速度决定;三次 `respond` 行高达 ~22.4s
  (completion 仅 147 token,纯上游慢)。
- **本地无可优化项**;可选手段都属于"改变调用方式"而非"消除浪费":
  缩短 planner prompt、轻决策(明显缺槽)规则短路不经 LLM、规划流式化。
  均涉及行为/契约变化,需规划窗口立项裁决(见 §6 建议)。

## 3. Top2:工具调用的"死服务税"(retrieve 轮 ~6.1s,占比 17.2%)

### 现象

10 次 `deal_retriever` 耗时 4066–4112ms(p50 4086ms,方差 ±25ms)、
10 次 `deal_ranker` 耗时 2015–2033ms、1 次 `kb_search` 2060ms——
**零方差,是超时/拒绝型开销而非计算型**。

### 归因(逐项实证)

1. **会话期间 retrieval_service(:8001)与 ranking_service(:8002)未在运行**:
   - `rank_mode` 15/15 全为 `rule`、`model_score` 全 null(experiment_group=treatment
     本应触发 shadow 打分)——shadow 调用从未成功;
   - 但推荐结果含真实南山商户(gb-40290 等 gb-4xxxx),说明 DealCatalog 本地兜底
     (CATALOG_BACKEND=postgres,PG 在线)正常接管——**降级链功能正确**。
2. **本机 Windows 连接拒绝成本实测 2.05s/次**(WinError 10061,`::1` 与 `127.0.0.1`
   同值,非即时 RST)——"失败很快"在此平台不成立:
   - `deal_ranker` 2025ms ≈ 1 次拒绝(Rank() 直连 POST,无健康检查);
   - `kb_search` 2060ms ≈ 1 次拒绝(KnowledgeRetriever 直连 POST);
   - `deal_retriever` 4080ms ≈ 2 次拒绝(Healthy() GET + 后续路径各一次;
     精确到调用点的拆分留作阶段 C 复现实验,不影响结论)。
3. **对照实验(本报告实跑)**:临时启动 retrieval_service 后,
   `/v1/health` = 38ms,`/v1/retrieve/deals`(top_k=100,火锅/深圳)= 42–63ms。
   即健康服务的完整检索路径 <100ms;死服务让它贵了 **40~100 倍**。

### 结论

降级链"能兜底"但"兜底很贵":每检索轮白付 ~6.1s。这不是演示事故(用户当时
并未察觉服务未起),而是架构性浪费——健康检查无负缓存,每轮重新付 2s 拒绝成本。

## 4. Top3:compose 与 guard 流式纠偏(25.5%,含 33% 触发率的事实校验回退)

- 流式 compose 总时长 p50 6.7s、首 token p50 4.4s——同为上游延迟,本地仅透传。
- **专项发现(质量+延迟双重相关)**:15 轮中 5 轮(33%)命中
  `llm_stream_guard_fallback`——流式输出全程(~6–8s)跑完后被事实校验判违规,
  整段作废改发模板。
- 查 `guard_detail`(2026-08-27 + 08-30 两天真实流量共 11 例；另有 1 行测试用户
  `p7-smoke` 已剔除)呈现**明确误杀模式**:
  `money claim <N> not in candidate price/original_price/derived set` 中的 N
  正是**用户输入或会话 context 早前轮次中的预算数字**(人均150→150、200元→200、
  500预算→500);LLM 在回复中复述预算即被判"编造价格"。极端例:`团购券可以开发票吗?`
  (纯知识问答、无商品)也因文中出现 500 被判违规回退。
- 延迟影响:每次误杀浪费一整段流式 6–8s;质量影响:回答退化为模板。
  **整改建议(豁免用户输入中出现过的数字)需规划窗口立项,本报告不动代码。**

## 5. 阶段 C 候选优化项逐项裁决(plan 清单 vs 实测)

| # | 候选项 | 实测依据 | 预期收益 | 裁决建议 |
|---|---|---|---|---|
| 1 | Healthy() 健康检查加 TTL 缓存 | 在线时 health 仅 38ms,原设计收益小;但**离线时负缓存可避免每轮 2×2.05s 重复拒绝** | 离线场景 **~4s/轮**(retriever);在线场景 ~40ms/轮 | ✅ **做,且必须含失败负缓存**(建议 TTL 10–30s 成功/失败同缓存) |
| 2 | 查询向量 LRU 缓存 | 在线 retrieve 全程 45ms(已含向量计算) | <20ms/轮 | ❌ **砍**,无数据支撑 |
| 3 | shadow 改 fire-and-forget | shadow 同步阻塞,服务挂时白付 2.03s/轮(15/15 轮全部白付) | **~2s/轮**(服务挂时);在线时省几 ms | ✅ **做**;需声明风险:异步后候选审计(candidates_json 的 model_score)可能缺失,建议失败时记 rank_mode=rule 即可(现状已如此) |
| 4 | httplib keep-alive 连接复用 | 在线完整检索路径 <100ms,连接建立占比小 | <10ms/轮 | ❌ **砍**(低优先),无数据支撑 |
| 5 | UserProfile 缓存/减 SQL | profile_resolve p50 = 77ms | <80ms/轮 | ❌ **砍**,占比 0.3% |

**plan 清单外、数据驱动的新增项(须用户/规划窗口确认后立项)**:

| # | 新增项 | 依据 | 预期收益 |
|---|---|---|---|
| 6 | 死服务快速失败:连接超时 2s→500ms + 负缓存,或启动时一次性探测后整个进程期降级 | §3:拒绝成本 2.05s/次 ×3 工具 | 服务全挂时每轮省 **~6.1s**;演示前忘记起检索/排序服务时体验质变 |
| 7 | 事实校验豁免"用户输入/会话 context 中出现过的金额数字" | §4:11 例 guard_detail 中 ≥8 例是该误杀模式 | 每次省 6–8s 浪费流式 + 回答质量;触发率 33%→预期 <5% |
| 8 | planner 上游延迟手段(prompt 瘦身/轻决策规则短路/流式规划) | §2:56.8% 占比,p50 13.9s | 最大单项收益(秒级),但改行为,需单独裁决 |

## 6. 出口判据核对(plan 阶段 B)

- ✅ 分阶段占比表(§1);Top3 耗时归因到具体文件/调用点
  ([retrieval_client.cpp:15](../src/tools/retrieval_client.cpp) Healthy、
  [ranker_client.cpp:33](../src/tools/ranker_client.cpp) Post 直连无健康检查、
  [deal_tools.cpp:188](../src/tools/deal_tools.cpp) 每轮 Healthy+POST 双调用);
- ✅ 每个候选优化项均有 ms 级量化收益(§5),无数据支撑的项(2/4/5)明确砍掉;
- ✅ 数字全部可复算:`data/observability.db` 的 `stage_ms_json`/`llm_calls`/
  `guard_detail` 三表交叉,对照实验(§3.3)可重跑。

## 附:平台事实记录

本机(Windows 11)localhost 连接拒绝(WinError 10061)实测耗时 **~2.05s/次**
(`::1` 与 `127.0.0.1` 相同),并非教科书式的即时 RST。凡涉及"失败检测"的设计
(健康检查、连接超时、降级触发)在此平台都必须按 2s/次计价——这放大了负缓存的价值。
