/* util.c — 基础层实现。见 util.h 的契约注释。
 *
 * 平台说明：Windows 上用 Win32 宽字符 API（MultiByteToWideChar /
 * WideCharToMultiByte / GetEnvironmentVariableW），Linux 上用手写
 * UTF-8 编解码（wchar_t 为 UTF-32 码点）。两种平台对合法输入的
 * 转换都是精确的；非法输入按头文件约定的策略处理。
 */
#ifndef _WIN32
#endif

#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
/* MSVC 把 strcpy/wcscat 标为 C4996（/WX 下即错误），用安全版替代。 */
#define CDC_STRCPY(dst, dstsz, src) strcpy_s((dst), (dstsz), (src))
#define CDC_WCSCAT(dst, dstsz, src) wcscat_s((dst), (dstsz), (src))
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define CDC_STRCPY(dst, dstsz, src) strcpy((dst), (src))
#define CDC_WCSCAT(dst, dstsz, src) wcscat((dst), (src))
#endif

/* ---------------- 错误码 ---------------- */

void cdc_err_init(cdc_err_t *err) {
    if (!err) return;
    err->code = CDC_OK;
    err->message[0] = '\0';
}

void cdc_err_set(cdc_err_t *err, int code, const char *fmt, ...) {
    va_list ap;
    if (!err) return;
    err->code = code;
    if (!fmt) {
        err->message[0] = '\0';
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof err->message, fmt, ap);
    va_end(ap);
}

const char *cdc_err_str(int code) {
    switch (code) {
        case CDC_ERR_OK:          return "ok";
        case CDC_ERR_NOMEM:       return "out of memory";
        case CDC_ERR_INVALID_ARG: return "invalid argument";
        case CDC_ERR_IO:          return "I/O error";
        case CDC_ERR_NOT_FOUND:   return "not found";
        case CDC_ERR_OVERFLOW:    return "size overflow";
        case CDC_ERR_UTF8:        return "invalid UTF-8";
        case CDC_ERR_UTF16:       return "invalid UTF-16";
        case CDC_ERR_WIN32:       return "Win32 API failed";
        case CDC_ERR_EXISTS:      return "already exists";
        default:                  return "unknown error";
    }
}

/* ---------------- 动态字节缓冲 ---------------- */

