# 阿里云 ECS 部署记录（2026-09-17）

部署目标：8.148.148.94（阿里云广州，Ubuntu 22.04，2核4GiB）。
公网入口：**http://8.148.148.94:8080/** （安全组已放行 TCP 8080；80 与 8000 留给同机其他项目）。

## 拓扑

```
公网 → :8080 api_server (C++, systemd groupbuy-api)
         ├─ LLM_BASE_URL    → 127.0.0.1:8010 llm_gateway   (groupbuy-gateway)
         ├─ RETRIEVAL_URL   → 127.0.0.1:8001 retrieval     (groupbuy-retrieval, PG+vector)
         └─ RANKER_URL      → 127.0.0.1:8002 ranking       (groupbuy-ranking, shadow)
PostgreSQL 14 + pgvector 0.8.0 本机实例：groupbuy 库，9713 商品 + 22 知识段落，
512 维 bge-small-zh-v1.5 向量（HNSW 索引）。
```

注意网关端口是 **8010 而非默认 8000**——8000 被同机的排班演示容器占用。

## 关键步骤（可复现）

1. 系统包：`build-essential g++-12 cmake(≥3.25, pip 装) python3-venv postgresql
   postgresql-server-dev-14 libpq-dev`。pgvector 用源码编译（v0.8.0 tarball，
   `make && make install`）；Ubuntu 仓库无此包。
2. 代码包：`groupbuy + coro + build/_deps 的 *-src 源码缓存` 离线打包传服务器，
   CMake 用 `-DFETCHCONTENT_SOURCE_DIR_*` 指向本地缓存，sqlite3 源码预置到
   `build/_deps/sqlite3-src`，全程零下载编译。
3. 数据库：`CREATE ROLE agent LOGIN ...; CREATE DATABASE groupbuy OWNER agent;`
   应用 `sql/001_schema.sql` + `sql/002_vector.sql`，`GRANT ALL ON ALL TABLES`；
   然后 `scripts/pg_seed.py`（同步 9713 条）与 `scripts/pg_embed.py`
   （`HF_ENDPOINT=https://hf-mirror.com HF_HUB_DISABLE_XET=1`）。
4. Python venv：`psycopg[binary,pool] fastembed lightgbm`（阿里云 PyPI 镜像）。
5. systemd 单元 ×4 在 `/etc/systemd/system/groupbuy-*.service`，均
   `Restart=on-failure`，开机自启。api_server 环境：`AGENT_PORT=8080`、
   `CATALOG_BACKEND=postgres`、`RANKER_MODE=shadow`、`RATE_LIMIT_RPS=5`、
   `RATE_LIMIT_BURST=10`；PG_DSN 在 `/opt/groupbuy-agent/app/.env`（600 权限）。
   网关密钥在 `llm_gateway/.env.local`（600）。

## Linux 移植修复（已提交）

- `CMakeLists.txt`：`project(... LANGUAGES C CXX)`——sqlite3 是 C 文件，VS 生成器
  隐式启用 C，Makefile 生成器不会。
- `coro/src/scheduler.cpp`：补 `#include <mutex>`（GCC 不含隐式传递包含）。
- `service_circuit.hpp`：GCC bug 88165——带 NSDMI 的嵌套 struct 不能作默认实参，
  拆成两个构造函数。
- **GCC 11/12 协程 bug（本次最大的坑）**：堆分配的非平凡临时量绑定到协程
  const 引用形参、且跨越 co_await 挂起点时，帧清理阶段会被二次析构 →
  double-free/SIGABRT。MSVC 的堆不检测所以 Windows 从未暴露。修复方式是全部
  改为命名局部变量：`GetOrCreateSession` 的 optional 临时量、5 处
  `ConversationTurn{...}` 指定初始化临时量、`UpdateContext` 的字符串字面量
  （SSO 不会崩但同属 UB）、`ResponseComposer` 里转 std::function 的 lambda。
  Valgrind 证据：被 free 的指针落在协程帧块内部 2464 字节处。

## 验收结果

- `test_agent`（g++-12 Release）：**139/139 通过**（含 live-PG 用例）。
- 多轮对话（clarify→retrieve→retrieve）、SSE 流式（248 块）、feedback、
  版本健康检查全部通过；公网真实模型端到端通过。
- ranking 服务 `model_loaded=false` 为合法冷启动态；积累反馈后按
  CLAUDE.md 的训练流程产模型即可。

## 运维

```bash
systemctl status groupbuy-api groupbuy-gateway groupbuy-retrieval groupbuy-ranking
journalctl -u groupbuy-api -f
# 改代码后：本地改 → scp 到 /opt/groupbuy-agent/app → cmake --build build -j2
# → systemctl restart groupbuy-api（内存会话清空，Sessions 在 SQLite 里仍在）
```

未配置域名/HTTPS；鉴权关闭（AGENT_API_KEYS 空），仅靠限流，适合演示。
内部端口 8001/8002/8010 未在安全组放行，外部不可达。
