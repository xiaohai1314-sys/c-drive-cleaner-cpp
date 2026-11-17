/* util.h — 基础层：动态数组、UTF-8/UTF-16 转换、日志、错误码。
 *
 * 架构契约（docs/ARCHITECTURE.md §2）：
 *   - 字符串内部一律 UTF-8，调 Win32 API 前再转 UTF-16（本文件提供转换，
 *     调用方负责 free）。
 *   - 动态数组只用这里的 growable array，不许手写 realloc 模板。
 *   - 函数返回 int，0=成功（CDC_OK），非 0=错误码；出错时填 cdc_err_t，
 *     不许静默吞错。
 *   - 日志写 %LOCALAPPDATA%\C盘清理工具\logs\（与 Rust 版 logs.rs 同目录）。
 *
 * 所有权约定：凡返回指针的函数（cdc_utf8_to_wide、cdc_wide_to_utf8、
 * cdc_buf_take*），返回的都是 malloc 的内存，调用方用 free() 释放；
 * 结构体内指针（cdc_buf_t.data / cdc_vec_t.data）一律用对应的
 * cdc_buf_free / cdc_vec_free 释放，调用方不得直接 free。
 */
#ifndef CDC_UTIL_H
#define CDC_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDC_OK 0

/* ---------------- 错误码 ---------------- */

/* 错误码表。CDC_ERR_OK 与 CDC_OK 同值 0。 */
enum cdc_err_code {
    CDC_ERR_OK          = 0,  /* 无错 */
    CDC_ERR_NOMEM       = 1,  /* 内存分配失败 */
    CDC_ERR_INVALID_ARG = 2,  /* 参数非法（NULL、越界、空串、非法文件名） */
    CDC_ERR_IO          = 3,  /* 文件 / 系统 I/O 失败 */
    CDC_ERR_NOT_FOUND   = 4,  /* 文件 / 项不存在 */
    CDC_ERR_OVERFLOW    = 5,  /* 长度 / 容量溢出 */
    CDC_ERR_UTF8        = 6,  /* 非法 UTF-8 序列 */
    CDC_ERR_UTF16       = 7,  /* 非法 UTF-16 序列（孤立代理对等） */
    CDC_ERR_WIN32       = 8,  /* Win32 API 失败（message 里带 GetLastError 码） */
    CDC_ERR_EXISTS      = 9   /* 目标已存在 */
};

/* 码 + 一句话（UTF-8，NUL 结尾）。无错时 code=0 且 message 为空串。 */
typedef struct {
    int  code;
    char message[256];
} cdc_err_t;

void        cdc_err_init(cdc_err_t *err);
void        cdc_err_set(cdc_err_t *err, int code, const char *fmt, ...);
const char *cdc_err_str(int code); /* 静态字符串，如 "out of memory" */

/* ---------------- 动态字节缓冲 ---------------- */

typedef struct {
    uint8_t *data; /* malloc 的缓冲；用 cdc_buf_free 释放，不得直接 free */
    size_t   len;  /* 已用字节数（不含结尾 NUL） */
    size_t   cap;  /* 已分配字节数 */
} cdc_buf_t;

void cdc_buf_init(cdc_buf_t *b);
void cdc_buf_free(cdc_buf_t *b);                 /* 释放后回到 init 状态 */
void cdc_buf_clear(cdc_buf_t *b);                /* len=0，保留 cap */
int  cdc_buf_reserve(cdc_buf_t *b, size_t extra); /* 保证 cap >= len + extra */
int  cdc_buf_append(cdc_buf_t *b, const void *p, size_t n);
int  cdc_buf_push(cdc_buf_t *b, uint8_t byte);
int  cdc_buf_append_str(cdc_buf_t *b, const char *s); /* 追加 NUL 结尾串（不含 NUL） */
/* 交出所有权：返回 data，b 重置为 init 状态；失败返回 NULL 且 b 不变。 */
uint8_t *cdc_buf_take(cdc_buf_t *b);
/* 同上，但先补一个 NUL 结尾再交出（返回 char*，可直接当 C 字符串用）。 */
char *cdc_buf_take_cstr(cdc_buf_t *b);

/* ---------------- 通用动态数组 ---------------- */

typedef struct {
    void  *data;      /* 元素数组；用 cdc_vec_free 释放，不得直接 free */
    size_t len;       /* 元素个数 */
    size_t cap;       /* 已分配元素个数 */
    size_t elem_size; /* 每个元素字节数，init 时固定 */
} cdc_vec_t;

/* elem_size==0 返回 CDC_ERR_INVALID_ARG。 */
int   cdc_vec_init(cdc_vec_t *v, size_t elem_size);
void  cdc_vec_free(cdc_vec_t *v);                /* 释放后回到 init 状态 */
void  cdc_vec_clear(cdc_vec_t *v);               /* len=0，保留 cap */
int   cdc_vec_reserve(cdc_vec_t *v, size_t extra); /* 保证 cap >= len + extra */
int   cdc_vec_push(cdc_vec_t *v, const void *elem); /* 按值拷入 elem_size 字节 */
int   cdc_vec_pop(cdc_vec_t *v, void *out);      /* 弹出末尾元素拷到 out（可 NULL）；空返回 CDC_ERR_INVALID_ARG */
/* 越界返回 NULL（带边界检查）。 */
void *cdc_vec_get(cdc_vec_t *v, size_t i);
/* 不带检查的下标访问：调用前自行保证 i < len。 */
#define CDC_VEC_AT(v, T, i) (((T *)(v)->data)[(i)])

