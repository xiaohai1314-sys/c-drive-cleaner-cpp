/* gui.h — GUI 入口与 45 个后端命令的适配层（对照 Rust desktop/src/main.rs
 * 的 invoke_handler，命令表见 docs/bridge-commands.md）。
 *
 * 分层：
 *  - 纯函数（两平台可测，见 tests/test_gui.c）：frag 解析、dup 缓存键、
 *    索引作用域解析、任务坞、扫描状态。
 *  - GUI 入口：cdc_gui_register_commands()（Windows 下注册全部 45 个命令，
 *    并向 shred/privacy/system/im 注册进度回调；Linux 下空操作）、
 *    main.c 提供的索引编排入口（cdc_run_index_summary /
 *    cdc_index_scope_fresh / cdc_index_freshness）。
 *  - 45 个适配函数只在 Windows 下编译（#ifdef _WIN32，见 gui.c），
 *    Linux 默认分支由 main.c 直接打印 "GUI 仅支持 Windows" 并退出，
 *    不链接 host/。
 */
#ifndef CDC_GUI_H
#define CDC_GUI_H

#include <stddef.h>
#include <stdint.h>

#include "fileindex.h" /* cdc_fileindex_t（索引编排入口的参数类型） */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 纯函数：defrag 输出解析 ---------------- */

/* frag::parse_defrag 的移植结果；has_* 为 0 时对应字段无意义
 * （与 Rust 的 Option<f64>/Option<u64> 对应）。 */
typedef struct {
    int has_percent;
    double percent;
    int has_space_gb;
    double space_gb;
    int has_count;
    uint64_t count;
    int has_free_gb;
    double free_gb;
} cdc_frag_parsed_t;

/* 解析 defrag.exe /A /V 的输出（中/英文标签；先做 U+3000 折叠）。
 * 与 Rust frag.rs 的 parse_defrag 逐行一致，见 tests/test_gui.c。 */
cdc_frag_parsed_t cdc_frag_parse_defrag(const char *text);

/* ---------------- 纯函数：dup 缓存键 / 索引作用域 ---------------- */

/* 5 分钟 dups-*.json 缓存的文件名键：blake3（Rust Debug 格式的
 * roots 列表 + ":" + min_size + ":" + fast）的 64 位 hex。
 * 例：["C:\\"]:1048576:false。调用方 free()。 */
char *cdc_gui_dup_cache_key(const char *const *roots, size_t n_roots,
                            uint64_t min_size, int fast);

/* 解析 scan_dups/scan_big 的一个 root（Rust parse_index_scope_root）：
 * drive_out 填 "X:\\"（4 字节含 NUL）；prefix_out 为 NULL 表示整卷，
 * 否则为去掉尾斜杠的 root 原文（调用方 free()）。
 * 返回 1=成功，0=非法 root。 */
int cdc_gui_parse_index_scope(const char *root, char *drive_out,
                              char **prefix_out);

/* path 是否在目录 dir_prefix 之下（不含目录本身；边界必须是 '\\'；
 * 大小写不敏感；与 Rust path_under_dir 一致）。 */
int cdc_gui_path_under_dir(const char *path, const char *dir_prefix);

/* ---------------- 任务坞（Rust task.rs 的移植） ---------------- */

/* 开始一个任务。mutating 任务互斥：已有活跃 mutating 任务时返回 -1，
 * *out_error 为 "已有清理任务正在进行，请等它结束后再试"（调用方 free()）。
 * 同 id 重复 begin 会覆盖旧条目（与 Rust map.insert 一致）。
 * 成功返回 0。 */
int cdc_task_begin(const char *id, const char *title, int cancellable,
                   int mutating, char **out_error);
/* 结束一个任务（outcome "ok"/"cancelled"/"failed"，summary 为结语文案）。 */
void cdc_task_finish(const char *id, const char *outcome, const char *summary);
void cdc_task_set_stage(const char *id, const char *stage);
void cdc_task_set_unit(const char *id, const char *unit);
void cdc_task_set_total(const char *id, uint64_t total);
void cdc_task_set_progress(const char *id, uint64_t done, uint64_t total);
void cdc_task_add_done(const char *id, uint64_t amount);
int cdc_task_cancelled(const char *id);
/* 任务状态快照（JSON 对象，键为任务 id，值含 types.ts TaskStatus 的全部
 * 14 个字段；已结束超 30 秒的任务在此剪掉）。调用方 free()。 */
char *cdc_task_status_json(void);
/* 取消任务：id==NULL 时取消全部可取消的活跃任务。返回是否取消了任务。 */
int cdc_task_cancel(const char *id);

/* ---------------- 扫描状态（Rust scan_begin/scan_end 的移植） ---------------- */

void cdc_scan_begin(const char *kind, const char *stage);
void cdc_scan_stage(const char *kind, const char *stage);
void cdc_scan_end(const char *kind, const char *stage);
void cdc_scan_set_found(const char *kind, uint64_t found);
void cdc_scan_add_done(const char *kind, uint64_t amount);
int cdc_scan_cancelled(const char *kind);
/* 扫描状态快照（JSON 对象 {"dup": ScanStatus, "big": ScanStatus}）。 */
char *cdc_scan_status_json(void);

/* 删掉缓存目录下的 dups-*.json（Rust invalidate_dup_cache）。 */
void cdc_gui_invalidate_dup_cache(void);

/* ---------------- GUI 入口 ---------------- */

/* 注册全部 45 个后端命令（Windows）；同时向 shred/privacy/system/im
 * 注册进度/取消回调。Linux 下返回 0（空操作）。 */
int cdc_gui_register_commands(void);

/* ---- 以下三个函数由 main.c 实现（CLI 的索引编排，GUI 复用） ---- */

/* --index 编排（增量优先，断档回填）：quiet=1 时不打印到 stdout。
 * summary 总是填一条中文汇总（失败时为失败原因）。返回退出码
 * （0 全部成功，1 部分卷失败，2 非管理员）。 */
int cdc_run_index_summary(int quiet, int rebuild, char *summary, size_t cap);

/* 查询前确保一卷新鲜（Rust ensure_index_scope_fresh）：Fresh 直接用；
 * Behind 跑一次增量（仅增量，不回填；do_hash=1 时补算摘要）。
 * 返回 1=新鲜（serial_out 已填），0=不新鲜（serial_out 填卷序列号，
 * 卷打不开时填 0）。 */
int cdc_index_scope_fresh(cdc::FileIndex *idx, const char *drive, int do_hash,
                          uint64_t *serial_out);

/* 只读新鲜度（Rust freshness_from_state；index_status 用，不触发同步）。
 * state_out 填 fresh | behind | stale | missing | unavailable 之一；
 * serial_out 填卷序列号（卷打不开时填 0）。返回 0。 */
int cdc_index_freshness(cdc::FileIndex *idx, const char *drive,
                        uint64_t *serial_out, char *state_out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CDC_GUI_H */
