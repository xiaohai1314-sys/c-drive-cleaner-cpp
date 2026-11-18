# 前端桥接（WebView2 宿主 + Vue 适配）

切片 5 的交付说明。命令表见 `docs/bridge-commands.md`；架构契约见
`docs/ARCHITECTURE.md` §3–§4。

## 本切片写的文件

| 文件 | 作用 |
|---|---|
| `host/bridge.h` / `host/bridge.cpp` | 原生 COM 消息桥：信封解析、命令分发（工作线程）、回复队列（UI 线程 ExecuteScript）、事件推送 |
| `host/host.cpp` | 宿主外壳：无边框窗口、`https://cdc.local/` → `dist/` 资源映射、WebView2 启动、`window_*` 命令、消息循环。`src/main.c` 调 `cdc_host_run()` 启动 GUI |
| `host/CMakeLists.txt` | 宿主静态库目标（切片 1 的顶层 CMake 用 `add_subdirectory(host)` 接入） |
| `web/src/api/bridge.ts` | 前端桥：`invoke(cmd, args)`（与 Tauri 同签名）、id 配对、超时、事件订阅 |
| `web/src/api/tauri.ts` | 薄 shim：优先桥，降级到 Tauri `invoke`，再降级抛错 |
| `web/src/api/types.ts` | 增补 `IndexVolumeState` / `IndexState` / `IndexTaskState` |
| `web/src/stores/index.ts` | 索引状态 store（模块级 ref，与 dup/big store 同模式） |
| `web/src/components/IndexBar.vue` | 索引状态行 + 计划任务行（DupPage / BigPage 共用） |
| `web/src/pages/DupPage.vue` / `BigPage.vue` | 接入 IndexBar；扫描前刷新索引状态/计划任务；同步完成后重新扫描 |
| `tests/test_bridge.mjs` | 桥协议测试（Node，mock `chrome.webview`） |

## 协议

```
JS -> C : chrome.webview.postMessage({ id, cmd, args })
C  -> JS: window.__cdc_onMessage({ id, ok: true,  data })   // 成功
C  -> JS: window.__cdc_onMessage({ id, ok: false, error })  // 失败，error 为字符串
C  -> JS: window.__cdc_onMessage({ event, data })           // 推送，无 id
```

> 直接 post 对象，不能包 `JSON.stringify`：宿主读的是 `WebMessageAsJson`，
> 字符串会被它再包一层引号，信封解析失败且无回包，全部调用永远挂起
> （v0.1.0 的 bug；见 `web/src/api/bridge.ts` 文件头注释）。

- `id` 是前端自增整数；C 侧原样带回，前端按 id 配对 Promise。
- `args` 省略时前端发 `{}`；后端 handler 只取自己需要的键，忽略多余字段。
- 前端默认超时 5 分钟（`bridge.ts` 的 `DEFAULT_TIMEOUT_MS`），长命令
  （`scan_dups` / `scan_big` / `index_rebuild` 等）调用方应传更大的
  `timeoutMs`，0 表示不限。Tauri `invoke` 本身不超时，这是桥模式的新
  约束，不是后端行为变化。
- 进度仍走轮询（`scan_status` 500ms、`task_status`），与 Rust 版一致；
  事件通道（`cdc_bridge_push_event` / `onEvent`）已接好，留给后续的
  流式进度 / 增量结果。

## 构建接线：web/dist 产出位置与 host 加载方式

```
web/                    # Vue 3 + Vite + TypeScript
  npm ci && npm run build     # vue-tsc --noEmit && vite build
  └─ dist/                    # outDir: dist（vite.config.ts），base: "./"
       index.html
       assets/index-*.js      # ES module
       assets/index-*.css
          │
          ▼  打包时与 cdc.exe 放同一目录（见 ARCHITECTURE.md §4：先出
             cdc.exe + dist/ + rules.json 能跑；NSIS 随后）
<exe dir>/
  cdc.exe
  dist/                   # 宿主在启动时取 exe 所在目录 + "dist\"
  rules.json
```

宿主加载方式：**不是 `file://`**。Vite 产物是 ES module 脚本，Chromium
会以 CORS 为由拦截 `file://` 源的模块加载，页面白屏——这是任务原文
"host 以 file:// 加载" 在实现时发现走不通的地方，改动如下：

- `host.cpp` 把 `https://cdc.local/` 映射到 `dist/`（WebView2
  `AddWebResourceRequestedFilter` + `WebResourceRequested`，按扩展名
  回 Content-Type；`..` 越界与绝对路径返回 403），
  启动后 `Navigate(L"https://cdc.local/index.html")`。
- 与 Tauri 的 asset 协议同一路数；`vite.config.ts` 的 `base: "./"` 不用改，
  相对路径资源在虚拟域名下正常解析。
- `index.html` 里不要出现以 `/` 开头的绝对路径资源（Vite 的 `base`
  已保证是相对路径）。

WebView2 用户数据目录：`%LOCALAPPDATA%\C盘清理工具\webview2`
（与 Rust 版日志/索引目录同一父目录）。

## webview/webview 替代方案评估（2026-11-17 读官方 README 核实）

