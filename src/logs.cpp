/* logs.c — 移植自 Rust 版 desktop/src/logs.rs。 */
#include "logs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "safety.h"
#include "timeutil.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* 取可执行文件所在目录（LOCALAPPDATA 缺失时的退路，与 Rust 版一致）。 */
static char *exe_dir(void) {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(NULL, buf, 4096);
    if (n == 0 || n >= 4096) return NULL;
    char *utf8 = cdc_wide_to_utf8(buf);
    if (!utf8) return NULL;
    char *slash = strrchr(utf8, '\\');
    if (!slash) {
        free(utf8);
        return NULL;
    }
    *slash = '\0';
    return utf8;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    char *slash = strrchr(buf, '/');
    if (!slash) return NULL;
    *slash = '\0';
    return strdup(buf);
#endif
}

/* 拼 base + "\\" + sub（Windows）或 base + "/" + sub（Linux），并创建目录。 */
static char *join_and_create(const char *base, const char *sub) {
    if (!base) return NULL;
#ifdef _WIN32
    size_t n = strlen(base) + 1 + strlen(sub) + 1;
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s\\%s", base, sub);
    wchar_t *w = cdc_utf8_to_wide(out);
    if (w) {
        CreateDirectoryW(w, NULL);
        free(w);
    }
    return out;
#else
    size_t n = strlen(base) + 1 + strlen(sub) + 1;
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s/%s", base, sub);
    mkdir(out, 0755);
    return out;
#endif
}

char *cdc_app_data_dir(void) {
    char *base = cdc_getenv_utf8("LOCALAPPDATA");
    if (!base) base = exe_dir();
    if (!base) base = strdup(".");
    if (!base) return NULL;
    char *out = join_and_create(base, "C盘清理工具");
    free(base);
    return out;
}

/* cdc_cache_dir 的唯一实现在 util.c（此前这里和 mft.c 各有一份，
 * 同链会符号冲突；2026-11-17 统一到 util.c，本函数已删）。 */

char *cdc_logs_dir(void) {
    char *app = cdc_app_data_dir();
    if (!app) return NULL;
    char *out = join_and_create(app, "logs");
    free(app);
    return out;
}

/* 去尾空白（Rust str::trim_end：所有 Unicode 空白；这里只处理 ASCII
 * 空白 + 常见的 \r\n\t，路径与日志行里不会出现其它空白）。 */
static void rtrim_ascii(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == '\v' || s[n - 1] == '\f'))
        s[--n] = '\0';
}

#ifdef _WIN32
static int file_mtime_win(const wchar_t *wpath, uint64_t *out_mtime) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad))
        return 0;
    uint64_t ft = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
                  fad.ftLastWriteTime.dwLowDateTime;
    /* FILETIME → Unix 秒（mft_util 的算法内联，避免头文件依赖扩散）。 */
    *out_mtime = ft >= 116444736000000000ULL
                     ? (ft - 116444736000000000ULL) / 10000000ULL
                     : 0;
    return 1;
}
#endif

