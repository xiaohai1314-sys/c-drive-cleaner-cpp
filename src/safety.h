/* safety.h — 路径安全与句柄删除，逐行移植自 Rust 版 desktop/src/safety.rs。
 *
 * 红线（与 Rust 版一字不改）：
 *   - 所有规则清理一律按 untrusted=true 走：is_system_path 或不在 allowlist
 *     内的一律禁删。scan 与 clean 必须过同一道门（cdc_execution_forbidden）。
 *   - 删除一律走句柄：父目录逐级用 NtCreateFile 相对打开、扣住
 *     FILE_SHARE_DELETE，中间目录出现重解析点即拒绝；不留路径式回退。
 *   - 无法检查（元数据读失败）一律按不安全处理。
 *
 * 字符串内部一律 UTF-8（架构契约 §2）。返回指针的函数返回 malloc 的
 * 内存，调用方 free()；字符串数组用 cdc_strvec_free() 释放。
 * Windows 专属函数在 Linux 下返回 CDC_ERR_IO（"仅支持 Windows"），
 * 不静默成功；纯路径判定函数在两平台行为一致（供 Linux 自测）。
 */
#ifndef CDC_SAFETY_H
#define CDC_SAFETY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 共享小工具（本切片内各模块共用） ---------------- */

/* 取环境变量并转 UTF-8（Windows 上走 GetEnvironmentVariableW，避免
 * getenv 的 ANSI 代码页损坏中文路径）。变量不存在返回 NULL；
 * 存在但为空返回指向 "" 的分配。调用方 free()。 */
char *cdc_getenv_utf8(const char *name);

/* 按 UTF-8 路径打开文件（Windows 上走 _wfopen，避免 fopen 的 ANSI
 * 代码页损坏中文路径；Linux 上就是 fopen）。 */
FILE *cdc_fopen_utf8(const char *path, const char *mode);

/* 释放 malloc 的字符串数组（每个元素 + 数组本身）。 */
void cdc_strvec_free(char **vec, size_t n);

/* 单路径段 glob 匹配（对应 Rust glob::Pattern::matches；调用方先把
 * pattern 与 text 都转小写再比）。'*' 匹配任意串（不含 '/'），'?'
 * 匹配一个 UTF-8 码点，'[...]' 字符类支持范围与 '[!...]' 取反。
 * 段内不可能出现 '/'，但 '*' 同样不匹配 '/'。 */
int cdc_glob_match(const char *pattern, const char *text);

/* glob pattern 是否合法（Rust glob::Pattern::new 的判定：字符类未闭合、
 * 范围倒置即非法；非法 pattern 在 catalog / 规则加载时被丢弃）。 */
int cdc_glob_valid(const char *pattern);

/* 句柄：Windows 上就是 HANDLE（void*），Linux 上无意义（仅占位）。 */
typedef void *cdc_handle_t;

/* 打开的句柄链（父目录链）。vec 里是 cdc_handle_t，按打开顺序排列；
 * 释放时逐个 CloseHandle（Windows），Linux 下只是空壳。 */
typedef struct {
    cdc_vec_t vec; /* 元素 cdc_handle_t */
} cdc_handle_chain_t;

void cdc_handle_chain_free(cdc_handle_chain_t *chain);

/* 句柄 I/O 原语（Windows 上是 ReadFile/WriteFile/FlushFileBuffers/
 * GetFileSizeEx/SetFilePointerEx；Linux 下返回 CDC_ERR_IO）。 */
int cdc_handle_file_size(cdc_handle_t h, uint64_t *out_size, cdc_err_t *err);
int cdc_handle_read(cdc_handle_t h, uint8_t *buf, size_t want, size_t *out_got,
                    cdc_err_t *err);
int cdc_handle_write_all(cdc_handle_t h, const uint8_t *buf, size_t len,
                         cdc_err_t *err);
int cdc_handle_sync(cdc_handle_t h, cdc_err_t *err);
int cdc_handle_seek_start(cdc_handle_t h, cdc_err_t *err);

/* 目录遍历回调：返回 0 继续，非 0 立即停止（返回值原样返回给调用方）。
 * is_symlink 在 Windows 上指重解析点（含 junction），Linux 上指符号链接；
 * 遍历永不跟随链接。mtime 为 Unix 秒（取不到为 0）。attrs 在 Windows 上
 * 是 dwFileAttributes，Linux 上是 st_mode。 */
typedef int (*cdc_walk_fn)(void *ctx, const char *path, int is_dir,
                           int is_symlink, uint64_t size, uint64_t mtime,
                           uint32_t attrs);
/* 递归遍历 root（min_depth=1 时不回调 root 本身，与 Rust WalkDir
 * .min_depth(1) 一致；min_depth=0 时回调 root 本身）。 */
int cdc_walk_dir(const char *root, int min_depth, cdc_walk_fn fn, void *ctx,
                 cdc_err_t *err);

/* ---------------- 纯路径判定（两平台一致） ---------------- */

/* basename：把 '/' 换成 '\\'，去尾 '\\'，取最后一个 '\\' 之后。 */
char *cdc_basename(const char *path);
/* normalize：与 Rust safety::normalize 逐行一致（去空白/引号、'/'→'\\'、
 * 盘符大写、折叠连续 '\\'、补前导 '\\'、去尾 '\\'；"C:" → "C:\"）。 */
