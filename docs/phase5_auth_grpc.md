# Phase 5 鉴权与限流 + gRPC 服务间通信试点 — 收口文档

日期:2026-08-27。对应 plan:`phase5-auth-grpc.md`(用户确认执行 A~E)。
提交链:`5d6e93d`(A 鉴权)→ `fb9ac08`(B 限流)→ `2f7a668`(C gRPC 工具链)→
`bdee99d`(D 双协议并存)→ 本文件随阶段 E commit。

## 1. 鉴权(阶段 A)

- 组件:`include/agent/api_auth.hpp` / `src/agent/api_auth.cpp`,纯函数式、零依赖。
- 配置:`AGENT_API_KEYS`(逗号分隔多 key;**空/未设 = 鉴权关闭**,默认行为逐字节同前)。
- 保护端点:`/v1/chat`、`/v1/chat/stream`、`/v1/feedback`、`/v1/metrics`
  (handler 顶部统一校验 `X-Api-Key` header)。`/v1/health` 与静态资源豁免(健康检查惯例)。
- 拒绝形状:401 `{"error": "missing or invalid API key", "trace_id": "t-...-authN"}`。
- 比较为恒定时间(长度差折入累加器、不跨 key 提前退出),防时序侧信道。
- 辅助:`AGENT_PORT`(默认 8080)允许起第二实例(e2e 专项依赖)。
- e2e S7(独立实例 :8081,AGENT_API_KEYS=test-key):无 key 401 / 错 key 401 /
  对 key 200 / stream·feedback·metrics 无 key 均 401 / health·静态豁免。离线 60/60。

## 2. 限流(阶段 B)

- 组件:`include/agent/rate_limiter.hpp` / `src/agent/rate_limiter.cpp`,内存令牌桶。
- 配置:`RATE_LIMIT_RPS` / `RATE_LIMIT_BURST`;**空/0 = 不限**。burst 缺省 max(1, rps)。
- 桶 key:user_id → API key → `anonymous`(coro 不暴露对端地址)。
- SSE 流式按请求计 1 次(入口扣令牌),不按连接时长。
- 超限:429 + `Retry-After` header + `{"error","retry_after_seconds","trace_id"}`。
  (coro 库补 `Status::TooManyRequests`,coro 仓库 7728256。)
- 指标:`/v1/metrics` 响应加 `api_guard{auth_rejected, rate_limited}` 内存计数器
  (additive 键,进程内计数,重启清零)。
- e2e S8(独立实例 :8082,RPS=2/BURST=2):6 连发 ≥3 个 429、Retry-After 存在、
  等 1.2s 恢复 200、metrics 计数器 ≥3。离线 67/67。

## 3. gRPC 试点(阶段 C/D)

架构(并存试点,非替换;HTTP JSON 契约面冻结):

```
api_server ──RETRIEVAL_PROTOCOL=grpc──> retrieval_service :8011 (gRPC, GRPC_PORT)
        ╲__RETRIEVAL_PROTOCOL=http(默认)_> retrieval_service :8001 (HTTP)
        gRPC 任何失败 → 逐调用回退 HTTP → 再失败走本地子串匹配(原降级链不变)
```

- 契约:`proto/retrieval.proto`,逐字段镜像 HTTP JSON 三端点。presence 语义用
  proto3 `optional` 保留(缺省≠0:max_price=0 是真实过滤,缺省=不过滤);
  `relaxed_level`/`effective_category` 仅放宽链触发时设置(同 Phase 3-C additive 规则)。
- C++:`GrpcRetrievalClient`(RETRIEVAL_PROTOCOL/RETRIEVAL_GRPC_ADDR 控制)。
  无 `ENABLE_GRPC` 编译时整个类是 HTTP 直通,OFF 构建行为逐字节同前。