void cdc_buf_init(cdc_buf_t *b) {
    if (!b) return;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void cdc_buf_free(cdc_buf_t *b) {
    if (!b) return;
    free(b->data);
    cdc_buf_init(b);
}

void cdc_buf_clear(cdc_buf_t *b) {
    if (!b) return;
    b->len = 0;
}

/* 容量翻倍增长（溢出时直接跳到 needed）；失败返回 NOMEM。 */
static int buf_grow(cdc_buf_t *b, size_t needed) {
    size_t cap;
    uint8_t *p;
    if (b->cap >= needed) return CDC_OK;
    cap = b->cap ? b->cap : 64;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    p = (uint8_t *)realloc(b->data, cap);
    if (!p) return CDC_ERR_NOMEM;
    b->data = p;
    b->cap = cap;
    return CDC_OK;
}

int cdc_buf_reserve(cdc_buf_t *b, size_t extra) {
    size_t needed;
    if (!b) return CDC_ERR_INVALID_ARG;
    if (extra > SIZE_MAX - b->len) return CDC_ERR_OVERFLOW;
    needed = b->len + extra;
    return buf_grow(b, needed);
}

int cdc_buf_append(cdc_buf_t *b, const void *p, size_t n) {
    int rc;
    if (!b) return CDC_ERR_INVALID_ARG;
    if (n > 0 && !p) return CDC_ERR_INVALID_ARG;
    if (n == 0) return CDC_OK;
    rc = cdc_buf_reserve(b, n);
    if (rc != CDC_OK) return rc;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return CDC_OK;
}

int cdc_buf_push(cdc_buf_t *b, uint8_t byte) {
    return cdc_buf_append(b, &byte, 1);
}

int cdc_buf_append_str(cdc_buf_t *b, const char *s) {
    if (!s) return CDC_ERR_INVALID_ARG;
    return cdc_buf_append(b, s, strlen(s));
}

uint8_t *cdc_buf_take(cdc_buf_t *b) {
    uint8_t *p;
    if (!b) return NULL;
    p = b->data;
    cdc_buf_init(b);
    return p;
}

char *cdc_buf_take_cstr(cdc_buf_t *b) {
    int rc;
    char *p;
    if (!b) return NULL;
    rc = cdc_buf_reserve(b, 1);
    if (rc != CDC_OK) return NULL;
    b->data[b->len] = '\0';
    p = (char *)cdc_buf_take(b);
    return p;
}

/* ---------------- 通用动态数组 ---------------- */

int cdc_vec_init(cdc_vec_t *v, size_t elem_size) {
    if (!v || elem_size == 0) return CDC_ERR_INVALID_ARG;
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    v->elem_size = elem_size;
    return CDC_OK;
}

void cdc_vec_free(cdc_vec_t *v) {
    if (!v) return;
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    /* elem_size 保留：free 后的 vec 仍可继续 push（与 Rust Vec::clear 语义不同，
     * 这里 free 即回到“未 init 但 elem_size 已知”状态；要彻底复位再调 init）。 */
}

void cdc_vec_clear(cdc_vec_t *v) {
    if (!v) return;
    v->len = 0;
}

static int vec_grow(cdc_vec_t *v, size_t needed_elems) {
    size_t cap, bytes;
    void *p;
    if (v->cap >= needed_elems) return CDC_OK;
    cap = v->cap ? v->cap : 16;
    while (cap < needed_elems) {
        if (cap > SIZE_MAX / 2) {
            cap = needed_elems;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / v->elem_size) return CDC_ERR_OVERFLOW;
    bytes = cap * v->elem_size;
    p = realloc(v->data, bytes);
    if (!p) return CDC_ERR_NOMEM;
    v->data = p;
    v->cap = cap;
    return CDC_OK;
}

int cdc_vec_reserve(cdc_vec_t *v, size_t extra) {
    if (!v || v->elem_size == 0) return CDC_ERR_INVALID_ARG;
    if (extra > SIZE_MAX - v->len) return CDC_ERR_OVERFLOW;
    return vec_grow(v, v->len + extra);
}

int cdc_vec_push(cdc_vec_t *v, const void *elem) {
    int rc;
    if (!v || v->elem_size == 0) return CDC_ERR_INVALID_ARG;
    if (!elem) return CDC_ERR_INVALID_ARG;
    rc = cdc_vec_reserve(v, 1);
    if (rc != CDC_OK) return rc;
    memcpy((uint8_t *)v->data + v->len * v->elem_size, elem, v->elem_size);
    v->len++;
    return CDC_OK;
}

int cdc_vec_pop(cdc_vec_t *v, void *out) {
    if (!v || v->elem_size == 0) return CDC_ERR_INVALID_ARG;
    if (v->len == 0) return CDC_ERR_INVALID_ARG;
    v->len--;
    if (out)
        memcpy(out, (uint8_t *)v->data + v->len * v->elem_size, v->elem_size);
    return CDC_OK;
}

void *cdc_vec_get(cdc_vec_t *v, size_t i) {
    if (!v || i >= v->len) return NULL;
    return (uint8_t *)v->data + i * v->elem_size;
}

/* 码点 -> UTF-8（调用方保证 cp 合法：<0x80 或 0x800..0x10FFFF 且非代理区）。
 * 两平台共用：POSIX 的 cdc_wide_to_utf8 与目录枚举的 UTF-16LE 解析都用它。 */
static size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

#ifndef _WIN32

/* 严格解码一个 UTF-8 序列：拒绝截断、超长编码、代理区码点、>U+10FFFF。 */
static int utf8_decode(const unsigned char *s, size_t len,
                       uint32_t *cp, size_t *adv) {
    unsigned char c;
    uint32_t v;
    size_t n, i;
    if (len == 0 || !s || !cp || !adv) return CDC_ERR_UTF8;
    c = s[0];
    if (c < 0x80) {
        *cp = c;
        *adv = 1;
        return CDC_OK;
    }
    if (c < 0xC2) return CDC_ERR_UTF8; /* 0x80-0xC1：续字节或超长编码 */
    if (c < 0xE0) {
        v = c & 0x1F;
        n = 2;
    } else if (c < 0xF0) {
        v = c & 0x0F;
        n = 3;
    } else if (c < 0xF5) {
        v = c & 0x07;
        n = 4;
    } else {
        return CDC_ERR_UTF8;
    }
    if (len < n) return CDC_ERR_UTF8;
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return CDC_ERR_UTF8;
        v = (v << 6) | (s[i] & 0x3F);
    }
    /* 超长编码 */
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && v < 0x10000))
        return CDC_ERR_UTF8;
    if (v >= 0xD800 && v <= 0xDFFF) return CDC_ERR_UTF8; /* 代理区 */
    if (v > 0x10FFFF) return CDC_ERR_UTF8;
    *cp = v;
    *adv = n;
    return CDC_OK;
}


