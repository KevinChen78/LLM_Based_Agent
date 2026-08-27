# 验收报告:Phase 5 阶段 A — API key 鉴权

- **结论**:✅ 通过
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`3d17b97`(Phase 3 收口);验收对象 = commit `5d6e93d`(Phase 5 阶段 A)
- **对应 plan**:`phase5-auth-grpc.md` 阶段 A(注:Phase 4 输出事实校验 Guard 按用户指令挂起不废,plan 头部已注明)
- **涉及文件**:`include/agent/api_auth.hpp`、`src/agent/api_auth.cpp`、`apps/api_server/main.cpp`、`scripts/e2e_multi_turn.py`、`tests/test_api_auth.cpp`

## 规划层核对

| 验收点(plan 阶段 A) | 结果 | 证据 |
|---|---|---|
| `AGENT_API_KEYS` 逗号分隔多 key,空=关闭 | ✅ | api_auth.cpp FromEnv;主实例 e2e 强制空 env 防泄漏 |
| 恒定时间比较防时序侧信道 | ✅ | KeysEqual:长度异或 + max(a,b) 全扫不提前退出;多 key 匹配扫完全部 key(代码注释与实现一致) |
| 四个受保护端点统一校验,401 {error, trace_id} | ✅ | e2e S7 实证:无 key/错 key 401、对 key 200 |
| /v1/health 与静态资源豁免 | ✅ | e2e S7 health_exempt/static_exempt 通过 |
| 鉴权关闭时行为逐字节同前 | ✅ | 默认(无 AGENT_API_KEYS)e2e 原 51 项全过 |
| e2e 第二实例专项 | ✅ | 独立 :8081 实例带 test-key,9 项鉴权检查全过 |

## 契约层核对

- 降级链/检索端点/stdlib 策略:✅ 未触碰(纯 C++,零新依赖)
- 401 响应为新错误路径,不改变 200 成功路径形状

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| build_windows.ps1 + CTest | ✅ **114/114**(含工作区未提交的阶段 B 限流用例一并构建通过;阶段 A 自身为 108+1SKIP 口径) |
| e2e 离线 | ✅ **67/67**(原 51 + S7 鉴权 9 项 + S8 限流 7 项——S8 为未提交的阶段 B 代码,提前验证通过) |

## 差距清单

无。

**说明**:本次构建/e2e 实测覆盖的是「阶段 A commit + 工作区阶段 B 未提交代码」的组合态,两者均通过;阶段 B 的正式验收待其 commit 后按 plan 阶段 B 验收点(429/Retry-After/metrics 计数器/默认不限零回归)单独进行——S8 七项已提前全过,预期无悬念。

## 下一阶段建议

阶段 C(gRPC FetchContent 源码构建)是项目迄今最重依赖,plan 已定 ENABLE_GRPC=OFF 默认隔离 + 60 分钟超时触发备选路线评审——验收时我将按「OFF 构建与现状逐字节一致 + ON 构建通过 + 构建时长记录」三点核对。
