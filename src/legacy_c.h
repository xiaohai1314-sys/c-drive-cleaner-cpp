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
