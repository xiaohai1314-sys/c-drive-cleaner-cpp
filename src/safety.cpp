/* safety.c — 移植自 Rust 版 desktop/src/safety.rs（逐行对照，行为不变）。
 *
 * 注意点（对照 Rust 源码）：
 *   - 大小写折叠只做 ASCII：Rust 的 to_lowercase()/to_uppercase() 是
 *     Unicode 感知的，C 这里只折叠 A-Z；中文路径不受影响，非 ASCII
 *     字母（如 Ä/ä）的大小写差异不会被折叠——实测中未见此类路径，
 *     记为已知微小偏差（见汇报）。
 *   - trim 只处理 ASCII 空白（" \t\n\v\f\r"）；Rust trim() 还会去掉
 *     Unicode 空白，路径里基本不出现。
 *   - glob 匹配是单段匹配：pattern 与 text 都是单个路径段（文件名或
 *     catalog 的一段），'*' 不跨 '/'。
 */
#include "safety.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allowlist_data.h"
#include "cJSON.h"
#include "mft_util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------- 常量表（与 Rust 版逐字一致） ---------------- */

static const char *const PASSWORD_FILENAMES[] = {
    "login data", "login data-journal", "login data for account",
    "login data for account-journal", "logins.json", "logins-backup.json",
    "key4.db", "key3.db",
};
static const char *const CRITICAL_FILES[] = {
    "pagefile.sys", "hiberfil.sys", "swapfile.sys", "bootmgr",
    "ntldr", "ntuser.dat", "ntuser.dat.log",
};
static const char *const NEVER_CLEAN_ROOTS[] = {
    "\\recovery", "\\system volume information",
};
static const char *const NEVER_CLEAN_FILES[] = { "usmt.ppkg" };
static const char *const LIBRARY_NAMES[] = {
    "documents", "desktop", "downloads", "my documents",
    "文档", "桌面", "下载",
    "pictures", "music", "videos",
};

/* ---------------- 内部字符串小工具 ---------------- */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *out = (char *)malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static char *xstrndup(const char *s, size_t n) {
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* ASCII 小写（原地）。 */
static void lower_ascii_in_place(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + ('a' - 'A'));
    }
}

static char *lower_ascii_dup(const char *s) {
    char *out = xstrdup(s);
    if (out) lower_ascii_in_place(out);
    return out;
}

static int starts_with(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* 去掉首尾 ASCII 空白（返回新串）。 */
static char *trim_ascii(const char *s) {
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\v' ||
                  *s == '\f' || *s == '\r'))
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' ||
                     s[n - 1] == '\v' || s[n - 1] == '\f' || s[n - 1] == '\r'))
        n--;
    return xstrndup(s, n);
}

/* 去掉首尾指定字符（返回新串）。 */
static char *trim_chars(const char *s, char c) {
    while (*s == c) s++;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == c) n--;
    return xstrndup(s, n);
}

static char *trim_trailing_chars(const char *s, const char *chars) {
    size_t n = strlen(s);
    while (n > 0 && strchr(chars, s[n - 1])) n--;
    return xstrndup(s, n);
}

/* 把 s 里的 '/' 换成 '\\'（原地）。 */
static void slash_to_backslash(char *s) {
    for (; *s; s++) {
        if (*s == '/') *s = '\\';
    }
}

/* 按分隔符切分，跳过空段；返回 malloc 的字符串数组（cdc_strvec_free 释放）。 */
static char **split_parts(const char *s, size_t *out_n) {
    *out_n = 0;
    cdc_vec_t v;
    if (cdc_vec_init(&v, sizeof(char *)) != CDC_OK) return NULL;
    const char *p = s;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '\\' && *p != '/') p++;
        char *part = xstrndup(start, (size_t)(p - start));
        if (!part || cdc_vec_push(&v, &part) != CDC_OK) {
            free(part);
            goto fail;
        }
    }
    {
        char **arr = (char **)v.data;
        size_t n = v.len;
        /* 交出所有权：cdc_vec_free 会 free data，这里只重置 vec 本身。 */
        v.data = NULL;
        v.len = v.cap = 0;
        cdc_vec_free(&v);
        *out_n = n;
        return arr;
    }
fail:
    for (size_t i = 0; i < v.len; i++) free(((char **)v.data)[i]);
    cdc_vec_free(&v);
    return NULL;
}

void cdc_strvec_free(char **vec, size_t n) {
    if (!vec) return;
    for (size_t i = 0; i < n; i++) free(vec[i]);
    free(vec);
}

char *cdc_getenv_utf8(const char *name) {
#ifdef _WIN32
    wchar_t *wname = cdc_utf8_to_wide(name);
    if (!wname) return NULL;
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(wname, buf, 4096);
    if (n == 0) {
        free(wname);
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return NULL;
        return xstrdup(""); /* 存在但为空 */
    }
    if (n >= 4096) {
        /* 超长：按需分配再读一次。 */
        wchar_t *big = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
        if (!big) {
            free(wname);
            return NULL;
        }
        if (GetEnvironmentVariableW(wname, big, n + 1) == 0) {
            free(big);
            free(wname);
            return NULL;
        }
        char *out = cdc_wide_to_utf8(big);
        free(big);
        free(wname);
        return out;
    }
    free(wname);
    return cdc_wide_to_utf8(buf);
#else
    const char *v = getenv(name);
    if (!v) return NULL;
    return xstrdup(v);
#endif
}

FILE *cdc_fopen_utf8(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
#ifdef _WIN32
    wchar_t *wpath = cdc_utf8_to_wide(path);
    wchar_t *wmode = cdc_utf8_to_wide(mode);
    if (!wpath || !wmode) {
        free(wpath);
        free(wmode);
        return NULL;
    }
    FILE *f = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return f;
#else
    return fopen(path, mode);
#endif
}

/* ---------------- UTF-8 码点解码（供 glob '?' 与字符类用） ---------------- */

static size_t utf8_decode_one(const char *s, uint32_t *cp) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
              (uint32_t)(s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD; /* 非法字节按一个 U+FFFD 计，与 Rust lossy 策略一致 */
    return 1;
}

/* ---------------- glob 单段匹配 ---------------- */

/* 匹配 "[...]" 字符类：*p 指向 '['；成功返回 1 并把 *p 移到 ']' 之后。 */
static int glob_class(const char **pp, uint32_t cp) {
    const char *p = *pp + 1; /* 跳过 '[' */
    int negate = 0;
    if (*p == '!') {
        negate = 1;
        p++;
    }
    int matched = 0;
    int first = 1;
    while (*p && (*p != ']' || first)) {
        uint32_t lo;
        size_t adv = utf8_decode_one(p, &lo);
        p += adv;
        first = 0;
        if (*p == '-' && p[1] && p[1] != ']') {
            uint32_t hi;
            p++; /* 跳过 '-' */
            size_t adv2 = utf8_decode_one(p, &hi);
            p += adv2;
            if (lo <= cp && cp <= hi) matched = 1;
        } else if (lo == cp) {
            matched = 1;
        }
    }
    if (*p == ']') p++;
    *pp = p;
    return negate ? !matched : matched;
}