#endif /* _WIN32 */

wchar_t *cdc_utf8_to_wide(const char *utf8) {
    if (!utf8) return NULL;
#ifdef _WIN32
    {
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
        wchar_t *out;
        if (n <= 0) return NULL;
        out = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
        if (!out) return NULL;
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, n) <= 0) {
            free(out);
            return NULL;
        }
        return out;
    }
#else
    {
        /* 先校验并计数，再一次分配。 */
        const unsigned char *s = (const unsigned char *)utf8;
        size_t len = strlen(utf8), i = 0, count = 0;
        uint32_t cp;
        size_t adv;
        cdc_vec_t v;
        wchar_t *out;
        while (i < len) {
            if (utf8_decode(s + i, len - i, &cp, &adv) != CDC_OK) return NULL;
            i += adv;
            count++;
        }
        if (cdc_vec_init(&v, sizeof(wchar_t)) != CDC_OK) return NULL;
        if (cdc_vec_reserve(&v, count + 1) != CDC_OK) {
            cdc_vec_free(&v);
            return NULL;
        }
        i = 0;
        while (i < len) {
            wchar_t w;
            if (utf8_decode(s + i, len - i, &cp, &adv) != CDC_OK) {
                cdc_vec_free(&v);
                return NULL;
            }
            i += adv;
            w = (wchar_t)cp;
            if (cdc_vec_push(&v, &w) != CDC_OK) {
                cdc_vec_free(&v);
                return NULL;
            }
        }
        out = (wchar_t *)malloc((v.len + 1) * sizeof(wchar_t));
        if (!out) {
            cdc_vec_free(&v);
            return NULL;
        }
        if (v.len > 0) memcpy(out, v.data, v.len * sizeof(wchar_t));
        out[v.len] = 0;
        cdc_vec_free(&v);
        return out;
    }
#endif
}

char *cdc_wide_to_utf8(const wchar_t *wide) {
    if (!wide) return NULL;
#ifdef _WIN32
    {
        /* 不传 WC_ERR_INVALID_CHARS：孤立代理对按 U+FFFD 处理。 */
        int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
        char *out;
        if (n <= 0) return NULL;
        out = (char *)malloc((size_t)n);
        if (!out) return NULL;
        if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, n, NULL, NULL) <= 0) {
            free(out);
            return NULL;
        }
        return out;
    }
#else
    {
        cdc_buf_t b;
        size_t i;
        cdc_buf_init(&b);
        for (i = 0; wide[i] != 0; i++) {
            uint32_t cp = (uint32_t)wide[i];
            char tmp[4];
            size_t n;
            if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
                cp = 0xFFFD; /* 孤立代理对 / 超范围 -> U+FFFD */
            n = utf8_encode(cp, tmp);
            if (cdc_buf_append(&b, tmp, n) != CDC_OK) {
                cdc_buf_free(&b);
                return NULL;
            }
        }
        return cdc_buf_take_cstr(&b);
    }
