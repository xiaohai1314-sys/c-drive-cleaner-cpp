/* rules.c — 移植自 Rust 版 desktop/src/rules.rs + main.rs 的 Rule/all_rules。
 *
 * 与 Rust 版的差异（有意为之，见头文件注释）：
 *   - config 的第三候选（编译期 CARGO_MANIFEST_DIR）没有等价物：C 版只找
 *     exe 旁与 exe 下 resources/ 里的 rules_update.json。
 *   - 下载走 WinHTTP（仅 Windows）；Linux 下 update() 直接报错。
 */
#include "rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "logs.h"
#include "rules_data.h"
#include "safety.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include "blake3/blake3.h"
#else
#include <unistd.h>
#endif

static const char *const TIERS[] = { "conservative", "standard", "deep" };
static const char *const LIBRARY_PARTS[] = {
    "documents", "desktop", "downloads", "my documents", "文档", "桌面",
};

void cdc_rule_free_members(cdc_rule_t *r) {
    if (!r) return;
    free(r->key);
    free(r->name);
    free(r->tier);
    free(r->kind);
    free(r->describe);
    free(r->warning);
    free(r->note);
    cdc_strvec_free(r->paths, r->npaths);
    cdc_strvec_free(r->glob_dirs, r->nglob_dirs);
    cdc_strvec_free(r->patterns, r->npatterns);
    cdc_strvec_free(r->exclude, r->nexclude);
    memset(r, 0, sizeof(*r));
}

void cdc_rules_free(cdc_rule_t *rules, size_t n) {
    if (!rules) return;
    for (size_t i = 0; i < n; i++) cdc_rule_free_members(&rules[i]);
    free(rules);
}

char *cdc_expand_env(const char *raw) {
    if (!raw) return strdup("");
    cdc_buf_t out;
    cdc_buf_init(&out);
    const char *p = raw;
    while (*p) {
        if (*p == '%') {
            const char *close = strchr(p + 1, '%');
            if (close) {
                char *name = strndup(p + 1, (size_t)(close - (p + 1)));
                char *value = name ? cdc_getenv_utf8(name) : NULL;
                free(name);
                if (value) {
                    cdc_buf_append_str(&out, value);
                    free(value);
                    p = close + 1;
                    continue;
                }
            }
        }
        cdc_buf_push(&out, (uint8_t)*p);
        p++;
    }
    char *s = cdc_buf_take_cstr(&out);
    if (!s) {
        cdc_buf_free(&out);
        return strdup("");
    }
    return s;
}

/* ---- JSON 取值小工具（严格：类型错即整文件失败，与 serde 一致） ---- */

static char *json_string_dup(cJSON *obj, const char *key, int *bad) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return strdup("");
    if (!cJSON_IsString(v)) {
        *bad = 1;
        return NULL;
    }
    return strdup(v->valuestring ? v->valuestring : "");
}

static char **json_string_array(cJSON *obj, const char *key, size_t *out_n,
                                int *bad) {
    *out_n = 0;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return NULL;
    if (!cJSON_IsArray(v)) {
        *bad = 1;
        return NULL;
    }
    size_t n = (size_t)cJSON_GetArraySize(v);
    if (n == 0) return NULL;
    char **arr = (char **)calloc(n, sizeof(char *));
    if (!arr) {
        *bad = 1;
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(v, (int)i);
        if (!cJSON_IsString(e)) {
            cdc_strvec_free(arr, i);
            *bad = 1;
            return NULL;
        }
        arr[i] = strdup(e->valuestring ? e->valuestring : "");
        if (!arr[i]) {
            cdc_strvec_free(arr, i);
            *bad = 1;
            return NULL;
        }
    }
    *out_n = n;
    return arr;
}

static int json_bool(cJSON *obj, const char *key, int *bad) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return 0;
    if (!cJSON_IsBool(v)) {
        *bad = 1;
        return 0;
    }
    return cJSON_IsTrue(v) ? 1 : 0;
}

static uint64_t json_u64(cJSON *obj, const char *key, uint64_t def, int *bad) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return def;
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 ||
        v->valuedouble > 18446744073709551615.0) {
        *bad = 1;
        return def;
    }
    return (uint64_t)v->valuedouble;
}