int cdc_glob_match(const char *pattern, const char *text) {
    const char *p = pattern, *t = text;
    const char *star_p = NULL, *star_t = NULL;
    while (*t) {
        if (*p == '*') {
            /* 连续 '*' 合并。 */
            while (*p == '*') p++;
            star_p = p;
            star_t = t;
            if (!*p) return 1; /* 尾 '*' 匹配剩余全部（段内无 '/'） */
            continue;
        }
        if (*p == '?' && *t != '/') {
            uint32_t cp;
            size_t adv = utf8_decode_one(t, &cp);
            (void)cp;
            p++;
            t += adv;
            continue;
        }
        if (*p == '[') {
            uint32_t cp;
            size_t adv = utf8_decode_one(t, &cp);
            const char *save = p;
            if (glob_class(&p, cp)) {
                t += adv;
                continue;
            }
            p = save; /* 字符类不匹配：回退到 '*' 回溯 */
        } else if (*p && *p == *t) {
            p++;
            t++;
            continue;
        }
        /* 不匹配：回溯到最近的 '*'，让它多吃一个字符。 */
        if (star_p && *star_t && *star_t != '/') {
            p = star_p;
            star_t += 1; /* '*' 多吃一个字节（UTF-8 尾字节也行，最终看整串） */
            t = star_t;
            continue;
        }
        return 0;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

/* ---------------- basename / normalize ---------------- */

char *cdc_basename(const char *path) {
    if (!path) return xstrdup("");
    char *text = xstrdup(path);
    if (!text) return NULL;
    slash_to_backslash(text);
    char *trimmed = trim_trailing_chars(text, "\\");
    free(text);
    if (!trimmed) return NULL;
    char *last = strrchr(trimmed, '\\');
    char *out = xstrdup(last ? last + 1 : trimmed);
    free(trimmed);
    return out;
}

char *cdc_normalize(const char *path) {
    if (!path) return xstrdup("");
    char *trimmed = trim_ascii(path);
    if (!trimmed) return NULL;
    char *noquote = trim_chars(trimmed, '"');
    free(trimmed);
    if (!noquote) return NULL;
    slash_to_backslash(noquote);
    char *text = noquote;
    if (strlen(text) >= 2 && text[1] == ':') {
        char drive[3];
        drive[0] = (char)toupper((unsigned char)text[0]);
        drive[1] = ':';
        drive[2] = '\0';
        char *rest = xstrdup(text + 2);
        free(text);
        if (!rest) return NULL;
        /* 折叠连续 '\\'：Rust 是 while contains("\\\\") replace，效果等同。 */
        {
            char *w = rest, *r = rest;
            int prev_bs = 0;
            while (*r) {
                if (*r == '\\') {
                    if (!prev_bs) *w++ = *r;
                    prev_bs = 1;
                } else {
                    *w++ = *r;
                    prev_bs = 0;
                }
                r++;
            }
            *w = '\0';
        }
        if (rest[0] == '\0' || strcmp(rest, "\\") == 0) {
            free(rest);
            char *out = (char *)malloc(4);
            if (!out) return NULL;
            out[0] = drive[0];
            out[1] = ':';
            out[2] = '\\';
            out[3] = '\0';
            return out;
        }
        if (rest[0] != '\\') {
            size_t n = strlen(rest);
            char *nr = (char *)malloc(n + 2);
            if (!nr) {
                free(rest);
                return NULL;
            }
            nr[0] = '\\';
            memcpy(nr + 1, rest, n + 1);
            free(rest);
            rest = nr;
        }
        {
            char *t = trim_trailing_chars(rest, "\\");
            free(rest);
            if (!t) return NULL;
            size_t n = strlen(t);
            char *out = (char *)malloc(2 + n + 1);
            if (!out) {
                free(t);
                return NULL;
            }
            out[0] = drive[0];
            out[1] = ':';
            memcpy(out + 2, t, n + 1);
            free(t);
            return out;
        }
    }
    {
        char *out = trim_trailing_chars(text, "\\/");
        free(text);
        return out;
    }
}

/* tail：normalize 后去盘符、小写、补前导 '\\'。 */
static char *cdc_tail(const char *path) {
    char *norm = cdc_normalize(path);
    if (!norm) return NULL;
    char *rest = norm;
    if (strlen(norm) >= 2 && norm[1] == ':') rest = norm + 2;
    char *out = xstrdup(rest);
    free(norm);
    if (!out) return NULL;
    slash_to_backslash(out);
    lower_ascii_in_place(out);
    if (out[0] != '\\') {
        size_t n = strlen(out);
        char *nr = (char *)malloc(n + 2);
        if (!nr) {
            free(out);
            return NULL;
        }
        nr[0] = '\\';
        memcpy(nr + 1, out, n + 1);
        free(out);
        out = nr;
    }
    return out;
}

static int is_under(const char *tail_text, const char *marker) {
    char *m = lower_ascii_dup(marker);
    if (!m) return 0;
    char *mm = m;
    if (m[0] != '\\') {
        size_t n = strlen(m);
        mm = (char *)malloc(n + 2);
        if (!mm) {
            free(m);
            return 0;
        }
        mm[0] = '\\';
        memcpy(mm + 1, m, n + 1);
        free(m);
    }
    int ok = strcmp(tail_text, mm) == 0;
    if (!ok) {
        size_t n = strlen(mm);
        char *pref = (char *)malloc(n + 2);
        if (pref) {
            memcpy(pref, mm, n);
            pref[n] = '\\';
            pref[n + 1] = '\0';
            ok = starts_with(tail_text, pref);
            free(pref);
        }
    }
    free(mm);
    return ok;
}

int cdc_under_marker(const char *path, const char * const *markers,
                       size_t nmarkers) {
    char *text = cdc_tail(path);
    if (!text) return 0;
    int ok = 0;
    for (size_t i = 0; i < nmarkers; i++) {
        if (is_under(text, markers[i])) {
            ok = 1;
            break;
        }
    }
    free(text);
    return ok;
}

int cdc_is_drive_root(const char *path) {
    char *norm = cdc_normalize(path);
    if (!norm) return 0;
    size_t n = strlen(norm);
    int ok = (n == 2 && norm[1] == ':') ||
             (n == 3 && norm[1] == ':' && (norm[2] == '\\' || norm[2] == '/'));
    free(norm);
    return ok;
}

int cdc_is_critical_file(const char *path) {
    char *name = cdc_basename(path);
    if (!name) return 0;
    lower_ascii_in_place(name);
    int ok = 0;
    for (size_t i = 0; i < sizeof(CRITICAL_FILES) / sizeof(*CRITICAL_FILES); i++) {
        if (strcmp(name, CRITICAL_FILES[i]) == 0) {
            ok = 1;
            break;
        }
    }
    free(name);
    return ok;
}

int cdc_is_never_clean(const char *path) {
    if (!path) return 1;
    char *t = trim_ascii(path);
    if (!t) return 1;
    int empty = t[0] == '\0';
    free(t);
    if (empty) return 1;
    char *name = cdc_basename(path);
    if (!name) return 1;
    lower_ascii_in_place(name);
    int hit = 0;
    for (size_t i = 0; i < sizeof(NEVER_CLEAN_FILES) / sizeof(*NEVER_CLEAN_FILES); i++) {
        if (strcmp(name, NEVER_CLEAN_FILES[i]) == 0) {
            hit = 1;
            break;
        }
    }
    free(name);
    if (hit) return 1;
    return cdc_under_marker(path, NEVER_CLEAN_ROOTS,
                            sizeof(NEVER_CLEAN_ROOTS) / sizeof(*NEVER_CLEAN_ROOTS));
}

int cdc_is_password_store(const char *path) {
    char *name = cdc_basename(path);
    if (!name) return 0;
    lower_ascii_in_place(name);
    int ok = 0;
    for (size_t i = 0; i < sizeof(PASSWORD_FILENAMES) / sizeof(*PASSWORD_FILENAMES); i++) {
        if (strcmp(name, PASSWORD_FILENAMES[i]) == 0) {
            ok = 1;
            break;
        }
    }
    free(name);
    return ok;
}

int cdc_path_syntax_error(const char *path) {
    if (!path) return 1;
    char *text = trim_ascii(path);
    if (!text) return 1;
    int bad = 0;
    if (text[0] == '\0') {
        bad = 1;
    } else if (starts_with(text, "\\\\") || starts_with(text, "//")) {
        bad = 1;
    } else {
        const char *body = text;
        if (strlen(text) >= 2 && text[1] == ':') body = text + 2;
        if (strchr(body, ':')) {
            bad = 1;
        } else {
            /* 按 '\\' 或 '/' 切分，任一段为 ".." 即非法。 */
            const char *p = text;
            while (*p && !bad) {
                while (*p == '\\' || *p == '/') p++;
                if (!*p) break;
                const char *start = p;
                while (*p && *p != '\\' && *p != '/') p++;
                if ((size_t)(p - start) == 2 && start[0] == '.' && start[1] == '.')
                    bad = 1;
            }
        }
    }
    free(text);
    return bad;
}

/* env_roots：SystemRoot、WINDIR、ProgramFiles、ProgramFiles(x86)、ProgramData。 */
static char **env_roots(size_t *out_n) {
    static const char *const NAMES[] = {
        "SystemRoot", "WINDIR", "ProgramFiles", "ProgramFiles(x86)", "ProgramData",
    };
    *out_n = 0;
    cdc_vec_t v;
    if (cdc_vec_init(&v, sizeof(char *)) != CDC_OK) return NULL;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(*NAMES); i++) {
        char *value = cdc_getenv_utf8(NAMES[i]);
        if (!value) continue;
        char *norm = cdc_normalize(value);
        free(value);
        if (!norm) continue;
        lower_ascii_in_place(norm);
        if (cdc_vec_push(&v, &norm) != CDC_OK) {
            free(norm);
            goto fail;
        }
    }
    {
        char **arr = (char **)v.data;
        size_t n = v.len;
        v.data = NULL;
        v.len = v.cap = 0;
        cdc_vec_free(&v);
        *out_n = n;
        return arr;
    }
fail:
    for (size_t i = 0; i < v.len; i++) free(((char **)v.data)[i]);
    cdc_vec_free(&v);
    return NULL;
}

int cdc_is_system_path(const char *path) {
    if (!path) return 1;
    char *t = trim_ascii(path);
    if (!t) return 1;
    int empty = t[0] == '\0';
    free(t);
    if (empty || cdc_is_drive_root(path) || cdc_is_critical_file(path)) return 1;
    static const char *const MARKERS[] = {
        "\\windows", "\\system volume information", "\\recovery", "\\boot",
        "\\$recycle.bin",
    };
    if (cdc_under_marker(path, MARKERS, sizeof(MARKERS) / sizeof(*MARKERS)))
        return 1;
    char *target = cdc_normalize(path);
    if (!target) return 1;
    lower_ascii_in_place(target);
    char *tt = trim_trailing_chars(target, "\\/");
    free(target);
    if (!tt) return 1;
    static const char *const KEYS[] = { "SystemRoot", "WINDIR" };
    int ok = 0;
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(*KEYS); i++) {
        char *value = cdc_getenv_utf8(KEYS[i]);
        if (!value) continue;
        char *root = cdc_normalize(value);
        free(value);
        if (!root) continue;
        lower_ascii_in_place(root);
        char *rt = trim_trailing_chars(root, "\\/");
        free(root);
        if (!rt) continue;
        if (rt[0] != '\0') {
            if (strcmp(tt, rt) == 0) {
                ok = 1;
            } else {
                size_t n = strlen(rt);
                char *pref = (char *)malloc(n + 2);
                if (pref) {
                    memcpy(pref, rt, n);
                    pref[n] = '\\';
                    pref[n + 1] = '\0';
                    ok = starts_with(tt, pref);
                    free(pref);
                }
            }
        }
        free(rt);
        if (ok) break;
    }
    free(tt);
    return ok;
}

int cdc_is_protected_path(const char *path) {
    if (cdc_is_system_path(path)) return 1;
    static const char *const MARKERS[] = {
        "\\program files", "\\program files (x86)", "\\programdata", "\\perflogs",
    };
    if (cdc_under_marker(path, MARKERS, sizeof(MARKERS) / sizeof(*MARKERS)))
        return 1;
    char *target = cdc_normalize(path);
    if (!target) return 1;
    lower_ascii_in_place(target);
    char *tt = trim_trailing_chars(target, "\\/");
    free(target);
    if (!tt) return 1;
    size_t nroots = 0;
    char **roots = env_roots(&nroots);
    int ok = 0;
    for (size_t i = 0; roots && i < nroots; i++) {
        char *rt = trim_trailing_chars(roots[i], "\\/");
        if (!rt) continue;
        if (rt[0] != '\0') {
            if (strcmp(tt, rt) == 0) {
                ok = 1;
            } else {
                size_t n = strlen(rt);
                char *pref = (char *)malloc(n + 2);
                if (pref) {
                    memcpy(pref, rt, n);
                    pref[n] = '\\';
                    pref[n + 1] = '\0';
                    ok = starts_with(tt, pref);
                    free(pref);
                }
            }
        }
        free(rt);
        if (ok) break;
    }
    cdc_strvec_free(roots, nroots);
    if (!ok) {
        char *profile = cdc_getenv_utf8("USERPROFILE");
        if (profile) {
            char *norm = cdc_normalize(profile);
            free(profile);
            if (norm) {
                lower_ascii_in_place(norm);
                char *pt = trim_trailing_chars(norm, "\\/");
                free(norm);
                if (pt) {
                    ok = strcmp(tt, pt) == 0;
                    free(pt);
                }
            }
        }
    }
    free(tt);
    return ok;
}

/* user_library_roots：4 个 base × 10 个库名。 */
static char **user_library_roots(size_t *out_n) {
    static const char *const BASES[] = { "USERPROFILE", "HOME", "OneDrive",
                                         "OneDriveConsumer" };
    static const char *const NAMES[] = {
        "Documents", "Desktop", "Downloads", "My Documents",
        "文档", "桌面", "下载",
        "Pictures", "Music", "Videos",
    };
    *out_n = 0;
    cdc_vec_t v;
    if (cdc_vec_init(&v, sizeof(char *)) != CDC_OK) return NULL;
    for (size_t b = 0; b < sizeof(BASES) / sizeof(*BASES); b++) {
        char *base = cdc_getenv_utf8(BASES[b]);
        if (!base) continue;
        char *bt = trim_trailing_chars(base, "\\");
        free(base);
        if (!bt) continue;
        for (size_t i = 0; i < sizeof(NAMES) / sizeof(*NAMES); i++) {
            size_t n = strlen(bt) + 1 + strlen(NAMES[i]) + 1;
            char *joined = (char *)malloc(n);
            if (!joined) continue;
            snprintf(joined, n, "%s\\%s", bt, NAMES[i]);
            char *norm = cdc_normalize(joined);
            free(joined);
            if (!norm) continue;
            lower_ascii_in_place(norm);
            if (cdc_vec_push(&v, &norm) != CDC_OK) free(norm);
        }
        free(bt);
    }
    {
        char **arr = (char **)v.data;
        size_t n = v.len;
        v.data = NULL;
        v.len = v.cap = 0;
        cdc_vec_free(&v);
        *out_n = n;
        return arr;
    }
}

int cdc_is_user_library(const char *path) {
    if (!path) return 0;
    char *t = trim_ascii(path);
    if (!t) return 0;
    int empty = t[0] == '\0';
    free(t);
    if (empty) return 0;
    char *target = cdc_normalize(path);
    if (!target) return 0;
    lower_ascii_in_place(target);
    char *tt = trim_trailing_chars(target, "\\");
    free(target);
    if (!tt) return 0;
    size_t nroots = 0;
    char **roots = user_library_roots(&nroots);
    int ok = 0;
    for (size_t i = 0; roots && i < nroots; i++) {
        char *rt = trim_trailing_chars(roots[i], "\\");
        if (!rt) continue;
        if (strcmp(tt, rt) == 0) {
            ok = 1;
        } else {
            size_t n = strlen(rt);
            char *pref = (char *)malloc(n + 2);
            if (pref) {
                memcpy(pref, rt, n);
                pref[n] = '\\';
                pref[n + 1] = '\0';
                ok = starts_with(tt, pref);
                free(pref);
            }
        }
        free(rt);
        if (ok) break;
    }
    cdc_strvec_free(roots, nroots);
    if (ok) {
        free(tt);
        return 1;
    }
    /* tail 兜底：C:\Users\< anyone >\<库名> 或 C:\Users\<库名>。 */
    char *tail = cdc_tail(path);
    free(tt);
    if (!tail) return 0;
    size_t nparts = 0;
    char **parts = split_parts(tail, &nparts);
    free(tail);
    if (!parts) return 0;
    if (nparts >= 3 && strcmp(parts[0], "users") == 0) {
        for (size_t i = 0; i < sizeof(LIBRARY_NAMES) / sizeof(*LIBRARY_NAMES); i++) {
            if (strcmp(parts[2], LIBRARY_NAMES[i]) == 0) {
                ok = 1;
                break;
            }
        }
    }
    if (!ok && nparts >= 2 && strcmp(parts[0], "users") == 0) {
        for (size_t i = 0; i < sizeof(LIBRARY_NAMES) / sizeof(*LIBRARY_NAMES); i++) {
            if (strcmp(parts[1], LIBRARY_NAMES[i]) == 0) {
                ok = 1;
                break;
            }
        }
    }
    cdc_strvec_free(parts, nparts);
    return ok;
}

