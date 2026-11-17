# third_party —  vendored 第三方源码（不改上游文件）

本目录只放**直接取用的上游发布产物**，不做任何修改；升级时整
个替换对应子目录并更新下面的版本记录。

| 库 | 版本 | 来源 | 校验 |
| --- | --- | --- | --- |
| SQLite amalgamation | 3.54.1 | https://sqlite.org/2026/sqlite-amalgamation-3540100.zip | zip SHA-256 `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9` |
| BLAKE3（官方 C 实现） | 1.8.7 | https://github.com/BLAKE3-team/BLAKE3，tag `1.8.7` | commit `f3149ec5bb5449af877ba20377a11008ff499fa2` |
| cJSON | 1.7.19 | https://github.com/DaveGamble/cJSON，tag `v1.7.19` | commit `c859b25da02955fef659d658b8f324b5cde87be3` |
| xxHash（官方 C 实现） | 0.8.4 | https://github.com/Cyan4973/xxHash，tag `v0.8.4` | commit `c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0` |
| mimalloc（Microsoft 的 malloc 替代） | 3.7.0 | https://github.com/microsoft/mimalloc，tag `v3.7.0` | commit `1ed67b5156a763441d2ec3c20221a7b3cd866129` |

## 各目录内容（只取构建需要的最小集合）

- `sqlite3/`：`sqlite3.c` + `sqlite3.h`（amalgamation；`shell.c`、
  `sqlite3ext.h` 未取用）。许可证：公有领域（见 sqlite3.h 头注释）。
- `blake3/`：`blake3.{c,h}`、`blake3_dispatch.c`、`blake3_portable.c`、
  `blake3_impl.h` + SIMD 内联汇编替代版：`blake3_sse2.c`、`blake3_sse41.c`、
  `blake3_avx2.c`、`blake3_avx512.c`、`blake3_neon.c`。
  上游的 Unix/Windows 汇编版（`*.S` / `*.asm`）**有意未取**——内联函数
  （intrinsics）版在 gcc/clang/MSVC 三路都能编，省一套汇编构建分支。
  许可证：CC0 1.0 / Apache-2.0 双许可（见 blake3.h）。
- `cjson/`：`cJSON.c` + `cJSON.h`（`cJSON_Utils.*` 未取用）。
  许可证：MIT（见 cJSON.h）。
- `xxhash/`：`xxhash.c` + `xxhash.h`（`xxhsum`、`xxh_x86dispatch.c`、
  tests/docs 未取用）。采样层只用 XXH3-64（见 `src/sampling.c`）。
  许可证：BSD-2-Clause（见 xxhash.h 头注释）。
- `mimalloc/`：`include/`（`mimalloc.h`、`mimalloc-override.h`、
  `mimalloc-new-delete.h`、`mimalloc-profile.h`、`mimalloc-stats.h`、
  内部头 `mimalloc/*`）+ `src/`（`static.c` 单文件聚合整个库：
  `alloc/`、`arena/`、`heap/`、`prim/`、`profile/`、`util/`）。
  只编 `src/static.c` 一个翻译单元，不用上游的 CMake。
  全局替换方式：编译自家源码（含 sqlite3.c / cJSON.c）时
  `-include mimalloc-override.h`（见 Makefile 的 CDC_MIMALLOC 一节）；
  mimalloc 自身编译时不加该宏。`test/`、`bin/`、`doc/`、`cmake/`、
  `contrib/` 未取用。
  许可证：MIT（见 LICENSE）。

## 升级方法

```sh
# SQLite：到 https://sqlite.org/download.html 找最新 amalgamation zip
# BLAKE3 / cJSON / xxHash：git ls-remote --tags <仓库地址> 确认最新 tag 后
git clone --depth 1 --branch <tag> <仓库地址> /tmp/<name>
# 按上面“各目录内容”把文件拷回来，更新本文件版本号 + 校验值，
# 再跑一遍 make check 与 Windows 交叉编译。
```
