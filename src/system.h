// system.h — 系统级深度瘦身与维护，移植自 Rust 版 desktop/src/system.rs：
// Windows 更新/组件库清理、休眠文件管理、全局回收站清空、资源管理器
// 联动、微信/QQ 数据迁移（搬家 + NTFS Junction + 恢复日志）。
// 另含 main.rs 的系统路径辅助函数：system32_path、
// fixed_drives、drive_free_space。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cdc_hooks.h"
#include "util.h"

namespace cdc {

/* %SystemRoot%\System32\<name>（SystemRoot 未设置时退回 C:\Windows）。 */
std::string system32_path(std::string_view name);

/* 本机固定磁盘根目录（如 "C:\"），无则返回 ["C:\\"]。 */
int fixed_drives(std::vector<std::string> *out, Error *err);

/* GetDiskFreeSpaceExW：可用字节数与总字节数。 */
int drive_free_space(std::string_view drive, uint64_t *out_free, uint64_t *out_total,
                     Error *err);

/* 在资源管理器中高亮选中指定文件或目录（explorer.exe /select）。
 * 路径不存在返回 kErrNotFound。Windows 专属。 */
int reveal_in_explorer(std::string_view path, Error *err);

/* 用系统默认程序或资源管理器打开指定路径。Windows 专属。 */
int open_path(std::string_view path, Error *err);

struct HiberInfo {
    bool enabled = false; /* hiberfil.sys 是否存在 */
    uint64_t size = 0;
    std::string path;
    std::string description;
};

int query_hiber(HiberInfo *out, Error *err);

/* mode: "off" / "reduced" / "full" | "on"；其它值返回
 * kErrInvalidArg。成功时 *out_msg 为提示文案。 */
int set_hiber(std::string_view mode, std::string *out_msg, Error *err);

/* SHEmptyRecycleBinW：清空全部磁盘的回收站。 */
int empty_all_recycle_bins(Error *err);

struct SystemItem {
    std::string key; /* sys_update_dl / sys_delivery_opt / sys_crash_dumps /
                        sys_dism_winsxs */
    std::string name;
    std::string describe;
    uint64_t size = 0;
    uint64_t count = 0;
    bool safe_to_clean = false;
    bool is_command = false;
};

int scan_system_deep(std::vector<SystemItem> *out, Error *err);

/* 清理一项：成功时 *out_freed 与 *out_count 为释放字节/文件数，
 * *out_msg 为中文结果文案。 */
int clean_system_item(std::string_view key, uint64_t *out_freed, uint64_t *out_count,
                      std::string *out_msg, Error *err);

/* 注册进度/阶段/取消回调（任务 id "system" / "im_move"）。 */
void system_set_hooks(const cdc_hooks_t *hooks);

struct DriveTarget {
    std::string letter; /* 如 "D:\" */
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
};

struct MoveAnalysis {
    std::string source;
    bool exists = false;
    uint64_t size = 0;
    uint64_t file_count = 0;
    bool is_junction = false;
    std::vector<DriveTarget> targets;
};

int analyze_im_migration(std::string_view kind, MoveAnalysis *out, Error *err);

/* 启动时调用：恢复上次中断的搬家（原目录改名后、建联接前中断时，
 * 把备份目录改回原名）。 */
void recover_pending_move();

/* 执行搬家：复制 → 校验 → 原目录改名备份 → mklink /J → 删备份。
 * 成功时 *out_msg 为结果文案。 */
int execute_im_migration(std::string_view source, std::string_view target_dir,
                         std::string_view kind, std::string *out_msg, Error *err);

/* 供自测使用的纯函数（与 Rust 版 tests 里的同名函数一致）。 */
bool safe_for_quoted_cmd_path(std::string_view path);
bool valid_recovery_paths(std::string_view source, std::string_view backup,
                          std::span<const std::string> allowed_roots);

}  // namespace cdc