int cdc_is_within(const char *path, const char **roots, size_t nroots) {
    if (nroots == 0 || !roots) return 0;
    char *target = cdc_normalize(path);
    if (!target) return 0;
    lower_ascii_in_place(target);
    char *tt = trim_trailing_chars(target, "\\/");
    free(target);
    if (!tt) return 0;
    int ok = 0;
    for (size_t i = 0; i < nroots; i++) {
        char *base = cdc_normalize(roots[i]);
        if (!base) continue;
        lower_ascii_in_place(base);
        char *bt = trim_trailing_chars(base, "\\/");
        free(base);
        if (!bt) continue;
        if (bt[0] != '\0') {
            if (strcmp(tt, bt) == 0) {
                ok = 1;
            } else {
                size_t n = strlen(bt);
                char *pref = (char *)malloc(n + 2);
                if (pref) {
                    memcpy(pref, bt, n);
                    pref[n] = '\\';
                    pref[n + 1] = '\0';
                    ok = starts_with(tt, pref);
                    free(pref);
                }
            }
        }
        free(bt);
        if (ok) break;
    }
    free(tt);
    return ok;
}

int cdc_reparse_point_status(const char *path, int *out_is_reparse,
                             cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!path || !out_is_reparse) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "path is NULL");
        return CDC_ERR_INVALID_ARG;
    }
#ifdef _WIN32
    wchar_t *wpath = cdc_utf8_to_wide(path);
    if (!wpath) {
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 path");
        return CDC_ERR_UTF8;
    }
    DWORD attrs = GetFileAttributesW(wpath);
    free(wpath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            if (err)
                cdc_err_set(err, CDC_ERR_NOT_FOUND, "file not found: %s", path);
            return CDC_ERR_NOT_FOUND;
        }
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "GetFileAttributesW failed (%lu): %s",
                        (unsigned long)code, path);
        return CDC_ERR_IO;
    }
    *out_is_reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return CDC_OK;
#else
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            if (err)
                cdc_err_set(err, CDC_ERR_NOT_FOUND, "file not found: %s", path);
            return CDC_ERR_NOT_FOUND;
        }
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "lstat failed: %s", path);
        return CDC_ERR_IO;
    }
    *out_is_reparse = S_ISLNK(st.st_mode) ? 1 : 0;
    return CDC_OK;
#endif
}

int cdc_is_reparse_point(const char *path) {
    int out = 0;
    return cdc_reparse_point_status(path, &out, NULL) == CDC_OK && out;
}

/* ---------------- glob 有效性（供 catalog / 规则加载过滤非法 pattern） ---------------- */

/* Rust glob::Pattern::new 在字符类未闭合、范围倒置（[z-a]）时返回 Err，
 * 调用方用 filter_map 丢弃。本函数做同样判定。 */
static int glob_valid(const char *pattern) {
    const char *p = pattern;
    while (*p) {
        if (*p == '[') {
            p++;
            if (*p == '!') p++;
            if (*p == ']') p++; /* 首个 ']' 是字面量 */
            int closed = 0;
            uint32_t prev = 0;
            int have_prev = 0;
            while (*p) {
                if (*p == ']') {
                    closed = 1;
                    p++;
                    break;
                }
                uint32_t cp;
                size_t adv = utf8_decode_one(p, &cp);
                if (*p == '-' && p[adv] && p[adv] != ']') {
                    uint32_t hi;
                    size_t adv2 = utf8_decode_one(p + adv + 1, &hi);
                    (void)adv2;
                    if (have_prev && prev > hi) return 0; /* 倒置范围 */
                    p += adv + 1 + adv2;
                    have_prev = 0;
                    continue;
                }
                prev = cp;
                have_prev = 1;
                p += adv;
            }
            if (!closed) return 0;
        } else {
            p++;
        }
    }
    return 1;
}

int cdc_glob_valid(const char *pattern) {
    if (!pattern) return 0;
    return glob_valid(pattern);
}

/* ---------------- AppData catalog ---------------- */

typedef struct {
    char **segs; /* 小写段（glob pattern 串） */
    size_t nsegs;
} catalog_pattern_t;

typedef struct {
    char *app; /* 小写应用目录名 */
    cdc_vec_t patterns; /* catalog_pattern_t */
} catalog_app_t;

typedef struct {
    cdc_vec_t apps; /* catalog_app_t */
} catalog_map_t;

static void catalog_pattern_free(catalog_pattern_t *p) {
    cdc_strvec_free(p->segs, p->nsegs);
    p->segs = NULL;
    p->nsegs = 0;
}

static void catalog_map_free(catalog_map_t *m) {
    for (size_t i = 0; i < m->apps.len; i++) {
        catalog_app_t *app = &CDC_VEC_AT(&m->apps, catalog_app_t, i);
        free(app->app);
        for (size_t j = 0; j < app->patterns.len; j++)
            catalog_pattern_free(
                &CDC_VEC_AT(&app->patterns, catalog_pattern_t, j));
        cdc_vec_free(&app->patterns);
    }
    cdc_vec_free(&m->apps);
}

/* 从嵌入的 appdata_path_allowlist.json 建 catalog（与 Rust catalog()
 * 逐行一致：只取 %LOCALAPPDATA% / %APPDATA% 开头的条目；首段含 glob
 * 字符的整条丢弃；其余段逐个解析，非法 pattern 的段丢弃）。 */
static int catalog_build(catalog_map_t maps[2]) {
    cJSON *root = cJSON_Parse(CDC_ALLOWLIST_JSON);
    if (!root) return CDC_ERR_IO;
    cJSON *paths = cJSON_GetObjectItemCaseSensitive(root, "paths");
    if (!cJSON_IsArray(paths)) {
        cJSON_Delete(root);
        return CDC_ERR_IO;
    }
    static const char *const KEYS[2] = { "localappdata", "appdata" };
    int rc = CDC_OK;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, paths) {
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        char *folded = xstrdup(item->valuestring);
        if (!folded) {
            rc = CDC_ERR_NOMEM;
            break;
        }
        slash_to_backslash(folded);
        char *lower = lower_ascii_dup(folded);
        if (!lower) {
            free(folded);
            rc = CDC_ERR_NOMEM;
            break;
        }
        for (int k = 0; k < 2; k++) {
            char marker[32];
            snprintf(marker, sizeof(marker), "%%%s\\", KEYS[k]);
            if (!starts_with(lower, marker)) continue;
            const char *relative = folded + strlen(marker);
            size_t nparts = 0;
            char **parts = split_parts(relative, &nparts);
            if (!parts) {
                rc = CDC_ERR_NOMEM;
                break;
            }
            if (nparts == 0 || strpbrk(parts[0], "*?[") != NULL) {
                /* Rust 这里是内层 for 的 continue：换下一个 key 再试
                 * （实际上另一个 key 的 marker 不可能匹配，直接跳过本条）。 */
                cdc_strvec_free(parts, nparts);
                continue;
            }
            /* patterns = parts[1..]，逐段小写、非法 glob 的段丢弃。 */
            catalog_pattern_t pat;
            pat.segs = NULL;
            pat.nsegs = 0;
            if (nparts > 1) {
                pat.segs = (char **)calloc(nparts - 1, sizeof(char *));
                if (!pat.segs) {
                    cdc_strvec_free(parts, nparts);
                    rc = CDC_ERR_NOMEM;
                    break;
                }
                for (size_t i = 1; i < nparts; i++) {
                    char *seg = lower_ascii_dup(parts[i]);
                    if (!seg) continue;
                    if (!glob_valid(seg)) {
                        free(seg);
                        continue;
                    }
                    pat.segs[pat.nsegs++] = seg;
                }
            }
            char *app_lower = lower_ascii_dup(parts[0]);
            cdc_strvec_free(parts, nparts);
            if (!app_lower) {
                catalog_pattern_free(&pat);
                rc = CDC_ERR_NOMEM;
                break;
            }
            /* 按 app 名找已有条目，没有则新建。 */
            catalog_app_t *slot = NULL;
            for (size_t i = 0; i < maps[k].apps.len; i++) {
                catalog_app_t *a = &CDC_VEC_AT(&maps[k].apps, catalog_app_t, i);
                if (strcmp(a->app, app_lower) == 0) {
                    slot = a;
                    break;
                }
            }
            if (!slot) {
                catalog_app_t fresh;
                fresh.app = app_lower;
                if (cdc_vec_init(&fresh.patterns, sizeof(catalog_pattern_t)) !=
                    CDC_OK) {
                    free(app_lower);
                    catalog_pattern_free(&pat);
                    rc = CDC_ERR_NOMEM;
                    break;
                }
                if (cdc_vec_push(&maps[k].apps, &fresh) != CDC_OK) {
                    free(app_lower);
                    cdc_vec_free(&fresh.patterns);
                    catalog_pattern_free(&pat);
                    rc = CDC_ERR_NOMEM;
                    break;
                }
                slot = &CDC_VEC_AT(&maps[k].apps, catalog_app_t,
                                    maps[k].apps.len - 1);
            } else {
                free(app_lower);
            }
            if (cdc_vec_push(&slot->patterns, &pat) != CDC_OK) {
                catalog_pattern_free(&pat);
                rc = CDC_ERR_NOMEM;
                break;
            }
            break; /* Rust 的 break：命中一个 key 即停 */
        }
        free(folded);
        free(lower);
        if (rc != CDC_OK) break;
    }
    cJSON_Delete(root);
    return rc;
}

#ifdef _WIN32
static CRITICAL_SECTION g_catalog_lock;
static LONG g_catalog_lock_init = 0;
static void catalog_lock(void) {
    if (InterlockedCompareExchange(&g_catalog_lock_init, 1, 0) == 0)
        InitializeCriticalSection(&g_catalog_lock);
    EnterCriticalSection(&g_catalog_lock);
}
static void catalog_unlock(void) { LeaveCriticalSection(&g_catalog_lock); }
#else
#include <pthread.h>
static pthread_mutex_t g_catalog_lock = PTHREAD_MUTEX_INITIALIZER;
static void catalog_lock(void) { pthread_mutex_lock(&g_catalog_lock); }
static void catalog_unlock(void) { pthread_mutex_unlock(&g_catalog_lock); }
#endif

static catalog_map_t g_catalog[2];
static int g_catalog_ready = 0;

static int catalog_ensure(void) {
    catalog_lock();
    if (!g_catalog_ready) {
        if (cdc_vec_init(&g_catalog[0].apps, sizeof(catalog_app_t)) == CDC_OK &&
            cdc_vec_init(&g_catalog[1].apps, sizeof(catalog_app_t)) == CDC_OK) {
            if (catalog_build(g_catalog) == CDC_OK) {
                g_catalog_ready = 1;
            } else {
                catalog_map_free(&g_catalog[0]);
                catalog_map_free(&g_catalog[1]);
            }
        }
    }
    catalog_unlock();
    return g_catalog_ready ? CDC_OK : CDC_ERR_IO;
}

static int catalog_matches(char **parts, size_t nparts, int key_idx) {
    if (nparts == 0) return 0;
    if (catalog_ensure() != CDC_OK) return 0;
    char *app_lower = lower_ascii_dup(parts[0]);
    if (!app_lower) return 0;
    catalog_map_t *map = &g_catalog[key_idx];
    int ok = 0;
    catalog_lock();
    for (size_t i = 0; i < map->apps.len && !ok; i++) {
        catalog_app_t *app = &CDC_VEC_AT(&map->apps, catalog_app_t, i);
        if (strcmp(app->app, app_lower) != 0) continue;
        for (size_t j = 0; j < app->patterns.len && !ok; j++) {
            catalog_pattern_t *pat =
                &CDC_VEC_AT(&app->patterns, catalog_pattern_t, j);
            if (nparts < pat->nsegs + 1) continue;
            int all = 1;
            for (size_t s = 0; s < pat->nsegs; s++) {
                char *value = lower_ascii_dup(parts[1 + s]);
                if (!value) {
                    all = 0;
                    break;
                }
                if (!cdc_glob_match(pat->segs[s], value)) all = 0;
                free(value);
                if (!all) break;
            }
            if (all) ok = 1;
        }
    }
    catalog_unlock();
    free(app_lower);
    return ok;
}