static int parse_one_rule(cJSON *item, cdc_rule_t *out) {
    int bad = 0;
    memset(out, 0, sizeof(*out));
    out->key = json_string_dup(item, "key", &bad);
    out->name = json_string_dup(item, "name", &bad);
    out->tier = json_string_dup(item, "tier", &bad);
    out->kind = json_string_dup(item, "kind", &bad);
    out->describe = json_string_dup(item, "describe", &bad);
    out->warning = json_string_dup(item, "warning", &bad);
    out->note = json_string_dup(item, "note", &bad);
    out->paths = json_string_array(item, "paths", &out->npaths, &bad);
    out->glob_dirs =
        json_string_array(item, "glob_dirs", &out->nglob_dirs, &bad);
    out->patterns =
        json_string_array(item, "patterns", &out->npatterns, &bad);
    out->exclude = json_string_array(item, "exclude", &out->nexclude, &bad);
    out->min_age_minutes = json_u64(item, "min_age_minutes", 0, &bad);
    out->requires_confirm = json_bool(item, "requires_confirm", &bad);
    out->prune_dirs = json_bool(item, "prune_dirs", &bad);
    if (bad) {
        cdc_rule_free_members(out);
        return CDC_ERR_INVALID_ARG;
    }
    return CDC_OK;
}

int cdc_rules_parse_text(const char *text, cdc_rule_t **out_rules,
                         size_t *out_n, uint64_t *out_version,
                         cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out_rules || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out_rules = NULL;
    *out_n = 0;
    if (out_version) *out_version = 0;
    cJSON *root = cJSON_Parse(text ? text : "");
    if (!root) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则格式不正确");
        return CDC_ERR_INVALID_ARG;
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (out_version && cJSON_IsNumber(version) && version->valuedouble >= 0)
        *out_version = (uint64_t)version->valuedouble;
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(root, "categories");
    if (!cJSON_IsArray(cats)) {
        cJSON_Delete(root);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则文件缺少 categories");
        return CDC_ERR_INVALID_ARG;
    }
    size_t n = (size_t)cJSON_GetArraySize(cats);
    cdc_rule_t *rules = NULL;
    if (n > 0) {
        rules = (cdc_rule_t *)calloc(n, sizeof(cdc_rule_t));
        if (!rules) {
            cJSON_Delete(root);
            if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
            return CDC_ERR_NOMEM;
        }
    }
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(cats, (int)i);
        if (!cJSON_IsObject(item) ||
            parse_one_rule(item, &rules[i]) != CDC_OK) {
            cdc_rules_free(rules, i);
            cJSON_Delete(root);
            if (err)
                cdc_err_set(err, CDC_ERR_INVALID_ARG,
                            "规则格式不正确（第 %zu 条）", i + 1);
            return CDC_ERR_INVALID_ARG;
        }
    }
    cJSON_Delete(root);
    *out_rules = rules;
    *out_n = n;
    return CDC_OK;
}

/* ---- 文件读写小工具（UTF-8 路径） ---- */

static char *read_file_utf8(const char *path, size_t *out_len) {
    FILE *f = cdc_fopen_utf8(path, "rb");
    if (!f) return NULL;
    cdc_buf_t b;
    cdc_buf_init(&b);
    uint8_t chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (cdc_buf_append(&b, chunk, got) != CDC_OK) break;
    }
    fclose(f);
    if (out_len) *out_len = b.len;
    char *s = cdc_buf_take_cstr(&b);
    if (!s) {
        cdc_buf_free(&b);
        return NULL;
    }
    return s;
}

#ifdef _WIN32
static int write_file_utf8(const char *path, const char *data, size_t len) {
    FILE *f = cdc_fopen_utf8(path, "wb");
    if (!f) return CDC_ERR_IO;
    size_t wrote = len > 0 ? fwrite(data, 1, len, f) : 0;
    int ok = (wrote == len);
    if (fclose(f) != 0) ok = 0;
    return ok ? CDC_OK : CDC_ERR_IO;
}
#endif /* _WIN32 — 只在 Windows-only 的 update() 里用 */

static int copy_file_utf8(const char *from, const char *to) {
    FILE *in = cdc_fopen_utf8(from, "rb");
    if (!in) return CDC_ERR_IO;
    FILE *out = cdc_fopen_utf8(to, "wb");
    if (!out) {
        fclose(in);
        return CDC_ERR_IO;
    }
    uint8_t chunk[65536];
    size_t got;
    int ok = 1;
    while ((got = fread(chunk, 1, sizeof(chunk), in)) > 0) {
        if (fwrite(chunk, 1, got, out) != got) {
            ok = 0;
            break;
        }
    }
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    return ok ? CDC_OK : CDC_ERR_IO;
}

