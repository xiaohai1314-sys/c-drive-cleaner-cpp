/* im.c — 移植自 Rust 版 desktop/src/im.rs。
 * Windows 专属的路径探测（注册表、ini、SHGetFolderPathW）在 Linux 下
 * 返回空/回退值；classify / list_media / thumbnail / preview 的纯逻辑
 * 两平台一致。 */
#include "im.h"

#include <stdlib.h>
#include <string.h>

#include "logs.h"
#include "cdc_process.h"
#include "recycle.h"
#include "safety.h"
#include "system.h"
#include "timeutil.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static cdc_hooks_t g_hooks = { 0 };

void cdc_im_set_hooks(const cdc_hooks_t *hooks) {
    if (hooks) {
        g_hooks = *hooks;
    } else {
        memset(&g_hooks, 0, sizeof(g_hooks));
    }
}

static int cancelled(void) {
    return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, "im_clean");
}

/* ---------------- 扩展名表（与 Rust 版逐字一致） ---------------- */

static const char *VIDEO_EXT[] = {
    ".mp4", ".avi", ".mov", ".mkv", ".wmv", ".flv", ".m4v", ".3gp",
    ".mpeg", ".mpg", ".ts", ".webm", ".rmvb", ".vob", ".m2ts",
};
static const char *IMAGE_EXT[] = {
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".heic", ".tif",
    ".tiff", ".ico", ".raw", ".psd", ".svg", ".jfif",
};
static const char *AUDIO_EXT[] = { ".silk", ".amr", ".mp3", ".m4a",
                                   ".wav", ".ogg", ".aac", ".aud" };
static const char *DOC_EXT[] = {
    ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".txt",
    ".csv", ".rtf", ".ofd", ".md", ".wps", ".et", ".dps", ".dwg",
    ".dxf", ".xmind",
};
static const char *ARCHIVE_EXT[] = { ".zip", ".rar", ".7z", ".tar", ".gz",
                                     ".bz2", ".xz", ".iso", ".cab" };
static const char *FILE_EXT[] = { ".psd", ".ai" };
static const char *SKIP_EXT[] = {
    ".db", ".db-wal", ".db-shm", ".sqlite", ".sqlite3", ".ini", ".key",
    ".xml", ".json", ".pfx", ".crt", ".exe", ".dll", ".cfg",
};
static const char *SKIP_DIR_PARTS[] = {
    "db_storage", "config", "account", "apputil",
    "login", "db", "database", "databases",
};

typedef struct {
    const char *kind;
    const char **names;
    size_t nnames;
} media_dir_t;

static const char *SNS_NAMES[] = { "sns", "moments", "timeline" };
static const char *VIDEO_NAMES[] = { "video", "videos" };
static const char *IMAGE_NAMES[] = { "image", "images", "img", "attach",
                                     "msgattach", "custom_emotion", "emotion" };
static const char *AUDIO_NAMES[] = { "voice", "audio", "ptt", "sound" };
static const char *FILE_NAMES[] = { "file", "files", "filerecv", "file_recv",
                                    "fav", "favorite" };
static const char *LOG_NAMES[] = { "log", "logs", "xlog", "crash", "crashinfo" };
static const char *CACHE_NAMES[] = { "cache", "temp", "emoji", "emoticon",
                                     "cdncomm" };
static const media_dir_t MEDIA_DIRS[] = {
    { "sns", SNS_NAMES, 3 },   { "video", VIDEO_NAMES, 2 },
    { "image", IMAGE_NAMES, 7 }, { "audio", AUDIO_NAMES, 4 },
    { "file", FILE_NAMES, 6 },   { "log", LOG_NAMES, 5 },
    { "cache", CACHE_NAMES, 5 },
};

#define N_VIDEO (sizeof(VIDEO_EXT) / sizeof(VIDEO_EXT[0]))
#define N_IMAGE (sizeof(IMAGE_EXT) / sizeof(IMAGE_EXT[0]))
#define N_AUDIO (sizeof(AUDIO_EXT) / sizeof(AUDIO_EXT[0]))
#define N_DOC (sizeof(DOC_EXT) / sizeof(DOC_EXT[0]))
#define N_ARCHIVE (sizeof(ARCHIVE_EXT) / sizeof(ARCHIVE_EXT[0]))
#define N_FILE (sizeof(FILE_EXT) / sizeof(FILE_EXT[0]))
#define N_SKIP (sizeof(SKIP_EXT) / sizeof(SKIP_EXT[0]))
#define N_SKIPDIR (sizeof(SKIP_DIR_PARTS) / sizeof(SKIP_DIR_PARTS[0]))
#define N_MEDIADIRS (sizeof(MEDIA_DIRS) / sizeof(MEDIA_DIRS[0]))

static int ext_in(const char *ext, const char **table, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ext, table[i]) == 0) return 1;
    }
    return 0;
}

/* ASCII 小写化（Rust to_lowercase 是 Unicode 全量；路径里的非 ASCII
 * 字母（如 Ä/ä）不会折叠——中文路径不受影响，已知微小偏差）。 */
static void ascii_lower_inplace(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + ('a' - 'A'));
    }
}

static char *ascii_lower_dup(const char *s) {
    char *out = strdup(s ? s : "");
    if (out) ascii_lower_inplace(out);
    return out;
}