static int known_appdata_target(const char *path) {
    char *t = trim_ascii(path);
    if (!t) return 0;
    char *raw = xstrdup(t);
    free(t);
    if (!raw) return 0;
    slash_to_backslash(raw);
    char *lower = lower_ascii_dup(raw);
    if (!lower) {
        free(raw);
        return 0;
    }
    static const char *const KEYS[2] = { "localappdata", "appdata" };
    for (int k = 0; k < 2; k++) {
        char marker[32];
        snprintf(marker, sizeof(marker), "%%%s\\", KEYS[k]);
        if (!starts_with(lower, marker)) continue;
        const char *relative = raw + strlen(marker);
        size_t nparts = 0;
        char **parts = split_parts(relative, &nparts);
        int ok = parts ? catalog_matches(parts, nparts, k) : 0;
        cdc_strvec_free(parts, nparts);
        free(raw);
        free(lower);
        return ok; /* Rust 这里直接 return */
    }
    static const char *const ENVS[2] = { "LOCALAPPDATA", "APPDATA" };
    for (int k = 0; k < 2; k++) {
        char *root_env = cdc_getenv_utf8(ENVS[k]);
        if (!root_env) continue;
        char *root = cdc_normalize(root_env);
        free(root_env);
        if (!root) continue;
        lower_ascii_in_place(root);
        char *target = cdc_normalize(raw);
        if (!target) {
            free(root);
            continue;
        }
        lower_ascii_in_place(target);
        int under = strcmp(target, root) == 0;
        if (!under) {
            size_t n = strlen(root);
            char *pref = (char *)malloc(n + 2);
            if (pref) {
                memcpy(pref, root, n);
                pref[n] = '\\';
                pref[n + 1] = '\0';
                under = starts_with(target, pref);
                free(pref);
            }
        }
        if (under) {
            const char *relative = target + strlen(root);
            while (*relative == '\\' || *relative == '/') relative++;
            size_t nparts = 0;
            char **parts = split_parts(relative, &nparts);
            int ok = parts ? catalog_matches(parts, nparts, k) : 0;
            cdc_strvec_free(parts, nparts);
            free(root);
            free(target);
            free(raw);
            free(lower);
            return ok;
        }
        free(root);
        free(target);
    }
    free(raw);
    free(lower);
    return 0;
}

int cdc_under_remote_allowlist(const char *path) {
    if (cdc_path_syntax_error(path)) return 0;
    char *normalized = cdc_normalize(path);
    if (!normalized) return 0;
    if (cdc_is_system_path(normalized) || cdc_is_user_library(normalized)) {
        free(normalized);
        return 0;
    }
    cdc_vec_t roots;
    if (cdc_vec_init(&roots, sizeof(char *)) != CDC_OK) {
        free(normalized);
        return 0;
    }
    static const char *const TEMP_KEYS[] = { "TEMP", "TMP" };
    for (size_t i = 0; i < sizeof(TEMP_KEYS) / sizeof(*TEMP_KEYS); i++) {
        char *value = cdc_getenv_utf8(TEMP_KEYS[i]);
        if (!value) continue;
        if (!cdc_path_syntax_error(value)) {
            if (cdc_vec_push(&roots, &value) != CDC_OK) free(value);
        } else {
            free(value);
        }
    }
    char *local = cdc_getenv_utf8("LOCALAPPDATA");
    if (local) {
        char *lt = trim_trailing_chars(local, "\\");
        free(local);
        if (lt) {
            size_t n = strlen(lt) + 6;
            char *temp = (char *)malloc(n);
            if (temp) {
                snprintf(temp, n, "%s\\Temp", lt);
                if (cdc_vec_push(&roots, &temp) != CDC_OK) free(temp);
            }
            free(lt);
        }
    }
    const char **arr = (const char **)roots.data;
    int within = cdc_is_within(normalized, arr, roots.len);
    for (size_t i = 0; i < roots.len; i++) free(((char **)roots.data)[i]);
    cdc_vec_free(&roots);
    if (within) {
        free(normalized);
        return 1;
    }
    int ok = known_appdata_target(normalized);
    free(normalized);
    return ok;
}

int cdc_execution_forbidden(const char *path, int untrusted) {
    if (cdc_path_syntax_error(path)) return 1;
    if (cdc_is_never_clean(path)) return 1;
    if (cdc_is_reparse_point(path)) return 1;
    if (cdc_is_password_store(path) || cdc_is_user_library(path)) return 1;
    if (cdc_is_drive_root(path) || cdc_is_critical_file(path)) return 1;
    if (untrusted && (cdc_is_system_path(path) || !cdc_under_remote_allowlist(path)))
        return 1;
    return 0;
}

int cdc_explicit_delete_forbidden(const char *path) {
    if (cdc_path_syntax_error(path)) return 1;
    if (cdc_is_never_clean(path)) return 1;
    if (cdc_is_reparse_point(path)) return 1;
    if (cdc_is_password_store(path) || cdc_is_drive_root(path) ||
        cdc_is_critical_file(path))
        return 1;
    return cdc_is_protected_path(path);
}

/* ---------------- 目录遍历 ---------------- */

/* 递归目录遍历（单一实现，走 util.c 的 cdc_enum_dir）：Windows 用
 * NtQueryDirectoryFile 批量取，POSIX 用 opendir/readdir。
 * 语义与旧的两份实现一致：不跟随链接、回调失败即停、子目录递归
 * 与 min_depth 门限无关（门限只管回调）；打不开的目录按 err 报告。
 * 两处有意的统一：
 *  - 缺失目录：旧 Windows 版 FindFirstFileW 拿 ERROR_FILE_NOT_FOUND
 *    当空目录返回 OK，POSIX 版返回 NOT_FOUND；新实现两平台都返回
 *    NOT_FOUND（调用方要么忽略返回值、要么把失败按空目录处理，
 *    见 shred.c，故无行为变化）。
 *  - 名字解码：旧 Windows 版遇孤立代理对返回 UTF8 错、中断整棵遍历；
 *    新实现按 U+FFFD 替换后继续（与 cdc_wide_to_utf8 同策略）。 */
static int walk_dir_rec(const char *dir, int depth, int min_depth,
                        cdc_walk_fn fn, void *ctx, cdc_err_t *err);

typedef struct {
    const char *dir;
    int depth;
    int min_depth;
    cdc_walk_fn fn;
    void *ctx;
    cdc_err_t *err;
} WalkDirCtx;

static int walk_enum_cb(const char *name, int is_dir, int is_symlink, uint64_t size,
                        uint64_t mtime, uint32_t attrs, void *ctx) {
    WalkDirCtx *c = (WalkDirCtx *)ctx;
    char *full;
    int rc = CDC_OK;
    size_t dn = strlen(c->dir);
    int trailing = dn > 0 && (c->dir[dn - 1] == '\\' || c->dir[dn - 1] == '/');
    size_t fn_len = dn + (trailing ? 0 : 1) + strlen(name) + 1;
    full = (char *)malloc(fn_len);
    if (!full) return CDC_ERR_NOMEM;
#ifdef _WIN32
    snprintf(full, fn_len, trailing ? "%s%s" : "%s\\%s", c->dir, name);
#else
    snprintf(full, fn_len, trailing ? "%s%s" : "%s/%s", c->dir, name);
#endif
    if (c->depth >= c->min_depth) {
        rc = c->fn(c->ctx, full, is_dir, is_symlink, size, mtime, attrs);
        if (rc != 0) {
            free(full);
            return rc;
        }
    }
    if (is_dir && !is_symlink)
        rc = walk_dir_rec(full, c->depth + 1, c->min_depth, c->fn, c->ctx, c->err);
    free(full);
    return rc;
}

static int walk_dir_rec(const char *dir, int depth, int min_depth,
                        cdc_walk_fn fn, void *ctx, cdc_err_t *err) {
    WalkDirCtx c;
    int rc;
    c.dir = dir;
    c.depth = depth;
    c.min_depth = min_depth;
    c.fn = fn;
    c.ctx = ctx;
    c.err = err;
    rc = cdc_enum_dir(dir, walk_enum_cb, &c);
    if (rc == CDC_ERR_NOT_FOUND) {
        if (err)
            cdc_err_set(err, CDC_ERR_NOT_FOUND, "directory not found: %s", dir);
        return CDC_ERR_NOT_FOUND;
    }
    if (rc == CDC_ERR_IO) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "directory enumeration failed: %s", dir);
        return CDC_ERR_IO;
    }
    return rc;
}

int cdc_walk_dir(const char *root, int min_depth, cdc_walk_fn fn, void *ctx,
                 cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!root || !fn) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "root/fn is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    if (min_depth <= 0) {
        /* 先回调 root 本身（Rust WalkDir min_depth(0) 的行为）。 */
        int is_reparse = 0;
        cdc_err_t e2;
        int rc2 = cdc_reparse_point_status(root, &is_reparse, &e2);
        if (rc2 != CDC_OK) {
            if (err) *err = e2;
            return rc2;
        }
        uint64_t size = 0, mtime = 0;
        uint32_t attrs = 0;
        int is_dir = 0;
#ifdef _WIN32
        wchar_t *wroot = cdc_utf8_to_wide(root);
        if (wroot) {
            DWORD a = GetFileAttributesW(wroot);
            if (a != INVALID_FILE_ATTRIBUTES) {
                attrs = a;
                is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(wroot, GetFileExInfoStandard, &fad)) {
                uint64_t ft = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime
                               << 32) |
                              fad.ftLastWriteTime.dwLowDateTime;
                mtime = mft_filetime_to_unix(ft);
                if (!is_dir)
                    size = ((uint64_t)fad.nFileSizeHigh << 32) |
                           fad.nFileSizeLow;
            }
            free(wroot);
        }
#else
        struct stat st;
        if (lstat(root, &st) == 0) {
            attrs = (uint32_t)st.st_mode;
            is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            if (S_ISREG(st.st_mode)) size = (uint64_t)st.st_size;
            mtime = (uint64_t)st.st_mtime;
        }
#endif
        int rc = fn(ctx, root, is_dir, is_reparse, size, mtime, attrs);
        if (rc != 0) return rc;
    }
    return walk_dir_rec(root, 1, min_depth, fn, ctx, err);
}

/* ---------------- 句柄链与句柄 I/O ---------------- */

void cdc_handle_chain_free(cdc_handle_chain_t *chain) {
    if (!chain) return;
#ifdef _WIN32
    for (size_t i = 0; i < chain->vec.len; i++) {
        cdc_handle_t h = CDC_VEC_AT(&chain->vec, cdc_handle_t, i);
        if (h) CloseHandle((HANDLE)h);
    }
#endif
    cdc_vec_free(&chain->vec);
    free(chain);
}

#ifdef _WIN32 /* chain_new / chain_push 只在 Win32 链式打开里用 */
static cdc_handle_chain_t *chain_new(void) {
    cdc_handle_chain_t *c =
        (cdc_handle_chain_t *)calloc(1, sizeof(cdc_handle_chain_t));
    if (!c) return NULL;
    if (cdc_vec_init(&c->vec, sizeof(cdc_handle_t)) != CDC_OK) {
        free(c);
        return NULL;
    }
    return c;
}

