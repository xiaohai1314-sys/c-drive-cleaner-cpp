/* privacy.c — 移植自 Rust 版 desktop/src/privacy.rs。
 * SQLite 通过 third_party/sqlite3（-Ithird_party/sqlite3）。
 * Windows 专属部分（注册表、%LOCALAPPDATA% 下的浏览器目录）在
 * Linux 下返回空结果：scan() 只有最近文件/媒体等纯文件逻辑可跑，
 * 注册表项计数为 0。 */
#include "privacy.h"

#include <stdlib.h>
#include <string.h>

#include "logs.h"
#include "cdc_process.h"
#include "recycle.h"
#include "safety.h"
#include "sqlite3.h"
#include "system.h"
#include "timeutil.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

static cdc_hooks_t g_hooks = { 0 };

void cdc_privacy_set_hooks(const cdc_hooks_t *hooks) {
    if (hooks) {
        g_hooks = *hooks;
    } else {
        memset(&g_hooks, 0, sizeof(g_hooks));
    }
}

static int cancelled(void) {
    return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, "privacy");
}

static uint64_t g_done = 0;
static uint64_t g_total = 0;

static void add_done(void) {
    g_done++;
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "privacy", g_done, g_total);
}

static void set_total(uint64_t total) {
    g_total = total;
    g_done = 0;
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "privacy", 0, total);
}

/* ---------------- 常量表（与 Rust 版逐字一致） ---------------- */

static const char *SHOP_DOMAINS[] = {
    "taobao.com", "tmall.com", "jd.com",   "yangkeduo.com",
    "pinduoduo.com", "amazon.com", "amazon.cn", "smzdm.com",
    "suning.com",   "dangdang.com",
};
static const char *FINANCE_DOMAINS[] = {
    "alipay.com", "paypal.com", "icbc.com.cn", "ccb.com",
    "cmbchina.com", "unionpay.com", "abchina.com",
};
static const char *MEDICAL_DOMAINS[] = {
    "haodf.com", "dxy.cn", "xywy.com", "chunyuyisheng.com", "guahao.com",
};
static const char *SENSITIVE_WORDS[] = { "身份证", "密码", "银行卡",
                                         "合同",   "护照", "工资" };
static const char *RECENT_MEDIA_EXT[] = { ".mp4", ".mkv", ".avi", ".mp3",
                                          ".flac", ".wav", ".mov" };

#define N_SHOP (sizeof(SHOP_DOMAINS) / sizeof(SHOP_DOMAINS[0]))
#define N_FINANCE (sizeof(FINANCE_DOMAINS) / sizeof(FINANCE_DOMAINS[0]))
#define N_MEDICAL (sizeof(MEDICAL_DOMAINS) / sizeof(MEDICAL_DOMAINS[0]))

static const char *RECENT_DOCS =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RecentDocs";
#ifdef _WIN32
static const char *USER_ASSIST =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist";
#endif /* _WIN32 */
static const char *WMP_RECENT =
    "Software\\Microsoft\\MediaPlayer\\Player\\RecentFileList";
static const char *USB_STORAGE = "SYSTEM\\CurrentControlSet\\Enum\\USBSTOR";

/* ---------------- 小工具 ---------------- */

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