static int is_dir_utf8(const char *path) {
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES &&
           (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static char *path_join_win(const char *a, const char *b) {
    cdc_buf_t buf;
    cdc_buf_init(&buf);
    cdc_buf_append_str(&buf, a);
    size_t n = strlen(a);
    while (b[0] == '\\' || b[0] == '/') b++;
    if (n > 0 && a[n - 1] != '\\' && a[n - 1] != '/')
        cdc_buf_append_str(&buf, "\\");
    cdc_buf_append_str(&buf, b);
    return cdc_buf_take_cstr(&buf);
}

/* ---------------- 根目录探测 ---------------- */

static char *documents_dir(void) {
#ifdef _WIN32
    wchar_t buffer[260];
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, buffer);
    if (hr >= 0) {
        size_t len = 0;
        while (len < 260 && buffer[len] != 0) len++;
        if (len > 0) {
            buffer[len] = 0;
            char *path = cdc_wide_to_utf8(buffer);
            if (path && is_dir_utf8(path)) return path;
            free(path);
        }
    }
#endif
    char *profile = cdc_getenv_utf8("USERPROFILE");
    cdc_buf_t b;
    cdc_buf_init(&b);
    cdc_buf_append_str(&b, profile ? profile : "");
    free(profile);
    cdc_buf_append_str(&b, "\\Documents");
    return cdc_buf_take_cstr(&b);
}

static char *trim_dup(const char *s, size_t n) {
    while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' ||
                     s[n - 1] == '\r'))
        n--;
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* reg.exe query HKCU\Software\Tencent\WeChat /v FileSavePath 的解析。
 * 输出经 CP_ACP 转 UTF-8（比 Rust 的 from_utf8_lossy 更准：中文路径
 * 不会变 U+FFFD）。 */
static char *wechat_registry_path_value(void) {
    char *reg = cdc_system32_path("reg.exe");
    char *so = NULL, *se = NULL;
    long code = -1;
    cdc_err_t e;
    int rc = cdc_run_command(reg, "query HKCU\\Software\\Tencent\\WeChat /v FileSavePath",
                             1, &so, &se, &code, &e);
    free(reg);
    free(se);
    if (rc != CDC_OK || code != 0 || !so) {
        free(so);
        return NULL;
    }
    char *found = NULL;
    char *save = NULL;
    for (char *line = strtok_r(so, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        /* 去行尾 \r。 */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
        if (!strstr(line, "FileSavePath")) continue;
        /* split_whitespace：parts[2..].join(" ")。 */
        cdc_vec_t parts;
        if (cdc_vec_init(&parts, sizeof(char *)) != CDC_OK) break;
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            char *tok = trim_dup(start, (size_t)(p - start));
            if (tok && cdc_vec_push(&parts, &tok) != CDC_OK) free(tok);
        }
        if (parts.len >= 3) {
            cdc_buf_t b;
            cdc_buf_init(&b);
            for (size_t i = 2; i < parts.len; i++) {
                if (i > 2) cdc_buf_append_str(&b, " ");
                cdc_buf_append_str(&b, ((char **)parts.data)[i]);
            }
            char *val = cdc_buf_take_cstr(&b);
            if (val && strcmp(val, "MyDocument:") != 0) found = val;
            else free(val);
        }
        cdc_strvec_free((char **)parts.data, parts.len);
        parts.data = NULL;
        parts.len = parts.cap = 0;
        cdc_vec_free(&parts);
        if (found) break;
    }
    free(so);
    return found;
}

static char *resolve_wechat_path(char *value) {
    if (!value) return NULL;
    if (is_dir_utf8(value)) return value;
    char *sub = path_join_win(value, "WeChat Files");
    free(value);
    if (is_dir_utf8(sub)) return sub;
    free(sub);
    return NULL;
}

static char *wechat_registry_path(void) {
    return resolve_wechat_path(wechat_registry_path_value());
}

static int utf8_valid_simple(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t extra = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return 0;
        }
        if (i + extra >= n) return 0;
        for (size_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return 0;
        i += extra + 1;
    }
    return 1;
}

static char *wechat_ini_path_value(void) {
    char *roaming = cdc_getenv_utf8("APPDATA");
    char *dir = path_join_win(roaming ? roaming : "",
                              "Tencent\\WeChat\\All Users\\config");
    free(roaming);
    char *ini = path_join_win(dir, "3ebffe94.ini");
    free(dir);
    FILE *f = cdc_fopen_utf8(ini, "rb");
    free(ini);
    if (!f) return NULL;
    cdc_buf_t content;
    cdc_buf_init(&content);
    uint8_t chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
        cdc_buf_append(&content, chunk, got);
    fclose(f);
    /* Rust read_to_string 要求合法 UTF-8，否则整个文件读不到。 */
    if (!utf8_valid_simple(content.data, content.len)) {
        cdc_buf_free(&content);
        return NULL;
    }
    char *text = cdc_buf_take_cstr(&content);
    if (!text) return NULL;
    char *found = NULL;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        char *key = trim_dup(line, (size_t)(eq - line));
        char *val = trim_dup(eq + 1, strlen(eq + 1));
        if (key && val) {
            char *kl = ascii_lower_dup(key);
            if (kl && strcmp(kl, "filesavepath") == 0 &&
                strcmp(val, "MyDocument:") != 0) {
                found = strdup(val);
            }
            free(kl);
        }
        free(key);
        free(val);
        if (found) break;
    }
    free(text);
    return found;
}