- Python:`retrieval_service/grpc_server.py`,同进程同 handler 函数双协议复用;
  grpcio 缺失/端口占用自动降级 HTTP-only。stubs(`retrieval_service/gen/`)由
  `python scripts/gen_grpc_py.py` 再生成,已提交进库。
- 构建:`-DENABLE_GRPC=ON`(默认 OFF)。首次源码构建实测 **29 分钟**(<60 阈值);
  依赖全部树内(providers=module),无需 vcpkg/预装 protoc/Go。
- 矩阵:`python scripts/test_pg_retrieval.py` — 24 例 pg×json 一致 **且**
  24 例 grpc≡http(双后端);grpcio 缺失时协议维自动 SKIP 不影响原矩阵。

### 复现/踩坑记录(阶段 C)

1. github.com 直连间歇性超时;CMake FetchContent 子模块失败会清掉整棵源码树重来。
   解法(用户授权 ghfast.top 镜像后):手工 `git clone --depth 1 --branch v1.71.0`
   到 `build/grpc-local`,nested submodule 用命令级
   `git -c url."https://ghfast.top/https://github.com/".insteadOf="https://github.com/"
   submodule update --init --recursive --depth 1`(repo-local/命令级,不动全局配置);
   校验 HEAD SHA `4e9357bca…` 与上游一致;configure 加
   `-DFETCHCONTENT_SOURCE_DIR_GRPC=<path>` 免下载。
2. `install(EXPORT) ... absl_* not in any export set`:仅 `gRPC_INSTALL=OFF` 不够,
   须同时 `protobuf_INSTALL=OFF` + `utf8_range_ENABLE_INSTALL=OFF`(已内置于
   proto/CMakeLists.txt)。
3. `grpc::ClientContext` 不可拷贝——工厂返回 `unique_ptr`。
4. boringssl(absl lts_20240722 系)在 VS2022 下**不需要** Go 即可构建(预生成源码)。

### 回滚

- gRPC:不设 `RETRIEVAL_PROTOCOL`(或置 http)即完全走旧路径;二进制层面
  默认构建(ENABLE_GRPC=OFF)不含任何 gRPC 代码路径。
- 鉴权/限流:清掉 `AGENT_API_KEYS` / `RATE_LIMIT_*` 即恢复无保护默认。
- 代码级回滚:按提交链逆序 revert(每阶段一个 commit,D 依赖 C 的工具链)。

## 4. 测试证据汇总

| 项 | 结果 |
|---|---|
| 默认(OFF)构建 + CTest | 113 过 / 1 SKIP(PG live) |
| ENABLE_GRPC=ON 全量构建 | 通过(29 min);smoke 27B 往返 OK;单测 113 过 |
| e2e 离线(含 S7 鉴权 9 项、S8 限流 7 项) | 67/67 |
| 检索矩阵 pg×json | 24/24 |
| 检索矩阵 grpc≡http(双后端) | 24/24 |
| live:grpc 路径 chat 出单 | 2 items(gb-20001/gb-20003) |
| live:gRPC 端口死亡回退 HTTP | 出单一致(同 2 items) |
| banner | `Retrieval protocol: grpc (127.0.0.1:8011, HTTP fallback http://…)` |

## 5. 环境变量(新增)

| 变量 | 默认 | 说明 |
|---|---|---|
| `AGENT_API_KEYS` | 空=关 | API key 鉴权,逗号分隔 |
| `RATE_LIMIT_RPS` / `RATE_LIMIT_BURST` | 空=不限 | 每用户令牌桶 |
| `AGENT_PORT` | 8080 | api_server 端口(多实例) |
| `RETRIEVAL_PROTOCOL` | http | http \| grpc |
| `RETRIEVAL_GRPC_ADDR` | 空 | 如 127.0.0.1:8011 |
| `GRPC_PORT`(retrieval_service) | 空=关 | gRPC 前端端口 |
| `ENABLE_GRPC`(CMake) | OFF | gRPC 工具链开关 |