static int is_file_utf8(const char *path) {
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES &&
           (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static char *path_join2(const char *a, const char *b) {
    cdc_buf_t buf;
    cdc_buf_init(&buf);
    cdc_buf_append_str(&buf, a);
    size_t n = strlen(a);
    /* b 可能以 '\' 开头（如 "\Google\..."），a 末尾可能已有分隔符：
     * 只保留一个分隔符，避免 "Local\\Google" 这种双斜杠。 */
    while (b[0] == '\\' || b[0] == '/') b++;
    if (n > 0 && a[n - 1] != '\\' && a[n - 1] != '/')
        cdc_buf_append_str(&buf, "\\");
    cdc_buf_append_str(&buf, b);
    return cdc_buf_take_cstr(&buf);
}

/* 本地 strdup（malloc 版；与 safety.c / mft.c 各自的 static 同款，
 * 不用 libc strdup，避免 MSVC C4996）。 */
static char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* 列出目录下的直接子项名（不递归；走 util.c 的 cdc_enum_dir，与旧
 * 实现同语义：返回名，不含 "." ".."；不存在/不可读 = 空）。
 * 名字解码失败的项：旧 Windows 版静默跳过（cdc_wide_to_utf8 返回
 * NULL），新实现按 U+FFFD 替换后收录（与 cdc_wide_to_utf8 同策略，
 * 调用方只拿名做存在性比对，替换后的名匹配不上，无行为变化）。 */
typedef struct {
    cdc_vec_t *names;
} ListNamesCtx;

static int list_names_cb(const char *name, int is_dir, int is_symlink, uint64_t size,
                         uint64_t mtime, uint32_t attrs, void *ctx) {
    ListNamesCtx *c = (ListNamesCtx *)ctx;
    char *u;
    (void)is_dir;
    (void)is_symlink;
    (void)size;
    (void)mtime;
    (void)attrs;
    u = xstrdup(name);
    if (!u) return 0; /* 内存不足：跳过本项（旧实现同款：push 失败即丢弃） */
    if (cdc_vec_push(c->names, &u) != CDC_OK) {
        free(u);
        return 0;
    }
    return 0;
}

static int list_dir_names(const char *dir, char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    cdc_vec_t names;
    ListNamesCtx c;
    int rc;
    if (cdc_vec_init(&names, sizeof(char *)) != CDC_OK) return CDC_ERR_NOMEM;
    c.names = &names;
    rc = cdc_enum_dir(dir, list_names_cb, &c);
    /* 不存在/不可读 = 空（旧实现同款：两平台都返回 OK）。 */
    if (rc == CDC_ERR_NOT_FOUND || rc == CDC_ERR_IO) rc = CDC_OK;
    if (rc != CDC_OK) {
        cdc_vec_free(&names);
        return rc;
    }
    *out = (char **)names.data;
    *out_n = names.len;
    names.data = NULL;
    names.len = names.cap = 0;
    cdc_vec_free(&names);
    return CDC_OK;
}

/* 纳秒时间戳（connect_copy 的临时文件名 nonce；与 Rust 的
 * SystemTime::now().duration_since(UNIX_EPOCH).as_nanos() 同源）。 */
static uint64_t now_nanos(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* 1601-01-01 到 1970-01-01 共 11644473600 秒。 */
    if (t < 116444736000000000ULL) return 0;
    return (t - 116444736000000000ULL) * 100ULL;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ---------------- 浏览器 ---------------- */

typedef struct {
    char *id;   /* chrome / edge / qqbrowser / quark / brave / firefox */
    char *name; /* Chrome / Edge / QQ浏览器 / 夸克浏览器 / Brave / Firefox */
    char *root;
    int firefox;
} browser_spec_t;

static void spec_free(browser_spec_t *s) {
    if (!s) return;
    free(s->id);
    free(s->name);
    free(s->root);
}

static void specs_free(browser_spec_t *specs, size_t n) {
    for (size_t i = 0; i < n; i++) spec_free(&specs[i]);
    free(specs);
}

static browser_spec_t *browser_specs(size_t *out_n) {
    *out_n = 0;
    cdc_vec_t specs;
    if (cdc_vec_init(&specs, sizeof(browser_spec_t)) != CDC_OK) return NULL;
    char *local = cdc_getenv_utf8("LOCALAPPDATA");
    char *roaming = cdc_getenv_utf8("APPDATA");
    static const struct {
        const char *id;
        const char *name;
        const char *suffix;
    } CHROMIUM[] = {
        { "chrome", "Chrome", "\\Google\\Chrome\\User Data" },
        { "edge", "Edge", "\\Microsoft\\Edge\\User Data" },
        { "qqbrowser", "QQ浏览器", "\\Tencent\\QQBrowser\\User Data" },
        { "quark", "夸克浏览器", "\\Quark\\User Data" },
        { "brave", "Brave", "\\BraveSoftware\\Brave-Browser\\User Data" },
    };
    for (size_t i = 0; i < sizeof(CHROMIUM) / sizeof(CHROMIUM[0]); i++) {
        char *root = path_join2(local ? local : "", CHROMIUM[i].suffix);
        if (is_dir_utf8(root)) {
            browser_spec_t s;
            s.id = strdup(CHROMIUM[i].id);
            s.name = strdup(CHROMIUM[i].name);
            s.root = root;
            s.firefox = 0;
            if (!s.id || !s.name || !s.root) {
                free(s.id);
                free(s.name);
                free(s.root);
                continue;
            }
            if (cdc_vec_push(&specs, &s) != CDC_OK) spec_free(&s);
        } else {
            free(root);
        }
    }
    char *ff = path_join2(roaming ? roaming : "", "\\Mozilla\\Firefox\\Profiles");
    if (is_dir_utf8(ff)) {
        browser_spec_t s;
        s.id = strdup("firefox");
        s.name = strdup("Firefox");
        s.root = ff;
        s.firefox = 1;
        if (!s.id || !s.name || !s.root) {
            free(s.id);
            free(s.name);
            free(s.root);
        } else if (cdc_vec_push(&specs, &s) != CDC_OK) {
            spec_free(&s);
        }
    } else {
        free(ff);
    }
    free(local);
    free(roaming);
    *out_n = specs.len;
    browser_spec_t *arr = (browser_spec_t *)specs.data;
    specs.data = NULL;
    specs.len = specs.cap = 0;
    cdc_vec_free(&specs);
    return arr;
}

/* 配置目录列表：Chromium 只取 Default / Profile* / Guest Profile；
 * Firefox 取全部子目录。 */
static int profiles_of(const browser_spec_t *spec, char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    char **names = NULL;
    size_t nnames = 0;
    list_dir_names(spec->root, &names, &nnames);
    cdc_vec_t found;
    if (cdc_vec_init(&found, sizeof(char *)) != CDC_OK) {
        cdc_strvec_free(names, nnames);
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < nnames; i++) {
        char *full = path_join2(spec->root, names[i]);
        if (!is_dir_utf8(full)) {
            free(full);
            continue;
        }
        if (spec->firefox) {
            if (cdc_vec_push(&found, &full) != CDC_OK) free(full);
            continue;
        }
        if (strcmp(names[i], "Default") == 0 ||
            strncmp(names[i], "Profile", 7) == 0 ||
            strcmp(names[i], "Guest Profile") == 0) {
            if (cdc_vec_push(&found, &full) != CDC_OK) free(full);
        } else {
            free(full);
        }
    }
    cdc_strvec_free(names, nnames);
    *out = (char **)found.data;
    *out_n = found.len;
    found.data = NULL;
    found.len = found.cap = 0;
    cdc_vec_free(&found);
    return CDC_OK;
}

static char *history_file(const browser_spec_t *spec, const char *profile) {
    char *p = spec->firefox ? path_join2(profile, "places.sqlite")
                            : path_join2(profile, "History");
    if (is_file_utf8(p)) return p;
    free(p);
    return NULL;
}

static int cookie_files(const browser_spec_t *spec, const char *profile,
                        char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    cdc_vec_t files;
    if (cdc_vec_init(&files, sizeof(char *)) != CDC_OK) return CDC_ERR_NOMEM;
    if (spec->firefox) {
        char *p = path_join2(profile, "cookies.sqlite");
        if (is_file_utf8(p)) {
            if (cdc_vec_push(&files, &p) != CDC_OK) free(p);
        } else {
            free(p);
        }
    } else {
        char *p1 = path_join2(profile, "Cookies");
        char *net = path_join2(profile, "Network");
        char *p2 = path_join2(net, "Cookies");
        free(net);
        const char *cands[2] = { p1, p2 };
        for (int i = 0; i < 2; i++) {
            if (is_file_utf8(cands[i]) &&
                !cdc_is_password_store(cands[i])) {
                char *dup = strdup(cands[i]);
                if (dup && cdc_vec_push(&files, &dup) != CDC_OK) free(dup);
            }
        }
        free(p1);
        free(p2);
    }
    *out = (char **)files.data;
    *out_n = files.len;
    files.data = NULL;
    files.len = files.cap = 0;
    cdc_vec_free(&files);
    return CDC_OK;
}

/* ---------------- SQLite 副本 ---------------- */

typedef struct {
    sqlite3 *db;
    char *tmp_path; /* 用完删除的临时副本 */
} db_copy_t;

static void db_copy_close(db_copy_t *c) {
    if (!c) return;
    if (c->db) sqlite3_close(c->db);
    if (c->tmp_path) {
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(c->tmp_path);
        if (w) {
            DeleteFileW(w);
            free(w);
        }
#else
        remove(c->tmp_path);
#endif
        free(c->tmp_path);
    }
    c->db = NULL;
    c->tmp_path = NULL;
}

/* 拷一份到 %TEMP%\cdc-privacy-{stamp}-{pid}-{nanos}.db 再打开
 *（浏览器可能正占用原文件；密码库一律不碰）。 */
static int connect_copy(const char *path, db_copy_t *out) {
    memset(out, 0, sizeof(*out));
    if (!is_file_utf8(path) || cdc_is_password_store(path)) return CDC_ERR_IO;
    char *tmpdir = cdc_getenv_utf8("TEMP");
    if (!tmpdir || !tmpdir[0]) {
        free(tmpdir);
#ifdef _WIN32
        tmpdir = cdc_getenv_utf8("TMP");
        if (!tmpdir || !tmpdir[0]) {
            free(tmpdir);
            tmpdir = strdup("C:\\Windows\\Temp");
        }
#else
        tmpdir = strdup("/tmp");
#endif
    }
    char *stamp = cdc_stamp();
    cdc_buf_t buf;
    cdc_buf_init(&buf);
    char tmp[128];
#ifdef _WIN32
    snprintf(tmp, sizeof(tmp), "cdc-privacy-%s-%lu-%llu.db", stamp ? stamp : "",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long long)now_nanos());
#else
    snprintf(tmp, sizeof(tmp), "cdc-privacy-%s-%ld-%llu.db", stamp ? stamp : "",
             (long)getpid(), (unsigned long long)now_nanos());
#endif
    free(stamp);
    cdc_buf_append_str(&buf, tmpdir);
    free(tmpdir);
    size_t n = buf.len;
    if (n > 0 && buf.data[n - 1] != '\\' && buf.data[n - 1] != '/')
        cdc_buf_append_str(&buf, "\\");
    cdc_buf_append_str(&buf, tmp);
    char *tmp_path = cdc_buf_take_cstr(&buf);
    if (!tmp_path) return CDC_ERR_NOMEM;
    /* 复制原文件。 */
    int copied = 0;
#ifdef _WIN32
    wchar_t *ws = cdc_utf8_to_wide(path);
    wchar_t *wd = cdc_utf8_to_wide(tmp_path);
    if (ws && wd) copied = CopyFileW(ws, wd, FALSE) != 0;
    free(ws);
    free(wd);
#else
    FILE *in = cdc_fopen_utf8(path, "rb");
    FILE *o = in ? cdc_fopen_utf8(tmp_path, "wb") : NULL;
    if (in && o) {
        uint8_t chunk[65536];
        size_t got;
        copied = 1;
        while ((got = fread(chunk, 1, sizeof(chunk), in)) > 0) {
            if (fwrite(chunk, 1, got, o) != got) {
                copied = 0;
                break;
            }
        }
    }
    if (in) fclose(in);
    if (o) fclose(o);
#endif
    if (!copied) {
        free(tmp_path);
        return CDC_ERR_IO;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(tmp_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        free(tmp_path);
        return CDC_ERR_IO;
    }
    out->db = db;
    out->tmp_path = tmp_path;
    return CDC_OK;
}

/* COUNT(*) 查询：成功返回 CDC_OK 且 *out_count 有值；表不存在等
 * 失败返回 CDC_ERR_NOT_FOUND（调用方按 Rust 的 or_else 换备用表）。 */
static int sql_count(sqlite3 *db, const char *sql, int64_t *out_count) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return CDC_ERR_NOT_FOUND;
    int rc = sqlite3_step(st);
    int ok = rc == SQLITE_ROW;
    if (ok) *out_count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ok ? CDC_OK : CDC_ERR_NOT_FOUND;
}

static int sql_count_params(sqlite3 *db, const char *sql,
                            const char **params, size_t nparams,
                            int64_t *out_count) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return CDC_ERR_NOT_FOUND;
    for (size_t i = 0; i < nparams; i++)
        sqlite3_bind_text(st, (int)i + 1, params[i], -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    int ok = rc == SQLITE_ROW;
    if (ok) *out_count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ok ? CDC_OK : CDC_ERR_NOT_FOUND;
}

/* SELECT COUNT(*) FROM <table> WHERE url LIKE ? OR ...（域名固定常量）。 */
static int domain_count(sqlite3 *db, const char *table,
                        const char **domains, size_t ndomains,
                        int64_t *out_count) {
    cdc_buf_t sql;
    cdc_buf_init(&sql);
    cdc_buf_append_str(&sql, "SELECT COUNT(*) FROM ");
    cdc_buf_append_str(&sql, table);
    cdc_buf_append_str(&sql, " WHERE ");
    for (size_t i = 0; i < ndomains; i++) {
        if (i > 0) cdc_buf_append_str(&sql, " OR ");
        cdc_buf_append_str(&sql, "url LIKE ?");
    }
    char *patterns[32];
    size_t n = ndomains < 32 ? ndomains : 32;
    char *patbuf[32];
    for (size_t i = 0; i < n; i++) {
        cdc_buf_t p;
        cdc_buf_init(&p);
        cdc_buf_append_str(&p, "%");
        cdc_buf_append_str(&p, domains[i]);
        cdc_buf_append_str(&p, "%");
        patbuf[i] = cdc_buf_take_cstr(&p);
        patterns[i] = patbuf[i];
    }
    char *sql_s = cdc_buf_take_cstr(&sql);
    int rc = sql_count_params(db, sql_s ? sql_s : "", (const char **)patterns,
                              n, out_count);
    free(sql_s);
    for (size_t i = 0; i < n; i++) free(patbuf[i]);
    return rc;
}

typedef struct {
    int64_t urls;
    int64_t downloads;
    int64_t shop;
    int64_t money;
    int64_t medical;
    int urls_available;
} history_stats_t;

static history_stats_t history_stats(const char *path) {
    history_stats_t s;
    memset(&s, 0, sizeof(s));
    db_copy_t c;
    if (connect_copy(path, &c) != CDC_OK) return s;
    int64_t v = 0;
    if (sql_count(c.db, "SELECT COUNT(*) FROM urls", &v) == CDC_OK ||
        sql_count(c.db, "SELECT COUNT(*) FROM moz_places", &v) == CDC_OK) {
        s.urls = v;
        s.urls_available = 1;
    }
    if (sql_count(c.db, "SELECT COUNT(*) FROM downloads", &v) == CDC_OK) {
        s.downloads = v;
    } else if (sql_count(c.db,
                         "SELECT COUNT(*) FROM moz_annos a JOIN "
                         "moz_anno_attributes n ON a.anno_attribute_id = "
                         "n.id WHERE n.name LIKE 'downloads/%'",
                         &v) == CDC_OK) {
        s.downloads = v;
    }
    if (domain_count(c.db, "urls", SHOP_DOMAINS, N_SHOP, &v) == CDC_OK) {
        s.shop = v;
    } else if (domain_count(c.db, "moz_places", SHOP_DOMAINS, N_SHOP, &v) ==
               CDC_OK) {
        s.shop = v;
    }
    if (domain_count(c.db, "urls", FINANCE_DOMAINS, N_FINANCE, &v) == CDC_OK) {
        s.money = v;
    } else if (domain_count(c.db, "moz_places", FINANCE_DOMAINS, N_FINANCE,
                            &v) == CDC_OK) {
        s.money = v;
    }
    if (domain_count(c.db, "urls", MEDICAL_DOMAINS, N_MEDICAL, &v) == CDC_OK) {
        s.medical = v;
    } else if (domain_count(c.db, "moz_places", MEDICAL_DOMAINS, N_MEDICAL,
                            &v) == CDC_OK) {
        s.medical = v;
    }
    db_copy_close(&c);
    return s;
}

static int64_t cookie_count(const char *path) {
    db_copy_t c;
    if (connect_copy(path, &c) != CDC_OK) return 0;
    int64_t v = 0;
    if (sql_count(c.db, "SELECT COUNT(*) FROM cookies", &v) != CDC_OK)
        sql_count(c.db, "SELECT COUNT(*) FROM moz_cookies", &v);
    db_copy_close(&c);
    return v;
}

/* ---------------- 清理用 SQL ---------------- */

static int sql_statements(const char *kind, char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    static const char *HISTORY[] = {
        "DELETE FROM visits",
        "DELETE FROM urls",
        "DELETE FROM moz_historyvisits",
        "DELETE FROM moz_places",
    };
    static const char *COOKIES[] = {
        "DELETE FROM cookies",
        "DELETE FROM moz_cookies",
    };
    static const char *DOWNLOADS[] = {
        "DELETE FROM downloads_url_chains",
        "DELETE FROM downloads",
        ("DELETE FROM moz_annos WHERE anno_attribute_id IN (SELECT id FROM "
         "moz_anno_attributes WHERE name LIKE 'downloads/%')"),
    };
    const char **list = NULL;
    size_t n = 0;
    if (strcmp(kind, "history") == 0) {
        list = HISTORY;
        n = sizeof(HISTORY) / sizeof(HISTORY[0]);
    } else if (strcmp(kind, "cookies") == 0) {
        list = COOKIES;
        n = sizeof(COOKIES) / sizeof(COOKIES[0]);
    } else if (strcmp(kind, "downloads") == 0) {
        list = DOWNLOADS;
        n = sizeof(DOWNLOADS) / sizeof(DOWNLOADS[0]);
    } else if (strcmp(kind, "shop") == 0 || strcmp(kind, "money") == 0 ||
               strcmp(kind, "medical") == 0) {
        const char **domains = NULL;
        size_t nd = 0;
        if (strcmp(kind, "shop") == 0) {
            domains = SHOP_DOMAINS;
            nd = N_SHOP;
        } else if (strcmp(kind, "money") == 0) {
            domains = FINANCE_DOMAINS;
            nd = N_FINANCE;
        } else {
            domains = MEDICAL_DOMAINS;
            nd = N_MEDICAL;
        }
        cdc_buf_t likes;
        cdc_buf_init(&likes);
        for (size_t i = 0; i < nd; i++) {
            if (i > 0) cdc_buf_append_str(&likes, " OR ");
            cdc_buf_append_str(&likes, "url LIKE '%");
            cdc_buf_append_str(&likes, domains[i]);
            cdc_buf_append_str(&likes, "%'");
        }
        char *likes_s = cdc_buf_take_cstr(&likes);
        const char *fmts[3] = {
            "DELETE FROM visits WHERE url IN (SELECT id FROM urls WHERE %s)",
            "DELETE FROM urls WHERE %s",
            "DELETE FROM moz_places WHERE %s",
        };
        cdc_vec_t outv;
        if (cdc_vec_init(&outv, sizeof(char *)) != CDC_OK) {
            free(likes_s);
            return CDC_ERR_NOMEM;
        }
        for (int i = 0; i < 3; i++) {
            char buf[4096];
            snprintf(buf, sizeof(buf), fmts[i], likes_s ? likes_s : "");
            char *dup = strdup(buf);
            if (dup && cdc_vec_push(&outv, &dup) != CDC_OK) free(dup);
        }
        free(likes_s);
        *out = (char **)outv.data;
        *out_n = outv.len;
        outv.data = NULL;
        outv.len = outv.cap = 0;
        cdc_vec_free(&outv);
        return CDC_OK;
    } else {
        return CDC_OK; /* 未知 kind：空列表 */
    }
    cdc_vec_t outv;
    if (cdc_vec_init(&outv, sizeof(char *)) != CDC_OK) return CDC_ERR_NOMEM;
    for (size_t i = 0; i < n; i++) {
        char *dup = strdup(list[i]);
        if (dup && cdc_vec_push(&outv, &dup) != CDC_OK) free(dup);
    }
    *out = (char **)outv.data;
    *out_n = outv.len;
    outv.data = NULL;
    outv.len = outv.cap = 0;
    cdc_vec_free(&outv);
    return CDC_OK;
}

static int apply_sql(const char *path, const char **statements, size_t nstmts) {
    if (!is_file_utf8(path) || cdc_is_password_store(path)) return 0;
    db_copy_t c;
    if (connect_copy(path, &c) != CDC_OK) return 0;
    int applied = 0;
    for (size_t i = 0; i < nstmts; i++) {
        char *emsg = NULL;
        if (sqlite3_exec(c.db, statements[i], NULL, NULL, &emsg) == SQLITE_OK)
            applied = 1;
        if (emsg) sqlite3_free(emsg);
    }
    if (!applied) {
        db_copy_close(&c);
        return 0;
    }
    sqlite3_close(c.db);
    c.db = NULL;
    /* 把改好的副本拷回原文件。 */
    int copied = 0;
#ifdef _WIN32
    wchar_t *ws = cdc_utf8_to_wide(c.tmp_path);
    wchar_t *wd = cdc_utf8_to_wide(path);
    if (ws && wd) copied = CopyFileW(ws, wd, FALSE) != 0;
    free(ws);
    free(wd);
#else
    FILE *in = cdc_fopen_utf8(c.tmp_path, "rb");
    FILE *o = in ? cdc_fopen_utf8(path, "wb") : NULL;
    if (in && o) {
        uint8_t chunk[65536];
        size_t got;
        copied = 1;
        while ((got = fread(chunk, 1, sizeof(chunk), in)) > 0) {
            if (fwrite(chunk, 1, got, o) != got) {
                copied = 0;
                break;
            }
        }
    }
    if (in) fclose(in);
    if (o) fclose(o);
#endif
    db_copy_close(&c);
    return copied;
}

/* ---------------- 注册表 ---------------- */

#ifdef _WIN32
typedef HKEY cdc_hkey_t;

static cdc_hkey_t hive_root(const char *hive) {
    return strcmp(hive, "HKLM") == 0 ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

static int reg_open(cdc_hkey_t root, const char *path, REGSAM access,
                    cdc_hkey_t *out_key) {
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return CDC_ERR_UTF8;
    HKEY key = NULL;
    LONG code = RegOpenKeyExW(root, w, 0, access, &key);
    free(w);
    if (code != ERROR_SUCCESS || !key) return CDC_ERR_NOT_FOUND;
    *out_key = key;
    return CDC_OK;
}

static int reg_list_values(cdc_hkey_t key, char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    cdc_vec_t names;
    if (cdc_vec_init(&names, sizeof(char *)) != CDC_OK) return CDC_ERR_NOMEM;
    DWORD index = 0;
    for (;;) {
        wchar_t buf[512];
        DWORD len = 512;
        LONG code = RegEnumValueW(key, index, buf, &len, NULL, NULL, NULL, NULL);
        if (code != ERROR_SUCCESS) break;
        char *u = cdc_wide_to_utf8(buf);
        if (u && cdc_vec_push(&names, &u) != CDC_OK) free(u);
        index++;
    }
    *out = (char **)names.data;
    *out_n = names.len;
    names.data = NULL;
    names.len = names.cap = 0;
    cdc_vec_free(&names);
    return CDC_OK;
}

static int reg_list_subkeys(cdc_hkey_t key, char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    cdc_vec_t names;
    if (cdc_vec_init(&names, sizeof(char *)) != CDC_OK) return CDC_ERR_NOMEM;
    DWORD index = 0;
    for (;;) {
        wchar_t buf[512];
        DWORD len = 512;
        FILETIME ft;
        LONG code = RegEnumKeyExW(key, index, buf, &len, NULL, NULL, NULL, &ft);
        if (code != ERROR_SUCCESS) break;
        char *u = cdc_wide_to_utf8(buf);
        if (u && cdc_vec_push(&names, &u) != CDC_OK) free(u);
        index++;
    }
    *out = (char **)names.data;
    *out_n = names.len;
    names.data = NULL;
    names.len = names.cap = 0;
    cdc_vec_free(&names);
    return CDC_OK;
}

/* 删除前先用 reg.exe 导出备份到 logs\backups\reg\{label}_{stamp}.reg。 */
static int backup_key(const char *hive, const char *path, const char *label) {
    char *dir = cdc_logs_dir();
    if (!dir) return 0;
    cdc_buf_t d;
    cdc_buf_init(&d);
    cdc_buf_append_str(&d, dir);
    free(dir);
    cdc_buf_append_str(&d, "\\backups\\reg");
    char *dir_s = cdc_buf_take_cstr(&d);
    if (!dir_s) return 0;
    /* 创建目录（逐级）。 */
    wchar_t *wd = cdc_utf8_to_wide(dir_s);
    if (wd) {
        for (wchar_t *p = wd + 1; *p; p++) {
            if (*p == L'\\') {
                *p = 0;
                CreateDirectoryW(wd, NULL);
                *p = L'\\';
            }
        }
        CreateDirectoryW(wd, NULL);
        free(wd);
    }
    char *stamp = cdc_stamp();
    cdc_buf_t f;
    cdc_buf_init(&f);
    cdc_buf_append_str(&f, dir_s);
    cdc_buf_append_str(&f, "\\");
    cdc_buf_append_str(&f, label);
    cdc_buf_append_str(&f, "_");
    cdc_buf_append_str(&f, stamp ? stamp : "");
    cdc_buf_append_str(&f, ".reg");
    free(stamp);
    char *file = cdc_buf_take_cstr(&f);
    free(dir_s);
    if (!file) return 0;
    char *reg_exe = cdc_system32_path("reg.exe");
    cdc_buf_t args;
    cdc_buf_init(&args);
    cdc_buf_append_str(&args, "export \"");
    cdc_buf_append_str(&args, hive);
    cdc_buf_append_str(&args, "\\");
    cdc_buf_append_str(&args, path);
    cdc_buf_append_str(&args, "\" \"");
    cdc_buf_append_str(&args, file);
    cdc_buf_append_str(&args, "\" /y");
    char *args_s = cdc_buf_take_cstr(&args);
    long code = -1;
    int rc = cdc_run_command(reg_exe ? reg_exe : "reg.exe", args_s, 0, NULL,
                             NULL, &code, NULL);
    free(reg_exe);
    free(args_s);
    free(file);
    return rc == CDC_OK && code == 0;
}

static int64_t clear_values(const char *hive, const char *path,
                            const char *label) {
    if (!backup_key(hive, path, label)) return 0;
    cdc_hkey_t key = NULL;
    if (reg_open(hive_root(hive), path, KEY_ALL_ACCESS, &key) != CDC_OK)
        return 0;
    char **names = NULL;
    size_t n = 0;
    reg_list_values(key, &names, &n);
    int64_t deleted = 0;
    for (size_t i = 0; i < n; i++) {
        wchar_t *w = cdc_utf8_to_wide(names[i]);
        if (w) {
            if (RegDeleteValueW(key, w) == ERROR_SUCCESS) deleted++;
            free(w);
        }
    }
    cdc_strvec_free(names, n);
    RegCloseKey(key);
    return deleted;
}

static int count_values(const char *hive, const char *path, int64_t *out) {
    cdc_hkey_t key = NULL;
    if (reg_open(hive_root(hive), path, KEY_READ, &key) != CDC_OK) return 0;
    char **names = NULL;
    size_t n = 0;
    reg_list_values(key, &names, &n);
    *out = (int64_t)n;
    cdc_strvec_free(names, n);
    RegCloseKey(key);
    return 1;
}

static int count_subkeys(const char *hive, const char *path, int64_t *out) {
    cdc_hkey_t key = NULL;
    if (reg_open(hive_root(hive), path, KEY_READ, &key) != CDC_OK) return 0;
    char **names = NULL;
    size_t n = 0;
    reg_list_subkeys(key, &names, &n);
    *out = (int64_t)n;
    cdc_strvec_free(names, n);
    RegCloseKey(key);
    return 1;
}
#else
static int64_t clear_values(const char *hive, const char *path,
                            const char *label) {
    (void)hive;
    (void)path;
    (void)label;
    return 0;
}

static int count_values(const char *hive, const char *path, int64_t *out) {
    (void)hive;
    (void)path;
    (void)out;
    return 0;
}

static int count_subkeys(const char *hive, const char *path, int64_t *out) {
    (void)hive;
    (void)path;
    (void)out;
    return 0;
}
#endif

static int recent_docs_paths(char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    cdc_vec_t paths;
    if (cdc_vec_init(&paths, sizeof(char *)) != CDC_OK) return CDC_ERR_NOMEM;
    char *base = strdup(RECENT_DOCS);
    if (base && cdc_vec_push(&paths, &base) != CDC_OK) free(base);
#ifdef _WIN32
    cdc_hkey_t key = NULL;
    if (reg_open(HKEY_CURRENT_USER, RECENT_DOCS, KEY_READ, &key) == CDC_OK) {
        char **subs = NULL;
        size_t n = 0;
        reg_list_subkeys(key, &subs, &n);
        for (size_t i = 0; i < n; i++) {
            cdc_buf_t b;
            cdc_buf_init(&b);
            cdc_buf_append_str(&b, RECENT_DOCS);
            cdc_buf_append_str(&b, "\\");
            cdc_buf_append_str(&b, subs[i]);
            char *p = cdc_buf_take_cstr(&b);
            if (p && cdc_vec_push(&paths, &p) != CDC_OK) free(p);
        }
        cdc_strvec_free(subs, n);
        RegCloseKey(key);
    }
#endif
    *out = (char **)paths.data;
    *out_n = paths.len;
    paths.data = NULL;
    paths.len = paths.cap = 0;
    cdc_vec_free(&paths);
    return CDC_OK;
}

static int userassist_count_keys(char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    cdc_vec_t paths;
    if (cdc_vec_init(&paths, sizeof(char *)) != CDC_OK) return CDC_ERR_NOMEM;
#ifdef _WIN32
    cdc_hkey_t key = NULL;
    if (reg_open(HKEY_CURRENT_USER, USER_ASSIST, KEY_READ, &key) == CDC_OK) {
        char **guids = NULL;
        size_t n = 0;
        reg_list_subkeys(key, &guids, &n);
        for (size_t i = 0; i < n; i++) {
            cdc_buf_t b;
            cdc_buf_init(&b);
            cdc_buf_append_str(&b, USER_ASSIST);
            cdc_buf_append_str(&b, "\\");
            cdc_buf_append_str(&b, guids[i]);
            cdc_buf_append_str(&b, "\\Count");
            char *p = cdc_buf_take_cstr(&b);
            if (p && cdc_vec_push(&paths, &p) != CDC_OK) free(p);
        }
        cdc_strvec_free(guids, n);
        RegCloseKey(key);
    }
#endif
    *out = (char **)paths.data;
    *out_n = paths.len;
    paths.data = NULL;
    paths.len = paths.cap = 0;
    cdc_vec_free(&paths);
    return CDC_OK;
}

/* ---------------- 最近文件 / 影音 ---------------- */

static char *recent_dir(void) {
    char *roaming = cdc_getenv_utf8("APPDATA");
    char *p = path_join2(roaming ? roaming : "", "\\Microsoft\\Windows\\Recent");
    free(roaming);
    return p;
}

static int recent_files(char ***out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    char *dir = recent_dir();
    char **names = NULL;
    size_t nnames = 0;
    list_dir_names(dir, &names, &nnames);
    cdc_vec_t found;
    if (cdc_vec_init(&found, sizeof(char *)) != CDC_OK) {
        free(dir);
        cdc_strvec_free(names, nnames);
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < nnames; i++) {
        char *full = path_join2(dir, names[i]);
        if (is_file_utf8(full)) {
            if (cdc_vec_push(&found, &full) != CDC_OK) free(full);
        } else {
            free(full);
        }
    }
    free(dir);
    cdc_strvec_free(names, nnames);
    *out = (char **)found.data;
    *out_n = found.len;
    found.data = NULL;
    found.len = found.cap = 0;
    cdc_vec_free(&found);
    return CDC_OK;
}

static int contains_any(const char *haystack, const char **words, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (strstr(haystack, words[i])) return 1;
    }
    return 0;
}

/* 最近打开的快捷方式：取 file_stem 再取扩展名，与 RECENT_MEDIA_EXT 比对。
 * 如 "movie.mp4.lnk" → stem "movie.mp4" → ext ".mp4"。 */
static int recent_is_media(const char *path) {
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    const char *slash = strrchr(base, '/');
    base = slash ? slash + 1 : base;
    /* 去掉最后一个扩展名（.lnk）。 */
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return 0;
    size_t stem_len = (size_t)(dot - base);
    /* stem 的扩展名。 */
    const char *dot2 = NULL;
    for (size_t i = 0; i < stem_len; i++) {
        if (base[i] == '.') dot2 = base + i;
    }
    if (!dot2) return 0;
    char ext[16];
    size_t elen = stem_len - (size_t)(dot2 - base);
    if (elen >= sizeof(ext)) return 0;
    for (size_t i = 0; i < elen; i++) {
        char ch = dot2[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + ('a' - 'A'));
        ext[i] = ch;
    }
    ext[elen] = '\0';
    for (size_t i = 0; i < sizeof(RECENT_MEDIA_EXT) / sizeof(RECENT_MEDIA_EXT[0]); i++) {
        if (strcmp(ext, RECENT_MEDIA_EXT[i]) == 0) return 1;
    }
    return 0;
}

static int utf8_valid(const uint8_t *s, size_t n) {
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
            if (cp == 0) return 0; /* 过短编码 */
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

/* VLC 最近播放：%APPDATA%\vlc\vlc-qt-interface.ini 里含 file:/// 的行数。
 * 文件不是合法 UTF-8 时 Rust 的 read_to_string 失败 → 计 0。 */
static int64_t vlc_recent_count(void) {
    char *roaming = cdc_getenv_utf8("APPDATA");
    char *dir = path_join2(roaming ? roaming : "", "\\vlc");
    free(roaming);
    char *file = path_join2(dir, "vlc-qt-interface.ini");
    free(dir);
    FILE *f = cdc_fopen_utf8(file, "rb");
    free(file);
    if (!f) return 0;
    cdc_buf_t content;
    cdc_buf_init(&content);
    uint8_t chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
        cdc_buf_append(&content, chunk, got);
    fclose(f);
    if (!utf8_valid(content.data, content.len)) {
        cdc_buf_free(&content);
        return 0;
    }
    int64_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= content.len; i++) {
        if (i == content.len || content.data[i] == '\n') {
            size_t line_len = i - start;
            if (line_len > 0 && content.data[start + line_len - 1] == '\r')
                line_len--;
            /* 行内小写化后找 "file:///"。 */
            int found = 0;
            const char *needle = "file:///";
            size_t nlen = strlen(needle);
            if (line_len >= nlen) {
                for (size_t k = 0; k + nlen <= line_len && !found; k++) {
                    size_t m = 0;
                    for (; m < nlen; m++) {
                        char a = (char)content.data[start + k + m];
                        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
                        if (a != needle[m]) break;
                    }
                    if (m == nlen) found = 1;
                }
            }
            if (found) count++;
            start = i + 1;
        }
    }
    cdc_buf_free(&content);
    return count;
}

/* ---------------- PrivacyItem ---------------- */

void cdc_privacy_item_free(cdc_privacy_item_t *item) {
    if (!item) return;
    free(item->id);
    free(item->section);
    free(item->name);
    cdc_strvec_free(item->files, item->nfiles);
    free(item->action);
    free(item->note);
}

void cdc_privacy_items_free(cdc_privacy_item_t *items, size_t n) {
    if (!items) return;
    for (size_t i = 0; i < n; i++) cdc_privacy_item_free(&items[i]);
    free(items);
}

void cdc_privacy_report_free(cdc_privacy_report_t *r) {
    if (!r) return;
    cdc_strvec_free(r->failed, r->nfailed);
    r->failed = NULL;
    r->nfailed = 0;
}

static int push_item(cdc_vec_t *items, const char *id, const char *section,
                     const char *name, int64_t count, int available,
                     char **files, size_t nfiles, const char *action,
                     const char *note) {
    cdc_privacy_item_t it;
    memset(&it, 0, sizeof(it));
    it.id = strdup(id);
    it.section = strdup(section);
    it.name = strdup(name);
    it.count = count;
    it.count_available = available;
    it.files = files;
    it.nfiles = nfiles;
    it.action = strdup(action);
    it.note = strdup(note ? note : "");
    if (!it.id || !it.section || !it.name || !it.action || !it.note) {
        cdc_privacy_item_free(&it);
        return CDC_ERR_NOMEM;
    }
    if (cdc_vec_push(items, &it) != CDC_OK) {
        cdc_privacy_item_free(&it);
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

static void report_fail(cdc_privacy_report_t *r, const char *msg) {
    char *dup = strdup(msg);
    if (!dup) return;
    char **nr = (char **)realloc(r->failed, (r->nfailed + 1) * sizeof(char *));
    if (!nr) {
        free(dup);
        return;
    }
    r->failed = nr;
    r->failed[r->nfailed++] = dup;
}

static void report_fail_fmt(cdc_privacy_report_t *r, const char *fmt,
                            const char *arg) {
    char buf[1024];
    snprintf(buf, sizeof(buf), fmt, arg);
    report_fail(r, buf);
}

/* ---------------- scan ---------------- */

int cdc_privacy_scan(cdc_privacy_item_t **out, size_t *out_n, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_n = 0;
    cdc_vec_t items;
    if (cdc_vec_init(&items, sizeof(cdc_privacy_item_t)) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }

    size_t nspecs = 0;
    browser_spec_t *specs = browser_specs(&nspecs);

    /* 每浏览器的 history / download / shop 三项（先收集，最后拼顺序：
     * Rust 版是 high（风险项在前）+ history + download + shop）。 */
    cdc_vec_t hist_items, down_items, shop_items;
    cdc_vec_init(&hist_items, sizeof(cdc_privacy_item_t));
    cdc_vec_init(&down_items, sizeof(cdc_privacy_item_t));
    cdc_vec_init(&shop_items, sizeof(cdc_privacy_item_t));

    int64_t money_total = 0, medical_total = 0, shop_total = 0;
    int money_available = 0, medical_available = 0, shop_available = 0;
    int64_t cookie_total = 0;
    int cookie_available = 0;
    cdc_vec_t cookie_files_all;
    cdc_vec_init(&cookie_files_all, sizeof(char *));

    for (size_t si = 0; si < nspecs; si++) {
        int64_t urls = 0, downloads = 0, shop = 0, money = 0, medical = 0;
        int urls_available = 0;
        char **profs = NULL;
        size_t nprofs = 0;
        profiles_of(&specs[si], &profs, &nprofs);
        for (size_t pi = 0; pi < nprofs; pi++) {
            char *hf = history_file(&specs[si], profs[pi]);
            if (hf) {
                history_stats_t st = history_stats(hf);
                urls += st.urls;
                downloads += st.downloads;
                shop += st.shop;
                money += st.money;
                medical += st.medical;
                urls_available |= st.urls_available;
                free(hf);
            }
            char **cks = NULL;
            size_t ncks = 0;
            cookie_files(&specs[si], profs[pi], &cks, &ncks);
            for (size_t ci = 0; ci < ncks; ci++) {
                int64_t cnt = cookie_count(cks[ci]);
                if (cnt > 0 || cookie_available) cookie_available = 1;
                cookie_total += cnt;
                char *dup = strdup(cks[ci]);
                if (dup && cdc_vec_push(&cookie_files_all, &dup) != CDC_OK)
                    free(dup);
            }
            cdc_strvec_free(cks, ncks);
        }
        cdc_strvec_free(profs, nprofs);
        money_total += money;
        medical_total += medical;
        shop_total += shop;
        money_available |= money > 0;
        medical_available |= medical > 0;
        shop_available |= urls_available;
        char idbuf[64];
        snprintf(idbuf, sizeof(idbuf), "hist_%s", specs[si].id);
        push_item(&hist_items, idbuf, "history", specs[si].name, urls,
                  urls_available, NULL, 0, "sql", "");
        snprintf(idbuf, sizeof(idbuf), "down_%s", specs[si].id);
        push_item(&down_items, idbuf, "download", specs[si].name, downloads,
                  urls_available, NULL, 0, "sql",
                  "只删浏览记录，不删除已下载的文件");
        snprintf(idbuf, sizeof(idbuf), "shop_%s", specs[si].id);
        push_item(&shop_items, idbuf, "shop", specs[si].name, shop,
                  urls_available, NULL, 0, "sql", "");
    }
    specs_free(specs, nspecs);

    char **recent = NULL;
    size_t nrecent = 0;
    recent_files(&recent, &nrecent);

    /* 敏感文件名（最近打开的文件名含敏感词）。 */
    cdc_vec_t sensitive;
    cdc_vec_init(&sensitive, sizeof(char *));
    for (size_t i = 0; i < nrecent; i++) {
        const char *base = strrchr(recent[i], '\\');
        base = base ? base + 1 : recent[i];
        const char *slash = strrchr(base, '/');
        base = slash ? slash + 1 : base;
        if (contains_any(base, SENSITIVE_WORDS,
                         sizeof(SENSITIVE_WORDS) / sizeof(SENSITIVE_WORDS[0]))) {
            char *dup = strdup(recent[i]);
            if (dup && cdc_vec_push(&sensitive, &dup) != CDC_OK) free(dup);
        }
    }

    push_item(&items, "risk_file", "high", "文件敏感隐私",
              (int64_t)sensitive.len, nrecent > 0, (char **)sensitive.data,
              sensitive.len, "files",
              "最近打开的文件名里含身份证、密码、合同等字样");
    sensitive.data = NULL;
    sensitive.len = sensitive.cap = 0;
    cdc_vec_free(&sensitive);
    push_item(&items, "risk_money", "high", "财产安全隐私", money_total,
              money_available, NULL, 0, "sql", "");
    push_item(&items, "risk_shop", "high", "网络购物痕迹", shop_total,
              shop_available, NULL, 0, "sql", "");
    push_item(&items, "risk_medical", "high", "医疗信息隐私", medical_total,
              medical_available, NULL, 0, "sql", "");
    push_item(&items, "cookies", "high", "Cookie", cookie_total,
              cookie_available, (char **)cookie_files_all.data,
              cookie_files_all.len, "sql", "清理后部分网站需要重新登录");
    cookie_files_all.data = NULL;
    cookie_files_all.len = cookie_files_all.cap = 0;
    cdc_vec_free(&cookie_files_all);

    /* history / download / shop 拼在 high 之后。 */
    for (size_t i = 0; i < hist_items.len; i++) {
        cdc_privacy_item_t it = ((cdc_privacy_item_t *)hist_items.data)[i];
        if (cdc_vec_push(&items, &it) != CDC_OK) cdc_privacy_item_free(&it);
    }
    hist_items.data = NULL;
    hist_items.len = hist_items.cap = 0;
    cdc_vec_free(&hist_items);
    for (size_t i = 0; i < down_items.len; i++) {
        cdc_privacy_item_t it = ((cdc_privacy_item_t *)down_items.data)[i];
        if (cdc_vec_push(&items, &it) != CDC_OK) cdc_privacy_item_free(&it);
    }
    down_items.data = NULL;
    down_items.len = down_items.cap = 0;
    cdc_vec_free(&down_items);
    for (size_t i = 0; i < shop_items.len; i++) {
        cdc_privacy_item_t it = ((cdc_privacy_item_t *)shop_items.data)[i];
        if (cdc_vec_push(&items, &it) != CDC_OK) cdc_privacy_item_free(&it);
    }
    shop_items.data = NULL;
    shop_items.len = shop_items.cap = 0;
    cdc_vec_free(&shop_items);

    /* 电脑：文件访问记录 + USB（只统计）。 */
    char **rd_paths = NULL;
    size_t nrd = 0;
    recent_docs_paths(&rd_paths, &nrd);
    int64_t recent_count = 0;
    int recent_available = 0;
    for (size_t i = 0; i < nrd; i++) {
        int64_t v = 0;
        if (count_values("HKCU", rd_paths[i], &v)) {
            recent_available = 1;
            recent_count += v;
        }
    }
    cdc_strvec_free(rd_paths, nrd);
    push_item(&items, "reg_file", "computer", "文件访问记录", recent_count,
              recent_available, NULL, 0, "registry",
              "最近文档列表，删除前会导出备份");
    int64_t usb = 0;
    count_subkeys("HKLM", USB_STORAGE, &usb);
    push_item(&items, "reg_usb", "computer", "USB 使用痕迹", usb, usb > 0,
              NULL, 0, "count",
              "只统计；清理 USB 记录需要改系统设备记录，Rust 版暂不执行");

    /* 软件：UserAssist + Prefetch .pf 计数（Prefetch 文件保留）。 */
    char **ua_keys = NULL;
    size_t nua = 0;
    userassist_count_keys(&ua_keys, &nua);
    int64_t userassist = 0;
    for (size_t i = 0; i < nua; i++) {
        int64_t v = 0;
        if (count_values("HKCU", ua_keys[i], &v)) userassist += v;
    }
    cdc_strvec_free(ua_keys, nua);
    char *sysroot = cdc_getenv_utf8("SystemRoot");
    char *prefetch = path_join2(sysroot ? sysroot : "", "\\Prefetch");
    free(sysroot);
    char **pf_names = NULL;
    size_t npf = 0;
    list_dir_names(prefetch, &pf_names, &npf);
    free(prefetch);
    int64_t prefetch_count = 0;
    for (size_t i = 0; i < npf; i++) {
        size_t n = strlen(pf_names[i]);
        if (n >= 3) {
            const char *ext = pf_names[i] + n - 3;
            if ((ext[0] == '.' ) &&
                (ext[1] == 'p' || ext[1] == 'P') &&
                (ext[2] == 'f' || ext[2] == 'F'))
                prefetch_count++;
        }
    }
    cdc_strvec_free(pf_names, npf);
    push_item(&items, "software", "software", "软件使用痕迹",
              userassist + prefetch_count, 1, NULL, 0, "userassist",
              "清理 UserAssist 记录；Prefetch 文件保留");

    /* 影音：最近打开的音视频快捷方式 + WMP + VLC。 */
    cdc_vec_t media_files;
    cdc_vec_init(&media_files, sizeof(char *));
    for (size_t i = 0; i < nrecent; i++) {
        if (recent_is_media(recent[i])) {
            char *dup = strdup(recent[i]);
            if (dup && cdc_vec_push(&media_files, &dup) != CDC_OK) free(dup);
        }
    }
    int64_t wmp = 0;
    count_values("HKCU", WMP_RECENT, &wmp);
    int64_t vlc = vlc_recent_count();
    push_item(&items, "media", "media", "影音播放痕迹",
              (int64_t)media_files.len + wmp + vlc, 1,
              (char **)media_files.data, media_files.len, "media",
              "清理最近播放列表和最近打开的音视频快捷方式");
    media_files.data = NULL;
    media_files.len = media_files.cap = 0;
    cdc_vec_free(&media_files);

    cdc_strvec_free(recent, nrecent);

    *out = (cdc_privacy_item_t *)items.data;
    *out_n = items.len;
    items.data = NULL;
    items.len = items.cap = 0;
    cdc_vec_free(&items);
    return CDC_OK;
}

/* ---------------- clean ---------------- */

typedef struct {
    char *path;     /* 数据库文件 */
    char **stmts;   /* 清理 SQL */
    size_t nstmts;
} sql_job_t;

static void sql_jobs_free(sql_job_t *jobs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(jobs[i].path);
        cdc_strvec_free(jobs[i].stmts, jobs[i].nstmts);
    }
    free(jobs);
}

static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int cdc_privacy_clean(const char **ids, size_t nids,
                      cdc_privacy_report_t *out, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "out is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    cdc_privacy_item_t *scanned = NULL;
    size_t nscanned = 0;
    if (cdc_privacy_scan(&scanned, &nscanned, NULL) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "隐私扫描失败");
        return CDC_ERR_IO;
    }
    /* 选中的项（action == "count" 的跳过）。 */
    cdc_vec_t files; /* char* */
    cdc_vec_init(&files, sizeof(char *));
    cdc_vec_t sql_jobs; /* sql_job_t */
    cdc_vec_init(&sql_jobs, sizeof(sql_job_t));
    cdc_vec_t registry_jobs; /* char*（id） */
    cdc_vec_init(&registry_jobs, sizeof(char *));
    int userassist = 0, media = 0;

    for (size_t i = 0; i < nscanned; i++) {
        cdc_privacy_item_t *item = &scanned[i];
        int selected = 0;
        for (size_t k = 0; k < nids; k++) {
            if (strcmp(ids[k], item->id) == 0) {
                selected = 1;
                break;
            }
        }
        if (!selected || strcmp(item->action, "count") == 0) continue;
        if (strcmp(item->action, "sql") == 0) {
            char **stmts = NULL;
            size_t nstmts = 0;
            const char *kind = NULL;
            if (strcmp(item->id, "risk_money") == 0) {
                kind = "money";
            } else if (strcmp(item->id, "risk_shop") == 0) {
                kind = "shop";
            } else if (strcmp(item->id, "risk_medical") == 0) {
                kind = "medical";
            } else if (strncmp(item->id, "hist_", 5) == 0) {
                kind = "history";
            } else if (strncmp(item->id, "down_", 5) == 0) {
                kind = "downloads";
            } else if (strncmp(item->id, "shop_", 5) == 0) {
                kind = "shop";
            } else if (strcmp(item->id, "cookies") == 0) {
                kind = "cookies";
            }
            if (!kind) continue;
            sql_statements(kind, &stmts, &nstmts);
            if (nstmts == 0) {
                cdc_strvec_free(stmts, nstmts);
                continue;
            }
            size_t nspecs = 0;
            browser_spec_t *specs = browser_specs(&nspecs);
            for (size_t si = 0; si < nspecs; si++) {
                if (strcmp(item->id, "cookies") != 0 &&
                    strcmp(item->id, "risk_money") != 0 &&
                    strcmp(item->id, "risk_shop") != 0 &&
                    strcmp(item->id, "risk_medical") != 0 &&
                    !ends_with(item->id, specs[si].id))
                    continue;
                char **profs = NULL;
                size_t nprofs = 0;
                profiles_of(&specs[si], &profs, &nprofs);
                for (size_t pi = 0; pi < nprofs; pi++) {
                    if (strcmp(item->id, "cookies") == 0) {
                        char **cks = NULL;
                        size_t ncks = 0;
                        cookie_files(&specs[si], profs[pi], &cks, &ncks);
                        for (size_t ci = 0; ci < ncks; ci++) {
                            sql_job_t job;
                            job.path = strdup(cks[ci]);
                            job.stmts = NULL;
                            job.nstmts = 0;
                            for (size_t s2 = 0; s2 < nstmts; s2++) {
                                char *dup = strdup(stmts[s2]);
                                if (!dup) continue;
                                char **nr = (char **)realloc(
                                    job.stmts,
                                    (job.nstmts + 1) * sizeof(char *));
                                if (!nr) {
                                    free(dup);
                                    break;
                                }
                                job.stmts = nr;
                                job.stmts[job.nstmts++] = dup;
                            }
                            if (job.path &&
                                cdc_vec_push(&sql_jobs, &job) != CDC_OK) {
                                free(job.path);
                                cdc_strvec_free(job.stmts, job.nstmts);
                            }
                        }
                        cdc_strvec_free(cks, ncks);
                        continue;
                    }
                    char *hf = history_file(&specs[si], profs[pi]);
                    if (hf) {
                        sql_job_t job;
                        job.path = hf;
                        job.stmts = NULL;
                        job.nstmts = 0;
                        for (size_t s2 = 0; s2 < nstmts; s2++) {
                            char *dup = strdup(stmts[s2]);
                            if (!dup) continue;
                            char **nr = (char **)realloc(
                                job.stmts,
                                (job.nstmts + 1) * sizeof(char *));
                            if (!nr) {
                                free(dup);
                                break;
                            }
                            job.stmts = nr;
                            job.stmts[job.nstmts++] = dup;
                        }
                        if (cdc_vec_push(&sql_jobs, &job) != CDC_OK) {
                            free(job.path);
                            cdc_strvec_free(job.stmts, job.nstmts);
                        }
                    }
                }
                cdc_strvec_free(profs, nprofs);
            }
            specs_free(specs, nspecs);
            cdc_strvec_free(stmts, nstmts);
        } else if (strcmp(item->action, "registry") == 0) {
            char *dup = strdup(item->id);
            if (dup && cdc_vec_push(&registry_jobs, &dup) != CDC_OK) free(dup);
        } else if (strcmp(item->action, "userassist") == 0) {
            userassist = 1;
        } else if (strcmp(item->action, "media") == 0) {
            for (size_t f = 0; f < item->nfiles; f++) {
                char *dup = strdup(item->files[f]);
                if (dup && cdc_vec_push(&files, &dup) != CDC_OK) free(dup);
            }
            media = 1;
        } else {
            for (size_t f = 0; f < item->nfiles; f++) {
                char *dup = strdup(item->files[f]);
                if (dup && cdc_vec_push(&files, &dup) != CDC_OK) free(dup);
            }
        }
    }
    cdc_privacy_items_free(scanned, nscanned);

    /* 去重：去空、去密码库、排序、去重。 */
    cdc_vec_t kept;
    cdc_vec_init(&kept, sizeof(char *));
    for (size_t i = 0; i < files.len; i++) {
        char *p = ((char **)files.data)[i];
        int empty = 1;
        for (const char *q = p; *q; q++) {
            if (*q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') {
                empty = 0;
                break;
            }
        }
        if (empty || cdc_is_password_store(p)) {
            free(p);
            continue;
        }
        if (cdc_vec_push(&kept, &p) != CDC_OK) free(p);
    }
    files.data = NULL;
    files.len = files.cap = 0;
    cdc_vec_free(&files);
    if (kept.len > 1)
        qsort(kept.data, kept.len, sizeof(char *), cmp_str);
    size_t uniq = 0;
    for (size_t i = 0; i < kept.len; i++) {
        if (uniq > 0 &&
            strcmp(((char **)kept.data)[uniq - 1],
                   ((char **)kept.data)[i]) == 0) {
            free(((char **)kept.data)[i]);
            continue;
        }
        ((char **)kept.data)[uniq++] = ((char **)kept.data)[i];
    }
    kept.len = uniq;

    uint64_t total_units = (uint64_t)kept.len + (uint64_t)sql_jobs.len +
                           (uint64_t)registry_jobs.len + (userassist ? 1 : 0) +
                           (media ? 1 : 0);
    set_total(total_units);
    if (cancelled()) {
        report_fail(out, "已取消");
        goto done;
    }
    if (kept.len > 0) {
        char **failed = NULL;
        size_t nfailed = 0;
        cdc_send_to_recycle((const char **)kept.data, kept.len, &failed,
                            &nfailed, NULL);
        for (size_t i = 0; i < kept.len; i++) {
            add_done();
            const char *p = ((char **)kept.data)[i];
            int is_failed = 0;
            for (size_t f = 0; f < nfailed; f++) {
                if (strcmp(failed[f], p) == 0) {
                    is_failed = 1;
                    break;
                }
            }
            if (is_failed || is_file_utf8(p)) {
                report_fail_fmt(out, "文件未能移入回收站：%s", p);
            } else {
                out->files++;
            }
        }
        cdc_strvec_free(failed, nfailed);
    }
    if (cancelled()) {
        report_fail(out, "已取消");
        goto done;
    }
    for (size_t i = 0; i < sql_jobs.len; i++) {
        add_done();
        sql_job_t *job = &((sql_job_t *)sql_jobs.data)[i];
        if (apply_sql(job->path, (const char **)job->stmts, job->nstmts)) {
            out->sql++;
        } else {
            report_fail_fmt(out, "数据库清理失败或数据源不可用：%s", job->path);
        }
    }
    if (cancelled()) {
        report_fail(out, "已取消");
        goto done;
    }
    for (size_t i = 0; i < registry_jobs.len; i++) {
        add_done();
        const char *job = ((char **)registry_jobs.data)[i];
        if (strcmp(job, "reg_file") == 0) {
            char **paths = NULL;
            size_t n = 0;
            recent_docs_paths(&paths, &n);
            int64_t deleted = 0;
            for (size_t k = 0; k < n; k++)
                deleted += clear_values("HKCU", paths[k], "recentdocs");
            cdc_strvec_free(paths, n);
            out->registry += deleted;
            if (deleted == 0)
                report_fail(out, "文件访问记录清理未完成：没有记录被清除");
        }
    }
    if (userassist) {
        add_done();
        char **keys = NULL;
        size_t n = 0;
        userassist_count_keys(&keys, &n);
        int64_t deleted = 0;
        for (size_t k = 0; k < n; k++)
            deleted += clear_values("HKCU", keys[k], "userassist");
        cdc_strvec_free(keys, n);
        out->registry += deleted;
        if (deleted == 0)
            report_fail(out, "软件使用痕迹清理未完成：没有记录被清除");
    }
    if (media) {
        add_done();
        out->registry += clear_values("HKCU", WMP_RECENT, "wmp");
    }
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "完成：文件 %lld 个，数据库 %lld 个，注册表记录 %lld 条",
                 (long long)out->files, (long long)out->sql,
                 (long long)out->registry);
        const char *lines[3] = { "开始清理", "清理：隐私痕迹", buf };
        cdc_write_log(lines, 3, NULL, NULL);
    }
done:
    for (size_t i = 0; i < kept.len; i++) free(((char **)kept.data)[i]);
    cdc_vec_free(&kept);
    sql_jobs_free((sql_job_t *)sql_jobs.data, sql_jobs.len);
    sql_jobs.data = NULL;
    sql_jobs.len = sql_jobs.cap = 0;
    cdc_vec_free(&sql_jobs);
    cdc_strvec_free((char **)registry_jobs.data, registry_jobs.len);
    registry_jobs.data = NULL;
    registry_jobs.len = registry_jobs.cap = 0;
    cdc_vec_free(&registry_jobs);
    return CDC_OK;
}