static char *wechat_ini_path(void) {
    return resolve_wechat_path(wechat_ini_path_value());
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void sort_dedup_strings(char ***arr, size_t *n) {
    if (*n > 1) qsort(*arr, *n, sizeof(char *), cmp_str);
    size_t uniq = 0;
    for (size_t i = 0; i < *n; i++) {
        if (uniq > 0 && strcmp((*arr)[uniq - 1], (*arr)[i]) == 0) {
            free((*arr)[i]);
            continue;
        }
        (*arr)[uniq++] = (*arr)[i];
    }
    *n = uniq;
}

static int default_root_candidates(const char *kind, char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    char *roaming = cdc_getenv_utf8("APPDATA");
    char *docs = documents_dir();
    cdc_vec_t cands;
    if (cdc_vec_init(&cands, sizeof(char *)) != CDC_OK) {
        free(roaming);
        free(docs);
        return CDC_ERR_NOMEM;
    }
    const char *rel[4];
    size_t nrel = 0;
    if (strcmp(kind, "wechat") == 0) {
        rel[0] = "Tencent\\WeChat";
        rel[1] = "Tencent\\xwechat";
        nrel = 2;
        char *a = path_join_win(docs, "WeChat Files");
        char *b = path_join_win(docs, "xwechat_files");
        if (a && cdc_vec_push(&cands, &a) != CDC_OK) free(a);
        if (b && cdc_vec_push(&cands, &b) != CDC_OK) free(b);
    } else {
        rel[0] = "Tencent\\QQ";
        rel[1] = "Tencent\\QQNT";
        nrel = 2;
        char *a = path_join_win(docs, "Tencent Files");
        if (a && cdc_vec_push(&cands, &a) != CDC_OK) free(a);
    }
    for (size_t i = 0; i < nrel; i++) {
        char *p = path_join_win(roaming ? roaming : "", rel[i]);
        if (p && cdc_vec_push(&cands, &p) != CDC_OK) free(p);
    }
    free(roaming);
    free(docs);
    *out = (char **)cands.data;
    *out_n = cands.len;
    cands.data = NULL;
    cands.len = cands.cap = 0;
    cdc_vec_free(&cands);
    return CDC_OK;
}

static void append_wechat_migration_candidate(cdc_vec_t *cands,
                                              const char *value) {
    char *a = strdup(value);
    if (a && cdc_vec_push(cands, &a) != CDC_OK) free(a);
    char *b = path_join_win(value, "WeChat Files");
    if (b && cdc_vec_push(cands, &b) != CDC_OK) free(b);
}

int cdc_im_migration_roots(const char *kind, char ***out, size_t *out_n,
                           cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_n = 0;
    if (strcmp(kind, "wechat") != 0 && strcmp(kind, "qq") != 0) return CDC_OK;
    char **cands = NULL;
    size_t ncands = 0;
    if (default_root_candidates(kind, &cands, &ncands) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    cdc_vec_t vec;
    if (cdc_vec_init(&vec, sizeof(char *)) != CDC_OK) {
        cdc_strvec_free(cands, ncands);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < ncands; i++) {
        if (cdc_vec_push(&vec, &cands[i]) != CDC_OK) free(cands[i]);
    }
    free(cands);
    if (strcmp(kind, "wechat") == 0) {
        char *rv = wechat_registry_path_value();
        if (rv) {
            append_wechat_migration_candidate(&vec, rv);
            free(rv);
        }
        char *iv = wechat_ini_path_value();
        if (iv) {
            append_wechat_migration_candidate(&vec, iv);
            free(iv);
        }
    }
    char **arr = (char **)vec.data;
    size_t n = vec.len;
    vec.data = NULL;
    vec.len = vec.cap = 0;
    cdc_vec_free(&vec);
    sort_dedup_strings(&arr, &n);
    *out = arr;
    *out_n = n;
    return CDC_OK;
}

int cdc_im_roots(const char *kind, char ***out, size_t *out_n, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_n = 0;
    if (strcmp(kind, "wechat") != 0 && strcmp(kind, "qq") != 0) return CDC_OK;
    char **cands = NULL;
    size_t ncands = 0;
    if (default_root_candidates(kind, &cands, &ncands) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    cdc_vec_t vec;
    if (cdc_vec_init(&vec, sizeof(char *)) != CDC_OK) {
        cdc_strvec_free(cands, ncands);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < ncands; i++) {
        if (cdc_vec_push(&vec, &cands[i]) != CDC_OK) free(cands[i]);
    }
    free(cands);
    if (strcmp(kind, "wechat") == 0) {
        char *rp = wechat_registry_path();
        if (rp) {
            if (cdc_vec_push(&vec, &rp) != CDC_OK) free(rp);
        }
        char *ip = wechat_ini_path();
        if (ip) {
            if (cdc_vec_push(&vec, &ip) != CDC_OK) free(ip);
        }
    }
    char **arr = (char **)vec.data;
    size_t n = vec.len;
    vec.data = NULL;
    vec.len = vec.cap = 0;
    cdc_vec_free(&vec);
    sort_dedup_strings(&arr, &n);
    /* 只保留存在的目录。 */
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_dir_utf8(arr[i])) {
            arr[kept++] = arr[i];
        } else {
            free(arr[i]);
        }
    }
    *out = arr;
    *out_n = kept;
    return CDC_OK;
}

/* ---------------- 分类 ---------------- */

int cdc_im_is_program_root(const char *root) {
    if (!root) return 0;
    char *r = strdup(root);
    if (!r) return 0;
    size_t n = strlen(r);
    while (n > 0 && (r[n - 1] == '\\' || r[n - 1] == '/')) r[--n] = '\0';
    ascii_lower_inplace(r);
    if (r[0] == '\0') {
        free(r);
        return 0;
    }
    int hit = 0;
    const char *keys[2] = { "APPDATA", "LOCALAPPDATA" };
    for (int i = 0; i < 2 && !hit; i++) {
        char *base = cdc_getenv_utf8(keys[i]);
        if (!base) continue;
        size_t bn = strlen(base);
        while (bn > 0 && (base[bn - 1] == '\\' || base[bn - 1] == '/'))
            base[--bn] = '\0';
        ascii_lower_inplace(base);
        if (base[0] != '\0') {
            if (strcmp(r, base) == 0) {
                hit = 1;
            } else {
                size_t bl = strlen(base);
                if (strncmp(r, base, bl) == 0 && r[bl] == '\\') hit = 1;
            }
        }
        free(base);
    }
    free(r);
    return hit;
}

/* parts_of：'/' → '\'，小写，分段，去空。 */
static int parts_of(const char *path, char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    char *lower = ascii_lower_dup(path);
    if (!lower) return CDC_ERR_NOMEM;
    for (char *p = lower; *p; p++)
        if (*p == '/') *p = '\\';
    cdc_vec_t parts;
    if (cdc_vec_init(&parts, sizeof(char *)) != CDC_OK) {
        free(lower);
        return CDC_ERR_NOMEM;
    }
    char *save = NULL;
    for (char *tok = strtok_r(lower, "\\", &save); tok;
         tok = strtok_r(NULL, "\\", &save)) {
        if (tok[0] == '\0') continue;
        char *dup = strdup(tok);
        if (dup && cdc_vec_push(&parts, &dup) != CDC_OK) free(dup);
    }
    free(lower);
    *out = (char **)parts.data;
    *out_n = parts.len;
    parts.data = NULL;
    parts.len = parts.cap = 0;
    cdc_vec_free(&parts);
    return CDC_OK;
}

static int parts_start_with(char **full, size_t nfull, char **base,
                            size_t nbase) {
    if (nfull <= nbase) return 0;
    for (size_t i = 0; i < nbase; i++) {
        if (strcmp(full[i], base[i]) != 0) return 0;
    }
    return 1;
}

/* relative_parts：path 去掉 root 前缀（前缀须完整匹配）。 */
static int relative_parts(const char *path, const char *root, char ***out,
                          size_t *out_n) {
    char **full = NULL;
    size_t nfull = 0;
    if (parts_of(path, &full, &nfull) != CDC_OK) return CDC_ERR_NOMEM;
    if (root) {
        char **base = NULL;
        size_t nbase = 0;
        if (parts_of(root, &base, &nbase) == CDC_OK) {
            if (parts_start_with(full, nfull, base, nbase)) {
                /* 去掉前 nbase 段。 */
                for (size_t i = 0; i < nbase; i++) free(full[i]);
                memmove(full, full + nbase,
                        (nfull - nbase) * sizeof(char *));
                nfull -= nbase;
            }
            cdc_strvec_free(base, nbase);
        }
    }
    *out = full;
    *out_n = nfull;
    return CDC_OK;
}

/* basename 的扩展名（含 '.'，小写）；无 '.' 返回 ""。 */
static char *ext_of(const char *path) {
    const char *base = path;
    const char *p = path;
    while (*p) {
        if (*p == '\\' || *p == '/') base = p + 1;
        p++;
    }
    const char *dot = strrchr(base, '.');
    if (!dot) return strdup("");
    return ascii_lower_dup(dot);
}

static const char *dir_kind_of(char **dirs, size_t ndirs, const char *ext) {
    for (size_t g = 0; g < N_MEDIADIRS; g++) {
        int any = 0;
        for (size_t i = 0; i < ndirs && !any; i++) {
            for (size_t k = 0; k < MEDIA_DIRS[g].nnames; k++) {
                if (strcmp(dirs[i], MEDIA_DIRS[g].names[k]) == 0) {
                    any = 1;
                    break;
                }
            }
        }
        if (!any) continue;
        /* 微信把视频封面图（.jpg）和视频放在同一个 Video 目录里，
         * 不能因为目录叫 Video 就把封面图也算成视频。 */
        if (strcmp(MEDIA_DIRS[g].kind, "video") == 0 &&
            !ext_in(ext, VIDEO_EXT, N_VIDEO))
            continue;
        return MEDIA_DIRS[g].kind;
    }
    return NULL;
}

static const char *received_subkind(const char *path) {
    char *ext = ext_of(path);
    const char *sub = "other";
    if (ext_in(ext, DOC_EXT, N_DOC)) {
        sub = "doc";
    } else if (ext_in(ext, VIDEO_EXT, N_VIDEO)) {
        sub = "video";
    } else if (ext_in(ext, IMAGE_EXT, N_IMAGE)) {
        sub = "image";
    } else if (ext_in(ext, AUDIO_EXT, N_AUDIO)) {
        sub = "music";
    } else if (ext_in(ext, ARCHIVE_EXT, N_ARCHIVE)) {
        sub = "archive";
    }
    free(ext);
    return sub;
}

int cdc_im_classify(const char *path, const char *root, char **out_kind,
                    char **out_sub) {
    if (out_kind) *out_kind = NULL;
    if (out_sub) *out_sub = NULL;
    if (!path) return 0;
    char **parts = NULL;
    size_t nparts = 0;
    if (relative_parts(path, root, &parts, &nparts) != CDC_OK) return 0;
    int result = 0;
    if (nparts == 0) goto done;
    char *ext = ext_of(parts[nparts - 1]);
    if (ext_in(ext, SKIP_EXT, N_SKIP)) {
        free(ext);
        goto done;
    }
    /* 目录段命中跳过名单。 */
    for (size_t i = 0; i + 1 < nparts; i++) {
        if (ext_in(parts[i], SKIP_DIR_PARTS, N_SKIPDIR)) {
            free(ext);
            goto done;
        }
    }
    if (cdc_im_is_program_root(root ? root : "")) {
        const char *kind =
            (strcmp(ext, ".log") == 0 || strcmp(ext, ".xlog") == 0 ||
             strcmp(ext, ".dmp") == 0)
                ? "log"
                : "cache";
        if (out_kind) *out_kind = strdup(kind);
        if (out_sub) *out_sub = strdup("");
        free(ext);
        result = 1;
        goto done;
    }
    const char *kind = dir_kind_of(parts, nparts - 1, ext);
    char *kind_dup = NULL;
    if (kind) {
        kind_dup = strdup(kind);
    } else if (ext[0] == '\0' || strcmp(ext, ".dat") == 0 ||
               strcmp(ext, ".tmp") == 0) {
        free(ext);
        goto done;
    } else if (ext_in(ext, VIDEO_EXT, N_VIDEO)) {
        kind_dup = strdup("video");
    } else if (ext_in(ext, IMAGE_EXT, N_IMAGE)) {
        kind_dup = strdup("image");
    } else if (ext_in(ext, AUDIO_EXT, N_AUDIO)) {
        kind_dup = strdup("audio");
    } else if (ext_in(ext, FILE_EXT, N_FILE) ||
               ext_in(ext, ARCHIVE_EXT, N_ARCHIVE) ||
               ext_in(ext, DOC_EXT, N_DOC)) {
        kind_dup = strdup("file");
    } else if (strcmp(ext, ".log") == 0 || strcmp(ext, ".xlog") == 0 ||
               strcmp(ext, ".dmp") == 0) {
        kind_dup = strdup("log");
    } else {
        free(ext);
        goto done;
    }
    free(ext);
    if (!kind_dup) goto done;
    int is_file_kind = strcmp(kind_dup, "file") == 0;
    if (out_kind) *out_kind = kind_dup;
    else free(kind_dup);
    if (out_sub) {
        *out_sub = strdup(is_file_kind ? received_subkind(path) : "");
    }
    result = 1;
done:
    cdc_strvec_free(parts, nparts);
    return result;
}

/* ---------------- list_media ---------------- */

void cdc_im_record_free(cdc_im_record_t *r) {
    if (!r) return;
    free(r->path);
    free(r->kind);
    free(r->sub);
    free(r->month);
    free(r->name);
    memset(r, 0, sizeof(*r));
}

void cdc_im_records_free(cdc_im_record_t *records, size_t n) {
    if (!records) return;
    for (size_t i = 0; i < n; i++) cdc_im_record_free(&records[i]);
    free(records);
}

typedef struct {
    cdc_vec_t *records; /* cdc_im_record_t */
    const char *root;
    uint64_t seq;
} list_ctx_t;

static int list_media_cb(void *ctx, const char *path, int is_dir,
                         int is_symlink, uint64_t size, uint64_t mtime,
                         uint32_t attrs) {
    (void)attrs;
    if (is_dir || is_symlink) return 0;
    list_ctx_t *c = (list_ctx_t *)ctx;
    char *kind = NULL, *sub = NULL;
    if (!cdc_im_classify(path, c->root, &kind, &sub)) return 0;
    cdc_im_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.path = strdup(path);
    rec.size = size;
    rec.mtime = mtime;
    rec.kind = kind;
    rec.sub = sub;
    rec.month = cdc_month_label(mtime);
    rec.name = cdc_basename(path);
    rec.seq = c->seq++;
    if (!rec.path || !rec.month || !rec.name) {
        cdc_im_record_free(&rec);
        return CDC_ERR_NOMEM;
    }
    if (cdc_vec_push(c->records, &rec) != CDC_OK) {
        cdc_im_record_free(&rec);
        return CDC_ERR_NOMEM;
    }
    return 0;
}

static int cmp_record_mtime_desc(const void *a, const void *b) {
    const cdc_im_record_t *l = (const cdc_im_record_t *)a;
    const cdc_im_record_t *r = (const cdc_im_record_t *)b;
    if (l->mtime > r->mtime) return -1;
    if (l->mtime < r->mtime) return 1;
    if (l->seq < r->seq) return -1;
    if (l->seq > r->seq) return 1;
    return 0;
}

int cdc_im_list_media(const char **roots, size_t nroots,
                      cdc_im_record_t **out, size_t *out_n, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_n = 0;
    cdc_vec_t records;
    if (cdc_vec_init(&records, sizeof(cdc_im_record_t)) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    list_ctx_t ctx;
    ctx.records = &records;
    ctx.seq = 0;
    for (size_t i = 0; i < nroots; i++) {
        if (!is_dir_utf8(roots[i])) continue;
        ctx.root = roots[i];
        cdc_walk_dir(roots[i], 1, list_media_cb, &ctx, NULL);
    }
    if (records.len > 1)
        qsort(records.data, records.len, sizeof(cdc_im_record_t),
              cmp_record_mtime_desc);
    *out = (cdc_im_record_t *)records.data;
    *out_n = records.len;
    records.data = NULL;
    records.len = records.cap = 0;
    cdc_vec_free(&records);
    return CDC_OK;
}

/* ---------------- 已扫描集合（缩略图协议白名单） ---------------- */

#ifdef _WIN32
static CRITICAL_SECTION g_scanned_lock;
static int g_scanned_lock_init = 0;
static void scanned_lock(void) {
    if (!g_scanned_lock_init) {
        InitializeCriticalSection(&g_scanned_lock);
        g_scanned_lock_init = 1;
    }
    EnterCriticalSection(&g_scanned_lock);
}
static void scanned_unlock(void) { LeaveCriticalSection(&g_scanned_lock); }
#else
static pthread_mutex_t g_scanned_lock = PTHREAD_MUTEX_INITIALIZER;
static void scanned_lock(void) { pthread_mutex_lock(&g_scanned_lock); }
static void scanned_unlock(void) { pthread_mutex_unlock(&g_scanned_lock); }
#endif

static cdc_vec_t g_scanned; /* char*，只增不减 */
static int g_scanned_init = 0;

void cdc_im_remember_scanned(const cdc_im_record_t *records, size_t n) {
    scanned_lock();
    if (!g_scanned_init) {
        if (cdc_vec_init(&g_scanned, sizeof(char *)) != CDC_OK) {
            scanned_unlock();
            return;
        }
        g_scanned_init = 1;
    }
    for (size_t i = 0; i < n; i++) {
        int exists = 0;
        for (size_t k = 0; k < g_scanned.len; k++) {
            if (strcmp(((char **)g_scanned.data)[k], records[i].path) == 0) {
                exists = 1;
                break;
            }
        }
        if (!exists) {
            char *dup = strdup(records[i].path);
            if (dup && cdc_vec_push(&g_scanned, &dup) != CDC_OK) free(dup);
        }
    }
    scanned_unlock();
}

int cdc_im_is_scanned(const char *path) {
    if (!path) return 0;
    scanned_lock();
    int found = 0;
    if (g_scanned_init) {
        for (size_t k = 0; k < g_scanned.len; k++) {
            if (strcmp(((char **)g_scanned.data)[k], path) == 0) {
                found = 1;
                break;
            }
        }
    }
    scanned_unlock();
    return found;
}

/* ---------------- 缩略图 ---------------- */

static const char *image_mime(const uint8_t *head, size_t n) {
    if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
        return "image/jpeg";
    if (n >= 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (n >= 6 && (memcmp(head, "GIF87a", 6) == 0 ||
                   memcmp(head, "GIF89a", 6) == 0))
        return "image/gif";
    if (n >= 12 && memcmp(head, "RIFF", 4) == 0 &&
        memcmp(head + 8, "WEBP", 4) == 0)
        return "image/webp";
    if (n >= 2 && head[0] == 'B' && head[1] == 'M') return "image/bmp";
    return NULL;
}

static int read_image(const char *path, const char **out_mime,
                      uint8_t **out_data, size_t *out_len, cdc_err_t *err) {
    const uint64_t MAX = 64ULL * 1024ULL * 1024ULL;
    FILE *f = cdc_fopen_utf8(path, "rb");
    if (!f) return 0;
    /* 文件大小（fseek/ftell 在 64MB 上限内够用；Linux 上用 stat 更稳）。 */
    uint64_t size = 0;
    int size_ok = 0;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (w) {
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(w, GetFileExInfoStandard, &attr)) {
            DWORD a = attr.dwFileAttributes;
            if ((a & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
                size_ok = 1;
            }
        }
        free(w);
    }
#else
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        size = (uint64_t)st.st_size;
        size_ok = 1;
    }
#endif
    if (!size_ok || size > MAX) {
        fclose(f);
        return 0;
    }
    uint8_t head[16];
    size_t nread = fread(head, 1, sizeof(head), f);
    if (nread < 16) {
        fclose(f);
        return 0;
    }
    int encrypted = 0;
    if (image_mime(head, 16)) {
        encrypted = 0;
    } else {
        uint8_t probe[16];
        for (int i = 0; i < 16; i++) probe[i] = (uint8_t)(head[i] ^ 0x05);
        if (!image_mime(probe, 16)) {
            fclose(f);
            return 0;
        }
        encrypted = 1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    uint8_t *data = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    if (!data) {
        fclose(f);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    size_t got = size > 0 ? fread(data, 1, (size_t)size, f) : 0;
    fclose(f);
    if (got != size) {
        free(data);
        return 0;
    }
    if (encrypted) {
        for (uint64_t i = 0; i < size; i++) data[i] ^= 0x05;
    }
    const char *mime = image_mime(data, (size_t)size);
    if (!mime) {
        free(data);
        return 0;
    }
    *out_mime = mime;
    *out_data = data;
    *out_len = (size_t)size;
    return 1;
}

int cdc_im_thumbnail(const char *path, const char **out_mime,
                     uint8_t **out_data, size_t *out_len, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (out_mime) *out_mime = NULL;
    if (out_data) *out_data = NULL;
    if (out_len) *out_len = 0;
    if (!path) return 0;
    int rc = read_image(path, out_mime, out_data, out_len, err);
    if (rc != 0) return rc;
    /* 朋友圈视频没有封面扩展名，同名的 `_t` 文件就是它的封面图。 */
    cdc_buf_t b;
    cdc_buf_init(&b);
    cdc_buf_append_str(&b, path);
    cdc_buf_append_str(&b, "_t");
    char *alt = cdc_buf_take_cstr(&b);
    if (!alt) return 0;
    rc = read_image(alt, out_mime, out_data, out_len, err);
    free(alt);
    return rc;
}

/* ---------------- 预览 ---------------- */

static int looks_binary(const uint8_t *data, size_t n) {
    if (n == 0) return 0;
    size_t sample = n < 4096 ? n : 4096;
    size_t odd = 0;
    for (size_t i = 0; i < sample; i++) {
        uint8_t b = data[i];
        if (b < 0x09 || (b > 0x0D && b < 0x20)) odd++;
    }
    return odd * 100 / sample > 10;
}

static void hex_preview(const uint8_t *data, size_t n, cdc_buf_t *out) {
    size_t total = n < 512 ? n : 512; /* 32 行 × 16 字节 */
    size_t offset = 0;
    int first = 1;
    while (offset < total) {
        size_t chunk = total - offset < 16 ? total - offset : 16;
        char hex[48] = { 0 };
        size_t hp = 0;
        for (size_t i = 0; i < chunk; i++) {
            if (i > 0) hex[hp++] = ' ';
            hp += (size_t)snprintf(hex + hp, sizeof(hex) - hp, "%02x",
                                   data[offset + i]);
        }
        char ascii[17];
        for (size_t i = 0; i < chunk; i++) {
            uint8_t b = data[offset + i];
            ascii[i] = (b >= 32 && b < 127) ? (char)b : '.';
        }
        ascii[chunk] = '\0';
        if (!first) cdc_buf_append_str(out, "\n");
        first = 0;
        char line[128];
        /* Rust: format!("{:<47}  {ascii}")——hex 段左对齐补到 47 列。 */
        snprintf(line, sizeof(line), "%-47s  %s", hex, ascii);
        cdc_buf_append_str(out, line);
        offset += chunk;
    }
}

/* UTF-8 lossy 解码：非法序列按 U+FFFD 计（与 Rust from_utf8_lossy 的
 * maximal-subpart 替换基本一致：每段非法子序列一个 U+FFFD）。 */
static void utf8_lossy_append(cdc_buf_t *out, const uint8_t *s, size_t n) {
    static const char REPL[3] = { (char)0xEF, (char)0xBF, (char)0xBD };
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            cdc_buf_append(out, &c, 1);
            i++;
            continue;
        }
        size_t extra = 0;
        uint32_t cp = 0;
        uint32_t min_cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
            min_cp = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
            min_cp = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
            min_cp = 0x10000;
        } else {
            cdc_buf_append(out, REPL, 3);
            i++;
            continue;
        }
        /* 统计连续的后续字节数（maximal subpart）。 */
        size_t valid = 0;
        uint32_t vcp = cp;
        while (valid < extra && i + 1 + valid < n &&
               (s[i + 1 + valid] & 0xC0) == 0x80) {
            vcp = (vcp << 6) | (s[i + 1 + valid] & 0x3F);
            valid++;
        }
        if (valid == extra && vcp >= min_cp && vcp <= 0x10FFFF &&
            !(vcp >= 0xD800 && vcp <= 0xDFFF)) {
            cdc_buf_append(out, s + i, extra + 1);
            i += extra + 1;
        } else {
            /* 非法：整个 maximal subpart（lead + 已有的后续字节）换一个
             * U+FFFD；若一个后续字节都没有，只消费 lead 字节。 */
            cdc_buf_append(out, REPL, 3);
            i += 1 + valid;
        }
    }
}

void cdc_im_preview_free(cdc_im_preview_t *p) {
    if (!p) return;
    free(p->text);
    memset(p, 0, sizeof(*p));
}

int cdc_im_preview(const char *path, cdc_im_preview_t *out, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !path) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL param");
        return CDC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    const uint64_t LIMIT = 64ULL * 1024ULL;
    uint64_t size = 0;
    int meta_ok = 0, is_file = 0;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (w) {
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(w, GetFileExInfoStandard, &attr)) {
            meta_ok = 1;
            is_file = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
            size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
        }
        free(w);
    }
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        meta_ok = 1;
        is_file = S_ISREG(st.st_mode);
        size = (uint64_t)st.st_size;
    }
