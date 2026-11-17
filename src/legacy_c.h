// legacy_c.h — TRANSITIONAL：C 版 util 接口（cdc_buf_t / cdc_vec_t /
// cdc_err_* / cdc_log / cdc_utf8_to_wide ...）的兼容层，给尚未迁移到
// C++ 的模块用；实现全部转发到 cdc:: 新接口（legacy_c.cpp）。
// 每个模块迁移完就不再用它；最后一个模块迁移后整个文件删除。
// 新代码不得使用本文件里的任何名字。
#pragma once

#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace cdc {
struct Error;
}

#define CDC_OK 0
#define CDC_ERR_OK 0
#define CDC_ERR_NOMEM 1
#define CDC_ERR_INVALID_ARG 2
#define CDC_ERR_IO 3
#define CDC_ERR_NOT_FOUND 4
#define CDC_ERR_OVERFLOW 5
#define CDC_ERR_UTF8 6
#define CDC_ERR_UTF16 7
#define CDC_ERR_WIN32 8
#define CDC_ERR_EXISTS 9

typedef cdc::Error cdc_err_t;
void cdc_err_init(cdc_err_t *err);
void cdc_err_set(cdc_err_t *err, int code, const char *fmt, ...);
const char *cdc_err_str(int code);

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} cdc_buf_t;
void cdc_buf_init(cdc_buf_t *b);
void cdc_buf_free(cdc_buf_t *b);
void cdc_buf_clear(cdc_buf_t *b);
int cdc_buf_reserve(cdc_buf_t *b, size_t extra);
int cdc_buf_append(cdc_buf_t *b, const void *p, size_t n);
int cdc_buf_push(cdc_buf_t *b, uint8_t byte);
int cdc_buf_append_str(cdc_buf_t *b, const char *s);
uint8_t *cdc_buf_take(cdc_buf_t *b);
char *cdc_buf_take_cstr(cdc_buf_t *b);

typedef struct {
    void *data;
    size_t len;
    size_t cap;
    size_t elem_size;
} cdc_vec_t;
int cdc_vec_init(cdc_vec_t *v, size_t elem_size);
void cdc_vec_free(cdc_vec_t *v);
void cdc_vec_clear(cdc_vec_t *v);
int cdc_vec_reserve(cdc_vec_t *v, size_t extra);
int cdc_vec_push(cdc_vec_t *v, const void *elem);
int cdc_vec_pop(cdc_vec_t *v, void *out);
void *cdc_vec_get(cdc_vec_t *v, size_t i);
#define CDC_VEC_AT(v, T, i) (((T *)(v)->data)[(i)])

wchar_t *cdc_utf8_to_wide(const char *utf8);
char *cdc_wide_to_utf8(const wchar_t *wide);

typedef int (*cdc_dir_entry_fn)(const char *name, int is_dir, int is_symlink, uint64_t size,
                                uint64_t mtime, uint32_t attrs, void *ctx);
int cdc_enum_dir(const char *dir, cdc_dir_entry_fn cb, void *ctx);

typedef enum { CDC_LOG_DEBUG = 0, CDC_LOG_INFO = 1, CDC_LOG_WARN = 2, CDC_LOG_ERROR = 3 } cdc_log_level_t;
void cdc_log(cdc_log_level_t level, const char *fmt, ...);
int cdc_log_dir(char *out, size_t cap);
char *cdc_cache_dir(void);

// ---- safety（C 版接口；实现转发到 safety.h 的 cdc:: 新接口） ----
#include <cstdio>
char *cdc_getenv_utf8(const char *name);
FILE *cdc_fopen_utf8(const char *path, const char *mode);
void cdc_strvec_free(char **vec, size_t n);
int cdc_glob_match(const char *pattern, const char *text);
int cdc_glob_valid(const char *pattern);
typedef void *cdc_handle_t;
typedef struct cdc_handle_chain cdc_handle_chain_t;
void cdc_handle_chain_free(cdc_handle_chain_t *chain);
int cdc_handle_file_size(cdc_handle_t h, uint64_t *out_size, cdc_err_t *err);
int cdc_handle_read(cdc_handle_t h, uint8_t *buf, size_t want, size_t *out_got, cdc_err_t *err);
int cdc_handle_write_all(cdc_handle_t h, const uint8_t *buf, size_t len, cdc_err_t *err);
int cdc_handle_sync(cdc_handle_t h, cdc_err_t *err);
int cdc_handle_seek_start(cdc_handle_t h, cdc_err_t *err);
typedef int (*cdc_walk_fn)(void *ctx, const char *path, int is_dir, int is_symlink, uint64_t size,
                           uint64_t mtime, uint32_t attrs);
int cdc_walk_dir(const char *root, int min_depth, cdc_walk_fn fn, void *ctx, cdc_err_t *err);
char *cdc_basename(const char *path);
char *cdc_normalize(const char *path);
int cdc_under_marker(const char *path, const char *const *markers, size_t nmarkers);
int cdc_is_drive_root(const char *path);
int cdc_is_critical_file(const char *path);
int cdc_is_never_clean(const char *path);
int cdc_is_password_store(const char *path);
int cdc_path_syntax_error(const char *path);
int cdc_is_system_path(const char *path);
int cdc_is_protected_path(const char *path);
int cdc_is_user_library(const char *path);
int cdc_is_within(const char *path, const char **roots, size_t nroots);
int cdc_is_reparse_point(const char *path);
int cdc_reparse_point_status(const char *path, int *out_is_reparse, cdc_err_t *err);
int cdc_under_remote_allowlist(const char *path);
int cdc_execution_forbidden(const char *path, int untrusted);
int cdc_explicit_delete_forbidden(const char *path);
typedef enum { CDC_DELETE_DELETED = 0, CDC_DELETE_SKIPPED = 1, CDC_DELETE_FAILED = 2 } cdc_delete_outcome_t;
int cdc_mark_delete(cdc_handle_t file);
cdc_delete_outcome_t cdc_delete_file_checked(const char *path, const char **roots, size_t nroots,
                                             int untrusted, cdc_err_t *err);
int cdc_pin_directory_checked(const char *path, const char **roots, size_t nroots,
                              cdc_handle_chain_t **out_chain, cdc_err_t *err);
int cdc_create_directory_tree_checked(const char *path, cdc_handle_chain_t **out_chain,
                                      cdc_err_t *err);
int cdc_create_directory_checked(const char *path, const char **roots, size_t nroots,
                                 cdc_handle_chain_t **out_chain, cdc_handle_t *out_dir,
                                 cdc_err_t *err);
int cdc_create_new_file_checked(const char *path, const char **roots, size_t nroots,
                                cdc_handle_chain_t **out_chain, cdc_handle_t *out_file,
                                cdc_err_t *err);
int cdc_open_read_file_checked(const char *path, const char **roots, size_t nroots,
                               cdc_handle_chain_t **out_chain, cdc_handle_t *out_file,
                               cdc_err_t *err);
int cdc_remove_directory_checked(const char *path, const char **roots, size_t nroots,
                                 int *out_deleted, cdc_err_t *err);
int cdc_remove_tree_checked(const char *root, const char **allowed_roots, size_t nroots,
                            uint64_t *out_removed, cdc_err_t *err);
int cdc_rename_directory_sibling_checked(const char *from, const char *to,
                                         const char **allowed_roots, size_t nroots,
                                         cdc_err_t *err);
int cdc_remove_empty_dirs(const char *root);

// ---- recycle ----
int cdc_send_to_recycle(const char **paths, size_t npaths, char ***out_failed,
                        size_t *out_nfailed, cdc_err_t *err);
