# Ranker Shadow 灰度运维 Checklist(Phase 2.3-C1)

> 目的:在**不改变任何线上排序输出**的前提下,用真实候选集跑通 LightGBM 模型推理,
> 积累 rule/model 双分数审计数据,为 active 灰度与效果评估提供依据。
> shadow 模式的输出排序始终由规则分决定(降级链不破)。

## 1. 起栈步骤

```powershell
# 1) 训练模型(管道冒烟可用模拟数据;效果判定必须真实数据)
python scripts/build_features.py                 # 默认排除 sim- 行
python scripts/train_ranker.py                   # 真实数据;门槛不足会拒绝(exit 2)
#    管道冒烟:python scripts/train_ranker.py --include-sim
#    检查 ranking_service/meta.json 的 includes_sim 字段,True=冒烟模型,不用于效果判定

# 2) 起四服务(顺序无所谓;start_all.bat 已含 ranking_service 且默认 RANKER_MODE=shadow)
#    retrieval_service :8001   llm_gateway :8000   ranking_service :8002   api_server :8080
set RANKER_MODE=shadow
set RANKER_SERVICE_URL=http://127.0.0.1:8002
set RANKER_TREATMENT_PCT=50      # 分桶比例(FNV-1a(user_id)%100 < pct → treatment)
set RANKER_EXPERIMENT=ranker-v1  # 实验名,进哈希盐,换名即重新分桶
.\start_all.bat
```

api_server banner 应打印 ranker 配置;`curl http://127.0.0.1:8002/v1/health`
应返回 `"model_loaded": true`(`false` 是合法健康状态 = 冷启动,C++ 侧自动规则分)。

## 2. 审计字段核对(observability.db)

每轮 retrieve 请求在 `recommendation_logs` 落一行,核对三件套:

```sql
SELECT trace_id, experiment_group, rank_mode,
       json_extract(candidates_json, '$[0].rule_score')  AS rule0,
       json_extract(candidates_json, '$[0].model_score') AS model0
FROM recommendation_logs
WHERE candidates_json IS NOT NULL AND candidates_json != ''
ORDER BY rowid DESC LIMIT 20;
```

| 字段 | 期望值 | 异常含义 |
|---|---|---|
| `experiment_group` | `control` / `treatment`(空 user_id 必为 control) | 全空 → RANKER_EXPERIMENT 未配置 |
| `rank_mode` | shadow 下恒为 `rule`(输出按规则分);服务缺席为 `rule` 或 `rule_fallback` | 出现 `model` → 不在 shadow 模式,检查 RANKER_MODE |
| `candidates_json[].model_score` | shadow + 模型已加载 → 非 null | 全 null → ranking_service 挂或无模型(模型分缺失是**降级不是故障**) |

汇总视图:`python -X utf8 scripts/evaluate.py` 的「排序 / 实验对比」段
(rank_mode 分布、分组满意率、位置敏感度、候选集含模型分占比)。

## 3. rule vs model 顺序差异对比

```python
# 对同一 trace 的 candidates_json 分别按 rule_score / model_score 排序,比较 top-3
import json, sqlite3
con = sqlite3.connect("data/observability.db")
for (trace, blob) in con.execute(
        "SELECT trace_id, candidates_json FROM recommendation_logs"
        " WHERE candidates_json LIKE '%model_score%' ORDER BY rowid DESC LIMIT 10"):
    cands = [c for c in json.loads(blob) if c.get("model_score") is not None]
    by_rule  = [c["item_id"] for c in sorted(cands, key=lambda c: -(c["rule_score"] or 0))][:3]
    by_model = [c["item_id"] for c in sorted(cands, key=lambda c: -c["model_score"])][:3]
    print(trace, "rule:", by_rule, "model:", by_model, "same:", by_rule == by_model)
```

顺序一致不代表模型无效(规则分本身是特征之一 rank_in_rules);长期观察
top-3 重合率趋势即可。效果判定走 feedback 满意率(阶段 E),不靠顺序差异。

## 4. 回滚操作

```powershell
set RANKER_MODE=off        # 完全关闭:零额外调用,行为与 Phase 2.1 之前一致
# 重启 api_server 生效
```

分级回滚(代价从小到大):
1. **服务降级**(无需操作):ranking_service 挂/无模型 → C++ 自动规则分,
   `rank_mode` 落 `rule_fallback`,延迟最多 +2s(读超时)。
2. **RANKER_MODE=off**:连 shadow 调用都不发,恢复纯规则。
3. **删除 ranking_service/model.txt + meta.json**:服务回冷启动
   `model_loaded=false`,等价于未训练。

## 5. 已知边界

- shadow 调用同步进行,候选 cap 50,读超时 2s;排序服务慢时表现为
  `rule_fallback`,不阻塞主链路。
- 模拟数据(sim- 前缀)默认不进 build_features / train_ranker / evaluate 的
  真实口径;冒烟模型(meta.json `includes_sim=true`)只验证链路。
- taboo 剔除永远在 C++ 侧先于模型,模型只重排幸存者 —— shadow 数据里
  candidates_json 已是 taboo 过滤后的集合。