static void rotate_old_logs(const char *dir) {
    /* 只留最新的 100 个 .log（按 mtime 降序）。 */
#ifdef _WIN32
    size_t dn = strlen(dir);
    char *pattern = (char *)malloc(dn + 3);
    if (!pattern) return;
    snprintf(pattern, dn + 3, "%s\\*", dir);
    wchar_t *wp = cdc_utf8_to_wide(pattern);
    free(pattern);
    if (!wp) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) return;
    typedef struct {
        char *path;
        uint64_t mtime;
    } row_t;
    cdc_vec_t rows;
    if (cdc_vec_init(&rows, sizeof(row_t)) != CDC_OK) {
        FindClose(h);
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char *name = cdc_wide_to_utf8(fd.cFileName);
        if (!name) continue;
        size_t nl = strlen(name);
        if (nl < 4 || strcmp(name + nl - 4, ".log") != 0) {
            free(name);
            continue;
        }
        size_t pl = dn + 1 + nl + 1;
        char *full = (char *)malloc(pl);
        if (!full) {
            free(name);
            continue;
        }
        snprintf(full, pl, "%s\\%s", dir, name);
        free(name);
        wchar_t *wf = cdc_utf8_to_wide(full);
        uint64_t mtime = 0;
        if (wf) {
            file_mtime_win(wf, &mtime);
            free(wf);
        }
        row_t row;
        row.path = full;
        row.mtime = mtime;
        if (cdc_vec_push(&rows, &row) != CDC_OK) free(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    /* 按 mtime 降序排，100 个之后的删掉。 */
    for (size_t i = 0; i < rows.len; i++) {
        for (size_t j = i + 1; j < rows.len; j++) {
            row_t *a = &CDC_VEC_AT(&rows, row_t, i);
            row_t *b = &CDC_VEC_AT(&rows, row_t, j);
            if (b->mtime > a->mtime) {
                row_t tmp = *a;
                *a = *b;
                *b = tmp;
            }
        }
    }
    for (size_t i = 100; i < rows.len; i++) {
        row_t *r = &CDC_VEC_AT(&rows, row_t, i);
        wchar_t *w = cdc_utf8_to_wide(r->path);
        if (w) {
            DeleteFileW(w);
            free(w);
        }
        free(r->path);
    }
    for (size_t i = 0; i < rows.len && i < 100; i++)
        free(CDC_VEC_AT(&rows, row_t, i).path);
    cdc_vec_free(&rows);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    typedef struct {
        char *path;
        uint64_t mtime;
    } row_t;
    cdc_vec_t rows;
    if (cdc_vec_init(&rows, sizeof(row_t)) != CDC_OK) {
        closedir(d);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nl = strlen(ent->d_name);
        if (nl < 4 || strcmp(ent->d_name + nl - 4, ".log") != 0) continue;
        size_t pl = strlen(dir) + 1 + nl + 1;
        char *full = (char *)malloc(pl);
        if (!full) continue;
        snprintf(full, pl, "%s/%s", dir, ent->d_name);
        struct stat st;
        uint64_t mtime = 0;
        if (stat(full, &st) == 0) mtime = (uint64_t)st.st_mtime;
        row_t row;
        row.path = full;
        row.mtime = mtime;
        if (cdc_vec_push(&rows, &row) != CDC_OK) free(full);
    }
    closedir(d);
    for (size_t i = 0; i < rows.len; i++) {
        for (size_t j = i + 1; j < rows.len; j++) {
            row_t *a = &CDC_VEC_AT(&rows, row_t, i);
            row_t *b = &CDC_VEC_AT(&rows, row_t, j);
            if (b->mtime > a->mtime) {
                row_t tmp = *a;
                *a = *b;
                *b = tmp;
            }
        }
    }
    for (size_t i = 100; i < rows.len; i++) {
        row_t *r = &CDC_VEC_AT(&rows, row_t, i);
        remove(r->path);
        free(r->path);
    }
    for (size_t i = 0; i < rows.len && i < 100; i++)
        free(CDC_VEC_AT(&rows, row_t, i).path);
    cdc_vec_free(&rows);
#endif
}

int cdc_write_log(const char **lines, size_t nlines, char **out_path,
                  cdc_err_t *err) {
    if (err) cdc_err_init(err);
    char *dir = cdc_logs_dir();
    if (!dir) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "cannot determine log dir");
        return CDC_ERR_IO;
    }
    char *stamp = cdc_stamp();
    if (!stamp) {
        free(dir);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    char *path = NULL;
    int index = 0;
    for (;;) {
        /* dir + sep + stamp + ("_NNNNN" 最多 6 位) + ".log" + NUL，留足余量。 */
        size_t n = strlen(dir) + 1 + strlen(stamp) + 6 + 4 + 1;
        free(path);
        path = (char *)malloc(n);
        if (!path) {
            free(dir);
            free(stamp);
            if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
            return CDC_ERR_NOMEM;
        }
        if (index == 0)
            snprintf(path, n, "%s%s%s.log", dir, sep, stamp);
        else
            snprintf(path, n, "%s%s%s_%d.log", dir, sep, stamp, index);
        FILE *f = cdc_fopen_utf8(path, "rb");
        if (!f) break; /* 不存在，可用 */
        fclose(f);
        index++;
        if (index > 10000) {
            /* 先拼错误文案再 free：cdc_err_set 把文案拷进 err->message，
             * 之后 stamp 才可释放（之前是先 free 再读 stamp，
             * use-after-free——同一秒内 10001 个日志文件才触发，
             * 几乎不可达但确实是 bug）。 */
            if (err)
                cdc_err_set(err, CDC_ERR_IO, "too many log files for %s",
                            stamp);
            free(path);
            free(dir);
            free(stamp);
            return CDC_ERR_IO;
        }
    }
    free(stamp);
    FILE *f = cdc_fopen_utf8(path, "wb");
    if (!f) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "cannot create log file: %s",
                             path);
        free(path);
        free(dir);
        return CDC_ERR_IO;
    }
    for (size_t i = 0; i < nlines; i++) {
        char *line = strdup(lines[i] ? lines[i] : "");
        if (!line) continue;
        rtrim_ascii(line);
        fprintf(f, "%s\n", line);
        free(line);
    }
    fclose(f);
    rotate_old_logs(dir);
    free(dir);
    if (out_path) {
        *out_path = path;
    } else {
        free(path);
    }
    return CDC_OK;
}

