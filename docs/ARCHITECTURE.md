# c-drive-cleaner C + Vue 3 重写 — 架构契约

目标：把 Rust + Tauri 版（参考实现：`~/workspace/cdc-main`，分支状态 2ecf6ff，含常驻索引 phases 1–4）完整移植为 C11 后端 + Vue 3 前端。
工作目录：`~/workspace/cdc-c/`。仓库：`xiaohai1314-sys/c-drive-cleaner-c`（公开，2026-11-17 新建）。

## 1. 目录布局

```
cdc-c/
  CMakeLists.txt            # 顶层构建：MinGW 交叉编译（本机）+ MSVC（用户机器）
  src/                      # C11 后端
    main.c                  # 入口：CLI (--index/--selftest) + GUI 启动
    util.{c,h}              # 动态数组、字符串、UTF-8/UTF-16 转换、日志、错误码
    mft.{c,h}               # $MFT 解析 + USN Journal（对应 mft.rs）
    fastio.{c,h}            # raw 卷直读 + IOCP（对应 fastio.rs）
    hashcache.{c,h}         # blake3 哈希缓存 SQLite（对应 hashcache.rs）
    scan.{c,h}              # scan_big / scan_dups 分阶段哈希流水线（对应 main.rs 扫描部分）
    safety.{c,h}            # 安全层（对应 safety.rs，逐行移植，行为不变）
    rules.{c,h}             # rules.json 解析/校验/远程更新（对应 rules.rs）
    fileindex.{c,h}         # 常驻索引 SQLite + USN 增量（对应 fileindex.rs）
    indextask.{c,h}         # 计划任务 schtasks + XML（对应 indextask.rs）
    privacy.{c,h}           # 浏览器/注册表清理（对应 privacy.rs）
    system.{c,h}            # 系统清理：休眠/DISM/碎片等（对应 system.rs + frag.rs）
    im.{c,h}                # 微信/QQ 搬家（对应 im.rs）
    shred.{c,h}             # 粉碎（对应 shred.rs）
    process.{c,h} recycle.{c,h} logs.{c,h} timeutil.{c,h}
    bridge.{c,h}            # 与前端的消息桥（见 §3）
  include/                  # 公共头（FileEntry/DupGroup/BigFile 等共享结构）
  third_party/
    sqlite3.{c,h}           # SQLite amalgamation（直接取用）
    blake3/                 # BLAKE3 官方 C 实现（blake3.c/dispatch/portable + x86 SIMD）
    cjson.{c,h}             # cJSON（rules.json 解析）
  host/                     # WebView2 宿主（C++，薄层；见 §4）
    host.cpp                # 建窗口、嵌 WebView2、接 bridge
  ui/                       # Vue 3 前端
    package.json vite.config.js index.html
    src/ App.vue views/ components/ api.js（桥调用封装）
    dist/                   # Vite 构建产物（宿主加载 file://dist/index.html）
  packaging/
    installer.nsi           # NSIS（MSI 后续再说）
  docs/ARCHITECTURE.md      # 本文件
```

## 2. C 后端约定（所有人遵守）

