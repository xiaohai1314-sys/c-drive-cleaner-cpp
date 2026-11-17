#include "logs.h"
#include "raii.h"
#include "safety.h"
#include "timeutil.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace {
using cdc::kErrInvalidArg;
using cdc::kErrIo;
using cdc::kErrNoMem;
using cdc::kErrNotFound;
using cdc::kOk;
void err(cdc_err_t *e, int code, const char *msg) {
    if (e)
        cdc::err_set(e, code, "%s", msg);
}
char *cstr(const std::string &s) {
    char *p = (char *)std::malloc(s.size() + 1);
    if (!p)
        return nullptr;
    if (!s.empty())
        std::memcpy(p, s.data(), s.size());
    p[s.size()] = 0;
    return p;
}
std::string join(const std::string &d, std::string_view n) {
    std::string p = d;
#ifdef _WIN32
    if (!p.empty() && p.back() != '\\')
        p += '\\';
#else
    if (!p.empty() && p.back() != '/')
        p += '/';
#endif
    p.append(n.data(), n.size());
    return p;
}
bool regular(const std::string &p) {
#ifdef _WIN32
    auto w = cdc::utf8_to_wide(p);
    if (!w)
        return false;
    DWORD a = GetFileAttributesW(w->c_str());
    return a != INVALID_FILE_ATTRIBUTES &&
           !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#else
    struct stat s{};
    return lstat(p.c_str(), &s) == 0 && S_ISREG(s.st_mode);
#endif
}
bool appendable(const std::string &p) {
#ifdef _WIN32
    auto w = cdc::utf8_to_wide(p);
    if (!w)
        return false;
    DWORD a = GetFileAttributesW(w->c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    return !(a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#else
    struct stat s{};
    if (lstat(p.c_str(), &s) != 0)
        return errno == ENOENT;
    return S_ISREG(s.st_mode);
#endif
}
bool valid(const char *n) {
    if (!n || !*n)
        return false;
    std::string_view s(n);
    return s.find('/') == s.npos && s.find('\\') == s.npos && s.find("..") == s.npos;
}
struct Item {
    std::string name, path;
    uint64_t mtime;
};
int items(const std::string &d, std::vector<Item> &v, bool logs) {
    return cdc::enum_dir(d, [&](const cdc::DirEntry &e) {
        if (e.is_dir || e.is_symlink ||
            (logs && (e.name.size() < 4 || e.name.substr(e.name.size() - 4) != ".log")))
            return 0;
        std::string n(e.name), p = join(d, n);
        if (regular(p))
            v.push_back({std::move(n), std::move(p), e.mtime});
        return 0;
    });
}
void rotate(const std::string &d) {
    std::vector<Item> v;
    if (items(d, v, true) != 0 || v.size() <= 100)
        return;
    std::stable_sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.mtime > b.mtime; });
    for (size_t i = 100; i < v.size(); ++i) {
#ifdef _WIN32
        auto w = cdc::utf8_to_wide(v[i].path);
        if (w)
            DeleteFileW(w->c_str());
#else
        std::remove(v[i].path.c_str());
#endif
    }
}
void trim(std::string &s) {
    while (!s.empty() && std::strchr(" \t\n\r\v\f", s.back()))
        s.pop_back();
}
std::string lossy(const std::vector<unsigned char> &b) {
    std::string o;
    o.reserve(b.size());
    for (size_t i = 0; i < b.size();) {
        uint8_t c = b[i];
        size_t n = 0;
        uint32_t cp = 0;
        if (c < 128) {
            n = 1;
            cp = c;
        } else if ((c & 224) == 192 && i + 1 < b.size() && (b[i + 1] & 192) == 128) {
            cp = ((c & 31) << 6) | (b[i + 1] & 63);
            if (cp >= 128)
                n = 2;
        } else if ((c & 240) == 224 && i + 2 < b.size() && (b[i + 1] & 192) == 128 &&
                   (b[i + 2] & 192) == 128) {
            cp = ((c & 15) << 12) | ((b[i + 1] & 63) << 6) | (b[i + 2] & 63);
            if (cp >= 2048 && (cp < 55296 || cp > 57343))
                n = 3;
        } else if ((c & 248) == 240 && i + 3 < b.size() && (b[i + 1] & 192) == 128 &&
                   (b[i + 2] & 192) == 128 && (b[i + 3] & 192) == 128) {
            cp = ((c & 7) << 18) | ((b[i + 1] & 63) << 12) | ((b[i + 2] & 63) << 6) |
                 (b[i + 3] & 63);
            if (cp >= 65536 && cp <= 1114111)
                n = 4;
        }
        if (!n) {
            o += "\xef\xbf\xbd";
            ++i;
        } else {
            o.append((char *)b.data() + i, n);
            i += n;
        }
    }
    return o;
}
} // namespace
extern "C" {
char *cdc_app_data_dir(void) {
    auto d = cdc::log_dir();
    if (!d)
        return nullptr;
    std::string s = *d;
#ifdef _WIN32
    if (s.size() >= 5 && s.compare(s.size() - 5, 5, "\\logs") == 0)
        s.resize(s.size() - 5);
#else
    if (s.size() >= 5 && s.compare(s.size() - 5, 5, "/logs") == 0)
        s.resize(s.size() - 5);
#endif
    return cstr(s);
}
char *cdc_logs_dir(void) {
    auto d = cdc::log_dir();
    return d ? cstr(*d) : nullptr;
}
int cdc_write_log(const char **lines, size_t n, char **out, cdc_err_t *e) {
    if (e)
        cdc::err_init(e);
    if (out)
        *out = nullptr;
    auto d = cdc::log_dir();
    if (!d) {
        err(e, kErrIo, "cannot determine log dir");
        return kErrIo;
    }
    char *st = cdc_stamp();
    if (!st) {
        err(e, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    std::string stamp(st);
    free(st);
    std::string p;
    for (int i = 0;; ++i) {
        p = join(*d, stamp) + (i ? "_" + std::to_string(i) : "") + ".log";
        auto q = cdc::fopen_utf8(p.c_str(), "rb");
        if (!q)
            break;
        if (i >= 10000) {
            cdc::err_set(e, kErrIo, "too many log files for %s", stamp.c_str());
            return kErrIo;
        }
    }
    auto f = cdc::fopen_utf8(p.c_str(), "wb");
    if (!f) {
        cdc::err_set(e, kErrIo, "cannot create log file: %s", p.c_str());
        return kErrIo;
    }
    for (size_t i = 0; i < n; ++i) {
        std::string s = lines && lines[i] ? lines[i] : "";
        trim(s);
        if (fwrite(s.data(), 1, s.size(), f.get()) != s.size() || fputc('\n', f.get()) == EOF) {
            err(e, kErrIo, "cannot write log file");
            return kErrIo;
        }
    }
    if (fflush(f.get()) || ferror(f.get())) {
        err(e, kErrIo, "cannot flush log file");
        return kErrIo;
    }
    if (fclose(f.release()) != 0) {
        err(e, kErrIo, "cannot close log file");
        return kErrIo;
    }
    rotate(*d);
    if (out) {
        *out = cstr(p);
        if (!*out) {
            err(e, kErrNoMem, "out of memory");
            return kErrNoMem;
        }
    }
    return 0;
}
int cdc_append_line(const char *n, const char *l) {
    if (!valid(n))
        return 0;
    auto d = cdc::log_dir();
    if (!d)
        return 0;
    std::string p = join(*d, n);
    if (!appendable(p))
        return 0;
    auto f = cdc::fopen_utf8(p.c_str(), "ab");
    if (!f)
        return 0;
    const char *s = l ? l : "";
    size_t z = strlen(s);
    if (fwrite(s, 1, z, f.get()) == z && fputc('\n', f.get()) != EOF)
        fflush(f.get());
    return 0;
}
int cdc_logs_list(cdc_log_entry_t **out, size_t *n, cdc_err_t *e) {
    if (e)
        cdc::err_init(e);
    if (!out || !n) {
        err(e, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    *out = nullptr;
    *n = 0;
    auto d = cdc::log_dir();
    if (!d) {
        err(e, kErrIo, "cannot determine log dir");
        return kErrIo;
    }
    std::vector<Item> v;
    if (items(*d, v, false) != 0) {
        err(e, kErrIo, "cannot enumerate log dir");
        return kErrIo;
    }
    std::stable_sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.mtime > b.mtime; });
    auto a = (cdc_log_entry_t *)malloc(v.size() * sizeof(cdc_log_entry_t));
    if (!a && !v.empty()) {
        err(e, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        a[i].name = cstr(v[i].name);
        if (!a[i].name) {
            for (size_t j = 0; j < i; ++j)
                free(a[j].name);
            free(a);
            err(e, kErrNoMem, "out of memory");
            return kErrNoMem;
        }
#ifdef _WIN32
        a[i].size = 0;
        auto w = cdc::utf8_to_wide(v[i].path);
        WIN32_FILE_ATTRIBUTE_DATA x{};
        if (w && GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &x))
            a[i].size = ((uint64_t)x.nFileSizeHigh << 32) | x.nFileSizeLow;
#else
        struct stat s{};
        a[i].size = lstat(v[i].path.c_str(), &s) == 0 ? (uint64_t)s.st_size : 0;
#endif
        a[i].mtime = v[i].mtime;
    }
    *out = a;
    *n = v.size();
    return 0;
}
void cdc_logs_list_free(cdc_log_entry_t *a, size_t n) {
    if (!a)
        return;
    for (size_t i = 0; i < n; ++i)
        free(a[i].name);
    free(a);
}
int cdc_log_read(const char *n, char **out, cdc_err_t *e) {
    if (e)
        cdc::err_init(e);
    if (!valid(n)) {
        err(e, kErrInvalidArg, "日志名不合法");
        return kErrInvalidArg;
    }
    if (!out) {
        err(e, kErrInvalidArg, "out_text is NULL");
        return kErrInvalidArg;
    }
    *out = nullptr;
    auto d = cdc::log_dir();
    if (!d) {
        err(e, kErrIo, "cannot determine log dir");
        return kErrIo;
    }
    std::string p = join(*d, n);
    if (!regular(p)) {
        err(e, kErrNotFound, "日志不存在");
        return kErrNotFound;
    }
    auto f = cdc::fopen_utf8(p.c_str(), "rb");
    if (!f) {
        err(e, kErrNotFound, "日志不存在");
        return kErrNotFound;
    }
    std::vector<unsigned char> b;
    unsigned char x[8192];
    for (;;) {
        size_t z = fread(x, 1, sizeof x, f.get());
        b.insert(b.end(), x, x + z);
        if (z < sizeof x) {
            if (ferror(f.get())) {
                err(e, kErrIo, "cannot read log file");
                return kErrIo;
            }
            break;
        }
    }
    std::string s = lossy(b);
    *out = cstr(s);
    if (!*out) {
        err(e, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    return 0;
}
}
