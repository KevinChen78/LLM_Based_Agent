# Phase 3 召回缺口修复 — 修复前后对照(阶段 D 收口)

日期:2026-08-27。范围:plan `phase3-recall-gap-fix.md` 阶段 A~D 全部落地后的
收口验证。基线 commit 64de433;阶段提交 8b60c2c(A)/ 9738184(B)/ 88de0c2(C)/
阶段 D 见本文件随附 commit。

## 1. 缺口背景

evaluate.py 真实口径空推荐率曾 >20%,缺口清单集中在两类查询:

- **深圳/早茶**:catalog 无「早茶」类目,LLM 自由填 category="早茶" → SQL
  结构化过滤 0 命中;
- **武汉/汉堡**:西式快餐商品缺失(数据缺口),且同属类目词表外词。

## 2. 三层修复(对应 A/B/C)

| 层 | 修复 | 机制 |
|---|---|---|
| A 源头 | prompt 注入有效类目白名单(DistinctCategories) | category 只能取列表值;取不到就留空、需求词进 keywords |
| B 数据 | 四城市补齐西式快餐商品(武汉 434 条含汉堡标签,深北沪各 4) | 「汉堡」直通文本召回,无需别名 |
| C 兜底 | 检索服务三级放宽链(精确→别名→去类目) | `CATEGORY_ALIASES` 早茶→粤菜等;响应 additive 字段 `relaxed_level`/`effective_category`,level 0 逐字节不变 |

C 层 live 实测:深圳/早茶→粤菜 15 行(L1),武汉/汉堡→西餐 868 行(L1),
不存在类目→去类目(L2)。双后端 diff 矩阵 20→24 例全一致。

## 3. 阶段 D 重放发现的四个新问题及修复

5 条历史空推荐查询用真模型(--real 全栈)重放,前三轮重放逐轮暴露问题,
均在阶段 D 内修复:

1. **clarify 死循环**(2/5 被追问):规则 1 原要求「city、category、budget
   任一缺失必追问」,与白名单规则「category 取不到就留空」冲突——category
   永远留空 → 永远追问。修:`clarify_rule` 条件化,有白名单时只强制
   city/budget,category 留空不追问(prompt_builder.cpp)。
2. **工具调用崩溃**:LLM 发 `"max_price": null`,`json.value()` 对 null
   抛 type_error → 工具失败 0 结果。修:deal_tools.cpp 加
   ArgStr/ArgNum/ArgInt null 容忍助手,null 视为缺省(单测
   NullArgsTolerated)。
3. **「预算没有要求」走 respond**:模型不识别无约束表述。修:clarify_rule
   补「没有要求/不限/都行 → budget=0、people=0,直接 retrieve」。
4. **max_price=0 过滤一切**:模型按新规则把「预算没有要求」译成
   max_price=0,而检索服务 `price <= 0` 语义(矩阵边界用例)过滤掉全部。
   修:DealRetriever 内 `max_price <= 0` 归一为「不设限」,不下发服务;
   检索服务契约不动(单测 ZeroMaxPriceMeansNoLimit)。

## 4. 修复后重放结果(5/5 非空)

| # | 查询 | 修复前 | 修复后 |
|---|---|---|---|
| 1 | 深圳 早茶 200元,2人 | 空 | 3 条(category="" + keywords=早茶,L0) |
| 2 | 深圳 早茶 500元,3人 | 空 | 3 条(同上) |
| 3 | 深圳 早茶 预算没有要求 2人用餐 | 空 | 20 条(max_price 归一后正常召回) |
| 4 | 武汉 华中科技大学 3人 吃汉堡 预算300元 | 空 | 20 条(汉堡数据直通) |
| 5 | 武汉 3人 吃汉堡 预算300元 | 空 | 3 条(汉堡数据直通) |

全部 action=retrieve、无追问;trace 审计确认 category 一律按白名单留空、
需求词进 keywords,未再出现 catalog 外类目值。

## 5. evaluate.py 口径复算

- 修复前真实口径空推荐率:**29.2%(7/24)**;
- 重放日志入库后:**24.1%(7/29)** —— 分子 7 条全部是修复前历史日志
  (缺口清单内容与前完全一致),新增 5 条重放全部非空只增大分母。
- 历史空日志不可回溯改写,该指标将随新真实流量积累继续下降;复算方法:
  `python -X utf8 scripts/evaluate.py` 看「空推荐率(按流量口径拆分)」真实行。

## 6. 测试证据

- 单测 102 过 / 1 SKIP(DealCatalogPg.LivePostgresMatchesFile,无 PG_TEST_DSN);
  新增/修改用例:test_kb.cpp(RecallAudit×2、NullArgsTolerated、
  ZeroMaxPriceMeansNoLimit)、test_category_vocab.cpp(clarify 规则断言)。
- 双后端一致性:`python scripts/test_pg_retrieval.py` 24 例矩阵(含
  relaxed_level/effective_category 跨后端比较)。
- 兼容性:prompt 无白名单/无画像时逐字节同前;检索响应 level 0 不加字段;
  max_price 归一只发生在 C++ 工具边界,服务契约语义不变。

## 7. 遗留

- evaluate.py 缺口清单仍显示历史 7 条(口径正确,非回归);
- 「武汉 华中科技大学」district 未抽取(走 keywords 召回,结果非空,可接受);
- PM 窗口验收 Phase 3 整阶段。