static int file_exists_utf8(const char *path) {
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES &&
           (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return access(path, F_OK) == 0;
#endif
}

/* 原子替换：写临时文件后 rename（Windows 上 MoveFileExW 覆盖）。 */
#ifdef _WIN32 /* 只在 Windows-only 的 update() 里用 */
static int rename_replace_utf8(const char *from, const char *to) {
#ifdef _WIN32
    wchar_t *wf = cdc_utf8_to_wide(from);
    wchar_t *wt = cdc_utf8_to_wide(to);
    if (!wf || !wt) {
        free(wf);
        free(wt);
        return CDC_ERR_IO;
    }
    BOOL ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING);
    free(wf);
    free(wt);
    return ok ? CDC_OK : CDC_ERR_IO;
#else
    return rename(from, to) == 0 ? CDC_OK : CDC_ERR_IO;
#endif
}
#endif /* _WIN32 */

char *cdc_remote_rules_path(void) {
    char *app = cdc_app_data_dir();
    if (!app) app = strdup(".");
    if (!app) return NULL;
    size_t n = strlen(app) + 1 + strlen("rules_remote.json") + 1;
    char *out = (char *)malloc(n);
    if (!out) {
        free(app);
        return NULL;
    }
#ifdef _WIN32
    snprintf(out, n, "%s\\rules_remote.json", app);
#else
    snprintf(out, n, "%s/rules_remote.json", app);
#endif
    free(app);
    return out;
}

char *cdc_remote_rules_backup_path(void) {
    char *path = cdc_remote_rules_path();
    if (!path) return NULL;
    size_t n = strlen(path) + strlen(".previous") + 1;
    /* "rules_remote.json" → "rules_remote.previous.json" */
    char *dot = strrchr(path, '.');
    char *out = (char *)malloc(n);
    if (!out) {
        free(path);
        return NULL;
    }
    if (!dot) {
        snprintf(out, n, "%s.previous", path);
    } else {
        size_t head = (size_t)(dot - path);
        memcpy(out, path, head);
        snprintf(out + head, n - head, ".previous%s", dot);
    }
    free(path);
    return out;
}

int cdc_rules_load_all(cdc_rule_t **out_rules, size_t *out_n, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out_rules || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out_rules = NULL;
    *out_n = 0;
    cdc_rule_t *rules = NULL;
    size_t n = 0;
    cdc_err_t e2;
    if (cdc_rules_parse_text(CDC_RULES_JSON, &rules, &n, NULL, &e2) != CDC_OK) {
        if (err) *err = e2;
        return CDC_ERR_INVALID_ARG;
    }
    /* 远程规则：读不到/解析失败时静默跳过（与 Rust 版一致）。 */
    char *remote_path = cdc_remote_rules_path();
    if (remote_path) {
        char *text = read_file_utf8(remote_path, NULL);
        if (text) {
            cdc_rule_t *extra = NULL;
            size_t m = 0;
            if (cdc_rules_parse_text(text, &extra, &m, NULL, NULL) ==
                CDC_OK) {
                for (size_t i = 0; i < m; i++) {
                    if (extra[i].key[0] == '\0') {
                        cdc_rule_free_members(&extra[i]);
                        continue;
                    }
                    int known = 0;
                    for (size_t j = 0; j < n; j++) {
                        if (strcmp(rules[j].key, extra[i].key) == 0) {
                            known = 1;
                            break;
                        }
                    }
                    if (known) {
                        cdc_rule_free_members(&extra[i]);
                        continue;
                    }
                    cdc_rule_t *nr = (cdc_rule_t *)realloc(
                        rules, (n + 1) * sizeof(cdc_rule_t));
                    if (!nr) {
                        cdc_rule_free_members(&extra[i]);
                        break;
                    }
                    rules = nr;
                    rules[n++] = extra[i];
                    memset(&extra[i], 0, sizeof(cdc_rule_t));
                }
                free(extra);
            }
            free(text);
        }
        free(remote_path);
    }
    *out_rules = rules;
    *out_n = n;
    return CDC_OK;
}

/* ---- 配置 ---- */

