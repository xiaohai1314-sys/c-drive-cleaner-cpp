// util.cpp — 基础层实现。见 util.h 的契约注释。
//
// 平台说明：Windows 上用 Win32 宽字符 API（MultiByteToWideChar /
// WideCharToMultiByte / GetEnvironmentVariableW），Linux 上用手写
// UTF-8 编解码（wchar_t 为 UTF-32 码点）。两种平台对合法输入的
// 转换都是精确的；非法输入按头文件约定的策略处理。
#include "util.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "raii.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cdc {

// ---------------- 错误码 ----------------

void err_init(Error *err) {
    if (err) err->clear();
}

void err_vset(Error *err, int code, const char *fmt, va_list ap) {
    if (!err) return;
    err->code = code;
    if (!fmt) {
        err->message[0] = '\0';
        return;
    }
    std::vsnprintf(err->message, sizeof err->message, fmt, ap);
}

void err_set(Error *err, int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    err_vset(err, code, fmt, ap);
    va_end(ap);
}

const char *err_str(int code) {
    switch (code) {
        case kErrOk: return "ok";
        case kErrNoMem: return "out of memory";
        case kErrInvalidArg: return "invalid argument";
        case kErrIo: return "I/O error";
        case kErrNotFound: return "not found";
        case kErrOverflow: return "size overflow";
        case kErrUtf8: return "invalid UTF-8";
        case kErrUtf16: return "invalid UTF-16";
        case kErrWin32: return "Win32 API failed";
        case kErrExists: return "already exists";
        default: return "unknown error";
    }
}

// ---------------- 字符串 ----------------

std::string vformat(const char *fmt, va_list ap) {
    std::string out;
    if (!fmt) return out;
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    std::vsnprintf(out.data(), out.size() + 1, fmt, ap);
    return out;
}

std::string format(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(fmt, ap);
    va_end(ap);
    return s;
}

void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

#ifndef _WIN32
namespace {

// 严格解码一个 UTF-8 序列：拒绝截断、超长编码、代理区码点、>U+10FFFF。
bool utf8_decode(const unsigned char *s, size_t len, uint32_t &cp, size_t &adv) {
    if (len == 0) return false;
    unsigned char c = s[0];
    if (c < 0x80) {
        cp = c;
        adv = 1;
        return true;
    }
    uint32_t v;
    size_t n;
    if (c < 0xC2) return false;  // 0x80-0xC1：续字节或超长编码
    if (c < 0xE0) {
        v = c & 0x1F;
        n = 2;
    } else if (c < 0xF0) {
        v = c & 0x0F;
        n = 3;
    } else if (c < 0xF5) {
        v = c & 0x07;
        n = 4;
    } else {
        return false;
    }
    if (len < n) return false;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return false;
        v = (v << 6) | (s[i] & 0x3F);
    }
    if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && v < 0x10000))
        return false;                               // 超长编码
    if (v >= 0xD800 && v <= 0xDFFF) return false;  // 代理区
    if (v > 0x10FFFF) return false;
    cp = v;
    adv = n;
    return true;
}

}  // namespace
#endif

std::optional<std::wstring> utf8_to_wide(const char *utf8) {
    if (!utf8) return std::nullopt;
#ifdef _WIN32
    if (utf8[0] == '\0') return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out.data(), n) <= 0)
        return std::nullopt;
    out.resize(static_cast<size_t>(n - 1));  // 去掉 API 写入的结尾 NUL
    return out;
#else
    const auto *s = reinterpret_cast<const unsigned char *>(utf8);
    size_t len = std::strlen(utf8), i = 0;
    std::wstring out;
    out.reserve(len);
    while (i < len) {
        uint32_t cp = 0;
        size_t adv = 0;
        if (!utf8_decode(s + i, len - i, cp, adv)) return std::nullopt;
        i += adv;
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
#endif
}

std::optional<std::string> wide_to_utf8(const wchar_t *wide) {
    if (!wide) return std::nullopt;
#ifdef _WIN32
    if (wide[0] == L'\0') return std::string();
    // 不传 WC_ERR_INVALID_CHARS：孤立代理对按 U+FFFD 处理。
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::nullopt;
    std::string out(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr) <= 0)
        return std::nullopt;
    out.resize(static_cast<size_t>(n - 1));
    return out;