- 语言：C11，不用 GCC/MSVC 专属扩展。字符串内部一律 UTF-8，调 Win32 API 前再转 UTF-16（`util.h` 提供 `utf8_to_wide` / `wide_to_utf8`，调用方负责 free）。
- 内存：显式所有权，头文件里用注释写明谁 free。动态数组用 `util.h` 的 growable array，不许手写 realloc 模板。
- 错误：函数返回 `int`，0=成功，非 0=错误码；出错时填 `cdc_err_t`（码 + 一句话）。不许静默吞错。
- 日志：写 `%LOCALAPPDATA%\C盘清理工具\logs\`，沿用 Rust 版文件名（`index-YYYYMMDD.log` 等）。
- **安全层是红线**：`safety.c` 逐行移植 `safety.rs`，行为一字不改。所有规则清理一律按 untrusted=true 走：`is_system_path` 或不在 allowlist 内的一律禁删。`scan` 计数与 `clean` 执行必须过同一道门（修复 Rust 版已知的“计数不过门、清理过门”不一致）。
- 分档阈值：大文件沿用已落地的 Duba 分档（exe|msi 100MB、视频/tmp 20MB、音频/压缩包/apk 5MB、文档/图片/日志 2MB、其他 50MB），`scan_big` 在 2MB 起枚举、按类型过滤。
- 去重流水线：大小分组 → head 4KB → sample 16KB×3 → probe 64KB@25%/75%（>16MB）→ full hash；fast 模式 strong 即终局。删除时必须重新 full-hash 候选与保留件双向验证后才删（与 Rust 版一致）。
- 常驻索引：独立库 `file-index-v1.db`，PK (volume_serial, frn)；USN 增量，changed FRN > 25000 → 全量重建；游标先于哈希落盘（中断可续跑）；`--index` 非管理员直接 exit 2，不弹 UAC。

## 3. 前后端消息桥（C ↔ Vue，JSON over WebView2 postMessage）

- JS → C：`chrome.webview.postMessage(JSON.stringify({id, cmd, args}))`。
- C → JS：`host.eval("window.__cdc_onMessage(" + json + ")")`；Vue 侧 `api.js` 按 id 配对 Promise，另收 `event` 推送（进度/流式结果）。
- 命令清单以 `main.rs` 里 `#[tauri::command]` 的函数为准，**名字与参数原样镜像**，包括：scan_big、scan_dups、clean、delete_dups、index_status、index_sync_now、index_rebuild、index_task_status/run/set_enabled/register/delete、frag、privacy、im、system 各命令。完整清单见 `docs/bridge-commands.md`（45 个命令，已抽完，2026-11-17）；实现见 `host/bridge.{h,cpp}` 与 `web/src/api/bridge.ts`，接线细节见 `docs/frontend-bridge.md`。
- 数据 JSON 形态与 Rust 版 `invoke` 返回一致：FileEntry{path,size,mtime,frn}、DupGroup、BigFile。时间用毫秒时间戳。
- 注意：宿主不走 `file://` 直接加载 dist（Vite 的 ES module 脚本会被 Chromium 以 CORS 拦截），改用 `https://cdc.local/` 虚拟域名映射 dist/（`host/host.cpp` 的 WebResourceRequested 回调），见 `docs/frontend-bridge.md`「构建接线」。

## 4. 宿主与构建

- 宿主：优先用开源 `webview/webview` 库（C API：create/bind/eval/run）；评估者先确认它在 MinGW 下能编过、能跑通 postMessage 双向；若不行，退回手写 WebView2 COM（C++）。评估已完成（2026-11-17，见 `docs/frontend-bridge.md`「webview/webview 替代方案评估」）：建议切，但先交付的原生 COM 方案（`host/`）保持可用，切换窗口选在 CMake 接线完成后一次性做。
- Vue：Vue 3 + Vite，单页应用，8 个页面做 tab 视图（不用 vue-router 也行）；`index.html`、`style.css`、`app.js` 的现有视觉与文案尽量复用，组件化重写。
- 构建：CMake。Debug/Release；Release 开 `-O2 -s`（strip）。本机用 llvm-mingw 交叉编译验证（`~/workspace/.scratch/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64/bin`），用户机器用 MSVC 或 MinGW。
- 打包：先出 `cdc.exe + dist/ + rules.json` 能跑；NSIS 安装器随后；MSI 再后。

## 5. 验证（本机无 Windows 的上限）

- 每块都要过 `x86_64-w64-mingw32-gcc` 交叉编译，0 error 0 warning。
- 纯逻辑函数（阈值、路径判定、USN gap 判定、SQL）必须有可跑的单测：Linux 下用 gcc 编译跑通，断言数写进提交说明。
- 真机验证（NTFS、计划任务触发、UAC、删除实效）只能在用户机器上做，留待用户跑 `--selftest` / `--index`。
