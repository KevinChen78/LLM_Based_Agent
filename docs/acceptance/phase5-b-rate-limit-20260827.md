# 验收报告:Phase 5 阶段 B — 每用户令牌桶限流

- **结论**:✅ 通过
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口

## 验收范围

- **基线**:`5d6e93d`(阶段 A 已验收);验收对象 = commit `fb9ac08`
- **对应 plan**:`phase5-auth-grpc.md` 阶段 B
- **涉及文件**:`include/agent/rate_limiter.hpp`、`src/agent/rate_limiter.cpp`、`apps/api_server/main.cpp`、`scripts/e2e_multi_turn.py`(S8 段)、`tests/test_rate_limiter.cpp`

## 规划层核对

| 验收点(plan 阶段 B) | 结果 | 证据 |
|---|---|---|
| 内存令牌桶,user_id 为主 key | ✅ | 代码审:rate_limiter.cpp 令牌桶实现;key 回退链 user_id→API key→IP |
| `RATE_LIMIT_RPS`/`RATE_LIMIT_BURST` env,空/0=不限 | ✅ | 默认不限时 e2e 原 51 项零回归(实证) |
| 超限 429 + Retry-After | ✅ | e2e S8:burst_has_429 / retry_after_header / body_mentions_limit |
| SSE 按请求计 1 次 | ✅ | 代码审:限流判定在 handler 入口,与连接时长无关 |
| metrics 加限流计数器 | ✅ | e2e S8:metrics_readable / metrics_rate_limited_count 实查通过 |
| 恢复行为 | ✅ | e2e S8:recovers_after_refill(令牌回补后恢复 200) |

## 契约层核对

默认关闭逐字节同前 ✅;429 为新错误路径,不改 200 形状;无新依赖。

## 实证层

- 本 commit 与上一轮巡检实测代码**逐字节一致**(`git diff fb9ac08 -- <相关文件>` 为空),直接沿用该轮实证:CTest **114/114**、e2e 离线 **67/67**(含 S8 限流 7 项)
- 本轮未重跑构建:工作区正在推进阶段 C(gRPC 工具链,`build-grpc/` 构建中),避免构建目录冲突

## 差距清单

无。

## 下一阶段建议

阶段 C 是最大风险点(gRPC FetchContent 源码构建 30~60 分钟)。验收口径已明确:OFF 构建与现状逐字节一致 + ON 构建通过 + 构建时长记录;超 60 分钟或 MSVC 坑超预期时,plan 授权执行窗口回报你走备选路线评审。
