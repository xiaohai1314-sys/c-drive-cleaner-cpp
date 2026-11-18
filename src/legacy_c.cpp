// legacy_c.cpp — TRANSITIONAL：见 legacy_c.h。C 版语义原样保留（malloc 所有权、
// 错误码），实现转发到 cdc:: 新接口。最后一个模块迁移后删除。
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include "util.h"
#include "timeutil.h"
#include "logs.h"

void cdc_err_init(cdc_err_t *err) { cdc::err_init(err); }
void cdc_err_set(cdc_err_t *err, int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    cdc::err_vset(err, code, fmt, ap);
    va_end(ap);
}
const char *cdc_err_str(int code) { return cdc::err_str(code); }

void cdc_buf_init(cdc_buf_t *b) {
    if (!b) return;
    b->data = NULL;
    b->len = b->cap = 0;
}
void cdc_buf_free(cdc_buf_t *b) {
    if (!b) return;
    free(b->data);
    cdc_buf_init(b);
}
void cdc_buf_clear(cdc_buf_t *b) {
    if (b) b->len = 0;
}
int cdc_buf_reserve(cdc_buf_t *b, size_t extra) {
    if (!b) return CDC_ERR_INVALID_ARG;
    if (extra > SIZE_MAX - b->len) return CDC_ERR_OVERFLOW;
    size_t needed = b->len + extra;
    if (b->cap >= needed) return CDC_OK;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    uint8_t *p = (uint8_t *)realloc(b->data, cap);
    if (!p) return CDC_ERR_NOMEM;
    b->data = p;
    b->cap = cap;
    return CDC_OK;
}
int cdc_buf_append(cdc_buf_t *b, const void *p, size_t n) {
    if (!b) return CDC_ERR_INVALID_ARG;
    if (n > 0 && !p) return CDC_ERR_INVALID_ARG;
    if (n == 0) return CDC_OK;
    int rc = cdc_buf_reserve(b, n);
    if (rc != CDC_OK) return rc;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return CDC_OK;
}
int cdc_buf_push(cdc_buf_t *b, uint8_t byte) { return cdc_buf_append(b, &byte, 1); }
int cdc_buf_append_str(cdc_buf_t *b, const char *s) {
    if (!s) return CDC_ERR_INVALID_ARG;
    return cdc_buf_append(b, s, strlen(s));
}
uint8_t *cdc_buf_take(cdc_buf_t *b) {
    if (!b) return NULL;
    uint8_t *p = b->data;
    cdc_buf_init(b);
    return p;
}
char *cdc_buf_take_cstr(cdc_buf_t *b) {
    if (!b || cdc_buf_reserve(b, 1) != CDC_OK) return NULL;
    b->data[b->len] = '\0';
    return (char *)cdc_buf_take(b);
}

int cdc_vec_init(cdc_vec_t *v, size_t elem_size) {
    if (!v || elem_size == 0) return CDC_ERR_INVALID_ARG;
    v->data = NULL;
    v->len = v->cap = 0;
    v->elem_size = elem_size;
    return CDC_OK;
}
void cdc_vec_free(cdc_vec_t *v) {
    if (!v) return;
    free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
}
void cdc_vec_clear(cdc_vec_t *v) {
    if (v) v->len = 0;
}
int cdc_vec_reserve(cdc_vec_t *v, size_t extra) {
    if (!v || v->elem_size == 0) return CDC_ERR_INVALID_ARG;
    if (extra > SIZE_MAX - v->len) return CDC_ERR_OVERFLOW;
    size_t needed = v->len + extra;
    if (v->cap >= needed) return CDC_OK;
    size_t cap = v->cap ? v->cap : 16;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            cap = needed;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / v->elem_size) return CDC_ERR_OVERFLOW;
    void *p = realloc(v->data, cap * v->elem_size);
    if (!p) return CDC_ERR_NOMEM;
    v->data = p;
    v->cap = cap;
    return CDC_OK;
}
int cdc_vec_push(cdc_vec_t *v, const void *elem) {
    if (!v || v->elem_size == 0 || !elem) return CDC_ERR_INVALID_ARG;
    int rc = cdc_vec_reserve(v, 1);
    if (rc != CDC_OK) return rc;
    memcpy((uint8_t *)v->data + v->len * v->elem_size, elem, v->elem_size);
    v->len++;
    return CDC_OK;
}
int cdc_vec_pop(cdc_vec_t *v, void *out) {
    if (!v || v->elem_size == 0 || v->len == 0) return CDC_ERR_INVALID_ARG;
    v->len--;
    if (out) memcpy(out, (uint8_t *)v->data + v->len * v->elem_size, v->elem_size);
    return CDC_OK;
}
void *cdc_vec_get(cdc_vec_t *v, size_t i) {
    if (!v || i >= v->len) return NULL;
    return (uint8_t *)v->data + i * v->elem_size;
}

