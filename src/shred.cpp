/* shred.c — 移植自 Rust 版 desktop/src/shred.rs。 */
#include "shred.h"

#include <stdlib.h>
#include <string.h>

#include "safety.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/stat.h>
#endif

static cdc_hooks_t g_hooks = {};

void cdc_shred_set_hooks(const cdc_hooks_t *hooks) {
    if (hooks) {
        g_hooks = *hooks;
    } else {
        memset(&g_hooks, 0, sizeof(g_hooks));
    }
}

static int cancelled(void) {
    return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, "shred");
}

static void add_done(uint64_t done) {
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "shred", done, 0);
}

void cdc_shred_report_free(cdc_shred_report_t *r) {
    if (!r) return;
    cdc_strvec_free(r->errors, r->nerrors);
    r->errors = NULL;
    r->nerrors = 0;
}

static void report_error(cdc_shred_report_t *r, const char *msg) {
    char *dup = strdup(msg);
    if (!dup) return;
    /* errors 是字符串数组：用 realloc 追加（小数组，够用）。 */
    char **nr = (char **)realloc(r->errors,
                                 (r->nerrors + 1) * sizeof(char *));
    if (!nr) {
        free(dup);
        return;
    }
    r->errors = nr;
    r->errors[r->nerrors++] = dup;
}

static void report_error_fmt(cdc_shred_report_t *r, const char *fmt,
                             const char *path) {
    char buf[1024];
    snprintf(buf, sizeof(buf), fmt, path);
    report_error(r, buf);
}

#ifdef _WIN32
static int fill_random(uint8_t *buf, size_t n) {
    /* BCryptGenRandom（系统首选 RNG），与 Rust getrandom 同源。 */
    return BCryptGenRandom(NULL, buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static int shred_file(const char *path, uint32_t passes, uint64_t *out_size,
                      cdc_err_t *err) {
    wchar_t *wpath = cdc_utf8_to_wide(path);
    if (!wpath) {
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 path: %s", path);
        return CDC_ERR_UTF8;
    }
    HANDLE h = CreateFileW(
        wpath, GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "无法安全打开文件：%s（Win32 %lu）",
                        path, (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        CloseHandle(h);
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "无法读取文件信息：%s", path);
        return CDC_ERR_IO;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(h);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "不能粉碎符号链接或目录联接：%s", path);
        return CDC_ERR_INVALID_ARG;
    }
    uint64_t size =
        ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    uint8_t *buffer = (uint8_t *)malloc(1u << 20);
    if (!buffer) {
        CloseHandle(h);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    int rc = CDC_OK;
    for (uint32_t pass = 0; pass < (passes < 1 ? 1 : passes); pass++) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN)) {
            if (err)
                cdc_err_set(err, CDC_ERR_IO, "%s：设置文件指针失败", path);
            rc = CDC_ERR_IO;
            break;
        }
        uint64_t left = size;
        while (left > 0) {
            size_t block = left > (1u << 20) ? (1u << 20) : (size_t)left;
            if (!fill_random(buffer, block)) {
                if (err)
                    cdc_err_set(err, CDC_ERR_IO, "获取安全随机数失败");
                rc = CDC_ERR_IO;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(h, buffer, (DWORD)block, &written, NULL) ||
                written != block) {
                if (err)
                    cdc_err_set(err, CDC_ERR_IO, "覆盖写入失败：%s", path);
                rc = CDC_ERR_IO;
                break;
            }
            left -= block;
        }
        if (rc != CDC_OK) break;
        if (!FlushFileBuffers(h)) {
            if (err)
                cdc_err_set(err, CDC_ERR_IO, "%s：刷盘失败", path);
            rc = CDC_ERR_IO;
            break;
        }
    }
    free(buffer);
    if (rc == CDC_OK && !cdc_mark_delete((cdc_handle_t)h)) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "覆盖后无法删除：%s", path);
        rc = CDC_ERR_IO;
    }
    CloseHandle(h);
    if (rc == CDC_OK) *out_size = size;
    return rc;
}
#else
static int shred_file(const char *path, uint32_t passes, uint64_t *out_size,
                      cdc_err_t *err) {
    (void)path;
    (void)passes;
    (void)out_size;
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "文件粉碎仅支持 Windows：%s", path);
    return CDC_ERR_IO;
}
#endif

typedef struct {
    cdc_vec_t files; /* char* */
    cdc_vec_t dirs;  /* char* */
} shred_collect_t;

static int shred_collect_cb(void *ctx, const char *path, int is_dir,
                            int is_symlink, uint64_t size, uint64_t mtime,
                            uint32_t attrs) {
    (void)size;
    (void)mtime;
    (void)attrs;
    if (is_symlink) return 0;
    shred_collect_t *c = (shred_collect_t *)ctx;
    char *dup = strdup(path);
    if (!dup) return CDC_ERR_NOMEM;
    cdc_vec_t *v = is_dir ? &c->dirs : &c->files;
    if (cdc_vec_push(v, &dup) != CDC_OK) {
        free(dup);
        return CDC_ERR_NOMEM;
    }
    return 0;
}