/* ---------------- UTF-8 / 宽字符转换 ---------------- */

/* UTF-8 -> 宽串。Windows 上 wchar_t 是 UTF-16 码元，Linux 上是 UTF-32 码点；
 * 两种平台对合法输入的转换都是精确的。返回 malloc 的 NUL 结尾宽串，
 * 调用方 free()；非法 UTF-8（截断、超长编码、代理区码点、>U+10FFFF）
 * 返回 NULL。空串返回指向单个 NUL 的分配（非 NULL）。 */
wchar_t *cdc_utf8_to_wide(const char *utf8);

/* 宽串 -> UTF-8。返回 malloc 的 NUL 结尾 UTF-8 串，调用方 free()；
 * 孤立代理对 / 超范围码点按 U+FFFD 处理（与 Rust from_utf16_lossy
 * 同策略）。NULL 输入返回 NULL。 */
char *cdc_wide_to_utf8(const wchar_t *wide);

/* ---------------- 目录枚举 ---------------- */

/* 单目录枚举回调：name 为 UTF-8 文件名（不含路径，仅在回调期间有效，
 * 要保留请自行拷贝）；is_dir；is_symlink（Windows 上指重解析点，
 * POSIX 上指符号链接）；size（目录/链接为 0）；mtime 为 Unix 秒
 * （取不到为 0）；attrs 在 Windows 上是 dwFileAttributes，POSIX 上
 * 是 st_mode。返回 0 继续，非 0 立即停止（该值原样返回给调用方）。 */
typedef int (*cdc_dir_entry_fn)(const char *name, int is_dir, int is_symlink,
                                uint64_t size, uint64_t mtime, uint32_t attrs,
                                void *ctx);

/* 枚举 dir 的直接子项（不含 "." ".."，不递归）。Windows 用
 * NtQueryDirectoryFile + 64KB 缓冲批量取（一次系统调用取多项，
 * 替代 FindFirstFileW/FindNextFileW 的逐项调用）；POSIX 用
 * opendir/readdir + lstat（与旧代码同语义）。
 * 返回 CDC_OK（零项也是 OK）；目录不存在返回 CDC_ERR_NOT_FOUND；
 * 其它打不开的情况返回 CDC_ERR_IO；回调返回的非 0 值原样返回。 */
int cdc_enum_dir(const char *dir, cdc_dir_entry_fn cb, void *ctx);

/* ---- 内部测试钩子 ---- */

/* 解析一包 FILE_DIRECTORY_INFORMATION（buf 长 n 字节），对每个非
 * "." ".." 的项调 cb。name 在一次调用内有效（复用的划痕缓冲 scratch）。
 * 供测试直接调用（覆盖 Windows 枚举最容易出错的链式遍历与 UTF-16LE
 * 解码）；生产代码只经 cdc_enum_dir 走它。buf 损坏（越界/截断）
 * 返回 CDC_ERR_IO；回调的非 0 返回值原样返回。 */
int cdc_direnum_parse_batch(const uint8_t *buf, size_t n, cdc_dir_entry_fn cb,
                            void *ctx, cdc_buf_t *scratch);

/* ---------------- 日志 ---------------- */

typedef enum {
    CDC_LOG_DEBUG = 0,
    CDC_LOG_INFO  = 1,
    CDC_LOG_WARN  = 2,
    CDC_LOG_ERROR = 3
} cdc_log_level_t;

void            cdc_log_set_level(cdc_log_level_t level); /* 默认 INFO */
cdc_log_level_t cdc_log_get_level(void);
/* 写一行日志：stderr + 日志文件（见下）。低于当前级别的调用直接丢弃。
 * 写日志文件失败不报错、不影响调用方（best-effort）。 */
void cdc_log(cdc_log_level_t level, const char *fmt, ...);

/* 日志目录：%LOCALAPPDATA%\C盘清理工具\logs\（与 Rust logs.rs 同规则；
 * LOCALAPPDATA 未设置时退回可执行文件所在目录）。目录不存在则创建。
 * out 由调用方提供；成功返回 CDC_OK，cap 不足返回 CDC_ERR_OVERFLOW，
 * 目录无法确定/创建返回 CDC_ERR_IO。 */
int cdc_log_dir(char *out, size_t cap);

/* 缓存目录：%LOCALAPPDATA%\C盘清理工具\mftcache\（与 Rust cache_dir() 同规则；
 * LOCALAPPDATA 未设置时退回可执行文件所在目录，再退回 "."）。目录不存在则创建。
 * 返回 malloc'd 字符串，调用方 free；失败返回 NULL。
 * 唯一实现在这里；mft.h / logs.h 的同名声明只是复用同一原型（不要另写实现）。 */
char *cdc_cache_dir(void);

/* 覆盖日志文件名（仅文件名，不含路径；不许含 '/'、'\\' 或 ".."，否则
 * 返回 CDC_ERR_INVALID_ARG 且保持原值）。默认文件名是 cdc-YYYYMMDD.log
 *（按本地日期每天一个文件）；调用方可改为固定名，如 index-20261117.log。 */
int cdc_log_set_file(const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* CDC_UTIL_H */
