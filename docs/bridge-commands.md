# 前后端命令表（C 桥 ↔ Vue）

来源：Rust 版 `~/workspace/cdc-main/desktop/src/main.rs` 的 `invoke_handler`
（分支 `index/merge-main-3cf8b26`，commit b837240）——45 个命令。
C 桥的命令名、参数名、返回值 JSON 形态与 Tauri 版**一字不改**，前端
`web/src/api/bridge.ts` 的 `invoke(cmd, args)` 与 Tauri `invoke` 签名一致，
各页面无需改业务代码。

约定：

- JS → C：`chrome.webview.postMessage(JSON.stringify({ id, cmd, args }))`，
  `args` 省略时按 `{}` 处理（Tauri 侧无参命令本来就不传 args）。
- C → JS：`window.__cdc_onMessage({ id, ok: true, data })` 或
  `{ id, ok: false, error }`。`data` 是命令返回值的 JSON（与 Tauri
  `invoke` 的返回值一致）；失败时 `error` 为字符串（对应 Rust 的
  `Result<T, String>` 的 `Err`，或桥自身的错误说明）。
- 时间一律毫秒/秒时间戳，与 Rust 版一致（`mtime` 是 unix 秒；
  `TaskStatus.started_ms / finished_ms` 是毫秒）。
- 标注 `task` 的命令会在后台任务表里建一条任务，前端用 `task_status`
  轮询；标注 `scan` 的命令用 `scan_status` 轮询进度。
- `window_*` 三个命令是 C 宿主侧的补充（Rust 版由 Tauri 窗口 API 提供），
  不在 Rust 命令表里。

## 扫描与清理

| 命令 | 参数（Rust 签名） | 返回 | 说明 |
|---|---|---|---|
| `disk_info` | — | `DiskInfo`（`Result`，失败为字符串） | C 盘总/已用/可用字节 |
| `scan_status` | — | `Record<string, ScanStatus>` | 轮询扫描进度（500ms 一次） |
| `cancel_scan` | `kind?: string` | `()` | `kind` 为 `"dup"`/`"big"`，省略则全取消 |
| `task_status` | — | `Record<string, TaskStatus>` | 后台任务表 |
| `cancel_task` | `id?: string` | `bool` | 取消指定任务 |
| `scan_categories` | — | `CategorySize[]` | C 盘瘦身扫描（task） |
| `scan_big` | `drives?: string[]` | `BigFile[]` | 大文件扫描（scan；分档阈值见架构 §2） |
| `scan_dups` | `root?: string, min_size?: number, drives?: string[], fast?: boolean` | `DupGroup[]` | 重复文件扫描（scan） |
| `delete_recycle` | `paths: string[]` | `DeleteReport` | 大文件/通用文件移入回收站 |
| `delete_dups` | `items: VerifyItem[]`（`{path, hash, size, mtime, keeper}`） | `DeleteReport` | 删除前重新 full-hash 双向验证 |
| `clean_categories` | `keys: string[]` | `DeleteReport` | 按规则清理（安全层 untrusted=true） |

## 索引（常驻索引 Phases 1–4）

| 命令 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `index_status` | — | `IndexState { volumes: IndexVolumeState[] }` | 只读。逐卷：`drive`、`state`（`fresh \| behind \| stale \| missing \| unavailable`）、`files`（索引行数）、`with_full`（有完整哈希的行数）、`updated_at`（USN 游标更新时间，unix 秒，0 表示无游标） |
| `index_sync_now` | — | `string`（摘要） | 需管理员；增量同步全部固定盘；返回人类可读摘要，前端 toast 显示 |
| `index_rebuild` | — | `string`（摘要） | 需管理员；清掉全部索引行和游标后全量重建；耗时，前端需二次确认 |
| `index_task_status` | — | `IndexTaskState { exists, enabled }` | 只读；计划任务 `\CDriveCleaner\Index` 是否已注册、是否启用 |
| `index_task_run` | — | `string` | 手动跑一次计划任务（后台执行）；也是 S4U 提权的验证入口 |
| `index_task_set_enabled` | `enabled: bool` | `string` | 启用/禁用计划任务 |
| `index_task_register` | — | `string` | 手动重注册（启动时已提权的前提下后台已幂等注册过一次） |
| `index_task_delete` | — | `string` | 删除计划任务；**UI 不提供按钮**（卸载流程用） |

## 碎片 / 系统

