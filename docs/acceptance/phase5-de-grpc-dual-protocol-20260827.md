# 验收报告:Phase 5 阶段 D(gRPC 双协议并存)+ E(收口)— 暨 Phase 5 整阶段收口

- **结论**:✅ 通过(附 1 条非阻塞观察)
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`2f7a668`(阶段 C 已验收);验收对象 = `bdee99d`(D)+ `f2fa3bb`(E)
- **对应 plan**:`phase5-auth-grpc.md` 阶段 D、E
- **涉及文件**:`src/tools/grpc_retrieval_client.cpp`、`retrieval_service/{grpc_server.py,main.py,gen/}`、`apps/api_server/main.cpp`、`scripts/test_pg_retrieval.py`、`docs/phase5_auth_grpc.md`、README/CLAUDE.md 等

## 规划层核对

| 验收点 | 结果 | 证据 |
|---|---|---|
| 协议×后端矩阵 | ✅ | test_pg_retrieval.py:24 例 pg×json 一致 + **24 例 gRPC≡HTTP 双后端一致**(含 relaxed_level/effective_category 字段) |
| `RETRIEVAL_PROTOCOL` 缺省零回归 | ✅ | 缺省 e2e 离线 **67/67** |
| 停 8011 自动回退 HTTP | ✅(独立实证) | ON 构建 api_server 配 grpc 协议 + 8011 确认无监听,两轮对话正常出单(2 条海鲜套餐),检索服务 HTTP 日志证实走了回退 |
| gRPC 存活路径真实生效 | ✅(独立实证) | 8011 在线 + 干净重启 api_server,检索出单且 HTTP 访问日志**零增长**(确走 gRPC) |
| 降级链延伸 | ✅ | 代码审:gRPC 逐调用失败 fall through → HTTP → 本地子串;Healthy() 在 gRPC 不可达时如实报告 HTTP 兜底状态 |
| Phase 3-C additive 字段跨协议保持 | ✅ | gRPC 响应映射含 `has_relaxed_level()` presence 语义,与 HTTP 同规则 |
| Python 侧降级 | ✅ | grpcio 缺失/端口占用 → HTTP-only;矩阵在 grpcio 缺席时协议维自动 SKIP |
| 阶段 E 文档 | ✅ | docs/phase5_auth_grpc.md 100 行(架构图、回滚、29min 构建记录、踩坑 3 条);CLAUDE.md 降级链加 gRPC→HTTP 条目;README 环境变量同步;.gitignore 加 build-grpc/ |

## 契约层核对

- HTTP JSON 契约面:✅ 冻结未动(矩阵逐字段一致证明)
- 降级链:✅ 延伸正确(gRPC→HTTP→本地),banner 打印协议/端口/回退目标
- stdlib 策略:✅ grpcio/grpcio-tools 仅检索服务,requirements.txt 注释标明;网关/e2e 纯 stdlib

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| build_windows.ps1(默认 OFF)+ CTest | ✅ 114/114 |
| e2e 离线(缺省协议) | ✅ 67/67 |
| test_pg_retrieval.py | ✅ 后端 24/24 + 协议 24/24 |
| ON 构建 api_server + grpc 协议 + 8011 宕 | ✅ 回退 HTTP 出单正常(检索日志佐证) |
| 同上 + 8011 在线干净重启 | ✅ 走 gRPC(HTTP 日志零增长)出单一致 |
| 清场 | ✅ 8001/8011/8080 验收后已释放 |

## 差距清单

| # | 现象 | 整改建议 | 严重度 |
|---|---|---|---|
| 1 | gRPC 回退**静默无日志**:8011 宕机期间每次调用先尝 gRPC 失败再落 HTTP,observability 与 spdlog 均无痕迹;且服务端重启后 gRPC 信道重连退避(默认 max 120s)长于单次 5s  deadline,恢复期内调用持续走 HTTP——功能正确但运维不可见 | grpc_retrieval_client.cpp 在 fall-through 处加一行 spdlog::warn(限频即可,如每 60s 一次);如需更快恢复可在 channel args 调低 `GRPC_ARG_MAX_RECONNECT_BACKOFF_MS` | 低(正确性无碍,可观测性短板) |

## Phase 5 整阶段收口意见

A(鉴权)→ B(限流)→ C(工具链)→ D(双协议)→ E(收口)五阶段全部验收通过。gRPC 试点达成"并存不替换"目标:HTTP 契约面冻结、OFF 构建零纠缠、双协议矩阵一致、回退实证成立。

## 下一阶段建议

1. 挂起的 **Phase 4 输出事实校验 Guard** 可接续(plan 已备,验收点明确)。
2. 若 gRPC 推广到 llm_gateway,注意 SSE 流式与 gRPC streaming 的映射需单独评估(plan 已标注)。
3. 差距#1 的限频 warn 日志可作为 Phase 4 或后续任意阶段的一行顺带修复。