static char *exe_dir_utf8(void) {
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

static char *config_path(void) {
    char *dir = exe_dir_utf8();
    if (!dir) return NULL;
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    const char *suffixes[2] = { "rules_update.json",
                                "resources/rules_update.json" };
    /* resources/ 里的斜杠在 Windows 上同样可用（CreateFile 接受 '/'）。 */
    for (int i = 0; i < 2; i++) {
        size_t n = strlen(dir) + 1 + strlen(suffixes[i]) + 1;
        char *p = (char *)malloc(n);
        if (!p) continue;
        snprintf(p, n, "%s%s%s", dir, sep, suffixes[i]);
        if (file_exists_utf8(p)) {
            free(dir);
            return p;
        }
        free(p);
    }
    free(dir);
    return NULL;
}

int cdc_rules_config(char **out_url, char **out_blake3, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (out_url) *out_url = strdup("");
    if (out_blake3) *out_blake3 = NULL;
    char *path = config_path();
    if (!path) return CDC_OK; /* 无配置 = url 为空（与 Rust 版一致） */
    char *text = read_file_utf8(path, NULL);
    free(path);
    if (!text) return CDC_OK;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return CDC_OK;
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (out_url && cJSON_IsString(url) && url->valuestring) {
        free(*out_url);
        /* 去首尾空白（Rust .trim()）。 */
        const char *s = url->valuestring;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                         s[n - 1] == '\n' || s[n - 1] == '\r'))
            n--;
        *out_url = strndup(s, n);
    }
    cJSON *hash = cJSON_GetObjectItemCaseSensitive(root, "blake3");
    if (out_blake3 && cJSON_IsString(hash) && hash->valuestring) {
        const char *s = hash->valuestring;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                         s[n - 1] == '\n' || s[n - 1] == '\r'))
            n--;
        if (n == 64) {
            char *h = strndup(s, n);
            if (h) {
                for (size_t i = 0; h[i]; i++) {
                    if (h[i] >= 'A' && h[i] <= 'F')
                        h[i] = (char)(h[i] + ('a' - 'A'));
                }
                *out_blake3 = h;
            }
        }
    }
    cJSON_Delete(root);
    return CDC_OK;
}

/* ---- 校验 ---- */

int cdc_rule_key_ok(const char *key) {
    if (!key || key[0] == '\0') return 0;
    if (strlen(key) > 80) return 0;
    for (const char *p = key; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int cdc_rule_path_allowed(const char *raw, char **out_err) {
    if (out_err) *out_err = NULL;
    if (!raw) {
        if (out_err) *out_err = strdup("路径为空");
        return CDC_ERR_INVALID_ARG;
    }
    const char *t = raw;
    while (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r') t++;
    if (*t == '\0') {
        if (out_err) *out_err = strdup("路径为空");
        return CDC_ERR_INVALID_ARG;
    }
    if (strlen(raw) > 500) {
        if (out_err) *out_err = strdup("路径含有非法字符");
        return CDC_ERR_INVALID_ARG;
    }
    for (const unsigned char *p = (const unsigned char *)raw; *p; p++) {
        if (*p < 32) {
            if (out_err) *out_err = strdup("路径含有非法字符");
            return CDC_ERR_INVALID_ARG;
        }
    }
    char *expanded = cdc_expand_env(raw);
    if (!expanded) {
        if (out_err) *out_err = strdup("路径展开失败");
        return CDC_ERR_NOMEM;
    }
    if (cdc_path_syntax_error(expanded)) {
        free(expanded);
        if (out_err) *out_err = strdup("路径格式不合法");
        return CDC_ERR_INVALID_ARG;
    }
    /* 按 '\\' 或 '/' 切分、小写，任一段命中 LIBRARY_PARTS 即拒绝。 */
    {
        char *lower = strdup(expanded);
        if (!lower) {
            free(expanded);
            if (out_err) *out_err = strdup("内存不足");
            return CDC_ERR_NOMEM;
        }
        for (char *p = lower; *p; p++) {
            if (*p == '/') *p = '\\';
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + ('a' - 'A'));
        }
        const char *p = lower;
        int bad = 0;
        while (*p) {
            while (*p == '\\') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != '\\') p++;
            size_t len = (size_t)(p - start);
            for (size_t i = 0;
                 i < sizeof(LIBRARY_PARTS) / sizeof(*LIBRARY_PARTS); i++) {
                if (strlen(LIBRARY_PARTS[i]) == len &&
                    strncmp(start, LIBRARY_PARTS[i], len) == 0) {
                    bad = 1;
                    break;
                }
            }
            if (bad) break;
        }
        free(lower);
        if (bad) {
            free(expanded);
            if (out_err) *out_err = strdup("远程规则不能指向文档、桌面或下载");
            return CDC_ERR_INVALID_ARG;
        }
    }
    char *name = cdc_basename(expanded);
    if (name) {
        for (char *p = name; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + ('a' - 'A'));
        }
        if (cdc_is_password_store(name) || strstr(name, "login data") != NULL) {
            free(name);
            free(expanded);
            if (out_err) *out_err = strdup("远程规则不能删除浏览器密码库");
            return CDC_ERR_INVALID_ARG;
        }
        free(name);
    }
    if (!cdc_under_remote_allowlist(expanded)) {
        free(expanded);
        if (out_err)
            *out_err = strdup("远程规则只能指向临时目录或固定 catalog 中的目录");
        return CDC_ERR_INVALID_ARG;
    }
    free(expanded);
    return CDC_OK;
}