#endif
    /* Rust 顺序：metadata 失败 → "读取失败"；不是文件 → "不是文件"。 */
    if (meta_ok && !is_file) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "不是文件");
        return CDC_ERR_INVALID_ARG;
    }
    FILE *f = cdc_fopen_utf8(path, "rb");
    if (!f) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "读取失败：%s", path);
        return CDC_ERR_IO;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)LIMIT);
    if (!buf) {
        fclose(f);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    size_t got = fread(buf, 1, (size_t)LIMIT, f);
    fclose(f);
    int binary = looks_binary(buf, got);
    cdc_buf_t text;
    cdc_buf_init(&text);
    if (binary) {
        hex_preview(buf, got, &text);
    } else {
        utf8_lossy_append(&text, buf, got);
    }
    free(buf);
    out->text = cdc_buf_take_cstr(&text);
    out->binary = binary;
    out->truncated = size > LIMIT;
    out->size = size;
    if (!out->text) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

/* ---------------- 清理 ---------------- */

void cdc_delete_report_free(cdc_delete_report_t *r) {
    if (!r) return;
    cdc_strvec_free(r->failed, r->nfailed);
    cdc_strvec_free(r->reasons, r->nreasons);
    r->failed = NULL;
    r->reasons = NULL;
    r->nfailed = 0;
    r->nreasons = 0;
}

static void report_push(cdc_vec_t *vec, const char *s) {
    char *dup = strdup(s);
    if (dup && cdc_vec_push(vec, &dup) != CDC_OK) free(dup);
}

static int kind_wanted(const char *kind, const char **kinds, size_t nkinds) {
    for (size_t i = 0; i < nkinds; i++) {
        if (strcmp(kind, kinds[i]) == 0) return 1;
    }
    return 0;
}

static int file_mtime_size(const char *path, uint64_t *out_size,
                           uint64_t *out_mtime) {
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return CDC_ERR_UTF8;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &attr)) {
        free(w);
        return CDC_ERR_NOT_FOUND;
    }
    free(w);
    if ((attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return CDC_ERR_INVALID_ARG;
    *out_size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
    uint64_t ft = ((uint64_t)attr.ftLastWriteTime.dwHighDateTime << 32) |
                  attr.ftLastWriteTime.dwLowDateTime;
    *out_mtime = ft >= 116444736000000000ULL
                     ? (ft - 116444736000000000ULL) / 10000000ULL
                     : 0;
    return CDC_OK;
#else
    struct stat st;
    if (stat(path, &st) != 0) return CDC_ERR_NOT_FOUND;
    if (!S_ISREG(st.st_mode)) return CDC_ERR_INVALID_ARG;
    *out_size = (uint64_t)st.st_size;
    *out_mtime = (uint64_t)st.st_mtime;
    return CDC_OK;
#endif
}

int cdc_im_clean(const char *kind, const char **kinds, size_t nkinds,
                 const char **paths, size_t npaths, int has_paths,
                 cdc_delete_report_t *out, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !kind) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL param");
        return CDC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char **roots = NULL;
    size_t nroots = 0;
    if (cdc_im_roots(kind, &roots, &nroots, NULL) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "无法探测 %s 数据目录", kind);
        return CDC_ERR_IO;
    }
    cdc_vec_t chosen; /* cdc_im_record_t */
    if (cdc_vec_init(&chosen, sizeof(cdc_im_record_t)) != CDC_OK) {
        cdc_strvec_free(roots, nroots);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    if (!has_paths) {
        cdc_im_record_t *all = NULL;
        size_t nall = 0;
        if (cdc_im_list_media((const char **)roots, nroots, &all, &nall,
                              NULL) == CDC_OK) {
            for (size_t i = 0; i < nall; i++) {
                if (!kind_wanted(all[i].kind, kinds, nkinds)) {
                    cdc_im_record_free(&all[i]);
                    continue;
                }
                if (cdc_vec_push(&chosen, &all[i]) != CDC_OK)
                    cdc_im_record_free(&all[i]);
            }
            free(all);
        }
    } else {
        for (size_t i = 0; i < npaths; i++) {
            const char *path = paths[i] ? paths[i] : "";
            int blank = 1;
            for (const char *p = path; *p; p++) {
                if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                    blank = 0;
                    break;
                }
            }
            if (blank) continue;
            if (!cdc_is_within(path, (const char **)roots, nroots)) continue;
            if (cdc_is_password_store(path) || cdc_is_protected_path(path))
                continue;
            /* 取包含该路径的最长根目录（与 Rust max_by_key(root.len()) 一致）。 */
            const char *best_root = NULL;
            size_t best_len = 0;
            for (size_t r = 0; r < nroots; r++) {
                if (cdc_is_within(path, (const char **)&roots[r], 1)) {
                    size_t l = strlen(roots[r]);
                    if (l > best_len) {
                        best_len = l;
                        best_root = roots[r];
                    }
                }
            }
            char *rk = NULL, *rs = NULL;
            if (!cdc_im_classify(path, best_root, &rk, &rs)) continue;
            if (!kind_wanted(rk, kinds, nkinds)) {
                free(rk);
                free(rs);
                continue;
            }
            uint64_t size = 0, mtime = 0;
            if (file_mtime_size(path, &size, &mtime) != CDC_OK) {
                free(rk);
                free(rs);
                continue;
            }
            cdc_im_record_t rec;
            memset(&rec, 0, sizeof(rec));
            rec.path = strdup(path);
            rec.size = size;
            rec.mtime = mtime;
            rec.kind = rk;
            rec.sub = rs;
            rec.month = cdc_month_label(mtime);
            rec.name = cdc_basename(path);
            if (!rec.path || !rec.month || !rec.name) {
                cdc_im_record_free(&rec);
                continue;
            }
            if (cdc_vec_push(&chosen, &rec) != CDC_OK)
                cdc_im_record_free(&rec);
        }
    }
    cdc_strvec_free(roots, nroots);
    if (chosen.len == 0) {
        cdc_vec_free(&chosen);
        return CDC_OK;
    }
    uint64_t total = (uint64_t)chosen.len;
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "im_clean", 0, total);
    if (cancelled()) {
        cdc_vec_t reasons;
        cdc_vec_init(&reasons, sizeof(char *));
        report_push(&reasons, "已取消");
        out->reasons = (char **)reasons.data;
        out->nreasons = reasons.len;
        reasons.data = NULL;
        reasons.len = reasons.cap = 0;
        cdc_vec_free(&reasons);
        for (size_t i = 0; i < chosen.len; i++)
            cdc_im_record_free(&((cdc_im_record_t *)chosen.data)[i]);
        cdc_vec_free(&chosen);
        return CDC_OK;
    }
    /* 批量送回收站（与 Rust 的 send_to_recycle(&batch) 一致：整批调用，
     * 失败的路径后续按“仍存在”判定）。 */
    cdc_vec_t batch;
    cdc_vec_init(&batch, sizeof(char *));
    for (size_t i = 0; i < chosen.len; i++) {
        char *dup = strdup(((cdc_im_record_t *)chosen.data)[i].path);
        if (dup && cdc_vec_push(&batch, &dup) != CDC_OK) free(dup);
    }
    char **failed = NULL;
    size_t nfailed = 0;
    cdc_send_to_recycle((const char **)batch.data, batch.len, &failed,
                        &nfailed, NULL);
    cdc_strvec_free(failed, nfailed);
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "im_clean", total, total);
    cdc_vec_t failed_vec, reasons_vec;
    cdc_vec_init(&failed_vec, sizeof(char *));
    cdc_vec_init(&reasons_vec, sizeof(char *));
    for (size_t i = 0; i < chosen.len; i++) {
        cdc_im_record_t *rec = &((cdc_im_record_t *)chosen.data)[i];
        int still_exists = 0;
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(rec->path);
        if (w) {
            DWORD a = GetFileAttributesW(w);
            still_exists = a != INVALID_FILE_ATTRIBUTES;
            free(w);
        }
#else
        struct stat st;
        still_exists = stat(rec->path, &st) == 0;
#endif
        if (still_exists) {
            report_push(&failed_vec, rec->path);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：送回收站失败（可能被占用）",
                     rec->path);
            report_push(&reasons_vec, buf);
        } else {
            out->freed += rec->size;
            out->deleted++;
        }
    }
    out->failed = (char **)failed_vec.data;
    out->nfailed = failed_vec.len;
    failed_vec.data = NULL;
    failed_vec.len = failed_vec.cap = 0;
    cdc_vec_free(&failed_vec);
    out->reasons = (char **)reasons_vec.data;
    out->nreasons = reasons_vec.len;
    reasons_vec.data = NULL;
    reasons_vec.len = reasons_vec.cap = 0;
    cdc_vec_free(&reasons_vec);
    /* 重复文件缓存失效（Rust 的 crate::invalidate_dup_cache()；
     * 本模块只通过 hooks 触发，由 bridge 注册实现）。 */
    if (g_hooks.invalidate_dup_cache)
        g_hooks.invalidate_dup_cache(g_hooks.ctx);
    {
        const char *label = strcmp(kind, "wechat") == 0 ? "微信" : "QQ";
        char buf[256];
        snprintf(buf, sizeof(buf), "完成：移到回收站 %llu 个，释放 %llu 字节",
                 (unsigned long long)out->deleted,
                 (unsigned long long)out->freed);
        cdc_buf_t b;
        cdc_buf_init(&b);
        cdc_buf_append_str(&b, "清理：");
        cdc_buf_append_str(&b, label);
        char *line2 = cdc_buf_take_cstr(&b);
        const char *lines[3] = { "开始清理", line2 ? line2 : "", buf };
        cdc_write_log(lines, 3, NULL, NULL);
        free(line2);
    }
    for (size_t i = 0; i < chosen.len; i++)
        cdc_im_record_free(&((cdc_im_record_t *)chosen.data)[i]);
    cdc_vec_free(&chosen);
    cdc_strvec_free((char **)batch.data, batch.len);
    batch.data = NULL;
    batch.len = batch.cap = 0;
    cdc_vec_free(&batch);
    return CDC_OK;
}
