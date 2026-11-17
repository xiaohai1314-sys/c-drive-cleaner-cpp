// safety.h — 路径安全与句柄删除，逐行移植自 Rust 版 desktop/src/safety.rs。
//
// 红线（与 Rust 版一字不改）：
//   - 所有规则清理一律按 untrusted=true 走：is_system_path 或不在 allowlist
//     内的一律禁删。scan 与 clean 必须过同一道门（execution_forbidden）。
//   - 删除一律走句柄：父目录逐级用 NtCreateFile 相对打开、扣住
//     FILE_SHARE_DELETE，中间目录出现重解析点即拒绝；不留路径式回退。
//   - 无法检查（元数据读失败）一律按不安全处理。
//
// 字符串内部一律 UTF-8。路径参数是 PathArg（可由 const char* / std::string /
// std::string_view 隐式构造；空指针按空串处理——C 版对 NULL 与 "" 的判定
// 结果在每个函数上都相同）。
// Windows 专属函数在 Linux 下返回 kErrIo（"仅支持 Windows"），不静默成功；
// 纯路径判定函数在两平台行为一致（供 Linux 自测）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util.h"

namespace cdc {

// 路径参数：空指针视为空串。
class PathArg {
public:
    PathArg(const char *s) : v_(s ? std::string_view(s) : std::string_view()) {}  // NOLINT
    PathArg(const std::string &s) : v_(s) {}                                      // NOLINT
    PathArg(std::string_view s) : v_(s) {}                                        // NOLINT
    std::string_view view() const { return v_; }
    operator std::string_view() const { return v_; }  // NOLINT
    std::string str() const { return std::string(v_); }

private:
    std::string_view v_;
};

// 允许的根目录列表（借用的 UTF-8 C 串）。
using RootList = std::span<const char *const>;

// ---------------- 共享小工具 ----------------

// 取环境变量（Windows 上走 GetEnvironmentVariableW，避免 getenv 的 ANSI
// 代码页损坏中文路径）。变量不存在返回 nullopt；存在但为空返回 ""。
std::optional<std::string> getenv_utf8(const char *name);

// 单路径段 glob 匹配（对应 Rust glob::Pattern::matches；调用方先把 pattern
// 与 text 都转小写再比）。'*' 匹配任意串（不含 '/'），'?' 匹配一个 UTF-8
// 码点，'[...]' 字符类支持范围与 '[!...]' 取反。
bool glob_match(std::string_view pattern, std::string_view text);

// glob pattern 是否合法（Rust glob::Pattern::new 的判定：字符类未闭合、
// 范围倒置即非法；非法 pattern 在 catalog / 规则加载时被丢弃）。
bool glob_valid(std::string_view pattern);

// 原生句柄：Windows 上就是 HANDLE，Linux 上无意义（仅占位）。不拥有。
using RawHandle = void *;

// 拥有的单个句柄（Windows CloseHandle；Linux 空壳）。move-only。
class OwnedHandle {
public:
    OwnedHandle() = default;
    explicit OwnedHandle(RawHandle h) : h_(h) {}
    OwnedHandle(OwnedHandle &&o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    OwnedHandle &operator=(OwnedHandle &&o) noexcept;
    OwnedHandle(const OwnedHandle &) = delete;
    OwnedHandle &operator=(const OwnedHandle &) = delete;
    ~OwnedHandle() { reset(); }
    RawHandle get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    void reset(RawHandle h = nullptr);
    // 交出所有权（不关闭）。
    RawHandle release() {
        RawHandle h = h_;
        h_ = nullptr;
        return h;
    }

private:
    RawHandle h_ = nullptr;
};

// 打开的父目录句柄链，按打开顺序排列；析构时逐个关闭。move-only。
class HandleChain {
public:
    HandleChain() = default;
    HandleChain(HandleChain &&) noexcept = default;
    HandleChain &operator=(HandleChain &&) noexcept = default;
    void push(OwnedHandle h) { handles_.push_back(std::move(h)); }
    bool empty() const { return handles_.empty(); }
    size_t size() const { return handles_.size(); }
    RawHandle back() const { return handles_.empty() ? nullptr : handles_.back().get(); }
    void clear() { handles_.clear(); }

private:
    std::vector<OwnedHandle> handles_;
};

// 句柄 I/O 原语（Windows 上是 GetFileSizeEx/ReadFile/WriteFile/
// FlushFileBuffers/SetFilePointerEx；Linux 下返回 kErrIo）。
int handle_file_size(RawHandle h, uint64_t &out_size, Error *err);
int handle_read(RawHandle h, uint8_t *buf, size_t want, size_t &out_got, Error *err);
int handle_write_all(RawHandle h, const uint8_t *buf, size_t len, Error *err);
int handle_sync(RawHandle h, Error *err);
int handle_seek_start(RawHandle h, Error *err);

// 目录遍历的一项。path 仅在回调期间有效。is_symlink 在 Windows 上指重解析点
// （含 junction），Linux 上指符号链接；遍历永不跟随链接。mtime 为 Unix 秒
// （取不到为 0）。attrs 在 Windows 上是 dwFileAttributes，Linux 上是 st_mode。
struct WalkEntry {
    std::string_view path;
    bool is_dir = false;
    bool is_symlink = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint32_t attrs = 0;
};
// 回调返回 0 继续，非 0 立即停止（返回值原样返回给调用方）。
using WalkFn = FunctionRef<int(const WalkEntry &)>;

// 递归遍历 root（min_depth=1 时不回调 root 本身，与 Rust WalkDir
// .min_depth(1) 一致；min_depth=0 时回调 root 本身）。
int walk_dir(const char *root, int min_depth, WalkFn fn, Error *err);

// ---------------- 纯路径判定（两平台一致） ----------------

// basename：把 '/' 换成 '\\'，去尾 '\\'，取最后一个 '\\' 之后。
std::string basename(PathArg path);
// normalize：与 Rust safety::normalize 逐行一致（去空白/引号、'/'→'\\'、
// 盘符大写、折叠连续 '\\'、补前导 '\\'、去尾 '\\'；"C:" → "C:\"）。
std::string normalize(PathArg path);

bool under_marker(PathArg path, std::span<const char *const> markers);
bool is_drive_root(PathArg path);
bool is_critical_file(PathArg path);
bool is_never_clean(PathArg path);
bool is_password_store(PathArg path);
bool path_syntax_error(PathArg path);
bool is_system_path(PathArg path);
bool is_protected_path(PathArg path);
bool is_user_library(PathArg path);
bool is_within(PathArg path, RootList roots);

// 重解析点：失败返回 false（Rust is_reparse_point 的 unwrap_or(false)）。
bool is_reparse_point(PathArg path);
// 同上，但保留元数据错误：成功返回 kOk 并填 out；文件不存在返回
// kErrNotFound；其它错误返回 kErrIo。删除/恢复路径应把无法检查视为不安全。
int reparse_point_status(PathArg path, bool &out_is_reparse, Error *err);

// 远程规则 allowlist：临时目录或 AppData catalog 内。
bool under_remote_allowlist(PathArg path);
// 总门禁：untrusted 时（所有规则一律如此）还要求非系统路径且在 allowlist 内。
bool execution_forbidden(PathArg path, bool untrusted);
// 用户主动勾选删除（大文件、重复副本、微信文件、粉碎）时的检查：允许
// 文档/桌面/下载，系统目录、密码库、盘符根、关键文件、永不清理名单、
// 重解析点仍然拦住。
bool explicit_delete_forbidden(PathArg path);

// ---------------- 句柄删除（Windows；Linux 下返回 kErrIo） ----------------

enum class DeleteOutcome { Deleted = 0, Skipped = 1, Failed = 2 };

// 在已打开的句柄上设置删除标记（句柄关闭时生效）。粉碎功能复用。
bool mark_delete(RawHandle file);

// 按经逐级句柄校验的父目录删除普通文件，不使用可被 junction 替换的
// 路径式回退。err 填失败原因（Skipped 时不填）。
DeleteOutcome delete_file_checked(PathArg path, RootList roots, bool untrusted, Error *err);

// 打开并固定一个普通目录（要求在 roots 内）。返回的链在用完前保持打开。
int pin_directory_checked(PathArg path, RootList roots, HandleChain &out_chain, Error *err);
// 安全创建目录树：已存在的祖先不带 FILE_SHARE_DELETE 打开，缺失的祖先相对
// 父目录创建；末段必须不存在。返回的链在使用期间保持打开。
int create_directory_tree_checked(PathArg path, HandleChain &out_chain, Error *err);
// 在已校验的父目录下创建一个新目录（FILE_CREATE，目标已存在即失败）。
int create_directory_checked(PathArg path, RootList roots, HandleChain &out_chain,
                             OwnedHandle &out_dir, Error *err);
// 在已校验的父目录下创建一个新普通文件（FILE_CREATE）。
int create_new_file_checked(PathArg path, RootList roots, HandleChain &out_chain,
                            OwnedHandle &out_file, Error *err);
// 在已校验的父目录下以只读方式打开一个普通文件。
int open_read_file_checked(PathArg path, RootList roots, HandleChain &out_chain,
                           OwnedHandle &out_file, Error *err);
// 用已校验的父句柄删除一个空普通目录。out_deleted=true 表示已删。
int remove_directory_checked(PathArg path, RootList roots, bool &out_deleted, Error *err);
// 递归删除迁移备份：不跟随任何重解析点，遇到链接子项直接保留（宁可删不
// 干净，也不跨过链接）。out_removed 为删除的条目数（含根目录本身）。
int remove_tree_checked(PathArg root, RootList allowed_roots, uint64_t &out_removed,
                        Error *err);
// 把普通目录重命名为同父目录下的新名（两端至少一端在 allowed_roots 内）。
int rename_directory_sibling_checked(PathArg from, PathArg to, RootList allowed_roots,
                                     Error *err);

// 自底向上删除空目录（不删 root 本身），never_clean 命中的目录保留。
// 返回删除的目录数。Linux 下删除恒失败，返回 0。
int remove_empty_dirs(const char *root);

}  // namespace cdc
