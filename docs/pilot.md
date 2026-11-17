# c-drive-cleaner C 版小试点

**这是什么**：回应“cleaner 用 C 写会怎么样”的一小块验证——不是全量重写，
也不是对 Rust 版的替换。只把最热的两块纯逻辑移植到 C，实测编译速度和
逻辑一致性，让“换不换 C”这个决定有数据而不是凭感觉。

**位置**：本仓库的 `src/`（种子状态，见根 README）

## 移植了什么（逐行对照 Rust 版）

| C 文件 | 对照的 Rust 源码 | 内容 |
|---|---|---|
| `src/mft_util.c` | `desktop/src/mft.rs` | `u16/u32/u64_at`、`read_uint_le` / `read_int_le`、`filetime_to_unix`、`apply_fixups`、`parse_runlist`、`parse_first_extent`、`utf16le` |
| `src/bigfile.c` | `desktop/src/main.rs:697` `big_file_threshold_bytes` | 全部 10 档分档阈值（安装包 100MB / 视频·tmp 20MB / 音频·压缩包·apk 5MB / 图片·文档·日志 2MB / 兜底 50MB）+ `is_system_file_name` |
| `src/mft_raw.c` | `mft.rs` 的 Win32 部分 | 仅 Windows 编译：`CreateFileW(\\.\C:)` + `FSCTL_GET_NTFS_VOLUME_DATA` 读卷信息（`probe C`） |
| `src/main.c` | `main.rs:737` `scan_big` | `selftest`（31 项断言）+ `big <dir>`（2MB 预过滤 → 分档阈值 → 跳过系统文件 → 按大小降序） |

## 实测结果（2026-11-17，同一台机器）

- `make check`：**31 项自测全过**，包括与 Rust 测试
  `big_file_thresholds_follow_kbigfile_tiers` 逐条一致的 13 条阈值断言。
- `big` 扫描（11 个边界 fixture）：5 个应进的进了（150M exe / 60M bin /
  25M mkv / 6M mp3 / 3M jpg），6 个应排除的都排除了（3M mp4 未到 20M 档、
  1M jpg 未到 2M 预过滤、4M mp3 未到 5M 档、40M bin 未到 50M 兜底、
  90M exe 未到 100M 档、`pagefile.sys` 系统文件），按大小降序输出。
- **干净编译**：C 原生 `gcc -O2` **0.39 秒**；Windows 交叉
  （llvm-mingw `x86_64-w64-mingw32-gcc`，含 `windows.h`）**1.9 秒**，
  `-Wall -Wextra -Werror` 干净，产出 PE32+ exe（`build/cdc-c-pilot.exe`，77KB）。
- Rust 对照（同一台机器、刚测的）：`cargo check --target
  x86_64-pc-windows-gnu --all-targets` **2.6 秒**——但这是**热缓存、零改动**
  的增量检查，不是干净编译，不能直接跟 C 的干净编译比。干净的 Rust
  构建仍以 CI 为准：release 构建 5 分 20 秒（聊天里 11 月 16 日的数字）。
  诚实结论：C 的干净编译确实是秒级，比 Rust 快一个量级以上；但“日常改
  一行重编”的差距没有干净编译那么夸张，因为 Rust 增量检查本身也只有几秒。

## 没做什么（别误会这是重写）

- **没碰 $MFT 完整解析**：`parse_record_in_place`（约 180 行，文件名、
  父目录、属性过滤）还没移植，这是 MFT 部分最大的一块。
- **没碰 USN / 快照 / 索引 / 安全层 / 删除逻辑**：`fileindex.rs`、
  `safety.rs`、recycle/shred、规则引擎都没动——C 里这些是最容易出
  内存安全问题的地方，也是 Rust 版真正的价值所在。
- **没做界面**：Tauri 的 1700 行 HTML/JS 界面不在 pilot 里；全量换 C
  的界面三选一（Win32 手写 / WebView2 嵌现有界面 / 先只做命令行）
  还没定。
- **`big` 在 Linux 上是目录遍历**，不是 $MFT 直读：它验证的是阈值
  过滤逻辑，不是扫描速度。C 搜索不会比 Rust 快（瓶颈在磁盘 I/O），
  这一点 pilot 没有、也不打算证伪——聊天里已经说清楚了。

## 下一步（三选一，等你定）

1. **到此为止**：pilot 就当一次验证，Rust 版继续走常驻索引那条线。
2. **继续一小块**：移植 `parse_record_in_place` + `$MFT` 块读取，
   在 Windows 上对同一块盘跑 Rust 版和 C 版，对比枚举结果是否逐条一致。
3. **真开工全量换 C**：先定界面走法（建议 WebView2 嵌现有 HTML，
   改动最小），再按模块拆，每块都像这次一样带自测 + 交叉编译过关。

## 怎么跑

```sh
cd ~/workspace/cdc-c-pilot
make check        # 自测 31 项
make time-build   # 干净编译计时（原生 + Windows 交叉）
./build/cdc-c-pilot big /path/to/dir   # 大文件扫描
```
