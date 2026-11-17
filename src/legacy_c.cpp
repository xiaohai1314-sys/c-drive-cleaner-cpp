// legacy_c.cpp — TRANSITIONAL：见 legacy_c.h。C 版语义原样保留（malloc 所有权、
// 错误码），实现转发到 cdc:: 新接口。最后一个模块迁移后删除。
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "util.h"

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
