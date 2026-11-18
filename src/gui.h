// gui.h — GUI 入口与 45 个后端命令的适配层（对照 Rust desktop/src/main.rs
// 的 invoke_handler，命令表见 docs/bridge-commands.md）。
//
// 分层：
//  - 纯函数（两平台可测，见 tests/test_gui.cpp）：frag 解析、dup 缓存键、
//    索引作用域解析、任务坞、扫描状态。
//  - GUI 入口：gui_register_commands()（Windows 下注册全部 45 个命令，
//    并向 shred/privacy/system/im 注册进度回调；Linux 下空操作）、
//    main.cpp 提供的索引编排入口（run_index_summary /
//    index_scope_fresh / index_freshness）。
//  - 45 个适配函数只在 Windows 下编译（#ifdef _WIN32，见 gui.cpp），
//    Linux 默认分支由 main.cpp 直接打印 "GUI 仅支持 Windows" 并退出，
//    不链接 host/。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fileindex.h" /* FileIndex（索引编排入口的参数类型） */
#include "util.h"

namespace cdc {

/* ---------------- 纯函数：defrag 输出解析 ---------------- */

/* frag::parse_defrag 的移植结果；has_* 为假时对应字段无意义
 * （与 Rust 的 Option<f64>/Option<u64> 对应）。 */
struct FragParsed {
    bool has_percent = false;
    double percent = 0;
    bool has_space_gb = false;
    double space_gb = 0;
    bool has_count = false;
    uint64_t count = 0;
    bool has_free_gb = false;
    double free_gb = 0;
};

/* 解析 defrag.exe /A /V 的输出（中/英文标签；先做 U+3000 折叠）。
 * 与 Rust frag.rs 的 parse_defrag 逐行一致，见 tests/test_gui.cpp。 */
FragParsed frag_parse_defrag(std::string_view text);

/* ---------------- 纯函数：dup 缓存键 / 索引作用域 ---------------- */

/* 5 分钟 dups-*.json 缓存的文件名键：blake3（Rust Debug 格式的
 * roots 列表 + ":" + min_size + ":" + fast）的 64 位 hex。
 * 例：["C:\\"]:1048576:false。 */
std::string gui_dup_cache_key(std::span<const std::string> roots, uint64_t min_size,
                              bool fast);

/* 解析 scan_dups/scan_big 的一个 root（Rust parse_index_scope_root）：
 * drive_out 填 "X:\\"；prefix_out 为空表示整卷，
 * 否则为去掉尾斜杠的 root 原文。
 * 返回 false 表示非法 root。 */
bool gui_parse_index_scope(std::string_view root, std::string *drive_out,
                           std::optional<std::string> *prefix_out);

/* path 是否在目录 dir_prefix 之下（不含目录本身；边界必须是 '\\'；
 * 大小写不敏感；与 Rust path_under_dir 一致）。 */
bool gui_path_under_dir(std::string_view path, std::string_view dir_prefix);

/* ---------------- 任务坞（Rust task.rs 的移植） ---------------- */

/* 开始一个任务。mutating 任务互斥：已有活跃 mutating 任务时返回 -1，
 * *out_error 为 "已有清理任务正在进行，请等它结束后再试"。
 * 同 id 重复 begin 会覆盖旧条目（与 Rust map.insert 一致）。
 * 成功返回 0。 */
int task_begin(std::string_view id, std::string_view title, bool cancellable,
               bool mutating, std::string *out_error);
/* 结束一个任务（outcome "ok"/"cancelled"/"failed"，summary 为结语文案）。 */
void task_finish(std::string_view id, std::string_view outcome, std::string_view summary);
void task_set_stage(std::string_view id, std::string_view stage);
void task_set_unit(std::string_view id, std::string_view unit);
void task_set_total(std::string_view id, uint64_t total);
void task_set_progress(std::string_view id, uint64_t done, uint64_t total);
void task_add_done(std::string_view id, uint64_t amount);
bool task_cancelled(std::string_view id);
/* 任务状态快照（JSON 对象，键为任务 id，值含 types.ts TaskStatus 的全部
 * 14 个字段；已结束超 30 秒的任务在此剪掉）。 */
std::string task_status_json();
/* 取消任务：id 为空时取消全部可取消的活跃任务。返回是否取消了任务。 */
bool task_cancel(std::optional<std::string_view> id);

/* ---------------- 扫描状态（Rust scan_begin/scan_end 的移植） ---------------- */

void scan_begin(std::string_view kind, std::string_view stage);
void scan_stage(std::string_view kind, std::string_view stage);
void scan_end(std::string_view kind, std::string_view stage);
void scan_set_found(std::string_view kind, uint64_t found);
void scan_add_done(std::string_view kind, uint64_t amount);
bool scan_cancelled(std::string_view kind);
/* 扫描状态快照（JSON 对象 {"dup": ScanStatus, "big": ScanStatus}）。 */
std::string scan_status_json();

/* 删掉缓存目录下的 dups-*.json（Rust invalidate_dup_cache）。 */
void gui_invalidate_dup_cache();

/* ---------------- GUI 入口 ---------------- */

/* 注册全部 45 个后端命令（Windows）；同时向 shred/privacy/system/im
 * 注册进度/取消回调。Linux 下返回 0（空操作）。 */
int gui_register_commands();

/* ---- 以下三个函数在 main.cpp 里实现（CLI 的索引编排，GUI 复用）。 ---- */

/* 只读新鲜度（Rust freshness_from_state；index_status 用，不触发同步）。
 * state_out 填 fresh | behind | stale | missing | unavailable 之一；
 * serial_out 填卷序列号（卷打不开时填 0）。返回 0。 */
int index_freshness(FileIndex *idx, std::string_view drive, uint64_t *serial_out,
                    std::string *state_out);

/* 查询前确保一卷新鲜（Rust ensure_index_scope_fresh）：Fresh 直接用；
 * Behind 跑一次增量（仅增量，不回填；do_hash 为真时补算摘要）。
 * 返回 true=新鲜（serial_out 已填），false=不新鲜（serial_out 填卷序列号，
 * 卷打不开时填 0）。 */
bool index_scope_fresh(FileIndex *idx, std::string_view drive, bool do_hash,
                       uint64_t *serial_out);

/* --index 编排（增量优先，断档回填）：quiet 为真时不打印到 stdout。
 * summary 总是填一条中文汇总（失败时为失败原因）。返回退出码
 * （0 全部成功，1 部分卷失败，2 非管理员）。 */
int run_index_summary(bool quiet, bool rebuild, std::string *summary);

}  // namespace cdc

/* GUI 入口的 C 名字（main.cpp 用；实现在 gui.cpp 里转发到
 * cdc::gui_register_commands）。 */
int cdc_gui_register_commands(void);