static int log_name_valid(const char *name) {
    if (!name || name[0] == '\0') return 0;
    if (strchr(name, '\\') || strchr(name, '/')) return 0;
    if (strstr(name, "..")) return 0;
    return 1;
}

int cdc_append_line(const char *name, const char *line) {
    if (!log_name_valid(name)) return CDC_OK;
    char *dir = cdc_logs_dir();
    if (!dir) return CDC_OK;
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *path = (char *)malloc(n);
    if (!path) {
        free(dir);
        return CDC_OK;
    }
    snprintf(path, n, "%s%s%s", dir, sep, name);
    free(dir);
    FILE *f = cdc_fopen_utf8(path, "ab");
    if (f) {
        fprintf(f, "%s\n", line ? line : "");
        fclose(f);
    }
    free(path);
    return CDC_OK;
}

static int cmp_log_mtime_desc(const void *a, const void *b) {
    const cdc_log_entry_t *x = (const cdc_log_entry_t *)a;
    const cdc_log_entry_t *y = (const cdc_log_entry_t *)b;
    if (x->mtime < y->mtime) return 1;
    if (x->mtime > y->mtime) return -1;
    return 0;
}

int cdc_logs_list(cdc_log_entry_t **out, size_t *out_n, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_n = 0;
    char *dir = cdc_logs_dir();
    if (!dir) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "cannot determine log dir");
        return CDC_ERR_IO;
    }
    cdc_vec_t rows;
    if (cdc_vec_init(&rows, sizeof(cdc_log_entry_t)) != CDC_OK) {
        free(dir);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
#ifdef _WIN32
    size_t dn = strlen(dir);
    char *pattern = (char *)malloc(dn + 3);
    if (pattern) {
        snprintf(pattern, dn + 3, "%s\\*", dir);
        wchar_t *wp = cdc_utf8_to_wide(pattern);
        free(pattern);
        if (wp) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(wp, &fd);
            free(wp);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    char *name = cdc_wide_to_utf8(fd.cFileName);
                    if (!name) continue;
                    cdc_log_entry_t row;
                    row.name = name;
                    row.size = ((uint64_t)fd.nFileSizeHigh << 32) |
                               fd.nFileSizeLow;
                    uint64_t ft =
                        ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                        fd.ftLastWriteTime.dwLowDateTime;
                    row.mtime = ft >= 116444736000000000ULL
                                    ? (ft - 116444736000000000ULL) / 10000000ULL
                                    : 0;
                    if (cdc_vec_push(&rows, &row) != CDC_OK) free(name);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t pl = strlen(dir) + 1 + strlen(ent->d_name) + 1;
            char *full = (char *)malloc(pl);
            if (!full) continue;
            snprintf(full, pl, "%s/%s", dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                cdc_log_entry_t row;
                row.name = strdup(ent->d_name);
                row.size = (uint64_t)st.st_size;
                row.mtime = (uint64_t)st.st_mtime;
                if (!row.name ||
                    cdc_vec_push(&rows, &row) != CDC_OK)
                    free(row.name);
            }
            free(full);
        }
        closedir(d);
    }