int cdc_rule_validate(const char *key, const char *kind, const char *tier,
                      int has_command, int command_non_empty,
                      const char **paths, size_t npaths,
                      const char **glob_dirs, size_t nglob_dirs,
                      cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!cdc_rule_key_ok(key)) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则 key 不合法");
        return CDC_ERR_INVALID_ARG;
    }
    const char *k = (kind && kind[0]) ? kind : "files";
    if (strcmp(k, "files") != 0) {
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则 %s 不是文件清理类型",
                        key);
        return CDC_ERR_INVALID_ARG;
    }
    const char *t = (tier && tier[0]) ? tier : "standard";
    int tier_ok = 0;
    for (size_t i = 0; i < sizeof(TIERS) / sizeof(*TIERS); i++) {
        if (strcmp(t, TIERS[i]) == 0) {
            tier_ok = 1;
            break;
        }
    }
    if (!tier_ok) {
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则 %s 的档位不合法", key);
        return CDC_ERR_INVALID_ARG;
    }
    if (has_command && command_non_empty) {
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则 %s 不能带命令", key);
        return CDC_ERR_INVALID_ARG;
    }
    for (size_t f = 0; f < 2; f++) {
        const char **list = f == 0 ? paths : glob_dirs;
        size_t n = f == 0 ? npaths : nglob_dirs;
        const char *field = f == 0 ? "paths" : "glob_dirs";
        for (size_t i = 0; i < n; i++) {
            char *why = NULL;
            if (cdc_rule_path_allowed(list[i], &why) != CDC_OK) {
                if (err)
                    cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则 %s：%s", key,
                                why ? why : "路径不合法");
                free(why);
                return CDC_ERR_INVALID_ARG;
            }
        }
        (void)field;
    }
    return CDC_OK;
}

/* 从 cJSON 规则对象里抽出 validate 所需的字段并校验。 */
static int validate_json_item(cJSON *item, cdc_err_t *err) {
    cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
    const char *key_s = (key && cJSON_IsString(key)) ? key->valuestring : "";
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
    const char *kind_s =
        (kind && cJSON_IsString(kind)) ? kind->valuestring : NULL;
    cJSON *tier = cJSON_GetObjectItemCaseSensitive(item, "tier");
    const char *tier_s =
        (tier && cJSON_IsString(tier)) ? tier->valuestring : NULL;
    cJSON *command = cJSON_GetObjectItemCaseSensitive(item, "command");
    int has_command = command != NULL;
    int command_non_empty = 0;
    if (command) {
        if (cJSON_IsArray(command))
            command_non_empty = cJSON_GetArraySize(command) > 0;
        else
            command_non_empty = 1; /* 非数组的 command 同样拒绝 */
    }
    /* paths / glob_dirs：非数组或含非字符串即格式错误。 */
    cdc_vec_t path_vec, glob_vec;
    if (cdc_vec_init(&path_vec, sizeof(const char *)) != CDC_OK ||
        cdc_vec_init(&glob_vec, sizeof(const char *)) != CDC_OK) {
        cdc_vec_free(&path_vec);
        cdc_vec_free(&glob_vec);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_INVALID_ARG;
    }
    int bad = 0;
    for (int f = 0; f < 2 && !bad; f++) {
        cJSON *list = cJSON_GetObjectItemCaseSensitive(
            item, f == 0 ? "paths" : "glob_dirs");
        if (!list) continue;
        if (!cJSON_IsArray(list)) {
            if (err)
                cdc_err_set(err, CDC_ERR_INVALID_ARG,
                            "规则 %s 的 %s 格式不正确", key_s,
                            f == 0 ? "paths" : "glob_dirs");
            bad = 1;
            break;
        }
        size_t n = (size_t)cJSON_GetArraySize(list);
        for (size_t i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(list, (int)i);
            if (!cJSON_IsString(e)) {
                if (err)
                    cdc_err_set(err, CDC_ERR_INVALID_ARG,
                                "规则 %s 的 %s 含有非字符串", key_s,
                                f == 0 ? "paths" : "glob_dirs");
                bad = 1;
                break;
            }
            const char *s = e->valuestring ? e->valuestring : "";
            if (cdc_vec_push(f == 0 ? &path_vec : &glob_vec, &s) !=
                CDC_OK) {
                bad = 1;
                break;
            }
        }
    }
    int rc = CDC_OK;
    if (!bad) {
        rc = cdc_rule_validate(
            key_s, kind_s, tier_s, has_command, command_non_empty,
            (const char **)path_vec.data, path_vec.len,
            (const char **)glob_vec.data, glob_vec.len, err);
    } else {
        rc = CDC_ERR_INVALID_ARG;
    }
    cdc_vec_free(&path_vec);
    cdc_vec_free(&glob_vec);
    return rc;
}

