# Windows 构建指南

## 环境要求

当前开发环境（Git Bash / MSYS）未安装 C++ 编译器和 CMake，因此无法直接构建。请在 Windows 上安装以下任一工具链：

### 方案 A：Visual Studio 2022（推荐）

1. 下载并安装 [Visual Studio 2022 Community](https://visualstudio.microsoft.com/vs/community/)。
2. 在安装器中选择 **“使用 C++ 的桌面开发”** 工作负载。
3. 勾选 **“C++ CMake 工具”** 组件（包含 CMake 和 Ninja）。
4. 安装完成后，打开 **x64 Native Tools Command Prompt for VS 2022**，执行：

```powershell
cd d:\Working\LLM_Based_Agent
.\scripts\build_windows.ps1
```

### 方案 B：MSYS2 + MinGW-w64

1. 安装 [MSYS2](https://www.msys2.org/)。
2. 在 MSYS2 UCRT64 终端中执行：

```bash
pacman -S mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-ninja
```

3. 然后运行项目脚本：

```bash
cd /d/Working/LLM_Based_Agent
./scripts/build_phase0.sh
```

### 方案 C：vcpkg（可选）

若希望使用 vcpkg 管理 C++ 依赖，可安装 vcpkg 后在 CMake 中设置 toolchain：

```powershell
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

> 当前项目使用 FetchContent 管理 `nlohmann/json`、`spdlog`、`googletest`、`cpp-httplib`，无需 vcpkg。

## 当前状态

- 项目骨架、核心类、Mock 工具、HTTP Server 入口均已编写完成。
- 由于缺少编译器，尚未进行编译验证。
- 已知潜在编译问题已静态检查并修复（头文件包含等）。

## 建议后续步骤

1. 安装上述任一工具链。
2. 运行 `scripts/build_windows.ps1` 或 `scripts/build_phase0.sh`。
3. 修复首次编译暴露的问题。
4. 接入真实 LLM Gateway。