结论：**建议切到 `webview/webview`（0.12.0），但本切片仍按任务要求先
交付了原生 COM 方案**；切换是一次性工作，见下。

| 维度 | 原生 COM（已交付） | webview/webview |
|---|---|---|
| 宿主代码量 | bridge.cpp + host.cpp 约 700 行 COM 样板（IUnknown、环境/控制器/资源/消息四个回调类、线程回 UI 的 PostMessage 队列） | 约 100 行：`webview_create` + `webview_bind("cdcInvoke", …)` + `webview_eval`；窗口、消息循环、线程回切（`webview_return` 内部走 dispatch，线程安全）全由库做 |
| JS 侧形态 | `chrome.webview.postMessage` + `window.__cdc_onMessage` + 前端自维护 id 配对 | `bind` 直接暴露一个返回 Promise 的 JS 函数（如 `window.cdcInvoke(cmd, args)`），id 配对由库做；`bridge.ts` 可瘦掉约一半 |
| MinGW 交叉编译 | 可（本机已验证，见下） | 官方明确支持：LLVM MinGW 在兼容列表里，有现成 MinGW 交叉 toolchain 文件；要求 C++14（宿主已是 C++17） |
| WebView2 依赖 | 仍需 WebView2 SDK 头文件（NuGet）做构建；运行时需用户机器装 Edge WebView2 Runtime | 同样需要 SDK 头文件做构建；但内置 loader（`WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL` 默认开）可省掉 `WebView2Loader.dll` 的分发 |
| 引入成本 | 零第三方依赖 | 一个依赖：FetchContent 拉 `webview/webview`（tag 0.12.0）或把 amalgamated 单头文件 vendoring 进 `third_party/`；MIT 协议 |
| 风险 | 自己维护的 COM 代码，出错面在我们手里（已尽量收敛） | 第三方库的版本升级、API 变化；但 14k star、2017 年至今的项目，C API 稳定 |

切换建议（何时做）：在切片 1 的 CMake 落地、顶层构建能跑通之后，
由宿主负责人一次性替换 `host/` 的实现层：保留 `bridge.h` 的
`cdc_command_handler` 注册表与命令名（后端不动），把 `bridge.cpp` 的
COM 部分换成 `webview_bind` 回调（在回调里按 `cmd` 查表分发，
`webview_return` 回包），`host.cpp` 换成 `webview_create`/`run`，
`https://cdc.local/` 资源映射改用 `webview` 的自定义 scheme 能力
（若库版本不支持，则保留一小段 WebView2 资源回调——那时宿主仍需
直接持有 `ICoreWebView2`，省事程度打折，需先验证）。
**不要**在多个切片并行期间切——host 接口正被 main.c 引用，切换窗口
选在 CMake 接线完成后。

## 已知缺口（不在本切片范围，留给后续）

1. **`convertFileSrc` / `imthumb`（已实现，2026-11-18）**：宿主的资源映射
   加了受控路由 `https://cdc.local/__imthumb__?path=<encodeURIComponent 后的路径>`，
   只服务此前 `im_scan` 返回过的路径（`im_is_scanned` 守门），并在 C 侧
   调 `im_thumbnail` 做 XOR 0x05 解密后按原图 MIME 回包；不是图片或不在
   扫描结果里一律 404。前端 `convertFileSrc(path, "imthumb")` 在桥模式下
   返回该 URL（见 `web/src/api/tauri.ts`）。
2. **`pickPaths`（已实现，2026-11-18）**：宿主命令 `pick_paths`
   （见 `docs/bridge-commands.md`「宿主侧补充命令」）用 `IFileOpenDialog`
   弹出原生文件对话框（多选；`kind: "folder"` 时加 `FOS_PICKFOLDERS`），
   用户取消返回 `[]`。对话框跑在桥的工作线程上，命令内自己
   `CoInitializeEx`。
3. **长命令超时**：默认 5 分钟；`scan_dups` 全盘大库、首次
   `index_rebuild` 可能更久，调用方按需传 `timeoutMs`（或 0 不限）。
   超时后命令在 C 侧继续跑（只断前端等待），重复点“立即同步”前
   `indexSyncing` 会拦住并发。
4. **WebView2 SDK 获取**：`host/CMakeLists.txt` 需要
   `CDC_WEBVIEW2_INCLUDE_DIR`（NuGet 包 `Microsoft.Web.WebView2` 的
   `build/native/include`）。CI 与用户机器构建时要先取包；本机
   交叉编译验证用的是 `/tmp` 里的最小桩头文件（见下），不是真 SDK。
5. **真机验证**：宿主是 Windows 专属，本机只做了交叉编译；窗口、
   资源映射、桥往返、计划任务行都要在用户机器上跑一遍才算数。

## 与 Rust 版行为不一致的地方

见 `docs/bridge-commands.md` 末尾（无参命令的 `args`、长命令超时、
`imthumb`、`pickPaths` 四项）。其余命令名、参数名、返回值形态与
Rust 版一致；索引 UI 的文案（`索引已同步（共 N 个文件，最后同步 …）`、
`计划任务：每日 03:00 自动同步（已启用）` 等）照搬 Rust 版旧 app.js
（Phase 3/4，commit 2ecf6ff），只补了“最后同步时间”这一项（任务要求）。