int cdc_rules_validate_text(const char *text, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    cJSON *root = cJSON_Parse(text ? text : "");
    if (!root) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则格式不正确");
        return CDC_ERR_INVALID_ARG;
    }
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(root, "categories");
    if (!cJSON_IsArray(cats)) {
        cJSON_Delete(root);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则文件缺少 categories");
        return CDC_ERR_INVALID_ARG;
    }
    if ((size_t)cJSON_GetArraySize(cats) > CDC_RULES_MAX_COUNT) {
        cJSON_Delete(root);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "远程规则过多（上限 %u 条）",
                        CDC_RULES_MAX_COUNT);
        return CDC_ERR_INVALID_ARG;
    }
    int rc = CDC_OK;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cats) {
        if (!cJSON_IsObject(item)) {
            if (err)
                cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则格式不正确");
            rc = CDC_ERR_INVALID_ARG;
            break;
        }
        if (validate_json_item(item, err) != CDC_OK) {
            rc = CDC_ERR_INVALID_ARG;
            break;
        }
    }
    cJSON_Delete(root);
    return rc;
}

/* ---- 远程更新 ---- */

#ifdef _WIN32
/* WinHTTP GET：返回 malloc 的响应体（UTF-8 文本），*out_len 为字节数。
 * 读满 MAX_BYTES+1 即停（调用方据此判定超限）。 */
static int http_get_https(const char *url, char **out_body, size_t *out_len,
                          cdc_err_t *err) {
    /* 拆 https://host[:port]/path */
    const char *rest = url + strlen("https://");
    const char *slash = strchr(rest, '/');
    size_t host_len = slash ? (size_t)(slash - rest) : strlen(rest);
    const char *path = slash ? slash : "/";
    if (host_len == 0 || host_len >= 512) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "下载地址不合法");
        return CDC_ERR_INVALID_ARG;
    }
    char host[512];
    memcpy(host, rest, host_len);
    host[host_len] = '\0';
    wchar_t *whost = cdc_utf8_to_wide(host);
    wchar_t *wpath = cdc_utf8_to_wide(path);
    if (!whost || !wpath) {
        free(whost);
        free(wpath);
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "下载地址编码失败");
        return CDC_ERR_UTF8;
    }
    HINTERNET session = NULL, conn = NULL, req = NULL;
    int rc = CDC_ERR_IO;
    cdc_buf_t body;
    size_t got_len = 0;
    cdc_buf_init(&body);
    session = WinHttpOpen(L"cdc-rules/1.0",
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto done;
    WinHttpSetTimeouts(session, 20000, 20000, 20000, 20000);
    conn = WinHttpConnect(session, whost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) goto done;
    req = WinHttpOpenRequest(conn, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             WINHTTP_FLAG_SECURE);
    if (!req) goto done;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        goto done;
    if (!WinHttpReceiveResponse(req, NULL)) goto done;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) goto done;
        if (avail == 0) break;
        if (body.len + avail > CDC_RULES_MAX_BYTES + 1) {
            avail = (DWORD)(CDC_RULES_MAX_BYTES + 1 - body.len);
            if (avail == 0) break;
        }
        if (cdc_buf_reserve(&body, avail) != CDC_OK) {
            rc = CDC_ERR_NOMEM;
            goto done;
        }
        DWORD got;
        got = 0;
        if (!WinHttpReadData(req, body.data + body.len, avail, &got) ||
            got == 0)
            goto done;
        body.len += got;
        if (body.len > CDC_RULES_MAX_BYTES) break;
    }
    got_len = body.len; /* 先取值：take_cstr 会把 b 重置为 init 状态 */
    *out_body = cdc_buf_take_cstr(&body);
    if (!*out_body) {
        rc = CDC_ERR_NOMEM;
        goto done;
    }
    *out_len = got_len;
    rc = CDC_OK;