static int chain_push(cdc_handle_chain_t *c, cdc_handle_t h, cdc_err_t *err) {
    if (cdc_vec_push(&c->vec, &h) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}
#endif /* _WIN32 */

int cdc_handle_file_size(cdc_handle_t h, uint64_t *out_size, cdc_err_t *err) {
    if (err) cdc_err_init(err);
#ifdef _WIN32
    if (!h || !out_size) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "handle is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx((HANDLE)h, &size)) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "GetFileSizeEx failed (%lu)",
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    *out_size = (uint64_t)size.QuadPart;
    return CDC_OK;
#else
    (void)h;
    (void)out_size;
    if (err) cdc_err_set(err, CDC_ERR_IO, "file handles are Windows-only");
    return CDC_ERR_IO;
#endif
}

int cdc_handle_read(cdc_handle_t h, uint8_t *buf, size_t want, size_t *out_got,
                    cdc_err_t *err) {
    if (err) cdc_err_init(err);
#ifdef _WIN32
    if (!h || !buf || !out_got) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL argument");
        return CDC_ERR_INVALID_ARG;
    }
    DWORD got = 0;
    if (!ReadFile((HANDLE)h, buf, (DWORD)want, &got, NULL)) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "ReadFile failed (%lu)",
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    *out_got = got;
    return CDC_OK;
#else
    (void)h;
    (void)buf;
    (void)want;
    (void)out_got;
    if (err) cdc_err_set(err, CDC_ERR_IO, "file handles are Windows-only");
    return CDC_ERR_IO;
#endif
}

int cdc_handle_write_all(cdc_handle_t h, const uint8_t *buf, size_t len,
                         cdc_err_t *err) {
    if (err) cdc_err_init(err);
#ifdef _WIN32
    if (!h || (!buf && len > 0)) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL argument");
        return CDC_ERR_INVALID_ARG;
    }
    size_t off = 0;
    while (off < len) {
        DWORD chunk = len - off > 0x40000000u ? 0x40000000u : (DWORD)(len - off);
        DWORD written = 0;
        if (!WriteFile((HANDLE)h, buf + off, chunk, &written, NULL)) {
            if (err)
                cdc_err_set(err, CDC_ERR_IO, "WriteFile failed (%lu)",
                            (unsigned long)GetLastError());
            return CDC_ERR_IO;
        }
        off += written;
    }
    return CDC_OK;
#else
    (void)h;
    (void)buf;
    (void)len;
    if (err) cdc_err_set(err, CDC_ERR_IO, "file handles are Windows-only");
    return CDC_ERR_IO;
#endif
}

int cdc_handle_sync(cdc_handle_t h, cdc_err_t *err) {
    if (err) cdc_err_init(err);
#ifdef _WIN32
    if (!h) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "handle is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    if (!FlushFileBuffers((HANDLE)h)) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "FlushFileBuffers failed (%lu)",
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    return CDC_OK;
#else
    (void)h;
    if (err) cdc_err_set(err, CDC_ERR_IO, "file handles are Windows-only");
    return CDC_ERR_IO;
#endif
}

int cdc_handle_seek_start(cdc_handle_t h, cdc_err_t *err) {
    if (err) cdc_err_init(err);
#ifdef _WIN32
    if (!h) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "handle is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx((HANDLE)h, zero, NULL, FILE_BEGIN)) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "SetFilePointerEx failed (%lu)",
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    return CDC_OK;
#else
    (void)h;
    if (err) cdc_err_set(err, CDC_ERR_IO, "file handles are Windows-only");
    return CDC_ERR_IO;
#endif
}

/* ---------------- Win32 句柄删除 ---------------- */

#ifdef _WIN32

#define CDC_FILE_ATTRIBUTE_REPARSE_POINT 0x400u
#define CDC_DELETE_ACCESS 0x00010000u
#define CDC_FILE_DISPOSITION_INFO_CLASS 4
#define CDC_FILE_BASIC_INFO_CLASS 0
#define CDC_FILE_RENAME_INFO_CLASS 10
#define CDC_FILE_READ_ATTRIBUTES 0x00000080u
#define CDC_FILE_READ_DATA 0x00000001u
#define CDC_FILE_WRITE_ATTRIBUTES 0x00000100u
#define CDC_FILE_WRITE_DATA 0x00000002u
#define CDC_FILE_ADD_FILE 0x00000002u
#define CDC_FILE_ADD_SUBDIRECTORY 0x00000004u
#define CDC_FILE_TRAVERSE 0x00000020u
#define CDC_SYNCHRONIZE_ACCESS 0x00100000u
#define CDC_FILE_SHARE_READ 0x00000001u
#define CDC_FILE_SHARE_WRITE 0x00000002u
#define CDC_FILE_SHARE_DELETE 0x00000004u
#define CDC_FILE_SHARE_ALL 0x00000007u
#define CDC_FILE_OPEN 1u
#define CDC_FILE_CREATE 2u
#define CDC_FILE_DIRECTORY_FILE 0x00000001u
#define CDC_FILE_NON_DIRECTORY_FILE 0x00000040u
#define CDC_FILE_OPEN_REPARSE_POINT 0x00200000u
#define CDC_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
#define CDC_OBJ_CASE_INSENSITIVE 0x00000040u
#define CDC_FILE_ATTRIBUTE_READONLY 0x00000001u
#define CDC_FILE_FLAG_BACKUP_SEMANTICS 0x02000000u

/* 与 Rust extern "system" 块逐项对应（ntdll，未经 windows_sys 封装）。 */
typedef struct {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} cdc_unicode_string_t;

typedef struct {
    ULONG Length;
    HANDLE RootDirectory;
    cdc_unicode_string_t *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} cdc_object_attributes_t;

typedef struct {
    union {
        LONG Status;
        PVOID Pointer;
    } u;
    ULONG_PTR Information;
} cdc_io_status_block_t;

extern "C" NTSTATUS NTAPI NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
    cdc_object_attributes_t *ObjectAttributes,
    cdc_io_status_block_t *IoStatusBlock, PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition,
    ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
extern "C" NTSTATUS NTAPI NtSetInformationFile(
    HANDLE FileHandle, cdc_io_status_block_t *IoStatusBlock,
    PVOID FileInformation, ULONG Length, ULONG FileInformationClass);
extern "C" ULONG NTAPI RtlNtStatusToDosError(NTSTATUS Status);

/* 内部 io 种类：只为 delete_file_checked 的 Skipped/Failed 分流服务。 */
typedef enum {
    IO_OK = 0,
    IO_NOT_FOUND,
    IO_DENIED,
    IO_INVALID,
    IO_EXISTS,
    IO_OTHER
} io_kind_t;

static io_kind_t io_kind_from_win32(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return IO_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
        return IO_DENIED;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
        return IO_INVALID;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return IO_EXISTS;
    default:
        return IO_OTHER;
    }
}

static int kind_to_cdc(io_kind_t k) {
    switch (k) {
    case IO_OK:
        return CDC_OK;
    case IO_NOT_FOUND:
        return CDC_ERR_NOT_FOUND;
    case IO_INVALID:
        return CDC_ERR_INVALID_ARG;
    case IO_EXISTS:
        return CDC_ERR_EXISTS;
    default:
        return CDC_ERR_IO;
    }
}

static io_kind_t nt_kind(NTSTATUS status, cdc_err_t *err, const char *what) {
    ULONG code = RtlNtStatusToDosError(status);
    io_kind_t k = io_kind_from_win32(code);
    if (err)
        cdc_err_set(err, kind_to_cdc(k), "%s failed: NTSTATUS 0x%08lX (Win32 %lu)",
                    what, (unsigned long)status, (unsigned long)code);
    return k;
}

static io_kind_t win32_kind(DWORD code, cdc_err_t *err, const char *what) {
    io_kind_t k = io_kind_from_win32(code);
    if (err)
        cdc_err_set(err, kind_to_cdc(k), "%s failed (Win32 %lu)", what,
                    (unsigned long)code);
    return k;
}

/* 取句柄元数据：是否目录 / 是否重解析点。 */
static io_kind_t handle_is_dir_reparse(HANDLE h, int *out_is_dir,
                                       int *out_is_reparse, cdc_err_t *err) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info))
        return win32_kind(GetLastError(), err, "GetFileInformationByHandle");
    if (out_is_dir)
        *out_is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (out_is_reparse)
        *out_is_reparse =
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return IO_OK;
}

/* win32_path_parts：纯字符串解析（与 Rust 版逐行一致）。 */
static io_kind_t win32_path_parts(const char *path, char **out_root,
                                  char ***out_parts, size_t *out_nparts,
                                  cdc_err_t *err) {
    char *raw = xstrdup(path);
    if (!raw) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return IO_OTHER;
    }
    slash_to_backslash(raw);
    size_t n = strlen(raw);
    if (n < 3 || !isalpha((unsigned char)raw[0]) || raw[1] != ':' ||
        raw[2] != '\\' || cdc_path_syntax_error(raw)) {
        free(raw);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "expected a normalized absolute drive path: %s", path);
        return IO_INVALID;
    }
    char *normalized = cdc_normalize(raw);
    free(raw);
    if (!normalized) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return IO_OTHER;
    }
    *out_root = xstrndup(normalized, 3);
    size_t nparts = 0;
    char **parts = split_parts(normalized + 3, &nparts);
    free(normalized);
    if (!*out_root || !parts) {
        free(*out_root);
        cdc_strvec_free(parts, nparts);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return IO_OTHER;
    }
    *out_parts = parts;
    *out_nparts = nparts;
    return IO_OK;
}

static io_kind_t open_drive_root_with_access(const char *root, DWORD access,
                                             HANDLE *out, cdc_err_t *err) {
    wchar_t *wide = cdc_utf8_to_wide(root);
    if (!wide) {
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 path: %s", root);
        return IO_INVALID;
    }
    HANDLE h = CreateFileW(wide, access, CDC_FILE_SHARE_ALL, NULL,
                           OPEN_EXISTING,
                           CDC_FILE_FLAG_BACKUP_SEMANTICS |
                               CDC_FILE_OPEN_REPARSE_POINT,
                           NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE)
        return win32_kind(GetLastError(), err, "CreateFileW(drive root)");
    int is_dir = 0, is_reparse = 0;
    io_kind_t k = handle_is_dir_reparse(h, &is_dir, &is_reparse, err);
    if (k != IO_OK) {
        CloseHandle(h);
        return k;
    }
    if (is_reparse || !is_dir) {
        CloseHandle(h);
        if (err)
            cdc_err_set(err, CDC_ERR_WIN32,
                        "drive root is not a plain directory: %s", root);
        return IO_DENIED;
    }
    *out = h;
    return IO_OK;
}

static io_kind_t open_relative(HANDLE parent, const char *name, DWORD access,
                               ULONG options, ULONG disposition, DWORD share,
                               HANDLE *out, cdc_err_t *err) {
    if (!name || name[0] == '\0' || strpbrk(name, "\\/:") != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "invalid relative name: %s",
                        name ? name : "(null)");
        return IO_INVALID;
    }
    wchar_t *wide = cdc_utf8_to_wide(name);
    if (!wide) {
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 name: %s", name);
        return IO_INVALID;
    }
    size_t wlen = wcslen(wide);
    if (wlen > (0xFFFFu - 2) / 2) {
        free(wide);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "path component too long: %s",
                        name);
        return IO_INVALID;
    }
    cdc_unicode_string_t object_name;
    object_name.Length = (USHORT)(wlen * 2);
    object_name.MaximumLength = (USHORT)(wlen * 2 + 2);
    object_name.Buffer = wide;
    cdc_object_attributes_t oa;
    oa.Length = sizeof(oa);
    oa.RootDirectory = parent;
    oa.ObjectName = &object_name;
    oa.Attributes = CDC_OBJ_CASE_INSENSITIVE;
    oa.SecurityDescriptor = NULL;
    oa.SecurityQualityOfService = NULL;
    cdc_io_status_block_t iosb;
    iosb.u.Status = 0;
    iosb.Information = 0;
    HANDLE handle = NULL;
    NTSTATUS status = NtCreateFile(
        &handle, access, &oa, &iosb, NULL, 0, share, disposition,
        options | CDC_FILE_OPEN_REPARSE_POINT |
            CDC_FILE_SYNCHRONOUS_IO_NONALERT,
        NULL, 0);
    free(wide);
    if (status < 0) return nt_kind(status, err, "NtCreateFile");
    *out = handle;
    return IO_OK;
}

