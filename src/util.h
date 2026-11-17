// util.h — 基础层：错误码、UTF-8/UTF-16 转换、格式化、目录枚举、日志。
//
// 架构契约（docs/ARCHITECTURE.md §2，C++ 版）：
//   - 字符串内部一律 UTF-8（std::string），调 Win32 API 前再转 UTF-16
//     （std::wstring，本文件提供转换）。
//   - 动态数组用 std::vector / std::string，资源用 RAII（raii.h），
//     不手写 malloc/free。
//   - 不用异常：函数返回 int，0 = 成功（kOk），非 0 = 错误码；出错时填
//     cdc::Error，不许静默吞错。可能失败的取值返回 std::optional。
//   - 日志写 %LOCALAPPDATA%\C盘清理工具\logs\（与 Rust 版 logs.rs 同目录）。
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define CDC_PRINTF_FMT(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define CDC_PRINTF_FMT(fmt_idx, arg_idx)
#endif

namespace cdc {

// ---------------- 错误码 ----------------

inline constexpr int kOk = 0;

enum ErrCode : int {
    kErrOk = 0,          // 无错
    kErrNoMem = 1,       // 内存分配失败
    kErrInvalidArg = 2,  // 参数非法（空指针、越界、空串、非法文件名）
    kErrIo = 3,          // 文件 / 系统 I/O 失败
    kErrNotFound = 4,    // 文件 / 项不存在
    kErrOverflow = 5,    // 长度 / 容量溢出
    kErrUtf8 = 6,        // 非法 UTF-8 序列
    kErrUtf16 = 7,       // 非法 UTF-16 序列
    kErrWin32 = 8,       // Win32 API 失败（message 里带 GetLastError 码）
    kErrExists = 9,      // 目标已存在
};

// 码 + 一句话（UTF-8）。无错时 code == kOk 且 message 为空串。
// message 定长 256 字节（与 C 版一致：前端看到的错误文案截断规则不变）。
struct Error {
    int code = kOk;
    char message[256] = {};

    bool ok() const { return code == kOk; }
    void clear() {
        code = kOk;
        message[0] = '\0';
    }
};

// 允许 err 为空指针（调用方不关心错误详情时传 nullptr）。
void err_init(Error *err);
void err_set(Error *err, int code, const char *fmt, ...) CDC_PRINTF_FMT(3, 4);
void err_vset(Error *err, int code, const char *fmt, va_list ap);
const char *err_str(int code);  // 静态字符串，如 "out of memory"

// ---------------- 字符串 ----------------

// printf 风格格式化到 std::string（不截断）。
std::string format(const char *fmt, ...) CDC_PRINTF_FMT(1, 2);
std::string vformat(const char *fmt, va_list ap);

// 追加一个码点的 UTF-8 编码（调用方保证 cp 合法：非代理区且 <= U+10FFFF）。
void append_utf8(std::string &out, uint32_t cp);

// 容量检查版 reserve：size()+extra 溢出或超过 max_size() 时返回 kErrOverflow，
// 否则 reserve 并返回 kOk。不用异常，供处理磁盘/网络给出的长度时防御溢出。
template <class Container>
int checked_reserve(Container &c, size_t extra) {
    if (extra > c.max_size() || c.size() > c.max_size() - extra) return kErrOverflow;
    c.reserve(c.size() + extra);
    return kOk;
}

// 带边界检查的元素指针：越界返回 nullptr。
template <class T>
T *ptr_at(std::vector<T> &v, size_t i) {
    return i < v.size() ? &v[i] : nullptr;
}
template <class T>
const T *ptr_at(const std::vector<T> &v, size_t i) {
    return i < v.size() ? &v[i] : nullptr;
}

// UTF-8 -> 宽串。Windows 上 wchar_t 是 UTF-16 码元，Linux 上是 UTF-32 码点；
// 两种平台对合法输入的转换都是精确的。空指针或非法 UTF-8（截断、超长编码、
// 代理区码点、>U+10FFFF）返回 nullopt；空串返回空 wstring。
std::optional<std::wstring> utf8_to_wide(const char *utf8);
inline std::optional<std::wstring> utf8_to_wide(const std::string &utf8) {
    return utf8_to_wide(utf8.c_str());
}

// 宽串 -> UTF-8。孤立代理对 / 超范围码点按 U+FFFD 处理（与 Rust
// from_utf16_lossy 同策略）。空指针返回 nullopt。
std::optional<std::string> wide_to_utf8(const wchar_t *wide);
inline std::optional<std::string> wide_to_utf8(const std::wstring &wide) {
    return wide_to_utf8(wide.c_str());
}

// 空指针安全的 C 串视图（nullptr -> 空视图）。
inline std::string_view sv(const char *s) { return s ? std::string_view(s) : std::string_view(); }

// ---------------- FunctionRef：不拥有的轻量回调引用 ----------------
//
// 替代 C 版的 "函数指针 + void *ctx"：可绑定 lambda / 函数，不分配内存，
// 不依赖 <functional>。被引用的可调用对象必须活到调用结束。
// 默认构造为空（operator bool 为 false），供 "回调可选" 的接口使用。
template <class Sig>
class FunctionRef;

template <class R, class... Args>
class FunctionRef<R(Args...)> {
public:
    FunctionRef() = default;
    FunctionRef(std::nullptr_t) {}
    template <class F,
              class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, FunctionRef> &&
                                       std::is_invocable_r_v<R, F &, Args...>>>
    FunctionRef(F &&f)  // NOLINT(google-explicit-constructor)
        : obj_(const_cast<void *>(static_cast<const void *>(&f))),
          call_([](void *obj, Args... args) -> R {
              return (*static_cast<std::remove_reference_t<F> *>(obj))(
                  std::forward<Args>(args)...);
          }) {}

    explicit operator bool() const { return call_ != nullptr; }
    R operator()(Args... args) const { return call_(obj_, std::forward<Args>(args)...); }

private:
    void *obj_ = nullptr;
    R (*call_)(void *, Args...) = nullptr;
};

// ---------------- 目录枚举 ----------------

// 单目录枚举项：name 为 UTF-8 文件名（不含路径，仅在回调期间有效）；
// is_symlink 在 Windows 上指重解析点，POSIX 上指符号链接；size（目录/链接为 0）；
// mtime 为 Unix 秒（取不到为 0）；attrs 在 Windows 上是 dwFileAttributes，
// POSIX 上是 st_mode。
struct DirEntry {
    std::string_view name;
    bool is_dir = false;
    bool is_symlink = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint32_t attrs = 0;
};

// 回调返回 0 继续，非 0 立即停止（该值原样返回给调用方）。
using DirEntryFn = FunctionRef<int(const DirEntry &)>;

// 枚举 dir 的直接子项（不含 "." ".."，不递归）。Windows 用
// NtQueryDirectoryFile + 64KB 缓冲批量取；POSIX 用 opendir/readdir + lstat。
// 返回 kOk（零项也是 OK）；目录不存在返回 kErrNotFound；其它打不开的情况
// 返回 kErrIo；dir 为空指针或 cb 为空返回 kErrInvalidArg；回调的非 0 值原样返回。
int enum_dir(const char *dir, DirEntryFn cb);
inline int enum_dir(const std::string &dir, DirEntryFn cb) { return enum_dir(dir.c_str(), cb); }

// 内部测试钩子：解析一包 FILE_DIRECTORY_INFORMATION（buf 长 n 字节），对每个
// 非 "." ".." 的项调 cb。scratch 是复用的名字缓冲。buf 损坏（越界/截断）返回
// kErrIo；回调的非 0 返回值原样返回。
int direnum_parse_batch(const uint8_t *buf, size_t n, DirEntryFn cb, std::string &scratch);

// ---------------- 日志 ----------------

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void log_set_level(LogLevel level);  // 默认 Info
LogLevel log_get_level();

// 写一行日志：stderr + 日志文件。低于当前级别的调用直接丢弃。
// 写日志文件失败不报错、不影响调用方（best-effort）。
void log(LogLevel level, const char *fmt, ...) CDC_PRINTF_FMT(2, 3);

// 日志目录：%LOCALAPPDATA%\C盘清理工具\logs（LOCALAPPDATA 未设置时退回
// 可执行文件所在目录）。目录不存在则创建；结果在进程内缓存。
// 无法确定/创建时返回 nullopt。
std::optional<std::string> log_dir();

// 缓存目录：%LOCALAPPDATA%\C盘清理工具\mftcache（规则同上，再退回 "."）。
// 目录不存在则创建。唯一实现在这里。
std::optional<std::string> cache_dir();

// 覆盖日志文件名（仅文件名；空串、含 '/'、'\\' 或 ".."、或长度 >= 256
// 返回 kErrInvalidArg 且保持原值）。默认文件名 cdc-YYYYMMDD.log。
int log_set_file(std::string_view filename);

}  // namespace cdc

// TRANSITIONAL：尚未迁移的模块仍用 C 版接口（cdc_buf_t / cdc_log 等），
// 由 legacy_c.h 以 cdc:: 新接口实现。最后一个模块迁移完后删除。
#include "legacy_c.h"