done:
    if (rc != CDC_OK && err && err->code == 0)
        cdc_err_set(err, rc, "下载失败：Win32 %lu",
                    (unsigned long)GetLastError());
    cdc_buf_free(&body);
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    free(whost);
    free(wpath);
    return rc;
}

static void blake3_hex(const uint8_t *data, size_t len, char out_hex[65]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    uint8_t digest[32];
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hexd[digest[i] >> 4];
        out_hex[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}
#endif /* _WIN32 */

int cdc_rules_update(cdc_rules_update_result_t *out, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "out is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
#ifdef _WIN32
    char *url = NULL;
    char *pin = NULL;
    if (cdc_rules_config(&url, &pin, err) != CDC_OK) {
        free(url);
        free(pin);
        return err->code;
    }
    if (!url || strncmp(url, "https://", 8) != 0) {
        free(url);
        free(pin);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "未配置规则更新地址（在 rules_update.json 里填写 "
                        "https 地址）");
        return CDC_ERR_INVALID_ARG;
    }
    char *body = NULL;
    size_t body_len = 0;
    int rc = http_get_https(url, &body, &body_len, err);
    free(url);
    if (rc != CDC_OK) {
        free(pin);
        return rc;
    }
    if (body_len > CDC_RULES_MAX_BYTES) {
        free(body);
        free(pin);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则文件过大");
        return CDC_ERR_INVALID_ARG;
    }
    if (pin) {
        char hex[65];
        blake3_hex((const uint8_t *)body, body_len, hex);
        if (strcmp(hex, pin) != 0) {
            free(body);
            free(pin);
            if (err)
                cdc_err_set(err, CDC_ERR_INVALID_ARG,
                            "规则完整性校验失败，未保存更新");
            return CDC_ERR_INVALID_ARG;
        }
        free(pin);
    }
    cJSON *parsed = cJSON_Parse(body);
    if (!parsed) {
        free(body);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则格式不正确");
        return CDC_ERR_INVALID_ARG;
    }
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(parsed, "categories");
    if (!cJSON_IsArray(cats)) {
        cJSON_Delete(parsed);
        free(body);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "规则文件缺少 categories");
        return CDC_ERR_INVALID_ARG;
    }
    if ((size_t)cJSON_GetArraySize(cats) > CDC_RULES_MAX_COUNT) {
        cJSON_Delete(parsed);
        free(body);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG, "远程规则过多（上限 %u 条）",
                        CDC_RULES_MAX_COUNT);
        return CDC_ERR_INVALID_ARG;
    }
    /* 逐条校验（与 Rust validate 逐项一致）。 */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cats) {
        if (!cJSON_IsObject(item) || validate_json_item(item, err) != CDC_OK) {
            cJSON_Delete(parsed);
            free(body);
            return CDC_ERR_INVALID_ARG;
        }
    }
    /* 版本比较：incoming < old → 拒绝。 */
    char *path = cdc_remote_rules_path();
    if (!path) {
        cJSON_Delete(parsed);
        free(body);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    uint64_t old_version = 0;
    {
        char *old_text = read_file_utf8(path, NULL);
        if (old_text) {
            cJSON *old = cJSON_Parse(old_text);
            if (old) {
                cJSON *v = cJSON_GetObjectItemCaseSensitive(old, "version");
                if (cJSON_IsNumber(v) && v->valuedouble >= 0)
                    old_version = (uint64_t)v->valuedouble;
                cJSON_Delete(old);
            }
            free(old_text);
        }
    }
    uint64_t incoming_version = 1;
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(parsed, "version");
        if (cJSON_IsNumber(v) && v->valuedouble >= 0)
            incoming_version = (uint64_t)v->valuedouble;
    }
    if (incoming_version < old_version) {
        cJSON_Delete(parsed);
        free(body);
        free(path);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "规则版本过旧：当前 %llu，收到 %llu",
                        (unsigned long long)old_version,
                        (unsigned long long)incoming_version);
        return CDC_ERR_INVALID_ARG;
    }
    /* 拼 payload {"version": N, "categories": [...]}（保留原始条目字段）。 */
    size_t accepted = (size_t)cJSON_GetArraySize(cats); /* 先取值：parsed
                                                        * 随后会被释放 */
    cJSON *payload = cJSON_CreateObject();
    cJSON *cats_copy = cJSON_Duplicate(cats, 1);
    if (!payload || !cats_copy) {
        cJSON_Delete(payload);
        cJSON_Delete(cats_copy);
        cJSON_Delete(parsed);
        free(body);
        free(path);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    cJSON_AddNumberToObject(payload, "version", (double)incoming_version);
    cJSON_AddItemToObject(payload, "categories", cats_copy);
    char *encoded = cJSON_Print(payload);
    cJSON_Delete(payload);
    cJSON_Delete(parsed);
    free(body);
    if (!encoded) {
        free(path);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    /* 原子写：temp → 备份旧文件 → rename。 */
    size_t tn = strlen(path) + strlen(".tmp") + 1;
    char *temp = (char *)malloc(tn);
    if (!temp) {
        cJSON_free(encoded);
        free(path);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    snprintf(temp, tn, "%s.tmp", path);
    if (write_file_utf8(temp, encoded, strlen(encoded)) != CDC_OK) {
        cJSON_free(encoded);
        free(temp);
        free(path);
        if (err) cdc_err_set(err, CDC_ERR_IO, "写入临时文件失败");
        return CDC_ERR_IO;
    }
    cJSON_free(encoded);
    if (file_exists_utf8(path)) {
        char *backup = cdc_remote_rules_backup_path();
        if (backup) {
            copy_file_utf8(path, backup); /* 备份失败不致命（与 Rust let _ = 一致） */
            free(backup);
        }
    }
    if (rename_replace_utf8(temp, path) != CDC_OK) {
        free(temp);
        free(path);
        if (err) cdc_err_set(err, CDC_ERR_IO, "替换规则文件失败");
        return CDC_ERR_IO;
    }
    free(temp);
    /* total = all_rules 总条数（与 Rust update() 返回值一致）。 */
    cdc_rule_t *all = NULL;
    size_t total = 0;
    if (cdc_rules_load_all(&all, &total, NULL) == CDC_OK)
        cdc_rules_free(all, total);
    out->accepted = accepted;
    out->total = total;
    {
        size_t mn = strlen(path) + 64;
        out->message = (char *)malloc(mn);
        if (out->message)
            snprintf(out->message, mn, "已保存到 %s，版本 %llu", path,
                     (unsigned long long)incoming_version);
    }
    free(path);
    return CDC_OK;
#else
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "规则更新仅支持 Windows（WinHTTP）");
    return CDC_ERR_IO;
#endif
}

int cdc_rules_rollback(char **out_message, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (out_message) *out_message = NULL;
    char *path = cdc_remote_rules_path();
    char *backup = cdc_remote_rules_backup_path();
    if (!path || !backup) {
        free(path);
        free(backup);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    if (!file_exists_utf8(backup)) {
        free(path);
        free(backup);
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "没有可回滚的规则版本");
        return CDC_ERR_NOT_FOUND;
    }
    char *text = read_file_utf8(backup, NULL);
    if (!text) {
        free(path);
        free(backup);
        if (err) cdc_err_set(err, CDC_ERR_IO, "读取备份规则失败");
        return CDC_ERR_IO;
    }
    cJSON *value = cJSON_Parse(text);
    if (!value) {
        free(text);
        free(path);
        free(backup);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "备份规则损坏");
        return CDC_ERR_INVALID_ARG;
    }
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(value, "categories");
    if (!cJSON_IsArray(cats)) {
        cJSON_Delete(value);
        free(text);
        free(path);
        free(backup);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "备份规则缺少 categories");
        return CDC_ERR_INVALID_ARG;
    }
    if ((size_t)cJSON_GetArraySize(cats) > CDC_RULES_MAX_COUNT) {
        cJSON_Delete(value);
        free(text);
        free(path);
        free(backup);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "备份规则数量超限");
        return CDC_ERR_INVALID_ARG;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cats) {
        if (!cJSON_IsObject(item) || validate_json_item(item, err) != CDC_OK) {
            cJSON_Delete(value);
            free(text);
            free(path);
            free(backup);
            return CDC_ERR_INVALID_ARG;
        }
    }
    uint64_t version = 0;
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(value, "version");
        if (cJSON_IsNumber(v) && v->valuedouble >= 0)
            version = (uint64_t)v->valuedouble;
    }
    cJSON_Delete(value);
    free(text);
    int rc = copy_file_utf8(backup, path);
    free(path);
    free(backup);
    if (rc != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "回滚失败");
        return CDC_ERR_IO;
    }
    if (out_message) {
        *out_message = (char *)malloc(64);
        if (*out_message)
            snprintf(*out_message, 64, "已回滚到规则版本 %llu",
                     (unsigned long long)version);
    }
    return CDC_OK;
}