static io_kind_t open_directory_chain_with_final_access(
    const char *path, DWORD final_access, cdc_handle_chain_t **out,
    cdc_err_t *err) {
    char *root = NULL;
    char **components = NULL;
    size_t ncomponents = 0;
    io_kind_t k = win32_path_parts(path, &root, &components, &ncomponents, err);
    if (k != IO_OK) return k;
    DWORD base_access = CDC_FILE_READ_ATTRIBUTES | CDC_FILE_TRAVERSE |
                        CDC_SYNCHRONIZE_ACCESS;
    DWORD root_access = base_access | (ncomponents == 0 ? final_access : 0);
    cdc_handle_chain_t *chain = chain_new();
    if (!chain) {
        free(root);
        cdc_strvec_free(components, ncomponents);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return IO_OTHER;
    }
    HANDLE root_handle = NULL;
    k = open_drive_root_with_access(root, root_access, &root_handle, err);
    free(root);
    if (k != IO_OK) {
        cdc_handle_chain_free(chain);
        cdc_strvec_free(components, ncomponents);
        return k;
    }
    if (chain_push(chain, (cdc_handle_t)root_handle, err) != CDC_OK) {
        CloseHandle(root_handle);
        cdc_handle_chain_free(chain);
        cdc_strvec_free(components, ncomponents);
        return IO_OTHER;
    }
    for (size_t i = 0; i < ncomponents; i++) {
        HANDLE parent = (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t,
                                          chain->vec.len - 1);
        DWORD access = base_access |
                       (i + 1 == ncomponents ? final_access : 0);
        HANDLE child = NULL;
        k = open_relative(parent, components[i], access,
                          CDC_FILE_DIRECTORY_FILE, CDC_FILE_OPEN,
                          CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE, &child,
                          err);
        if (k != IO_OK) {
            cdc_handle_chain_free(chain);
            cdc_strvec_free(components, ncomponents);
            return k;
        }
        int is_dir = 0, is_reparse = 0;
        k = handle_is_dir_reparse(child, &is_dir, &is_reparse, err);
        if (k != IO_OK || is_reparse || !is_dir) {
            CloseHandle(child);
            if (k == IO_OK) {
                if (err)
                    cdc_err_set(err, CDC_ERR_WIN32,
                                "reparse point or non-directory in parent "
                                "path: %s",
                                components[i]);
                k = IO_DENIED;
            }
            cdc_handle_chain_free(chain);
            cdc_strvec_free(components, ncomponents);
            return k;
        }
        if (chain_push(chain, (cdc_handle_t)child, err) != CDC_OK) {
            CloseHandle(child);
            cdc_handle_chain_free(chain);
            cdc_strvec_free(components, ncomponents);
            return IO_OTHER;
        }
    }
    cdc_strvec_free(components, ncomponents);
    *out = chain;
    return IO_OK;
}

static io_kind_t open_directory_chain(const char *path,
                                      cdc_handle_chain_t **out,
                                      cdc_err_t *err) {
    return open_directory_chain_with_final_access(path, 0, out, err);
}

/* parent 路径：normalize 后去掉最后一个段。 */
static char *parent_of(const char *path) {
    char *norm = cdc_normalize(path);
    if (!norm) return NULL;
    char *last = strrchr(norm, '\\');
    if (!last) {
        free(norm);
        return NULL;
    }
    if (last == norm + 2 && norm[1] == ':') {
        /* "C:\foo" → "C:\"（保留根反斜杠）。 */
        norm[3] = '\0';
        return norm;
    }
    *last = '\0';
    return norm;
}

static io_kind_t open_parent_chain_with_rights(
    const char *path, const char **roots, size_t nroots, DWORD parent_rights,
    cdc_handle_chain_t **out_chain, char **out_name, cdc_err_t *err) {
    if (!cdc_is_within(path, roots, nroots)) {
        if (err)
            cdc_err_set(err, CDC_ERR_WIN32,
                        "path is outside the allowed roots: %s", path);
        return IO_DENIED;
    }
    char *name = cdc_basename(path);
    if (!name || name[0] == '\0') {
        free(name);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "invalid file name: %s",
                        path);
        return IO_INVALID;
    }
    char *parent = parent_of(path);
    if (!parent) {
        free(name);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "path has no parent: %s",
                        path);
        return IO_INVALID;
    }
    io_kind_t k = open_directory_chain_with_final_access(parent, parent_rights,
                                                         out_chain, err);
    free(parent);
    if (k != IO_OK) {
        free(name);
        return k;
    }
    *out_name = name;
    return IO_OK;
}

static io_kind_t open_parent_chain(const char *path, const char **roots,
                                   size_t nroots, cdc_handle_chain_t **out_chain,
                                   char **out_name, cdc_err_t *err) {
    return open_parent_chain_with_rights(path, roots, nroots, 0, out_chain,
                                         out_name, err);
}

int cdc_mark_delete(cdc_handle_t file) {
    if (!file) return 0;
    uint8_t info = 1; /* FILE_DISPOSITION_INFO{ DeleteFile: TRUE }，1 字节 */
    return SetFileInformationByHandle((HANDLE)file,
                                      (FILE_INFO_BY_HANDLE_CLASS)CDC_FILE_DISPOSITION_INFO_CLASS, &info,
                                      1) != 0;
}

static int clear_readonly_and_delete(HANDLE file) {
    FILE_BASIC_INFO info;
    memset(&info, 0, sizeof(info));
    if (!GetFileInformationByHandleEx((HANDLE)file, FileBasicInfo, &info,
                                      sizeof(info)))
        return 0;
    if ((info.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0) return 0;
    info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    if (!SetFileInformationByHandle((HANDLE)file, FileBasicInfo, &info,
                                    sizeof(info)))
        return 0;
    return cdc_mark_delete((cdc_handle_t)file);
}

static io_kind_t open_plain_file(const char *path, const char **roots,
                                 size_t nroots, cdc_handle_chain_t **out_chain,
                                 HANDLE *out_file, cdc_err_t *err) {
    cdc_handle_chain_t *chain = NULL;
    char *name = NULL;
    io_kind_t k = open_parent_chain(path, roots, nroots, &chain, &name, err);
    if (k != IO_OK) return k;
    HANDLE parent =
        (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
    HANDLE file = NULL;
    k = open_relative(parent, name,
                      CDC_DELETE_ACCESS | CDC_FILE_READ_ATTRIBUTES |
                          CDC_FILE_WRITE_ATTRIBUTES | CDC_SYNCHRONIZE_ACCESS,
                      CDC_FILE_NON_DIRECTORY_FILE, CDC_FILE_OPEN,
                      CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE, &file, err);
    free(name);
    if (k != IO_OK) {
        cdc_handle_chain_free(chain);
        return k;
    }
    int is_dir = 0, is_reparse = 0;
    k = handle_is_dir_reparse(file, &is_dir, &is_reparse, err);
    if (k != IO_OK || is_reparse || is_dir) {
        CloseHandle(file);
        cdc_handle_chain_free(chain);
        if (k == IO_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_WIN32,
                            "target is a reparse point or directory: %s", path);
            return IO_DENIED;
        }
        return k;
    }
    *out_chain = chain;
    *out_file = file;
    return IO_OK;
}

static io_kind_t delete_plain_file(const char *path, const char **roots,
                                   size_t nroots, int *out_deleted,
                                   cdc_err_t *err) {
    cdc_handle_chain_t *chain = NULL;
    HANDLE file = NULL;
    io_kind_t k = open_plain_file(path, roots, nroots, &chain, &file, err);
    if (k != IO_OK) return k;
    int deleted = cdc_mark_delete((cdc_handle_t)file) ||
                  clear_readonly_and_delete(file);
    CloseHandle(file);
    cdc_handle_chain_free(chain);
    *out_deleted = deleted ? 1 : 0;
    return IO_OK;
}

cdc_delete_outcome_t cdc_delete_file_checked(const char *path,
                                             const char **roots, size_t nroots,
                                             int untrusted, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (cdc_execution_forbidden(path, untrusted))
        return CDC_DELETE_SKIPPED;
    int deleted = 0;
    cdc_err_t e2;
    io_kind_t k = delete_plain_file(path, roots, nroots, &deleted, &e2);
    if (k == IO_OK) {
        if (deleted) return CDC_DELETE_DELETED;
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "无法删除：%s", path);
        return CDC_DELETE_FAILED;
    }
    if (k == IO_NOT_FOUND) return CDC_DELETE_SKIPPED;
    if (k == IO_DENIED || k == IO_INVALID) return CDC_DELETE_SKIPPED;
    if (err)
        cdc_err_set(err, kind_to_cdc(k), "%s：%s", path, e2.message);
    return CDC_DELETE_FAILED;
}

int cdc_pin_directory_checked(const char *path, const char **roots,
                              size_t nroots, cdc_handle_chain_t **out_chain,
                              cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (cdc_path_syntax_error(path) || !cdc_is_within(path, roots, nroots)) {
        if (err)
            cdc_err_set(err, CDC_ERR_WIN32,
                        "directory is outside the allowed roots or has "
                        "invalid syntax: %s",
                        path);
        return CDC_ERR_WIN32;
    }
    io_kind_t k = open_directory_chain(path, out_chain, err);
    return kind_to_cdc(k);
}

/* 拼 parent_path + 段（处理根目录尾 '\\'）。 */
static char *join_child(const char *parent, const char *name) {
    size_t pn = strlen(parent);
    int trailing_bs = pn > 0 && parent[pn - 1] == '\\';
    size_t n = pn + (trailing_bs ? 0 : 1) + strlen(name) + 1;
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    snprintf(out, n, trailing_bs ? "%s%s" : "%s\\%s", parent, name);
    return out;
}