wchar_t *cdc_utf8_to_wide(const char *utf8) {
    auto w = cdc::utf8_to_wide(utf8);
    if (!w) return NULL;
    wchar_t *out = (wchar_t *)malloc((w->size() + 1) * sizeof(wchar_t));
    if (out) memcpy(out, w->c_str(), (w->size() + 1) * sizeof(wchar_t));
    return out;
}
char *cdc_wide_to_utf8(const wchar_t *wide) {
    auto s = cdc::wide_to_utf8(wide);
    if (!s) return NULL;
    char *out = (char *)malloc(s->size() + 1);
    if (out) memcpy(out, s->c_str(), s->size() + 1);
    return out;
}

int cdc_enum_dir(const char *dir, cdc_dir_entry_fn cb, void *ctx) {
    if (!cb) return CDC_ERR_INVALID_ARG;
    std::string name;
    return cdc::enum_dir(dir, [&](const cdc::DirEntry &e) {
        name.assign(e.name);
        return cb(name.c_str(), e.is_dir, e.is_symlink, e.size, e.mtime, e.attrs, ctx);
    });
}

void cdc_log(cdc_log_level_t level, const char *fmt, ...) {
    auto lv = static_cast<cdc::LogLevel>(level);
    if (lv < cdc::log_get_level() || !fmt) return;
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cdc::log(lv, "%s", msg);
}
int cdc_log_dir(char *out, size_t cap) {
    if (!out || cap == 0) return CDC_ERR_INVALID_ARG;
    auto d = cdc::log_dir();
    if (!d) return CDC_ERR_IO;
    if (d->size() + 1 > cap) return CDC_ERR_OVERFLOW;
    memcpy(out, d->c_str(), d->size() + 1);
    return CDC_OK;
}
char *cdc_cache_dir(void) {
    auto d = cdc::cache_dir();
    if (!d) return NULL;
    char *out = (char *)malloc(d->size() + 1);
    if (out) memcpy(out, d->c_str(), d->size() + 1);
    return out;
}

// ---- safety 转发 ----
#include "raii.h"
#include "safety.h"

struct cdc_handle_chain {
    cdc::HandleChain chain;
};