static int cmp_strlen_desc(const void *a, const void *b) {
    size_t la = strlen(*(char *const *)a);
    size_t lb = strlen(*(char *const *)b);
    if (la < lb) return 1;
    if (la > lb) return -1;
    return 0;
}

static void shred_one_file(const char *path, uint32_t passes,
                           cdc_shred_report_t *report, uint64_t *done) {
    (*done)++;
    add_done(*done);
    if (cdc_is_protected_path(path)) {
        report->files_failed++;
        report_error_fmt(report, "为保护系统，拒绝粉碎：%s", path);
        return;
    }
    uint64_t size = 0;
    cdc_err_t err;
    int rc = shred_file(path, passes, &size, &err);
    if (rc == CDC_OK) {
        report->freed += size;
        report->files_ok++;
    } else {
        report->files_failed++;
        report_error(report, err.message[0] ? err.message : "粉碎失败");
    }
}

static void shred_directory(const char *root, uint32_t passes,
                            cdc_shred_report_t *report, uint64_t *done) {
    shred_collect_t c;
    memset(&c, 0, sizeof(c));
    if (cdc_vec_init(&c.files, sizeof(char *)) != CDC_OK) return;
    if (cdc_vec_init(&c.dirs, sizeof(char *)) != CDC_OK) {
        cdc_vec_free(&c.files);
        return;
    }
    cdc_err_t err;
    if (cdc_walk_dir(root, 1, shred_collect_cb, &c, &err) != CDC_OK) {
        /* 遍历失败：按空目录处理（与 Rust filter_map(ok) 跳过失败项一致，
         * 失败的条目不计入文件数）。 */
    }
    for (size_t i = 0; i < c.files.len; i++) {
        if (cancelled()) {
            report_error(report, "已取消");
            break;
        }
        shred_one_file(((char **)c.files.data)[i], passes, report, done);
    }
    if (c.dirs.len > 1)
        qsort(c.dirs.data, c.dirs.len, sizeof(char *), cmp_strlen_desc);
    for (size_t i = 0; i < c.dirs.len; i++) {
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(((char **)c.dirs.data)[i]);
        if (w) {
            RemoveDirectoryW(w);
            free(w);
        }
#else
        remove(((char **)c.dirs.data)[i]);
#endif
    }
    if (report->files_failed == 0) {
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(root);
        if (w) {
            if (RemoveDirectoryW(w)) report->finished++;
            free(w);
        }
#else
        if (remove(root) == 0) report->finished++;
#endif
    }
    for (size_t i = 0; i < c.files.len; i++) free(((char **)c.files.data)[i]);
    for (size_t i = 0; i < c.dirs.len; i++) free(((char **)c.dirs.data)[i]);
    cdc_vec_free(&c.files);
    cdc_vec_free(&c.dirs);
}

int cdc_shred_paths(const char **paths, size_t npaths, uint32_t passes,
                    cdc_shred_report_t *out, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "out is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (passes < 1) passes = 1;
    if (passes > CDC_SHRED_MAX_PASSES) passes = CDC_SHRED_MAX_PASSES;
    uint64_t done = 0;
    for (size_t i = 0; i < npaths; i++) {
        const char *path = paths[i] ? paths[i] : "";
        if (cancelled()) {
            report_error(out, "已取消");
            break;
        }
        if (path[0] == '\0') {
            out->files_failed++;
            report_error_fmt(out, "文件不存在：%s", path);
            continue;
        }
        /* 存在性检查（不跟随链接：lstat / GetFileAttributesW）。 */
        int is_reparse = 0;
        cdc_err_t e2;
        int exists = cdc_reparse_point_status(path, &is_reparse, &e2) == CDC_OK;
        if (!exists) {
            out->files_failed++;
            report_error_fmt(out, "文件不存在：%s", path);
            continue;
        }
        if (is_reparse) {
            out->files_failed++;
            report_error_fmt(out, "不能粉碎符号链接或目录联接：%s", path);
            continue;
        }
        if (cdc_is_protected_path(path)) {
            out->files_failed++;
            report_error_fmt(out, "为保护系统，拒绝粉碎：%s", path);
            continue;
        }
        /* 是否目录。 */
        int is_dir = 0;
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(path);
        if (w) {
            DWORD a = GetFileAttributesW(w);
            is_dir = a != INVALID_FILE_ATTRIBUTES &&
                     (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
            free(w);
        }
#else
        struct stat st;
        is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
        if (is_dir) {
            shred_directory(path, passes, out, &done);
        } else {
            shred_one_file(path, passes, out, &done);
        }
    }
    return CDC_OK;
}
