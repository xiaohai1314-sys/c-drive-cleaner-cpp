/* system.h — 系统级深度瘦身与维护，移植自 Rust 版 desktop/src/system.rs：
 * Windows 更新/组件库清理、休眠文件管理、全局回收站清空、资源管理器
 * 联动、微信/QQ 数据迁移（搬家 + NTFS Junction + 恢复日志）。
 * 另含 main.rs 的系统路径辅助函数：cdc_system32_path、
 * cdc_fixed_drives、cdc_drive_free_space。 */
#ifndef CDC_SYSTEM_H
#define CDC_SYSTEM_H

#include <stddef.h>
#include <stdint.h>

#include "cdc_hooks.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* %SystemRoot%\System32\<name>（SystemRoot 未设置时退回 C:\Windows）。
 * 调用方 free()。 */
char *cdc_system32_path(const char *name);

/* 本机固定磁盘根目录（如 "C:\"），无则返回 ["C:\\"]。 */
int cdc_fixed_drives(char ***out, size_t *out_n, cdc_err_t *err);

/* GetDiskFreeSpaceExW：可用字节数与总字节数。 */
int cdc_drive_free_space(const char *drive, uint64_t *out_free,
                         uint64_t *out_total, cdc_err_t *err);

/* 在资源管理器中高亮选中指定文件或目录（explorer.exe /select）。
 * 路径不存在返回 CDC_ERR_NOT_FOUND。Windows 专属。 */
int cdc_reveal_in_explorer(const char *path, cdc_err_t *err);

/* 用系统默认程序或资源管理器打开指定路径。Windows 专属。 */
int cdc_open_path(const char *path, cdc_err_t *err);

typedef struct {
    int enabled; /* hiberfil.sys 是否存在 */
    uint64_t size;
    char *path;
    char *description;
} cdc_hiber_info_t;

void cdc_hiber_info_free(cdc_hiber_info_t *info);
int cdc_query_hiber(cdc_hiber_info_t *out, cdc_err_t *err);

/* mode: "off" / "reduced" / "full" | "on"；其它值返回
 * CDC_ERR_INVALID_ARG。成功时 *out_msg 为提示文案（调用方 free()）。 */
int cdc_set_hiber(const char *mode, char **out_msg, cdc_err_t *err);

/* SHEmptyRecycleBinW：清空全部磁盘的回收站。 */
int cdc_empty_all_recycle_bins(cdc_err_t *err);

typedef struct {
    char *key; /* sys_update_dl / sys_delivery_opt / sys_crash_dumps /
                  sys_dism_winsxs */
    char *name;
    char *describe;
    uint64_t size;
    uint64_t count;
    int safe_to_clean;
    int is_command;
} cdc_system_item_t;

void cdc_system_items_free(cdc_system_item_t *items, size_t n);
int cdc_scan_system_deep(cdc_system_item_t **out, size_t *out_n,
                         cdc_err_t *err);

/* 清理一项：成功时 *out_freed 与 *out_count 为释放字节/文件数，
 * *out_msg 为中文结果文案（调用方 free()）。 */
int cdc_clean_system_item(const char *key, uint64_t *out_freed,
                          uint64_t *out_count, char **out_msg,
                          cdc_err_t *err);

/* 注册进度/阶段/取消回调（任务 id "system" / "im_move"）。 */
void cdc_system_set_hooks(const cdc_hooks_t *hooks);

typedef struct {
    char *letter; /* 如 "D:\" */
    uint64_t free_bytes;
    uint64_t total_bytes;
} cdc_drive_target_t;

typedef struct {
    char *source;
    int exists;
    uint64_t size;
    uint64_t file_count;
    int is_junction;
    cdc_drive_target_t *targets;
    size_t ntargets;
} cdc_move_analysis_t;

void cdc_move_analysis_free(cdc_move_analysis_t *a);
int cdc_analyze_im_migration(const char *kind, cdc_move_analysis_t *out,
                             cdc_err_t *err);

/* 启动时调用：恢复上次中断的搬家（原目录改名后、建联接前中断时，
 * 把备份目录改回原名）。 */
void cdc_recover_pending_move(void);

/* 执行搬家：复制 → 校验 → 原目录改名备份 → mklink /J → 删备份。
 * 成功时 *out_msg 为结果文案（调用方 free()）。 */
int cdc_execute_im_migration(const char *source, const char *target_dir,
                             const char *kind, char **out_msg,
                             cdc_err_t *err);

/* 供自测使用的纯函数（与 Rust 版 tests 里的同名函数一致）。 */
int cdc_safe_for_quoted_cmd_path(const char *path);
int cdc_valid_recovery_paths(const char *source, const char *backup,
                             const char **allowed_roots, size_t nroots);

#ifdef __cplusplus
}
#endif

#endif /* CDC_SYSTEM_H */