namespace {
char *dup_c(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    if (p) memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
cdc::RootList roots_of(const char **roots, size_t n) {
    return roots ? cdc::RootList(roots, n) : cdc::RootList();
}
int give_chain(cdc::HandleChain &c, cdc_handle_chain_t **out) {
    if (!out) return CDC_ERR_INVALID_ARG;
    *out = new cdc_handle_chain{std::move(c)};
    return CDC_OK;
}
}  // namespace

char *cdc_getenv_utf8(const char *name) {
    std::optional<std::string> v = cdc::getenv_utf8(name);
    return v ? dup_c(*v) : NULL;
}
FILE *cdc_fopen_utf8(const char *path, const char *mode) {
    if (!path || !mode) return NULL;
    return cdc::fopen_utf8(path, mode).release();
}
void cdc_strvec_free(char **vec, size_t n) {
    if (!vec) return;
    for (size_t i = 0; i < n; i++) free(vec[i]);
    free(vec);
}
int cdc_glob_match(const char *p, const char *t) { return cdc::glob_match(cdc::sv(p), cdc::sv(t)); }
int cdc_glob_valid(const char *p) { return p ? cdc::glob_valid(p) : 0; }
void cdc_handle_chain_free(cdc_handle_chain_t *chain) { delete chain; }
int cdc_handle_file_size(cdc_handle_t h, uint64_t *out, cdc_err_t *err) {
    uint64_t v = 0;
    int rc = cdc::handle_file_size(h, v, err);
    if (rc == CDC_OK && out) *out = v;
    return rc;
}
int cdc_handle_read(cdc_handle_t h, uint8_t *buf, size_t want, size_t *out_got, cdc_err_t *err) {
    size_t got = 0;
    int rc = cdc::handle_read(h, buf, want, got, err);
    if (rc == CDC_OK && out_got) *out_got = got;
    return rc;
}
int cdc_handle_write_all(cdc_handle_t h, const uint8_t *buf, size_t len, cdc_err_t *err) {
    return cdc::handle_write_all(h, buf, len, err);
}
int cdc_handle_sync(cdc_handle_t h, cdc_err_t *err) { return cdc::handle_sync(h, err); }
int cdc_handle_seek_start(cdc_handle_t h, cdc_err_t *err) { return cdc::handle_seek_start(h, err); }
int cdc_walk_dir(const char *root, int min_depth, cdc_walk_fn fn, void *ctx, cdc_err_t *err) {
    if (!fn) return cdc::walk_dir(root, min_depth, nullptr, err);
    std::string path;
    return cdc::walk_dir(root, min_depth, [&](const cdc::WalkEntry &e) {
        path.assign(e.path);
        return fn(ctx, path.c_str(), e.is_dir, e.is_symlink, e.size, e.mtime, e.attrs);
    }, err);
}
char *cdc_basename(const char *path) { return dup_c(cdc::basename(path)); }
char *cdc_normalize(const char *path) { return dup_c(cdc::normalize(path)); }
int cdc_under_marker(const char *path, const char *const *markers, size_t n) {
    return markers ? cdc::under_marker(path, std::span<const char *const>(markers, n)) : 0;
}
int cdc_is_drive_root(const char *p) { return cdc::is_drive_root(p); }
int cdc_is_critical_file(const char *p) { return cdc::is_critical_file(p); }
int cdc_is_never_clean(const char *p) { return cdc::is_never_clean(p); }
int cdc_is_password_store(const char *p) { return cdc::is_password_store(p); }
int cdc_path_syntax_error(const char *p) { return cdc::path_syntax_error(p); }
int cdc_is_system_path(const char *p) { return cdc::is_system_path(p); }
int cdc_is_protected_path(const char *p) { return cdc::is_protected_path(p); }
int cdc_is_user_library(const char *p) { return cdc::is_user_library(p); }
int cdc_is_within(const char *p, const char **roots, size_t n) {
    return cdc::is_within(p, roots_of(roots, n));
}
int cdc_is_reparse_point(const char *p) { return cdc::is_reparse_point(p); }
int cdc_reparse_point_status(const char *path, int *out, cdc_err_t *err) {
    if (!path || !out) {
        cdc::err_set(err, CDC_ERR_INVALID_ARG, "path is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    bool r = false;
    int rc = cdc::reparse_point_status(path, r, err);
    if (rc == CDC_OK) *out = r;
    return rc;
}
int cdc_under_remote_allowlist(const char *p) { return cdc::under_remote_allowlist(p); }
int cdc_execution_forbidden(const char *p, int untrusted) {
    return cdc::execution_forbidden(p, untrusted != 0);
}
int cdc_explicit_delete_forbidden(const char *p) { return cdc::explicit_delete_forbidden(p); }
int cdc_mark_delete(cdc_handle_t file) { return cdc::mark_delete(file); }
cdc_delete_outcome_t cdc_delete_file_checked(const char *path, const char **roots, size_t n,
                                             int untrusted, cdc_err_t *err) {
    return (cdc_delete_outcome_t)cdc::delete_file_checked(path, roots_of(roots, n), untrusted != 0,
                                                          err);
}
int cdc_pin_directory_checked(const char *path, const char **roots, size_t n,
                              cdc_handle_chain_t **out_chain, cdc_err_t *err) {
    cdc::HandleChain c;
    int rc = cdc::pin_directory_checked(path, roots_of(roots, n), c, err);
    return rc == CDC_OK ? give_chain(c, out_chain) : rc;
}
int cdc_create_directory_tree_checked(const char *path, cdc_handle_chain_t **out_chain,
                                      cdc_err_t *err) {
    cdc::HandleChain c;
    int rc = cdc::create_directory_tree_checked(path, c, err);
    return rc == CDC_OK ? give_chain(c, out_chain) : rc;
}
int cdc_create_directory_checked(const char *path, const char **roots, size_t n,
                                 cdc_handle_chain_t **out_chain, cdc_handle_t *out_dir,
                                 cdc_err_t *err) {
    cdc::HandleChain c;
    cdc::OwnedHandle h;
    int rc = cdc::create_directory_checked(path, roots_of(roots, n), c, h, err);
    if (rc != CDC_OK) return rc;
    if (out_dir) *out_dir = h.release();  // C 版语义：调用方 CloseHandle
    return give_chain(c, out_chain);
}
int cdc_create_new_file_checked(const char *path, const char **roots, size_t n,
                                cdc_handle_chain_t **out_chain, cdc_handle_t *out_file,
                                cdc_err_t *err) {
    cdc::HandleChain c;
    cdc::OwnedHandle h;
    int rc = cdc::create_new_file_checked(path, roots_of(roots, n), c, h, err);
    if (rc != CDC_OK) return rc;
    if (out_file) *out_file = h.release();
    return give_chain(c, out_chain);
}
int cdc_open_read_file_checked(const char *path, const char **roots, size_t n,
                               cdc_handle_chain_t **out_chain, cdc_handle_t *out_file,
                               cdc_err_t *err) {
    cdc::HandleChain c;
    cdc::OwnedHandle h;
    int rc = cdc::open_read_file_checked(path, roots_of(roots, n), c, h, err);
    if (rc != CDC_OK) return rc;
    if (out_file) *out_file = h.release();
    return give_chain(c, out_chain);
}
int cdc_remove_directory_checked(const char *path, const char **roots, size_t n,
                                 int *out_deleted, cdc_err_t *err) {
    if (!out_deleted) {
        cdc::err_set(err, CDC_ERR_INVALID_ARG, "out_deleted is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    bool d = false;
    int rc = cdc::remove_directory_checked(path, roots_of(roots, n), d, err);
    *out_deleted = d;
    return rc;
}
int cdc_remove_tree_checked(const char *root, const char **roots, size_t n,
                            uint64_t *out_removed, cdc_err_t *err) {
    if (!out_removed) {
        cdc::err_set(err, CDC_ERR_INVALID_ARG, "out_removed is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    return cdc::remove_tree_checked(root, roots_of(roots, n), *out_removed, err);
}
int cdc_rename_directory_sibling_checked(const char *from, const char *to, const char **roots,
                                         size_t n, cdc_err_t *err) {
    return cdc::rename_directory_sibling_checked(from, to, roots_of(roots, n), err);
}
int cdc_remove_empty_dirs(const char *root) { return cdc::remove_empty_dirs(root); }

// ---- recycle 转发 ----
#include "recycle.h"

int cdc_send_to_recycle(const char **paths, size_t npaths, char ***out_failed,
                        size_t *out_nfailed, cdc_err_t *err) {
    cdc::err_init(err);
    if (!out_failed || !out_nfailed) {
        cdc::err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    std::vector<std::string> failed =
        cdc::send_to_recycle(std::span<const char *const>(paths, paths ? npaths : 0));
    *out_nfailed = failed.size();
    *out_failed = (char **)calloc(failed.size() ? failed.size() : 1, sizeof(char *));
    for (size_t i = 0; *out_failed && i < failed.size(); i++) (*out_failed)[i] = dup_c(failed[i]);
    return CDC_OK;
}

// ---- timeutil 转发 ----
cdc_local_time_t cdc_local_time(uint64_t seconds) {
    cdc::LocalTime t = cdc::local_time(seconds);
    cdc_local_time_t out;
    out.year = t.year;
    out.month = t.month;
    out.day = t.day;
    out.hour = t.hour;
    out.minute = t.minute;
    out.second = t.second;
    return out;
}
char *cdc_month_label(uint64_t seconds) { return dup_c(cdc::month_label(seconds)); }
char *cdc_stamp(void) { return dup_c(cdc::stamp()); }
char *cdc_human_stamp(uint64_t seconds) { return dup_c(cdc::human_stamp(seconds)); }

// ---- logs 转发 ----
char *cdc_app_data_dir(void) {
    auto d = cdc::app_data_dir();
    return d ? dup_c(*d) : NULL;
}
char *cdc_logs_dir(void) {
    auto d = cdc::logs_dir();
    return d ? dup_c(*d) : NULL;
}
int cdc_write_log(const char **lines, size_t nlines, char **out_path, cdc_err_t *err) {
    std::vector<std::string> v;
    v.reserve(nlines);
    for (size_t i = 0; i < nlines; i++)
        v.emplace_back(lines && lines[i] ? lines[i] : "");
    std::string out;
    std::string *outp = out_path ? &out : nullptr;
    int rc = cdc::write_log(v, outp, err);
    if (rc == CDC_OK && out_path) {
        *out_path = dup_c(out);
        if (!*out_path) {
            cdc::err_set(err, CDC_ERR_NOMEM, "out of memory");
            return CDC_ERR_NOMEM;
        }
    }
    return rc;
}
int cdc_append_line(const char *name, const char *line) {
    return cdc::append_line(name ? name : "", line ? line : "");
}
int cdc_logs_list(cdc_log_entry_t **out, size_t *out_n, cdc_err_t *err) {
    if (!out || !out_n) {
        cdc::err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out = nullptr;
    *out_n = 0;
    std::vector<cdc::LogEntry> v;
    int rc = cdc::logs_list(&v, err);
    if (rc != CDC_OK)
        return rc;
    auto *a = (cdc_log_entry_t *)malloc(v.size() * sizeof(cdc_log_entry_t));
    if (!a && !v.empty()) {
        cdc::err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        a[i].name = dup_c(v[i].name);
        if (!a[i].name) {
            for (size_t j = 0; j < i; ++j)
                free(a[j].name);
            free(a);
            cdc::err_set(err, CDC_ERR_NOMEM, "out of memory");
            return CDC_ERR_NOMEM;
        }
        a[i].size = v[i].size;
        a[i].mtime = v[i].mtime;
    }
    *out = a;
    *out_n = v.size();
    return CDC_OK;
}
void cdc_logs_list_free(cdc_log_entry_t *a, size_t n) {
    if (!a)
        return;
    for (size_t i = 0; i < n; ++i)
        free(a[i].name);
    free(a);
}
int cdc_log_read(const char *name, char **out_text, cdc_err_t *err) {
    if (!out_text) {
        cdc::err_set(err, CDC_ERR_INVALID_ARG, "out_text is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    *out_text = nullptr;
    std::string s;
    int rc = cdc::log_read(name ? name : "", &s, err);
    if (rc != CDC_OK)
        return rc;
    *out_text = dup_c(s);
    if (!*out_text) {
        cdc::err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}