#endif
}

/* ---------------- 日志 ---------------- */

#ifdef _WIN32
static SRWLOCK g_log_lock = SRWLOCK_INIT;
#define CDC_LOG_LOCK()   AcquireSRWLockExclusive(&g_log_lock)
#define CDC_LOG_UNLOCK() ReleaseSRWLockExclusive(&g_log_lock)
#else
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
#define CDC_LOG_LOCK()   pthread_mutex_lock(&g_log_lock)
#define CDC_LOG_UNLOCK() pthread_mutex_unlock(&g_log_lock)
#endif

static cdc_log_level_t g_log_level = CDC_LOG_INFO;
static char g_log_file[256] = ""; /* 为空 = 默认 cdc-YYYYMMDD.log */

void cdc_log_set_level(cdc_log_level_t level) {
    if (level < CDC_LOG_DEBUG) level = CDC_LOG_DEBUG;
    if (level > CDC_LOG_ERROR) level = CDC_LOG_ERROR;
    g_log_level = level;
}

cdc_log_level_t cdc_log_get_level(void) {
    return g_log_level;
}

int cdc_log_set_file(const char *filename) {
    if (!filename || filename[0] == '\0') return CDC_ERR_INVALID_ARG;
    if (strchr(filename, '/') || strchr(filename, '\\') || strstr(filename, ".."))
        return CDC_ERR_INVALID_ARG;
    if (strlen(filename) >= sizeof g_log_file) return CDC_ERR_INVALID_ARG;
    CDC_LOG_LOCK();
    CDC_STRCPY(g_log_file, sizeof g_log_file, filename);
    CDC_LOG_UNLOCK();
    return CDC_OK;
}

static const char *level_name(cdc_log_level_t level) {
    switch (level) {
        case CDC_LOG_DEBUG: return "DEBUG";
        case CDC_LOG_INFO:  return "INFO";
        case CDC_LOG_WARN:  return "WARN";
        case CDC_LOG_ERROR: return "ERROR";
        default:            return "INFO";
    }
}

static int local_now(struct tm *out) {
    time_t t = time(NULL);
    if (t == (time_t)-1) return CDC_ERR_IO;
#ifdef _WIN32
    if (localtime_s(out, &t) != 0) return CDC_ERR_IO;
#else
    if (localtime_r(&t, out) == NULL) return CDC_ERR_IO;
#endif
    return CDC_OK;
}

/* 带溢出检查的字符串拷贝 / 三段拼接（放不下返回 CDC_ERR_OVERFLOW，
 * 且不写出半截结果）。不用 snprintf 拼路径，避免 -Wformat-truncation
 * 在 -Werror 下误报。 */
static int str_copy(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n + 1 > cap) return CDC_ERR_OVERFLOW;
    memcpy(dst, src, n + 1);
    return CDC_OK;
}

static int str_concat3(char *dst, size_t cap, const char *a, const char *b,
                       const char *c) {
    size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
    if (la > SIZE_MAX - lb || la + lb > SIZE_MAX - lc) return CDC_ERR_OVERFLOW;
    if (la + lb + lc + 1 > cap) return CDC_ERR_OVERFLOW;
    memcpy(dst, a, la);
    memcpy(dst + la, b, lb);
    memcpy(dst + la + lb, c, lc + 1);
    return CDC_OK;
}

/* 逐级建目录（已存在则忽略失败）。path 会被原地改写再恢复。 */
#ifndef _WIN32
static void mkdir_p(char *path) {
    char *p;
    for (p = path + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
        }
    }
    mkdir(path, 0755);
}
#else
static void mkdir_p_w(wchar_t *path) {
    wchar_t *p;
    for (p = path + 1; *p != L'\0'; p++) {
        if (*p == L'\\') {
            *p = L'\0';
            _wmkdir(path);
            *p = L'\\';
        }
    }
    _wmkdir(path);
}
#endif

/* 解析并建好日志目录，结果缓存（只记成功的一次）。 */
static char g_log_dir_cache[4096] = "";
static int g_log_dir_cached = 0;