char *cdc_normalize(const char *path);

int cdc_under_marker(const char *path, const char * const *markers,
                     size_t nmarkers);
int cdc_is_drive_root(const char *path);
int cdc_is_critical_file(const char *path);
int cdc_is_never_clean(const char *path);
int cdc_is_password_store(const char *path);
int cdc_path_syntax_error(const char *path);
int cdc_is_system_path(const char *path);
int cdc_is_protected_path(const char *path);
int cdc_is_user_library(const char *path);
int cdc_is_within(const char *path, const char **roots, size_t nroots);

/* 重解析点：失败返回 0（Rust is_reparse_point 的 unwrap_or(false)）。 */
int cdc_is_reparse_point(const char *path);
/* 同上，但保留元数据错误：成功返回 CDC_OK 并填 *out；文件不存在返回
 * CDC_ERR_NOT_FOUND；其它错误返回 CDC_ERR_IO。删除/恢复路径应把无法
 * 检查视为不安全。 */
int cdc_reparse_point_status(const char *path, int *out_is_reparse,
                             cdc_err_t *err);

/* 远程规则 allowlist：临时目录或 AppData catalog 内。 */
int cdc_under_remote_allowlist(const char *path);
/* 总门禁：untrusted=1 时（所有规则一律如此）还要求非系统路径且在
 * allowlist 内。 */
int cdc_execution_forbidden(const char *path, int untrusted);
/* 用户主动勾选删除（大文件、重复副本、微信文件、粉碎）时的检查：
 * 允许文档/桌面/下载，系统目录、密码库、盘符根、关键文件、永不清理
 * 名单、重解析点仍然拦住。 */
int cdc_explicit_delete_forbidden(const char *path);

/* ---------------- 句柄删除（Windows；Linux 下返回 CDC_ERR_IO） ---------------- */

typedef enum {
    CDC_DELETE_DELETED = 0,
    CDC_DELETE_SKIPPED = 1,
    CDC_DELETE_FAILED = 2
} cdc_delete_outcome_t;

/* 在已打开的句柄上设置删除标记（句柄关闭时生效）。粉碎功能复用。 */
int cdc_mark_delete(cdc_handle_t file);

/* 按经逐级句柄校验的父目录删除普通文件，不使用可被 junction 替换的
 * 路径式回退。err 填失败原因（Skipped 时不填）。 */
cdc_delete_outcome_t cdc_delete_file_checked(const char *path,
                                             const char **roots, size_t nroots,
                                             int untrusted, cdc_err_t *err);

/* 打开并固定一个普通目录（要求在 roots 内）。返回的链在用完前保持打开。 */
int cdc_pin_directory_checked(const char *path, const char **roots,
                              size_t nroots, cdc_handle_chain_t **out_chain,
                              cdc_err_t *err);
/* 安全创建目录树：已存在的祖先不带 FILE_SHARE_DELETE 打开，缺失的祖先
 * 相对父目录创建；末段必须不存在。返回的链在使用期间保持打开。 */
int cdc_create_directory_tree_checked(const char *path,
                                      cdc_handle_chain_t **out_chain,
                                      cdc_err_t *err);
/* 在已校验的父目录下创建一个新目录（FILE_CREATE，目标已存在即失败）。 */
int cdc_create_directory_checked(const char *path, const char **roots,
                                 size_t nroots,
                                 cdc_handle_chain_t **out_chain,
                                 cdc_handle_t *out_dir, cdc_err_t *err);
/* 在已校验的父目录下创建一个新普通文件（FILE_CREATE）。 */
int cdc_create_new_file_checked(const char *path, const char **roots,
                                size_t nroots, cdc_handle_chain_t **out_chain,
                                cdc_handle_t *out_file, cdc_err_t *err);
/* 在已校验的父目录下以只读方式打开一个普通文件。 */
int cdc_open_read_file_checked(const char *path, const char **roots,
                               size_t nroots, cdc_handle_chain_t **out_chain,
                               cdc_handle_t *out_file, cdc_err_t *err);
/* 用已校验的父句柄删除一个空普通目录。*out_deleted=1 表示已删。 */
int cdc_remove_directory_checked(const char *path, const char **roots,
                                 size_t nroots, int *out_deleted,
                                 cdc_err_t *err);
/* 递归删除迁移备份：不跟随任何重解析点，遇到链接子项直接保留（宁可
 * 删不干净，也不跨过链接）。返回删除的条目数（含根目录本身）。 */
int cdc_remove_tree_checked(const char *root, const char **allowed_roots,
                            size_t nroots, uint64_t *out_removed,
                            cdc_err_t *err);
/* 把普通目录重命名为同父目录下的新名（两端至少一端在 allowed_roots 内）。 */
int cdc_rename_directory_sibling_checked(const char *from, const char *to,
                                         const char **allowed_roots,
                                         size_t nroots, cdc_err_t *err);

/* 自底向上删除空目录（不删 root 本身），never_clean 命中的目录保留。
 * 返回删除的目录数。Linux 下遍历后删除恒失败，返回 0。 */
int cdc_remove_empty_dirs(const char *root);

#ifdef __cplusplus
}
#endif

#endif /* CDC_SAFETY_H */