| 命令 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `list_drives` | — | `string[]` | 固定盘列表（如 `["C:\\", "D:\\"]`） |
| `frag_analyze` | `drive: string` | `FragReport` | 磁盘碎片分析（task） |
| `frag_optimize` | `drive: string` | `()`（`Result`） | 磁盘优化（task，可取消） |
| `system_scan` | — | `SystemItem[]` | 系统项扫描（task） |
| `system_clean` | `key: string` | `string`（`Result`） | 清理单个系统项；`sys_update_dl` / `sys_crash_dumps` 可取消 |
| `hiber_query` | — | `HiberInfo` | 休眠状态 |
| `hiber_set` | `mode: string` | `string`（`Result`） | 开/关休眠 |
| `recycle_empty` | — | `()`（`Result`） | 清空全部回收站（task） |
| `user_profile` | — | `string` | `%USERPROFILE%` |

## 微信 / QQ

| 命令 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `im_roots` | `kind: string` | `string[]` | 微信/QQ 数据目录 |
| `im_scan` | `kind: string` | `ImRecord[]` | 聊天文件扫描（task） |
| `im_preview` | `path: string` | `ImPreview`（`Result`） | 文本/hex 预览；仅限已扫描到的文件 |
| `im_clean` | `kind: string, kinds: string[], paths?: string[]` | `DeleteReport` | 清理聊天文件（task，可取消） |
| `im_move_analyze` | `kind: string` | `MoveAnalysis` | 搬家分析（task） |
| `im_move_execute` | `source: string, target_dir: string, kind: string` | `string`（`Result`） | 执行搬家（task，可取消） |

## 隐私 / 粉碎 / 规则 / 历史 / 其他

| 命令 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `privacy_scan` | — | `PrivacyItem[]` | 隐私痕迹扫描（task） |
| `privacy_clean` | `ids: string[]` | `PrivacyReport` | 清理隐私痕迹（task，可取消） |
| `shred` | `paths: string[], passes?: number` | `ShredReport` | 文件粉碎（task，可取消；`passes` 默认 1） |
| `history_list` | — | `LogEntry[]`（`{name, size, mtime}`） | 清理历史日志列表 |
| `history_read` | `name: string` | `string`（`Result`） | 读一条历史日志 |
| `rules_update` | — | `string`（`Result`） | 下载远程规则（task） |
| `rules_rollback` | — | `string`（`Result`） | 回滚到上一版规则 |
| `reveal_path` | `path: string` | `()`（`Result`） | 资源管理器中定位 |
| `open_path` | `path: string` | `()`（`Result`） | 打开文件 |
| `check_processes` | `targets: string[]` | `string[]` | 哪些目标进程正在运行 |
| `kill_process` | `name: string` | `number`（`Result`） | 按名杀进程，返回杀掉的数量 |

## 宿主侧补充命令（C host 提供，不走后端）

| 命令 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `window_minimize` | — | `null` | 最小化主窗口（供 `WindowBar.vue`） |
| `window_maximize` | — | `null` | 最大化/还原切换 |
| `window_close` | — | `null` | 关闭主窗口 |
| `pick_paths` | `kind: "file" \| "folder"` | `string[]` | 原生文件/文件夹选择对话框（供 `ShredPage.vue`）；用户取消返回 `[]` |

## 与 Rust 版行为不一致的地方（已知）

1. **无参命令的 `args`**：Tauri 侧无参命令不传 args；C 桥收到 `{}` 时按无参
   处理，后端 handler 收到空对象时应忽略未知字段（cJSON 解析只取
   需要的键）。
2. **长命令无内置超时**：Tauri `invoke` 本身不超时；桥前端默认 5 分钟
   超时（`bridge.ts` 可按命令改），`scan_dups` / `scan_big` /
   `index_rebuild` 等长命令调用方应传更大的 `timeoutMs`（见
   `docs/frontend-bridge.md`）。
3. **`imthumb` / `convertFileSrc`（已实现，2026-11-18）**：宿主在
   `https://cdc.local/__imthumb__?path=…` 挂了受控路由，只服务
   `im_scan` 返回过的路径，C 侧做 XOR 0x05 解密后按原图 MIME 回包；
   前端 `convertFileSrc(path, "imthumb")` 在桥模式下返回该 URL。
4. **`pickPaths`（已实现，2026-11-18）**：宿主命令 `pick_paths`
   （见下「宿主侧补充命令」）弹出原生文件对话框，用户取消返回 `[]`。
