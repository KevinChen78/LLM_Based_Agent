# 验收报告:Phase 2.1 学习式排序 + Phase 2.2 用户画像(阶段 A~F)

- **结论**:✅ 通过
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线 commit**:`64de433`(已验收态);本次验收对象为其后全部未提交改动(14 个修改文件 + 16 个新增文件/目录,无新 commit)
- **对应 plan**:`C:\Users\Kevin Chen\.claude\plans\cheeky-wondering-wand.md`(阶段 A~F)
- **涉及文件**:
  - 阶段 A:`include/agent/observability_store.hpp`、`src/observability/observability_store.cpp`、`include/agent/agent_orchestrator.hpp`、`src/agent/agent_orchestrator.cpp`
  - 阶段 B:`ranking_service/{main.py,features.py,.env.example}`、`scripts/{build_features.py,train_ranker.py}`、`requirements.txt`
  - 阶段 C:`include/agent/ranker_client.hpp`、`src/tools/ranker_client.cpp`、`include/agent/experiment_manager.hpp`、`src/agent/experiment_manager.cpp`、`include/agent/deal_tools.hpp`、`src/tools/deal_tools.cpp`、`apps/api_server/main.cpp`、`tests/{test_ranker_client.cpp,test_experiment.cpp}`
  - 阶段 D:`scripts/evaluate.py`
  - 阶段 E:`include/agent/{user_profile_store.hpp,preference_extractor.hpp}`、`src/memory/sqlite_user_profile_store.cpp`、`src/agent/preference_extractor.cpp`、`include/agent/prompt_builder.hpp`、`src/llm/prompt_builder.cpp`、`include/agent/task_planner.hpp`、`src/agent/task_planner.cpp`、`sql/003_user_profiles.sql`、`tests/test_user_profile.cpp`
  - 阶段 F:`web/app.js`、`README.md`、`CLAUDE.md`、`start_all.bat`、`.gitignore`、`tests/CMakeLists.txt`

## 规划层核对(对照 plan 逐阶段)

| 阶段 | 结果 | 关键证据 |
|---|---|---|
| A 埋点 | ✅ | `add_column_if_missing` + `PRAGMA table_info` 幂等迁移(observability_store.cpp:74-96);INSERT 13→16 列;RecLogEntry/RecAudit 新字段;orchestrator 注入 `user_id`(agent_orchestrator.cpp:343)且**不在** DealRanker::SchemaJson 中(deal_tools.cpp:269-286);`rank_audit` 顶层键解析回传(L387-388) |
| B ranking_service | ✅ | `/v1/health` 形状 `{status, model_loaded, model_version, feature_rows, profiles_available}`;`/v1/rank` 形状 `{model_loaded, model_version, items:[{item_id, model_score}]}`;`train_ranker.py --synthetic` 产出 model.txt;features.py 单点 + meta.json |
| C C++ 接线 | ✅ | off 零调用;shadow 只审计;`model_loaded=false`/服务失败→`rule_fallback`;taboo 剔除在模型调用之前(deal_tools.cpp:306-312);候选 cap 50;`deal_ranker` 工具名不变;banner 打 ranker 状态;三测试文件已手工加入 tests/CMakeLists.txt(plan 风险表末项排除) |
| D evaluate.py | ✅ | PRAGMA 检列旧库整段跳过(实证:验收前旧库无三列,运行不崩不打印该段);样本<30 标注;位置敏感度 by position |
| E 用户画像 | ✅ | 空画像/`{}` → prompt 零新增段落(prompt_builder.cpp:20-25);含"当轮显式输入永远优先"规则;插入位置在 `# 当前已填充槽位` 前;SqliteUserProfileStore 独立类独立连接同文件;memory 模式 profiles=nullptr 降级;`UserProfilePrompt.EmptyProfileLeavesPromptUntouched` 测试在 |
| F 前端+文档 | ✅ | web/app.js localStorage UUID(crypto.randomUUID → 手写兜底 → 异常回退 'web-user');README 补 RANKER_* 环境变量与训练流程;CLAUDE.md 降级链加 ranking_service 条目、改四进程描述;start_all.bat 加 :8002;.gitignore 排除 model.txt/meta.json/.env |

## 契约层核对(CLAUDE.md 不可破坏面)

- **降级链**:✅ 未破坏且按 plan 扩展——`RANKER_SERVICE_URL` 空/mode=off → 零调用纯规则分(实证 banner:"Ranking service: disabled (mode=off)");画像 store 打开失败 → 降级不崩(main.cpp:243-256)
- **检索三端点 JSON 形状**:✅ 未触碰(retrieval_service 无改动)
- **Python stdlib 策略**:✅ 唯一新增 pip 依赖 lightgbm,requirements.txt 注释明确限定 ranking_service + train_ranker.py 专用;e2e/evaluate/生成器保持纯 stdlib(evaluate.py 新增段只用 sqlite3/json)
- **观测回传通道模式**:✅ store 指针未穿工具签名;`rank_audit` 经 tool_result 顶层键回传,orchestrator 统一写库

## 实证层(本窗口独立实跑,非转述)

| 命令 | 结果 |
|---|---|
| `powershell scripts/build_windows.ps1` | ✅ 构建成功 |
| CTest(build 附带,WORKING_DIRECTORY=项目根) | ✅ **95/95 通过**,含新套件 ExperimentManager/RankerClientIntegration/UserProfileStore/PreferenceExtractor/UserProfilePrompt;`DealCatalogPg.LivePostgresMatchesFile` 因无 PG_TEST_DSN 按预期 SKIP |
| `RANKER_MODE=off python scripts/e2e_multi_turn.py` | ✅ **51/51 ALL PASS** |
| `python scripts/train_ranker.py --synthetic` | ✅ samples=2436 positives=966 groups=200 features=15,产出 model.txt(lgbm-20260827-022501) |
| 存量 `data/observability.db` 迁移实证:启动 api_server 后 PRAGMA 检查 | ✅ 旧库(无三列)启动后自动获得 `candidates_json`/`experiment_group`/`rank_mode`,幂等迁移真实生效 |
| 起 ranking_service 实测 `/v1/health` + `/v1/rank` | ✅ health: model_loaded=true, feature_rows=3, profiles_available=true;rank 返回合法 model_score,契约形状与 plan 逐字段一致 |
| `python scripts/evaluate.py`(验收前旧库) | ✅ 正常运行,排序段按预期不出现(旧库兼容) |

## 差距清单

无阻塞差距。两项观察(不影响通过):

1. **evaluate.py 现状数据**:空推荐率 41.7%、compose tokens 记 0(上游未返 usage)——均为存量口径问题,非本阶段引入。
2. **model.txt/meta.json 已被 .gitignore 排除**(训练产物可再生成,处理正确);但当前工作区所有改动**仍未 commit**,建议执行窗口尽快按 plan 的 A~F 切分提交,避免大杂烩 commit 或意外丢失。

## 下一阶段建议(供架构规划窗口参考)

1. 提示执行窗口完成 commit 切分(plan 原定每阶段一个 commit),commit 后可做一次轻量复验(git log 核对)。
2. Phase 2.1 真实效果依赖 feedback 数据积累,后续阶段可考虑:shadow 模式灰度开启的运维 checklist、`--real` e2e(7 项语义检查)在真模型下跑一轮验证画像段注入效果。
3. `sql/003_user_profiles.sql` 目前仅是 PG 终态参考,若后续 KB/商品外的数据也要向 PG 收敛,可规划 user_profiles 的 PG 迁移。