int cdc_log_dir(char *out, size_t cap) {
    int rc = CDC_OK;
    if (!out || cap == 0) return CDC_ERR_INVALID_ARG;
    CDC_LOG_LOCK();
    if (!g_log_dir_cached) {
#ifdef _WIN32
        wchar_t wpath[4096];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", wpath, 4096);
        if (n == 0 || n >= 4096) {
            /* 回退：可执行文件所在目录。 */
            n = GetModuleFileNameW(NULL, wpath, 4096);
            if (n == 0 || n >= 4096) {
                CDC_LOG_UNLOCK();
                return CDC_ERR_IO;
            }
            {
                wchar_t *slash = wcsrchr(wpath, L'\\');
                if (!slash) {
                    CDC_LOG_UNLOCK();
                    return CDC_ERR_IO;
                }
                *slash = L'\0';
            }
        }
        if (wcslen(wpath) + wcslen(L"\\C盘清理工具\\logs") + 1 > 4096) {
            CDC_LOG_UNLOCK();
            return CDC_ERR_OVERFLOW;
        }
        CDC_WCSCAT(wpath, 4096, L"\\C盘清理工具\\logs");
        mkdir_p_w(wpath);
        {
            char *u8 = cdc_wide_to_utf8(wpath);
            if (!u8) {
                CDC_LOG_UNLOCK();
                return CDC_ERR_IO;
            }
            if (strlen(u8) + 1 > sizeof g_log_dir_cache) {
                free(u8);
                CDC_LOG_UNLOCK();
                return CDC_ERR_OVERFLOW;
            }
            CDC_STRCPY(g_log_dir_cache, sizeof g_log_dir_cache, u8);
            free(u8);
        }
#else
        char base[4096], path[4096];
        const char *env = getenv("LOCALAPPDATA");
        if (env && env[0] != '\0') {
            if (str_copy(base, sizeof base, env) != CDC_OK) {
                CDC_LOG_UNLOCK();
                return CDC_ERR_OVERFLOW;
            }
        } else {
            char exe[4096];
            ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
            if (n > 0) {
                char *slash;
                exe[n] = '\0';
                slash = strrchr(exe, '/');
                if (slash) *slash = '\0';
                if (str_copy(base, sizeof base, exe) != CDC_OK) {
                    CDC_LOG_UNLOCK();
                    return CDC_ERR_OVERFLOW;
                }
            } else {
                if (str_copy(base, sizeof base, ".") != CDC_OK) {
                    CDC_LOG_UNLOCK();
                    return CDC_ERR_OVERFLOW;
                }
            }
        }
        if (str_concat3(path, sizeof path, base, "/C盘清理工具/logs", "") != CDC_OK) {
            CDC_LOG_UNLOCK();
            return CDC_ERR_OVERFLOW;
        }
        mkdir_p(path);
        if (strlen(path) + 1 > sizeof g_log_dir_cache) {
            CDC_LOG_UNLOCK();
            return CDC_ERR_OVERFLOW;
        }
        CDC_STRCPY(g_log_dir_cache, sizeof g_log_dir_cache, path);
#endif
        g_log_dir_cached = 1;
    }
    if (strlen(g_log_dir_cache) + 1 > cap) rc = CDC_ERR_OVERFLOW;
    else CDC_STRCPY(out, cap, g_log_dir_cache);
    CDC_LOG_UNLOCK();
    return rc;
}

/* 缓存目录 %LOCALAPPDATA%\C盘清理工具\mftcache（不存在则创建），malloc'd，
 * 调用方 free。规则与 cdc_log_dir 同款：LOCALAPPDATA 未设置时退回可执行
 * 文件所在目录，再退回 "."。
 *
 * 唯一实现：此前 mft.c 和 logs.c 各有一份，mft.c 的那份把"盘"写了两遍
 * （"C盘盘清理工具"，2026-11-17 切片 4 端到端验证发现），且两份同链会
 * 符号冲突；现统一到这里，两处旧实现已删。目录名是"C盘清理工具"
 * （一个"盘"），不要写成"C盘盘清理工具"。 */
