# c-drive-cleaner-cpp

Windows 磁盘清理工具的 C++20 移植版：原生后端负责 NTFS/MFT、索引、扫描与安全操作，Vue 3 + Vite + TypeScript 前端通过 WebView2 宿主与后端通信。仓库内的自有代码按 C++20 编译；`third_party/` 中的 SQLite、cJSON、BLAKE3、xxHash 和 mimalloc 保持各自的 C 实现。

## 当前状态与限制

项目已经不是早期的 C11“种子”：后端、索引任务、扫描/哈希、安全处理、GUI 命令适配层和 WebView2 消息桥均在仓库中。CI 覆盖 Linux 原生测试、Windows/MSVC 编译与关键测试、以及前端构建；这仍不等同于 Windows 真机验收。

当前仍有平台限制：

- Linux CI 不能验证 Windows NTFS 卷读取、USN、管理员权限、回收站/删除效果、计划任务、系统清理命令或 UAC 行为。
- GUI 只在 Windows 上启动；无参数运行会进入 WebView2 GUI。运行时需要用户机器安装 Edge WebView2 Runtime，并准备前端构建产物。
- Windows GUI 编译需要真实的 Microsoft WebView2 SDK（NuGet 包 `Microsoft.Web.WebView2`）头文件和 loader 库；仓库中的 `tools/webview2-stub` 只是交叉编译桩头，不能运行 GUI。
- `pickPaths`、图片缩略图等桥接能力在原生 WebView2 路径下仍有已知限制；长时间扫描/索引由后端继续执行，前端等待可能超时。详见 `docs/bridge-commands.md` 和 `docs/frontend-bridge.md`。
- Windows CI 使用真实 WebView2 SDK 编译并运行跨平台/Windows 单元测试，但不会启动 GUI，也不能替代实际 NTFS 卷、WebView2 Runtime、管理员权限、UAC 与计划任务的真机验收。

## 依赖

Linux 原生构建需要 GCC/G++、GNU make、pthread/dl 等系统开发环境。前端需要 Node.js 与 npm；锁文件在 `web/package-lock.json`，CI 使用 `npm ci`。

Windows 构建需要 CMake 3.20+ 或仓库 Makefile 所需的 MinGW/llvm-mingw 工具链。GUI 还需要：

- `CDC_WEBVIEW2_INCLUDE_DIR`：NuGet 包中的 `build/native/include`，其中包含 `WebView2.h`；
- `CDC_WEBVIEW2_LOADER_LIB`：MSVC 使用 `WebView2LoaderStatic.lib`，MinGW 使用对应的 `WebView2Loader.dll.lib`；
- 运行时安装 Edge WebView2 Runtime。

## 构建与测试

### Linux 原生（推荐的 CI 路径）

```sh
make check
```

这会编译 `build/native/cdc-cpp`、原生测试并运行全部 Makefile 测试，再运行 `cdc-cpp selftest`。测试需要时会通过 `tests/gen_fixtures.sh` 创建临时 fixture。清理构建目录：

```sh
make clean
```

等价的 CMake 路径为：

```sh
cmake --preset native
cmake --build --preset native
ctest --preset native
```

### 前端

```sh
cd web
npm ci
npm run build
```

`npm run build` 执行 TypeScript 检查后生成 Vite 产物到 `web/dist/`。桥接协议测试还可以在安装依赖后运行：

```sh
node tests/test_bridge.mjs
```

### Windows / WebView2

GitHub Actions 会在 Windows 2022/MSVC 上用固定版本的 WebView2 NuGet SDK 编译并运行测试；它不启动 GUI，也不验证用户机器上的 WebView2 Runtime 行为。

仅交叉编译验证（不能运行 GUI）可以使用仓库的桩头：

```sh
make win WEBVIEW2_INC=tools/webview2-stub
```

使用真实 SDK 时，将 `WEBVIEW2_INC` 指向 NuGet 包的 `build/native/include`，并提供 `WEBVIEW2_LOADER_LIB`；CMake 则传 `CDC_WEBVIEW2_INCLUDE_DIR` 和 `CDC_WEBVIEW2_LOADER_LIB`。MSVC 示例：

```sh
cmake -S . -B out/build/msvc -G "Visual Studio 17 2022" -A x64 \
  -DCDC_WEBVIEW2_INCLUDE_DIR=<path-to-package>/build/native/include \
  -DCDC_WEBVIEW2_LOADER_LIB=<path-to-package>/build/native/x64/WebView2LoaderStatic.lib
cmake --build out/build/msvc --config Release
ctest --test-dir out/build/msvc -C Release
```

## 使用

构建后，Linux 可运行与平台无关的 CLI 自测和目录大文件扫描：

```sh
build/native/cdc-cpp selftest
build/native/cdc-cpp big <directory>
```

在 Windows 上还可使用：

```text
cdc-cpp --index [--quiet] [--rebuild]
cdc-cpp probe <drive-letter>
```

`--index` 需要相应管理员权限；`probe` 用于 Windows 卷/NTFS 探测。无命令行参数时程序尝试启动 GUI，因此 Linux 构建不会提供可运行的 GUI。GUI 的完整命令和已知桥接差异见 `docs/bridge-commands.md`。

## 项目结构

- `src/`：C++20 后端、CLI、索引、扫描、安全处理和 GUI 命令适配。
- `host/`：Windows 原生 COM WebView2 宿主与消息桥。
- `web/`：Vue 3 + Vite + TypeScript 前端。
- `tests/`：C++ 原生测试、fixture 生成脚本和桥接协议测试。
- `CMakeLists.txt`、`CMakePresets.json`、`Makefile`：Linux、MinGW 交叉和 MSVC 构建入口。

## 参考实现

Rust + Tauri 版：[xiaohai1314-sys/c-drive-cleaner](https://github.com/xiaohai1314-sys/c-drive-cleaner)。
