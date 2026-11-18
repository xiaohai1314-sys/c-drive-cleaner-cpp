/* logs.cpp — 清理日志，移植自 Rust 版 desktop/src/logs.rs。 */
#include "logs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "raii.h"
#include "safety.h"
#include "timeutil.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace cdc {
namespace {

void err(Error *e, int code, const char *msg) {
    if (e)
        err_set(e, code, "%s", msg);
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
    auto w = utf8_to_wide(p);
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
    auto w = utf8_to_wide(p);
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
bool valid(std::string_view s) {
    if (s.empty())
        return false;
    return s.find('/') == s.npos && s.find('\\') == s.npos && s.find("..") == s.npos;
}
struct Item {
    std::string name, path;
    uint64_t mtime;
};
int items(const std::string &d, std::vector<Item> &v, bool logs) {
    return enum_dir(d, [&](const DirEntry &e) {
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
        auto w = utf8_to_wide(v[i].path);
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
}  // namespace

std::optional<std::string> app_data_dir() {
    auto d = log_dir();
    if (!d)
        return std::nullopt;
    std::string s = *d;
#ifdef _WIN32
    if (s.size() >= 5 && s.compare(s.size() - 5, 5, "\\logs") == 0)
        s.resize(s.size() - 5);
#else
    if (s.size() >= 5 && s.compare(s.size() - 5, 5, "/logs") == 0)
        s.resize(s.size() - 5);
#endif
    return s;
}

std::optional<std::string> logs_dir() { return log_dir(); }

int write_log(std::span<const std::string> lines, std::string *out, Error *e) {
    if (e)
        err_init(e);
    auto d = log_dir();
    if (!d) {
        err(e, kErrIo, "cannot determine log dir");
        return kErrIo;
    }
    std::string st = stamp();
    std::string p;
    for (int i = 0;; ++i) {
        p = join(*d, st) + (i ? "_" + std::to_string(i) : "") + ".log";
        auto q = fopen_utf8(p.c_str(), "rb");
        if (!q)
            break;
        if (i >= 10000) {
            err_set(e, kErrIo, "too many log files for %s", st.c_str());
            return kErrIo;
        }
    }
    auto f = fopen_utf8(p.c_str(), "wb");
    if (!f) {
        err_set(e, kErrIo, "cannot create log file: %s", p.c_str());
        return kErrIo;
    }
    for (const std::string &line : lines) {
        std::string s = line;
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
    if (out)
        *out = p;
    return kOk;
}

int append_line(std::string_view n, std::string_view l) {
    if (!valid(n))
        return kOk;
    auto d = log_dir();
    if (!d)
        return kOk;
    std::string p = join(*d, n);
    if (!appendable(p))
        return kOk;
    auto f = fopen_utf8(p.c_str(), "ab");
    if (!f)
        return kOk;
    if (fwrite(l.data(), 1, l.size(), f.get()) == l.size() && fputc('\n', f.get()) != EOF)
        fflush(f.get());
    return kOk;
}

int logs_list(std::vector<LogEntry> *out, Error *e) {
    if (e)
        err_init(e);
    if (!out) {
        err(e, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
    auto d = log_dir();
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
    out->reserve(v.size());
    for (auto &it : v) {
        LogEntry en;
        en.name = it.name;
#ifdef _WIN32
        en.size = 0;
        auto w = utf8_to_wide(it.path);
        WIN32_FILE_ATTRIBUTE_DATA x{};
        if (w && GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &x))
            en.size = ((uint64_t)x.nFileSizeHigh << 32) | x.nFileSizeLow;
#else
        struct stat s{};
        en.size = lstat(it.path.c_str(), &s) == 0 ? (uint64_t)s.st_size : 0;
#endif
        en.mtime = it.mtime;
        out->push_back(std::move(en));
    }
    return kOk;
}

int log_read(std::string_view n, std::string *out, Error *e) {
    if (e)
        err_init(e);
    if (!valid(n)) {
        err(e, kErrInvalidArg, "日志名不合法");
        return kErrInvalidArg;
    }
    if (!out) {
        err(e, kErrInvalidArg, "out_text is NULL");
        return kErrInvalidArg;
    }
    out->clear();
    auto d = log_dir();
    if (!d) {
        err(e, kErrIo, "cannot determine log dir");
        return kErrIo;
    }
    std::string p = join(*d, n);
    if (!regular(p)) {
        err(e, kErrNotFound, "日志不存在");
        return kErrNotFound;
    }
    auto f = fopen_utf8(p.c_str(), "rb");
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
    *out = lossy(b);
    return kOk;
}

}  // namespace cdc