#else
    std::string out;
    for (size_t i = 0; wide[i] != 0; i++) {
        auto cp = static_cast<uint32_t>(wide[i]);
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
        append_utf8(out, cp);
    }
    return out;
#endif
}

UniqueFile fopen_utf8(const char *path, const char *mode) {
    if (!path || !mode) return nullptr;
#ifdef _WIN32
    auto wpath = utf8_to_wide(path);
    auto wmode = utf8_to_wide(mode);
    if (!wpath || !wmode) return nullptr;
    return UniqueFile(_wfopen(wpath->c_str(), wmode->c_str()));
#else
    return UniqueFile(std::fopen(path, mode));
#endif
}

// ---------------- 日志 ----------------

namespace {

Mutex g_log_lock;
LogLevel g_log_level = LogLevel::Info;
std::string g_log_file;  // 为空 = 默认 cdc-YYYYMMDD.log
std::string g_log_dir_cache;
bool g_log_dir_cached = false;

const char *level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

bool local_now(std::tm &out) {
    std::time_t t = std::time(nullptr);
    if (t == static_cast<std::time_t>(-1)) return false;
#ifdef _WIN32
    return localtime_s(&out, &t) == 0;
#else
    return localtime_r(&t, &out) != nullptr;
#endif
}

// 逐级建目录（已存在则忽略失败）。
#ifdef _WIN32
void mkdir_p(std::wstring path) {
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == L'\\') {
            path[i] = L'\0';
            _wmkdir(path.c_str());
            path[i] = L'\\';
        }
    }
    _wmkdir(path.c_str());
}

// %LOCALAPPDATA%（未设置则可执行文件所在目录）+ suffix，建好目录后返回 UTF-8。
std::optional<std::string> app_subdir(const wchar_t *suffix) {
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 4096);
    std::wstring base;
    if (n == 0 || n >= 4096) {
        n = GetModuleFileNameW(nullptr, buf, 4096);
        if (n == 0 || n >= 4096) return std::nullopt;
        base.assign(buf, n);
        size_t slash = base.rfind(L'\\');
        if (slash == std::wstring::npos) return std::nullopt;
        base.resize(slash);
    } else {
        base.assign(buf, n);
    }
    base += suffix;
    if (base.size() + 1 > 4096) return std::nullopt;
    mkdir_p(base);
    return wide_to_utf8(base);
}
#else
void mkdir_p(std::string path) {
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            path[i] = '\0';
            mkdir(path.c_str(), 0755);
            path[i] = '/';
        }
    }
    mkdir(path.c_str(), 0755);
}

std::optional<std::string> app_subdir(const char *suffix) {
    std::string base;
    const char *env = std::getenv("LOCALAPPDATA");
    if (env && env[0] != '\0') {
        base = env;
    } else {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            base.assign(exe, static_cast<size_t>(n));
            size_t slash = base.rfind('/');
            if (slash != std::string::npos) base.resize(slash);
        } else {
            base = ".";
        }
    }
    base += suffix;
    mkdir_p(base);
    return base;
}
#endif

}  // namespace

void log_set_level(LogLevel level) {
    if (level < LogLevel::Debug) level = LogLevel::Debug;
    if (level > LogLevel::Error) level = LogLevel::Error;
    g_log_level = level;
}

LogLevel log_get_level() { return g_log_level; }

int log_set_file(std::string_view filename) {
    if (filename.empty()) return kErrInvalidArg;
    if (filename.find('/') != std::string_view::npos ||
        filename.find('\\') != std::string_view::npos ||
        filename.find("..") != std::string_view::npos)
        return kErrInvalidArg;
    if (filename.size() >= 256) return kErrInvalidArg;
    LockGuard lock(g_log_lock);
    g_log_file.assign(filename);
    return kOk;
}