int cdc_create_directory_tree_checked(const char *path,
                                      cdc_handle_chain_t **out_chain,
                                      cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (cdc_path_syntax_error(path)) {
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "invalid directory path: %s",
                        path);
        return CDC_ERR_INVALID_ARG;
    }
    char *root = NULL;
    char **components = NULL;
    size_t ncomponents = 0;
    io_kind_t k = win32_path_parts(path, &root, &components, &ncomponents, err);
    if (k != IO_OK) return kind_to_cdc(k);
    if (ncomponents == 0) {
        free(root);
        cdc_strvec_free(components, ncomponents);
        if (err)
            cdc_err_set(err, CDC_ERR_EXISTS,
                        "a volume root cannot be used as a new migration "
                        "target");
        return CDC_ERR_EXISTS;
    }
    DWORD base_access = CDC_FILE_READ_ATTRIBUTES | CDC_FILE_TRAVERSE |
                        CDC_SYNCHRONIZE_ACCESS;
    cdc_handle_chain_t *chain = chain_new();
    if (!chain) {
        free(root);
        cdc_strvec_free(components, ncomponents);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    HANDLE root_handle = NULL;
    k = open_drive_root_with_access(root, base_access, &root_handle, err);
    if (k != IO_OK) {
        free(root);
        cdc_strvec_free(components, ncomponents);
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    if (chain_push(chain, (cdc_handle_t)root_handle, err) != CDC_OK) {
        CloseHandle(root_handle);
        free(root);
        cdc_strvec_free(components, ncomponents);
        cdc_handle_chain_free(chain);
        return CDC_ERR_NOMEM;
    }
    char *parent_path = root; /* 接管 root 所有权 */
    for (size_t i = 0; i < ncomponents; i++) {
        int final_component = (i + 1 == ncomponents);
        HANDLE parent =
            (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
        HANDLE child = NULL;
        if (final_component) {
            cdc_handle_chain_t *create_parent = NULL;
            k = open_directory_chain_with_final_access(
                parent_path, CDC_FILE_ADD_SUBDIRECTORY, &create_parent, err);
            if (k != IO_OK) goto fail;
            HANDLE cp = (HANDLE)CDC_VEC_AT(&create_parent->vec, cdc_handle_t,
                                          create_parent->vec.len - 1);
            k = open_relative(cp, components[i], base_access,
                              CDC_FILE_DIRECTORY_FILE, CDC_FILE_CREATE,
                              CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE,
                              &child, err);
            cdc_handle_chain_free(create_parent);
            if (k != IO_OK) goto fail;
        } else {
            k = open_relative(parent, components[i], base_access,
                              CDC_FILE_DIRECTORY_FILE, CDC_FILE_OPEN,
                              CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE,
                              &child, err);
            if (k == IO_NOT_FOUND) {
                cdc_handle_chain_t *create_parent = NULL;
                k = open_directory_chain_with_final_access(
                    parent_path, CDC_FILE_ADD_SUBDIRECTORY, &create_parent,
                    err);
                if (k != IO_OK) goto fail;
                HANDLE cp =
                    (HANDLE)CDC_VEC_AT(&create_parent->vec, cdc_handle_t,
                                       create_parent->vec.len - 1);
                k = open_relative(cp, components[i], base_access,
                                  CDC_FILE_DIRECTORY_FILE, CDC_FILE_CREATE,
                                  CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE,
                                  &child, err);
                cdc_handle_chain_free(create_parent);
                if (k != IO_OK) goto fail;
            } else if (k != IO_OK) {
                goto fail;
            }
        }
        int is_dir = 0, is_reparse = 0;
        k = handle_is_dir_reparse(child, &is_dir, &is_reparse, err);
        if (k != IO_OK || is_reparse || !is_dir) {
            CloseHandle(child);
            if (k == IO_OK) {
                if (err)
                    cdc_err_set(err, CDC_ERR_WIN32,
                                "directory component is a reparse point or "
                                "non-directory: %s",
                                components[i]);
                k = IO_DENIED;
            }
            goto fail;
        }
        if (chain_push(chain, (cdc_handle_t)child, err) != CDC_OK) {
            CloseHandle(child);
            k = IO_OTHER;
            if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
            goto fail;
        }
        char *next_parent = join_child(parent_path, components[i]);
        free(parent_path);
        parent_path = next_parent;
        if (!parent_path) {
            k = IO_OTHER;
            if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
            goto fail;
        }
    }
    free(parent_path);
    cdc_strvec_free(components, ncomponents);
    *out_chain = chain;
    return CDC_OK;
fail:
    free(parent_path);
    cdc_strvec_free(components, ncomponents);
    cdc_handle_chain_free(chain);
    return kind_to_cdc(k);
}

int cdc_create_directory_checked(const char *path, const char **roots,
                                 size_t nroots,
                                 cdc_handle_chain_t **out_chain,
                                 cdc_handle_t *out_dir, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    cdc_handle_chain_t *chain = NULL;
    char *name = NULL;
    io_kind_t k = open_parent_chain_with_rights(path, roots, nroots,
                                                CDC_FILE_ADD_SUBDIRECTORY,
                                                &chain, &name, err);
    if (k != IO_OK) return kind_to_cdc(k);
    HANDLE parent =
        (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
    HANDLE dir = NULL;
    k = open_relative(parent, name,
                      CDC_FILE_READ_ATTRIBUTES | CDC_FILE_TRAVERSE |
                          CDC_SYNCHRONIZE_ACCESS,
                      CDC_FILE_DIRECTORY_FILE, CDC_FILE_CREATE,
                      CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE, &dir, err);
    free(name);
    if (k != IO_OK) {
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    int is_dir = 0, is_reparse = 0;
    k = handle_is_dir_reparse(dir, &is_dir, &is_reparse, err);
    if (k != IO_OK || is_reparse || !is_dir) {
        CloseHandle(dir);
        cdc_handle_chain_free(chain);
        if (k == IO_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_WIN32,
                            "created directory is not an ordinary directory: "
                            "%s",
                            path);
            return CDC_ERR_WIN32;
        }
        return kind_to_cdc(k);
    }
    *out_chain = chain;
    *out_dir = (cdc_handle_t)dir;
    return CDC_OK;
}

int cdc_create_new_file_checked(const char *path, const char **roots,
                                size_t nroots, cdc_handle_chain_t **out_chain,
                                cdc_handle_t *out_file, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    cdc_handle_chain_t *chain = NULL;
    char *name = NULL;
    io_kind_t k = open_parent_chain_with_rights(path, roots, nroots,
                                                CDC_FILE_ADD_FILE, &chain,
                                                &name, err);
    if (k != IO_OK) return kind_to_cdc(k);
    HANDLE parent =
        (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
    HANDLE file = NULL;
    k = open_relative(parent, name,
                      CDC_FILE_WRITE_DATA | CDC_FILE_READ_ATTRIBUTES |
                          CDC_SYNCHRONIZE_ACCESS,
                      CDC_FILE_NON_DIRECTORY_FILE, CDC_FILE_CREATE,
                      CDC_FILE_SHARE_READ, &file, err);
    free(name);
    if (k != IO_OK) {
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    int is_dir = 0, is_reparse = 0;
    k = handle_is_dir_reparse(file, &is_dir, &is_reparse, err);
    if (k != IO_OK || is_reparse || is_dir) {
        CloseHandle(file);
        cdc_handle_chain_free(chain);
        if (k == IO_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_WIN32,
                            "created file is not an ordinary file: %s", path);
            return CDC_ERR_WIN32;
        }
        return kind_to_cdc(k);
    }
    *out_chain = chain;
    *out_file = (cdc_handle_t)file;
    return CDC_OK;
}

int cdc_open_read_file_checked(const char *path, const char **roots,
                               size_t nroots, cdc_handle_chain_t **out_chain,
                               cdc_handle_t *out_file, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    cdc_handle_chain_t *chain = NULL;
    char *name = NULL;
    io_kind_t k = open_parent_chain(path, roots, nroots, &chain, &name, err);
    if (k != IO_OK) return kind_to_cdc(k);
    HANDLE parent =
        (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
    HANDLE file = NULL;
    k = open_relative(parent, name,
                      CDC_FILE_READ_DATA | CDC_FILE_READ_ATTRIBUTES |
                          CDC_SYNCHRONIZE_ACCESS,
                      CDC_FILE_NON_DIRECTORY_FILE, CDC_FILE_OPEN,
                      CDC_FILE_SHARE_READ, &file, err);
    free(name);
    if (k != IO_OK) {
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    int is_dir = 0, is_reparse = 0;
    k = handle_is_dir_reparse(file, &is_dir, &is_reparse, err);
    if (k != IO_OK || is_reparse || is_dir) {
        CloseHandle(file);
        cdc_handle_chain_free(chain);
        if (k == IO_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_WIN32,
                            "opened file is not an ordinary file: %s", path);
            return CDC_ERR_WIN32;
        }
        return kind_to_cdc(k);
    }
    *out_chain = chain;
    *out_file = (cdc_handle_t)file;
    return CDC_OK;
}

int cdc_remove_directory_checked(const char *path, const char **roots,
                                 size_t nroots, int *out_deleted,
                                 cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out_deleted) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "out_deleted is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    *out_deleted = 0;
    cdc_handle_chain_t *chain = NULL;
    char *name = NULL;
    io_kind_t k = open_parent_chain(path, roots, nroots, &chain, &name, err);
    if (k != IO_OK) return kind_to_cdc(k);
    HANDLE parent =
        (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
    HANDLE dir = NULL;
    k = open_relative(parent, name,
                      CDC_DELETE_ACCESS | CDC_FILE_READ_ATTRIBUTES |
                          CDC_SYNCHRONIZE_ACCESS,
                      CDC_FILE_DIRECTORY_FILE, CDC_FILE_OPEN,
                      CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE, &dir, err);
    free(name);
    if (k != IO_OK) {
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    int is_dir = 0, is_reparse = 0;
    k = handle_is_dir_reparse(dir, &is_dir, &is_reparse, err);
    if (k != IO_OK) {
        CloseHandle(dir);
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    if (is_reparse || !is_dir) {
        /* Rust 这里返回 Ok(false)，不是错误。 */
        CloseHandle(dir);
        cdc_handle_chain_free(chain);
        return CDC_OK;
    }
    int deleted = cdc_mark_delete((cdc_handle_t)dir);
    CloseHandle(dir);
    cdc_handle_chain_free(chain);
    *out_deleted = deleted ? 1 : 0;
    return CDC_OK;
}

/* remove_tree_checked 的递归体：枚举用路径式（已 pin 住父链），
 * 删除一律重新走句柄。 */
static io_kind_t remove_contents(const char *dir, const char **roots,
                                 size_t nroots, uint64_t *removed,
                                 cdc_err_t *err) {
    cdc_handle_chain_t *chain = NULL;
    io_kind_t k = open_directory_chain(dir, &chain, err);
    if (k != IO_OK) return k;
    HANDLE last =
        (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
    int is_dir = 0, is_reparse = 0;
    k = handle_is_dir_reparse(last, &is_dir, &is_reparse, err);
    if (k != IO_OK || is_reparse || !is_dir) {
        cdc_handle_chain_free(chain);
        if (k == IO_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_WIN32,
                            "tree root is not a plain directory: %s", dir);
            return IO_DENIED;
        }
        return k;
    }
    /* 链在整个路径式枚举期间保持打开（与 Rust 注释一致：防止路径段在
     * 枚举时被改名）；删除操作各自重新 pin 父链。 */
    size_t dn = strlen(dir);
    int trailing_bs = dn > 0 && dir[dn - 1] == '\\';
    size_t pn = dn + (trailing_bs ? 1 : 2) + 1;
    char *pattern = (char *)malloc(pn);
    if (!pattern) {
        cdc_handle_chain_free(chain);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return IO_OTHER;
    }
    snprintf(pattern, pn, trailing_bs ? "%s*" : "%s\\*", dir);
    wchar_t *wpattern = cdc_utf8_to_wide(pattern);
    free(pattern);
    if (!wpattern) {
        cdc_handle_chain_free(chain);
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 path: %s", dir);
        return IO_INVALID;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    free(wpattern);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        cdc_handle_chain_free(chain);
        if (code == ERROR_FILE_NOT_FOUND) return IO_OK; /* 空目录 */
        return win32_kind(code, err, "FindFirstFileW");
    }
    io_kind_t rc = IO_OK;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;
        char *name = cdc_wide_to_utf8(fd.cFileName);
        if (!name) {
            rc = IO_OTHER;
            if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid file name");
            break;
        }
        char *child = join_child(dir, name);
        free(name);
        if (!child) {
            rc = IO_OTHER;
            if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
            break;
        }
        DWORD attrs = fd.dwFileAttributes;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            /* 链接子项故意保留，不跟随、不删除。 */
            free(child);
            continue;
        }
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            uint64_t sub = 0;
            rc = remove_contents(child, roots, nroots, &sub, err);
            if (rc != IO_OK) {
                free(child);
                break;
            }
            *removed += sub;
            int deleted = 0;
            cdc_err_t e2;
            int crc = cdc_remove_directory_checked(child, roots, nroots,
                                                   &deleted, &e2);
            if (crc != CDC_OK) {
                if (err) *err = e2;
                rc = IO_OTHER;
                free(child);
                break;
            }
            if (deleted) (*removed)++;
        } else {
            int deleted = 0;
            cdc_err_t e2;
            io_kind_t dk = delete_plain_file(child, roots, nroots, &deleted,
                                             &e2);
            if (dk != IO_OK) {
                if (err) *err = e2;
                rc = dk;
                free(child);
                break;
            }
            if (deleted) (*removed)++;
        }
        free(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    cdc_handle_chain_free(chain);
    return rc;
}

int cdc_remove_tree_checked(const char *root, const char **allowed_roots,
                            size_t nroots, uint64_t *out_removed,
                            cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out_removed) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "out_removed is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    *out_removed = 0;
    if (!cdc_is_within(root, allowed_roots, nroots)) {
        if (err)
            cdc_err_set(err, CDC_ERR_WIN32,
                        "tree root is outside the allowed roots: %s", root);
        return CDC_ERR_WIN32;
    }
    uint64_t removed = 0;
    io_kind_t k = remove_contents(root, allowed_roots, nroots, &removed, err);
    if (k != IO_OK) return kind_to_cdc(k);
    int deleted = 0;
    int rc = cdc_remove_directory_checked(root, allowed_roots, nroots,
                                          &deleted, err);
    if (rc != CDC_OK) return rc;
    if (!deleted) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO,
                        "tree could not be fully removed: %s", root);
        return CDC_ERR_IO;
    }
    *out_removed = removed + 1;
    return CDC_OK;
}

int cdc_rename_directory_sibling_checked(const char *from, const char *to,
                                         const char **allowed_roots,
                                         size_t nroots, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (cdc_path_syntax_error(from) || cdc_path_syntax_error(to) ||
        (!cdc_is_within(from, allowed_roots, nroots) &&
         !cdc_is_within(to, allowed_roots, nroots))) {
        if (err)
            cdc_err_set(err, CDC_ERR_WIN32,
                        "rename endpoints are not permitted siblings: %s -> "
                        "%s",
                        from, to);
        return CDC_ERR_WIN32;
    }
    char *from_parent = parent_of(from);
    char *to_parent = parent_of(to);
    if (!from_parent || !to_parent) {
        free(from_parent);
        free(to_parent);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "rename endpoints have no parent: %s -> %s", from, to);
        return CDC_ERR_INVALID_ARG;
    }
    char *from_parent_n = cdc_normalize(from_parent);
    char *to_parent_n = cdc_normalize(to_parent);
    free(from_parent);
    free(to_parent);
    if (!from_parent_n || !to_parent_n ||
        strcmp(from_parent_n, to_parent_n) != 0) {
        /* Rust 这里是大小写敏感的 normalize 比较（不 lower）。 */
        free(from_parent_n);
        free(to_parent_n);
        if (err)
            cdc_err_set(err, CDC_ERR_WIN32,
                        "rename endpoints are not permitted siblings: %s -> "
                        "%s",
                        from, to);
        return CDC_ERR_WIN32;
    }
    char *from_name = cdc_basename(from);
    char *to_name = cdc_basename(to);
    if (!from_name || from_name[0] == '\0' || !to_name ||
        to_name[0] == '\0') {
        free(from_parent_n);
        free(to_parent_n);
        free(from_name);
        free(to_name);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "invalid rename name");
        return CDC_ERR_INVALID_ARG;
    }
    cdc_handle_chain_t *chain = NULL;
    io_kind_t k = open_directory_chain_with_final_access(
        from_parent_n, CDC_FILE_ADD_SUBDIRECTORY, &chain, err);
    free(from_parent_n);
    free(to_parent_n);
    if (k != IO_OK) {
        free(from_name);
        free(to_name);
        return kind_to_cdc(k);
    }
    HANDLE parent_handle =
        (HANDLE)CDC_VEC_AT(&chain->vec, cdc_handle_t, chain->vec.len - 1);
    HANDLE source = NULL;
    k = open_relative(parent_handle, from_name,
                      CDC_DELETE_ACCESS | CDC_FILE_READ_ATTRIBUTES |
                          CDC_SYNCHRONIZE_ACCESS,
                      CDC_FILE_DIRECTORY_FILE, CDC_FILE_OPEN,
                      CDC_FILE_SHARE_READ | CDC_FILE_SHARE_WRITE, &source,
                      err);
    if (k != IO_OK) {
        free(from_name);
        free(to_name);
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    int is_dir = 0, is_reparse = 0;
    k = handle_is_dir_reparse(source, &is_dir, &is_reparse, err);
    if (k != IO_OK || is_reparse || !is_dir) {
        CloseHandle(source);
        free(from_name);
        free(to_name);
        cdc_handle_chain_free(chain);
        if (k == IO_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_WIN32,
                            "rename source is not a plain directory: %s", from);
            return CDC_ERR_WIN32;
        }
        return kind_to_cdc(k);
    }
    HANDLE probe = NULL;
    k = open_relative(parent_handle, to_name,
                      CDC_FILE_READ_ATTRIBUTES | CDC_SYNCHRONIZE_ACCESS, 0,
                      CDC_FILE_OPEN, CDC_FILE_SHARE_ALL, &probe, err);
    if (k == IO_OK) {
        CloseHandle(probe);
        CloseHandle(source);
        free(from_name);
        free(to_name);
        cdc_handle_chain_free(chain);
        if (err)
            cdc_err_set(err, CDC_ERR_EXISTS, "rename target exists: %s", to);
        return CDC_ERR_EXISTS;
    }
    if (k != IO_NOT_FOUND) {
        CloseHandle(source);
        free(from_name);
        free(to_name);
        cdc_handle_chain_free(chain);
        return kind_to_cdc(k);
    }
    /* FILE_RENAME_INFORMATION：变长 WCHAR 尾，偏移与 Rust 手工布局一致。 */
    typedef struct {
        uint8_t replace_if_exists;
        void *root_directory;
        uint32_t file_name_length;
    } rename_prefix_t;
    wchar_t *wide = cdc_utf8_to_wide(to_name);
    free(from_name);
    free(to_name);
    if (!wide) {
        CloseHandle(source);
        cdc_handle_chain_free(chain);
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 name: %s", to);
        return CDC_ERR_UTF8;
    }
    size_t name_offset =
        offsetof(rename_prefix_t, file_name_length) + sizeof(uint32_t);
    size_t total_len = name_offset + wcslen(wide) * sizeof(wchar_t);
    size_t buf_len =
        total_len > sizeof(rename_prefix_t) ? total_len : sizeof(rename_prefix_t);
    uint8_t *buffer = (uint8_t *)calloc(1, buf_len);
    if (!buffer) {
        free(wide);
        CloseHandle(source);
        cdc_handle_chain_free(chain);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    {
        rename_prefix_t prefix;
        prefix.replace_if_exists = 0;
        prefix.root_directory = parent_handle;
        prefix.file_name_length = (uint32_t)(wcslen(wide) * 2);
        memcpy(buffer, &prefix, sizeof(prefix));
        memcpy(buffer + name_offset, wide,
               wcslen(wide) * sizeof(wchar_t));
    }
    free(wide);
    cdc_io_status_block_t iosb;
    iosb.u.Status = 0;
    iosb.Information = 0;
    NTSTATUS status = NtSetInformationFile(source, &iosb, buffer,
                                           (ULONG)total_len,
                                           CDC_FILE_RENAME_INFO_CLASS);
    free(buffer);
    CloseHandle(source);
    cdc_handle_chain_free(chain);
    if (status < 0) {
        io_kind_t nk = nt_kind(status, err, "NtSetInformationFile(rename)");
        return kind_to_cdc(nk);
    }
    return CDC_OK;
}

#else /* ! _WIN32 — 句柄删除在 Linux 下不可用，返回错误，不静默成功 */

int cdc_mark_delete(cdc_handle_t file) {
    (void)file;
    return 0;
}

cdc_delete_outcome_t cdc_delete_file_checked(const char *path,
                                             const char **roots, size_t nroots,
                                             int untrusted, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (cdc_execution_forbidden(path, untrusted))
        return CDC_DELETE_SKIPPED;
    (void)roots;
    (void)nroots;
    if (err)
        cdc_err_set(err, CDC_ERR_IO,
                    "handle-relative delete is Windows-only: %s", path);
    return CDC_DELETE_FAILED;
}

int cdc_pin_directory_checked(const char *path, const char **roots,
                              size_t nroots, cdc_handle_chain_t **out_chain,
                              cdc_err_t *err) {
    (void)path;
    (void)roots;
    (void)nroots;
    (void)out_chain;
    if (err) cdc_err_init(err);
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

int cdc_create_directory_tree_checked(const char *path,
                                      cdc_handle_chain_t **out_chain,
                                      cdc_err_t *err) {
    (void)path;
    (void)out_chain;
    if (err) cdc_err_init(err);
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

int cdc_create_directory_checked(const char *path, const char **roots,
                                 size_t nroots,
                                 cdc_handle_chain_t **out_chain,
                                 cdc_handle_t *out_dir, cdc_err_t *err) {
    (void)path;
    (void)roots;
    (void)nroots;
    (void)out_chain;
    (void)out_dir;
    if (err) cdc_err_init(err);
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

int cdc_create_new_file_checked(const char *path, const char **roots,
                                size_t nroots, cdc_handle_chain_t **out_chain,
                                cdc_handle_t *out_file, cdc_err_t *err) {
    (void)path;
    (void)roots;
    (void)nroots;
    (void)out_chain;
    (void)out_file;
    if (err) cdc_err_init(err);
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

int cdc_open_read_file_checked(const char *path, const char **roots,
                               size_t nroots, cdc_handle_chain_t **out_chain,
                               cdc_handle_t *out_file, cdc_err_t *err) {
    (void)path;
    (void)roots;
    (void)nroots;
    (void)out_chain;
    (void)out_file;
    if (err) cdc_err_init(err);
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

int cdc_remove_directory_checked(const char *path, const char **roots,
                                 size_t nroots, int *out_deleted,
                                 cdc_err_t *err) {
    (void)path;
    (void)roots;
    (void)nroots;
    if (err) cdc_err_init(err);
    if (out_deleted) *out_deleted = 0;
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

int cdc_remove_tree_checked(const char *root, const char **allowed_roots,
                            size_t nroots, uint64_t *out_removed,
                            cdc_err_t *err) {
    (void)root;
    (void)allowed_roots;
    (void)nroots;
    if (err) cdc_err_init(err);
    if (out_removed) *out_removed = 0;
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

int cdc_rename_directory_sibling_checked(const char *from, const char *to,
                                         const char **allowed_roots,
                                         size_t nroots, cdc_err_t *err) {
    (void)from;
    (void)to;
    (void)allowed_roots;
    (void)nroots;
    if (err) cdc_err_init(err);
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "handle chains are Windows-only");
    return CDC_ERR_IO;
}

#endif /* _WIN32 */

/* ---------------- remove_empty_dirs（两平台共用遍历，删除走句柄） ---------------- */

typedef struct {
    cdc_vec_t dirs; /* char* */
} collect_dirs_ctx_t;

static int collect_dirs_cb(void *ctx, const char *path, int is_dir,
                           int is_symlink, uint64_t size, uint64_t mtime,
                           uint32_t attrs) {
    (void)size;
    (void)mtime;
    (void)attrs;
    if (!is_dir || is_symlink) return 0;
    collect_dirs_ctx_t *c = (collect_dirs_ctx_t *)ctx;
    char *dup = xstrdup(path);
    if (!dup) return CDC_ERR_NOMEM;
    if (cdc_vec_push(&c->dirs, &dup) != CDC_OK) {
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

int cdc_remove_empty_dirs(const char *root) {
    collect_dirs_ctx_t ctx;
    if (cdc_vec_init(&ctx.dirs, sizeof(char *)) != CDC_OK) return 0;
    cdc_err_t err;
    if (cdc_walk_dir(root, 1, collect_dirs_cb, &ctx, &err) != CDC_OK) {
        for (size_t i = 0; i < ctx.dirs.len; i++)
            free(((char **)ctx.dirs.data)[i]);
        cdc_vec_free(&ctx.dirs);
        return 0;
    }
    if (ctx.dirs.len > 1)
        qsort(ctx.dirs.data, ctx.dirs.len, sizeof(char *), cmp_strlen_desc);
    char *root_norm = cdc_normalize(root);
    int removed = 0;
    const char *roots_arr[1];
    roots_arr[0] = root;
    for (size_t i = 0; i < ctx.dirs.len; i++) {
        char *path = ((char **)ctx.dirs.data)[i];
        char *pn = cdc_normalize(path);
        if (pn && root_norm && strcmp(pn, root_norm) == 0) {
            free(pn);
            continue;
        }
        free(pn);
        if (cdc_is_never_clean(path)) continue;
        int deleted = 0;
        if (cdc_remove_directory_checked(path, roots_arr, 1, &deleted,
                                         NULL) == CDC_OK &&
            deleted)
            removed++;
    }
    free(root_norm);
    for (size_t i = 0; i < ctx.dirs.len; i++)
        free(((char **)ctx.dirs.data)[i]);
    cdc_vec_free(&ctx.dirs);
    return removed;
}