#endif
    free(dir);
    if (rows.len > 1)
        qsort(rows.data, rows.len, sizeof(cdc_log_entry_t),
              cmp_log_mtime_desc);
    *out = (cdc_log_entry_t *)rows.data;
    *out_n = rows.len;
    /* 交出所有权：rows.data 已移交，只重置 vec 壳。 */
    rows.data = NULL;
    rows.len = rows.cap = 0;
    cdc_vec_free(&rows);
    return CDC_OK;
}

void cdc_logs_list_free(cdc_log_entry_t *entries, size_t n) {
    if (!entries) return;
    for (size_t i = 0; i < n; i++) free(entries[i].name);
    free(entries);
}

/* UTF-8 lossy：非法序列替换为 U+FFFD（与 Rust String::from_utf8_lossy
 * 同策略）。 */
static char *utf8_lossy(const uint8_t *data, size_t len) {
    cdc_buf_t b;
    cdc_buf_init(&b);
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        size_t adv = 0;
        if (c < 0x80) {
            adv = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len &&
                   (data[i + 1] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(c & 0x1F) << 6) |
                          (uint32_t)(data[i + 1] & 0x3F);
            adv = cp >= 0x80 ? 2 : 0;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len &&
                   (data[i + 1] & 0xC0) == 0x80 &&
                   (data[i + 2] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(c & 0x0F) << 12) |
                          ((uint32_t)(data[i + 1] & 0x3F) << 6) |
                          (uint32_t)(data[i + 2] & 0x3F);
            adv = (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) ? 3 : 0;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len &&
                   (data[i + 1] & 0xC0) == 0x80 &&
                   (data[i + 2] & 0xC0) == 0x80 &&
                   (data[i + 3] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(c & 0x07) << 18) |
                          ((uint32_t)(data[i + 1] & 0x3F) << 12) |
                          ((uint32_t)(data[i + 2] & 0x3F) << 6) |
                          (uint32_t)(data[i + 3] & 0x3F);
            adv = (cp >= 0x10000 && cp <= 0x10FFFF) ? 4 : 0;
        }
        if (adv == 0) {
            static const uint8_t repl[3] = { 0xEF, 0xBF, 0xBD };
            if (cdc_buf_append(&b, repl, 3) != CDC_OK) break;
            i++;
        } else {
            if (cdc_buf_append(&b, data + i, adv) != CDC_OK) break;
            i += adv;
        }
    }
    char *out = cdc_buf_take_cstr(&b);
    if (!out) {
        cdc_buf_free(&b);
        return strdup("");
    }
    return out;
}

int cdc_log_read(const char *name, char **out_text, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!log_name_valid(name)) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "日志名不合法");
        return CDC_ERR_INVALID_ARG;
    }
    if (!out_text) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "out_text is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    char *dir = cdc_logs_dir();
    if (!dir) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "cannot determine log dir");
        return CDC_ERR_IO;
    }
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *path = (char *)malloc(n);
    if (!path) {
        free(dir);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    snprintf(path, n, "%s%s%s", dir, sep, name);
    free(dir);
    FILE *f = cdc_fopen_utf8(path, "rb");
    if (!f) {
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "日志不存在");
        free(path);
        return CDC_ERR_NOT_FOUND;
    }
    /* 必须是普通文件（防目录穿越后读到目录）。 */
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    int is_file = 0;
    if (w) {
        DWORD a = GetFileAttributesW(w);
        is_file = a != INVALID_FILE_ATTRIBUTES &&
                  (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
        free(w);
    }
    if (!is_file) {
        fclose(f);
        free(path);
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "日志不存在");
        return CDC_ERR_NOT_FOUND;
    }
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        free(path);
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "日志不存在");
        return CDC_ERR_NOT_FOUND;
    }
#endif
    cdc_buf_t b;
    cdc_buf_init(&b);
    uint8_t chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (cdc_buf_append(&b, chunk, got) != CDC_OK) break;
    }
    fclose(f);
    free(path);
    char *text = utf8_lossy(b.data ? b.data : (const uint8_t *)"", b.len);
    cdc_buf_free(&b);
    if (!text) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    *out_text = text;
    return CDC_OK;
}