char *cdc_cache_dir(void) {
#ifdef _WIN32
    wchar_t wpath[4096];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", wpath, 4096);
    if (n == 0 || n >= 4096) {
        /* 回退：可执行文件所在目录。 */
        n = GetModuleFileNameW(NULL, wpath, 4096);
        if (n == 0 || n >= 4096) return NULL;
        wchar_t *slash = wcsrchr(wpath, L'\\');
        if (!slash) return NULL;
        *slash = L'\0';
    }
    if (wcslen(wpath) + wcslen(L"\\C盘清理工具\\mftcache") + 1 > 4096) return NULL;
    CDC_WCSCAT(wpath, 4096, L"\\C盘清理工具\\mftcache");
    mkdir_p_w(wpath);
    return cdc_wide_to_utf8(wpath);
#else
    char base[4096], path[4096];
    const char *env = getenv("LOCALAPPDATA");
    if (env && env[0] != '\0') {
        if (str_copy(base, sizeof base, env) != CDC_OK) return NULL;
    } else {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            char *slash;
            exe[n] = '\0';
            slash = strrchr(exe, '/');
            if (slash) *slash = '\0';
            if (str_copy(base, sizeof base, exe) != CDC_OK) return NULL;
        } else {
            if (str_copy(base, sizeof base, ".") != CDC_OK) return NULL;
        }
    }
    if (str_concat3(path, sizeof path, base, "/C盘清理工具/mftcache", "") != CDC_OK)
        return NULL;
    mkdir_p(path);
    return strdup(path);
#endif
}

