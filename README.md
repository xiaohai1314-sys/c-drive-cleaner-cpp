# c-drive-cleaner-c

C11 rewrite of [c-drive-cleaner](https://github.com/xiaohai1314-sys/c-drive-cleaner):
C backend (MFT/USN/index/safety) + Vue 3 frontend via WebView2.

Rust 版的问题只有一个：编译太慢。搜索速度的瓶颈在磁盘 I/O，换语言省不了；
省的是编译时间（C 干净编译秒级）和二进制体积（试点 exe 77KB）。

## 当前状态：种子

- `src/`：已移植的纯逻辑（试点验证通过）——`mft_util.c`（$MFT 解析函数）
  与 `bigfile.c`（10 档大文件阈值，与 Rust 版逐条一致），见 `docs/pilot.md`
- `src/mft_raw.c`：Windows 卷探测骨架（`CreateFileW(\\.\C:)` + `FSCTL_GET_NTFS_VOLUME_DATA`）
- `web/`：Vue 3 + Vite + TypeScript 前端（取自 Rust 版 main 3cf8b26，`npm run build` 通过）
- `docs/ARCHITECTURE.md`：架构契约——模块划分、API 约定、移植顺序

还没做的事：`parse_record_in_place`（$MFT 完整解析）、USN/快照/索引、
安全层、删除逻辑、WebView2 宿主与前后端桥接。按 `docs/ARCHITECTURE.md`
分片做，每片带自测 + Windows 交叉编译过关。

## 切片 1：基础层（已完成）

- `src/util.{c,h}`：`cdc_buf_t` / `cdc_vec_t` 动态数组、UTF-8/UTF-16
  转换（`cdc_utf8_to_wide` / `cdc_wide_to_utf8`）、日志 `cdc_log`
  （写 `%LOCALAPPDATA%\C盘清理工具\logs\`，与 Rust 版同目录）、错误码
  `cdc_err_t` 与 `#define CDC_OK 0`。自测 `tests/test_util.c`。
- `third_party/`：SQLite amalgamation 3.54.1、BLAKE3 官方 C 实现 1.8.7、
  cJSON 1.7.19（版本与来源见 `third_party/README.md`）。
- 顶层 `CMakeLists.txt` + `cmake/toolchain-mingw64.cmake` +
  `CMakePresets.json`：MinGW 交叉编译（llvm-mingw）与 MSVC 两条路，
  C11，`-Wall -Wextra -Werror`（MSVC：`/W4 /WX`，只加给自家目标）。
- `host/`：由切片 5 提供的原生 COM WebView2 宿主 + 消息桥（`cdc_host`
  静态库，`bridge.cpp` + `host.cpp`，已交叉编译验证）；顶层
  `CMakeLists.txt` 只做 `add_subdirectory(host)` 接入，并归一化
  `CDC_WEBVIEW2_INCLUDE_DIR`（NuGet 包 `Microsoft.Web.WebView2` 的
  `build/native/include`，以 `-D` 传入）。

## 构建

```sh
# C 后端自测 + 编译（Linux 原生 / Windows 交叉）
make check
make time-build

# CMake（与 Makefile 等价，另加第三方库与 host 目标）
cmake --preset native && cmake --build --preset native && ctest --preset native
cmake --preset mingw64 && cmake --build --preset mingw64
# host（WebView2）需要 SDK 头文件：以 -D 传入 NuGet 包的 include 目录
#   cmake --preset mingw64 -DCDC_WEBVIEW2_INCLUDE_DIR=<.../Microsoft.Web.WebView2/build/native/include>
# 不传时只报 WARNING，host 编译会在缺 WebView2.h 时失败；本机验证可用
# 切片 5 的桩头文件 /tmp/cdc-host-test/include（仅供交叉编译验证，不当真 SDK）。

# 前端
cd web && npm ci && npm run build
```

Windows 交叉编译用 llvm-mingw（`x86_64-w64-mingw32-gcc`）。

## 参考实现

Rust + Tauri 版：`xiaohai1314-sys/c-drive-cleaner`（main 3cf8b26，
含常驻索引 phases 1–4 的本地合入分支 `index/merge-main-3cf8b26`）。
