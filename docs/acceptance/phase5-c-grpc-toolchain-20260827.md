# 验收报告:Phase 5 阶段 C — gRPC 工具链落地

- **结论**:✅ 通过
- **验收日期**:2026-08-27
- **验收人**:PM 验收窗口(三层实证验收)

## 验收范围

- **基线**:`fb9ac08`(阶段 B 已验收);验收对象 = commit `2f7a668`
- **对应 plan**:`phase5-auth-grpc.md` 阶段 C(最大风险阶段)
- **涉及文件**:`CMakeLists.txt`、`proto/{CMakeLists.txt,retrieval.proto,smoke/grpc_proto_smoke.cpp}`

## 规划层核对

| 验收点(plan 阶段 C) | 结果 | 证据 |
|---|---|---|
| `ENABLE_GRPC` 默认 OFF,OFF 时构建与现状一致 | ✅ | CMakeLists.txt:`option(... OFF)` + `if(ENABLE_GRPC)` 门控 add_subdirectory(proto);本窗口默认构建实证通过,全程未触碰 gRPC |
| ON 构建通过 + 生成代码可链接 | ✅(独立复跑) | `build-grpc/bin/Release/grpc_proto_smoke.exe` 存在且本窗口实跑输出 `grpc_proto_smoke OK (27 bytes)`,exit=0 |
| 不接任何业务 | ✅ | commit 仅 4 文件(CMake 门控 + proto 定义 + smoke),无业务代码 |
| 构建时长 <60 分钟阈值 | ✅ | commit 记录首构 29 分钟 |
| proto 字段与 HTTP JSON 契约一一对应 | ✅ | retrieval.proto 90 行:RetrieveDeals/RetrieveKb/Health,字段为既有 JSON 形状的映射,无语义新增 |
| 单测不破 | ✅ | 114/114(含 ON/OFF 两构) |

**踩坑记录核实**:commit 记录了 github 超时→手工 clone(经用户授权镜像,SHA 与上游一致)+ FETCHCONTENT_SOURCE_DIR_GRPC 免下载、安装导出开关联动——处理路径合规(用户授权、repo-local、SHA 校验)。

## 契约层核对

- HTTP JSON 契约面:✅ 冻结未动(阶段 C 零业务接入)
- FetchContent 依赖哲学一致(不引入 vcpkg 系统级安装):✅
- OFF 构建产物与现状等价:✅ 实证

## 实证层(本窗口独立实跑)

| 命令 | 结果 |
|---|---|
| `build_windows.ps1`(默认 OFF) | ✅ 构建通过,114/114 单测 |
| `build-grpc/bin/Release/grpc_proto_smoke.exe` | ✅ `OK (27 bytes)` exit=0 |
| ON 构建产物存在性 | ✅ api_server/test_agent/grpc_proto_smoke 三件齐 |

## 差距清单

无。流程备注:阶段 D 文件(grpc_retrieval_client、grpc_server.py 等)在 C 提交前已在工作区出现,但执行窗口守住了"C 先独立 commit"的隔离要求,回滚性未受损。

## 下一阶段建议

阶段 D(双协议并存)验收重点预告:协议×后端矩阵(http/grpc × pg/json)、`RETRIEVAL_PROTOCOL` 缺省零回归、**停 8011 验 gRPC→HTTP 自动回退**(降级链延伸的关键实证)。