void cdc_log(cdc_log_level_t level, const char *fmt, ...) {
    char msg[2048];
    /* "%04d-%02d-%02d %02d:%02d:%02d"：6 个 int 最坏各 11 字符 + 5 个分隔符
     * + NUL = 72 字节，80 足够且不触发 -Wformat-truncation。 */
    char stamp[80];
    char filename[256];
    char path[4096];
    struct tm tmv;
    va_list ap;
    FILE *f;

    if (level < g_log_level) return;
    if (!fmt) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    CDC_LOG_LOCK();
    if (local_now(&tmv) == CDC_OK)
        snprintf(stamp, sizeof stamp, "%04d-%02d-%02d %02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    else
        snprintf(stamp, sizeof stamp, "0000-00-00 00:00:00");

    fprintf(stderr, "[%s] [%s] %s\n", stamp, level_name(level), msg);

    /* 日志文件：best-effort，写失败只静默跳过（不影响调用方）。 */
    if (g_log_file[0] != '\0') {
        if (str_copy(filename, sizeof filename, g_log_file) != CDC_OK)
            filename[0] = '\0';
    } else if (local_now(&tmv) == CDC_OK) {
        snprintf(filename, sizeof filename, "cdc-%04d%02d%02d.log",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    } else {
        filename[0] = '\0';
    }
    if (filename[0] != '\0') {
        char dir[4096];
        /* 注意：cdc_log_dir 内部会再加锁；这里先解锁再调，避免死锁。 */
        CDC_LOG_UNLOCK();
        if (cdc_log_dir(dir, sizeof dir) == CDC_OK &&
            str_concat3(path, sizeof path, dir, "/", filename) == CDC_OK) {
#ifdef _WIN32
            {
                wchar_t *wpath = cdc_utf8_to_wide(path);
                if (wpath) {
                    f = _wfopen(wpath, L"a");
                    free(wpath);
                } else {
                    f = NULL;
                }
            }
#else
            f = fopen(path, "a");
#endif
            if (f) {
                fprintf(f, "[%s] [%s] %s\n", stamp, level_name(level), msg);
                fclose(f);
            }
        }
        CDC_LOG_LOCK();
    }
    CDC_LOG_UNLOCK();
}

/* ---------------- 目录枚举 ---------------- */

/* FILETIME（100ns，1601 起）-> Unix 秒；与 mft_filetime_to_unix 同式，
 * 这里不 include mft_util.h（基础层不依赖上层模块）。 */
static uint64_t direnum_filetime_to_unix(uint64_t ft) {
    uint64_t secs = ft / 10000000ULL;
    return secs >= 11644473600ULL ? secs - 11644473600ULL : 0;
}

/* UTF-16LE 码元串（nbytes 字节，不要求 NUL 结尾）-> UTF-8，追加进 out。
 * 代理对正确组合；孤立代理对按 U+FFFD（与 cdc_wide_to_utf8 同策略）。 */
static int utf16le_append_utf8(const uint8_t *bytes, size_t nbytes,
                               cdc_buf_t *out) {
    size_t i = 0;
    while (i + 2 <= nbytes) {
        uint32_t cp = (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8);
        char tmp[4];
        size_t n;
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            /* 高代理：后跟低代理则组合，否则 U+FFFD。 */
            if (i + 2 <= nbytes) {
                uint32_t lo = (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                } else {
                    cp = 0xFFFD;
                }
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD; /* 孤立低代理 */
        }
        n = utf8_encode(cp, tmp);
        if (cdc_buf_append(out, tmp, n) != CDC_OK) return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

/* FILE_DIRECTORY_INFORMATION 的定长头（64 字节）+ 变长 UTF-16LE 名。
 * 字段偏移与 Windows 头文件一致；内核写出小端序，Windows 各架构
 * 全是小端，直接按本机序读。 */
#define CDC_DIRENUM_HDR_SIZE 64

static uint32_t direnum_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t direnum_u64le(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

int cdc_direnum_parse_batch(const uint8_t *buf, size_t n, cdc_dir_entry_fn cb,
                            void *ctx, cdc_buf_t *scratch) {
    size_t off = 0;
    if (!cb || !scratch) return CDC_ERR_INVALID_ARG;
    if (!buf && n != 0) return CDC_ERR_INVALID_ARG;
    while (off < n) {
        uint32_t next, attrs, name_len;
        uint64_t size, mtime;
        int is_dir, is_symlink, rc;
        const char *name;
        if (n - off < CDC_DIRENUM_HDR_SIZE) return CDC_ERR_IO; /* 截断的头 */
        next = direnum_u32le(buf + off + 0);
        attrs = direnum_u32le(buf + off + 56);
        name_len = direnum_u32le(buf + off + 60);
        if (name_len > n - off - CDC_DIRENUM_HDR_SIZE) return CDC_ERR_IO;
        cdc_buf_clear(scratch);
        if (utf16le_append_utf8(buf + off + 64, name_len, scratch) != CDC_OK)
            return CDC_ERR_NOMEM;
        if (cdc_buf_reserve(scratch, 1) != CDC_OK) return CDC_ERR_NOMEM;
        scratch->data[scratch->len] = '\0';
        name = (const char *)scratch->data;
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
            is_dir = (attrs & 0x10u) != 0; /* FILE_ATTRIBUTE_DIRECTORY */
            is_symlink = (attrs & 0x400u) != 0; /* FILE_ATTRIBUTE_REPARSE_POINT */
            size = is_dir ? 0 : direnum_u64le(buf + off + 40); /* EndOfFile */
            mtime = direnum_filetime_to_unix(direnum_u64le(buf + off + 24));
            rc = cb(name, is_dir, is_symlink, size, mtime, attrs, ctx);
            if (rc != 0) return rc;
        }
        if (next == 0) break;
        /* 链偏移在交付本项后才校验：坏链不吞掉前面已交付的有效项。 */
        if (next < CDC_DIRENUM_HDR_SIZE || next > n - off) return CDC_ERR_IO;
        off += next;
    }
    return CDC_OK;
}

int cdc_enum_dir(const char *dir, cdc_dir_entry_fn cb, void *ctx) {
    if (!dir || !cb) return CDC_ERR_INVALID_ARG;
#ifdef _WIN32
    {
        /* NtQueryDirectoryFile 用 GetProcAddress 动态取：不改链接
         * （不用加 -lntdll），每次枚举取一次（一次枚举一次系统调用
         * 级别的开销，与随后成批的目录读取相比可忽略）。 */
        typedef LONG(NTAPI *direnum_nt_query_fn)(
            HANDLE FileHandle, HANDLE Event, void *ApcRoutine,
            void *ApcContext, void *IoStatusBlock, void *FileInformation,
            ULONG Length, ULONG FileInformationClass, BOOLEAN ReturnSingleEntry,
            void *FileName, BOOLEAN RestartScan);
        typedef struct {
            union {
                LONG Status;
                void *Pointer;
            } u;
            ULONG_PTR Information;
        } direnum_io_status_t;
        wchar_t *wdir;
        HANDLE h;
        direnum_nt_query_fn pfn;
        HMODULE ntdll;
        uint8_t *buf;
        cdc_buf_t scratch;
        int rc = CDC_OK;
        BOOLEAN restart = TRUE;

        wdir = cdc_utf8_to_wide(dir);
        if (!wdir) return CDC_ERR_UTF8;
        h = CreateFileW(wdir, FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        free(wdir);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                return CDC_ERR_NOT_FOUND;
            return CDC_ERR_IO;
        }
        ntdll = GetModuleHandleW(L"ntdll.dll");
        pfn = ntdll ? (direnum_nt_query_fn)(void *)GetProcAddress(
                          ntdll, "NtQueryDirectoryFile")
                    : NULL;
        if (!pfn) {
            CloseHandle(h);
            return CDC_ERR_IO;
        }
        buf = (uint8_t *)malloc(65536);
        if (!buf) {
            CloseHandle(h);
            return CDC_ERR_NOMEM;
        }
        cdc_buf_init(&scratch);
        for (;;) {
            direnum_io_status_t iosb;
            LONG st;
            memset(&iosb, 0, sizeof iosb);
            st = pfn(h, NULL, NULL, NULL, &iosb, buf, 65536,
                     1 /* FileDirectoryInformation */, FALSE, NULL, restart);
            restart = FALSE;
            if ((uint32_t)st == 0xC0000018u) break; /* STATUS_NO_MORE_FILES */
            if (st != 0) {
                rc = CDC_ERR_IO;
                break;
            }
            if (iosb.Information == 0) break; /* 防御：成功但零字节 */
            rc = cdc_direnum_parse_batch(buf, (size_t)iosb.Information, cb,
                                         ctx, &scratch);
            if (rc != CDC_OK) break;
        }
        free(buf);
        cdc_buf_free(&scratch);
        CloseHandle(h);
        return rc;
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        cdc_buf_t path;
        int rc = CDC_OK;
        if (!d) {
            if (errno == ENOENT) return CDC_ERR_NOT_FOUND;
            return CDC_ERR_IO;
        }
        cdc_buf_init(&path);
        while ((e = readdir(d)) != NULL) {
            struct stat st;
            size_t dl;
            int is_dir, is_symlink;
            uint64_t size, mtime;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            /* lstat 取 size/mtime/is_dir/is_symlink（与旧代码同语义）；
             * 路径拼进复用的划痕缓冲，不逐项 malloc。 */
            cdc_buf_clear(&path);
            if (cdc_buf_append_str(&path, dir) != CDC_OK ||
                ((dl = strlen(dir)) > 0 && dir[dl - 1] != '/' &&
                 cdc_buf_push(&path, (uint8_t)'/') != CDC_OK) ||
                cdc_buf_append_str(&path, e->d_name) != CDC_OK ||
                cdc_buf_reserve(&path, 1) != CDC_OK) {
                rc = CDC_ERR_NOMEM;
                break;
            }
            path.data[path.len] = '\0';
            if (lstat((const char *)path.data, &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            is_symlink = S_ISLNK(st.st_mode) ? 1 : 0;
            size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
            mtime = (uint64_t)st.st_mtime;
            rc = cb(e->d_name, is_dir, is_symlink, size, mtime,
                    (uint32_t)st.st_mode, ctx);
            if (rc != 0) break;
        }
        closedir(d);
        cdc_buf_free(&path);
        return rc;
    }
#endif
}