std::optional<std::string> log_dir() {
    LockGuard lock(g_log_lock);
    if (!g_log_dir_cached) {
#ifdef _WIN32
        auto dir = app_subdir(L"\\C盘清理工具\\logs");
#else
        auto dir = app_subdir("/C盘清理工具/logs");
#endif
        if (!dir) return std::nullopt;
        g_log_dir_cache = std::move(*dir);
        g_log_dir_cached = true;
    }
    return g_log_dir_cache;
}

// 缓存目录 %LOCALAPPDATA%\C盘清理工具\mftcache（不存在则创建）。
// 目录名是"C盘清理工具"（一个"盘"），不要写成"C盘盘清理工具"
//（C 版早期 mft.c 的旧实现曾有此错，已统一到这里）。
std::optional<std::string> cache_dir() {
#ifdef _WIN32
    return app_subdir(L"\\C盘清理工具\\mftcache");
#else
    return app_subdir("/C盘清理工具/mftcache");
#endif
}

void log(LogLevel level, const char *fmt, ...) {
    if (level < g_log_level || !fmt) return;
    // 消息截断到 2047 字节（与 C 版的 char msg[2048] 一致）。
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    std::tm tmv{};
    bool have_time = local_now(tmv);
    std::string stamp = have_time ? format("%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900,
                                           tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                                           tmv.tm_min, tmv.tm_sec)
                                  : std::string("0000-00-00 00:00:00");
    std::string filename;
    {
        LockGuard lock(g_log_lock);
        std::fprintf(stderr, "[%s] [%s] %s\n", stamp.c_str(), level_name(level), msg);
        if (!g_log_file.empty())
            filename = g_log_file;
        else if (have_time)
            filename = format("cdc-%04d%02d%02d.log", tmv.tm_year + 1900, tmv.tm_mon + 1,
                              tmv.tm_mday);
    }
    if (filename.empty()) return;
    // 日志文件：best-effort，写失败只静默跳过。log_dir() 自己加锁。
    auto dir = log_dir();
    if (!dir) return;
    std::string path = *dir + "/" + filename;
    UniqueFile f = fopen_utf8(path.c_str(), "a");
    if (f) std::fprintf(f.get(), "[%s] [%s] %s\n", stamp.c_str(), level_name(level), msg);
}

// ---------------- 目录枚举 ----------------

namespace {

// FILETIME（100ns，1601 起）-> Unix 秒；与 mft_filetime_to_unix 同式，
// 基础层不依赖上层模块，故不 include mft_util.h。
uint64_t direnum_filetime_to_unix(uint64_t ft) {
    uint64_t secs = ft / 10000000ULL;
    return secs >= 11644473600ULL ? secs - 11644473600ULL : 0;
}

// UTF-16LE 码元串（nbytes 字节，不要求 NUL 结尾）-> UTF-8，追加进 out。
// 代理对正确组合；孤立代理对按 U+FFFD（与 wide_to_utf8 同策略）。
void utf16le_append_utf8(const uint8_t *bytes, size_t nbytes, std::string &out) {
    size_t i = 0;
    while (i + 2 <= nbytes) {
        uint32_t cp = static_cast<uint32_t>(bytes[i]) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 高代理：后跟低代理则组合，否则 U+FFFD。
            uint32_t lo = 0;
            if (i + 2 <= nbytes)
                lo = static_cast<uint32_t>(bytes[i]) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000u + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;  // 孤立低代理
        }
        append_utf8(out, cp);
    }
}

// FILE_DIRECTORY_INFORMATION 的定长头（64 字节）+ 变长 UTF-16LE 名。
// 字段偏移与 Windows 头文件一致；内核写出小端序。
constexpr size_t kDirenumHdrSize = 64;

uint32_t le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace

int direnum_parse_batch(const uint8_t *buf, size_t n, DirEntryFn cb, std::string &scratch) {
    if (!cb) return kErrInvalidArg;
    if (!buf && n != 0) return kErrInvalidArg;
    size_t off = 0;
    while (off < n) {
        if (n - off < kDirenumHdrSize) return kErrIo;  // 截断的头
        uint32_t next = le32(buf + off + 0);
        uint32_t attrs = le32(buf + off + 56);
        uint32_t name_len = le32(buf + off + 60);
        if (name_len > n - off - kDirenumHdrSize) return kErrIo;
        scratch.clear();
        utf16le_append_utf8(buf + off + 64, name_len, scratch);
        if (scratch != "." && scratch != "..") {
            DirEntry e;
            e.name = scratch;
            e.is_dir = (attrs & 0x10u) != 0;       // FILE_ATTRIBUTE_DIRECTORY
            e.is_symlink = (attrs & 0x400u) != 0;  // FILE_ATTRIBUTE_REPARSE_POINT
            e.size = e.is_dir ? 0 : le64(buf + off + 40);  // EndOfFile
            e.mtime = direnum_filetime_to_unix(le64(buf + off + 24));
            e.attrs = attrs;
            int rc = cb(e);
            if (rc != 0) return rc;
        }
        if (next == 0) break;
        // 链偏移在交付本项后才校验：坏链不吞掉前面已交付的有效项。
        if (next < kDirenumHdrSize || next > n - off) return kErrIo;
        off += next;
    }
    return kOk;
}

int enum_dir(const char *dir, DirEntryFn cb) {
    if (!dir || !cb) return kErrInvalidArg;
#ifdef _WIN32
    // NtQueryDirectoryFile 用 GetProcAddress 动态取（不用链接 ntdll）。
    using NtQueryFn = LONG(NTAPI *)(HANDLE FileHandle, HANDLE Event, void *ApcRoutine,
                                    void *ApcContext, void *IoStatusBlock,
                                    void *FileInformation, ULONG Length,
                                    ULONG FileInformationClass, BOOLEAN ReturnSingleEntry,
                                    void *FileName, BOOLEAN RestartScan);
    struct IoStatus {
        union {
            LONG Status;
            void *Pointer;
        } u;
        ULONG_PTR Information;
    };
    auto wdir = utf8_to_wide(dir);
    if (!wdir) return kErrUtf8;
    UniqueHandle h = adopt_handle(CreateFileW(
        wdir->c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return kErrNotFound;
        return kErrIo;
    }
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto pfn = ntdll ? reinterpret_cast<NtQueryFn>(
                           reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryDirectoryFile")))
                     : nullptr;
    if (!pfn) return kErrIo;
    std::vector<uint8_t> buf(65536);
    std::string scratch;
    BOOLEAN restart = TRUE;
    for (;;) {
        IoStatus iosb{};
        LONG st = pfn(h.get(), nullptr, nullptr, nullptr, &iosb, buf.data(),
                      static_cast<ULONG>(buf.size()), 1 /* FileDirectoryInformation */, FALSE,
                      nullptr, restart);
        restart = FALSE;
        if (static_cast<uint32_t>(st) == 0x80000006u) break;  // STATUS_NO_MORE_FILES
        if (st != 0) return kErrIo;
        if (iosb.Information == 0) break;  // 防御：成功但零字节
        int rc = direnum_parse_batch(buf.data(), static_cast<size_t>(iosb.Information), cb, scratch);
        if (rc != kOk) return rc;
    }
    return kOk;
#else
    UniqueDir d(opendir(dir));
    if (!d) return errno == ENOENT ? kErrNotFound : kErrIo;
    std::string path;
    size_t dl = std::strlen(dir);
    while (struct dirent *e = readdir(d.get())) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        // lstat 取 size/mtime/is_dir/is_symlink；路径拼进复用的缓冲。
        path.assign(dir, dl);
        if (dl > 0 && dir[dl - 1] != '/') path.push_back('/');
        path += e->d_name;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) continue;
        DirEntry de;
        de.name = e->d_name;
        de.is_dir = S_ISDIR(st.st_mode);
        de.is_symlink = S_ISLNK(st.st_mode);
        de.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
        de.mtime = static_cast<uint64_t>(st.st_mtime);
        de.attrs = static_cast<uint32_t>(st.st_mode);
        int rc = cb(de);
        if (rc != 0) return rc;
    }
    return kOk;
#endif
}

}  // namespace cdc
