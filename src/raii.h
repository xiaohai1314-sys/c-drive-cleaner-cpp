// raii.h — 资源的 RAII 包装（不用异常，构造/析构只做确定的事）。
//
//   UniqueFile          FILE*        -> fclose
//   UniqueHandle        HANDLE       -> CloseHandle（INVALID_HANDLE_VALUE 归一为空）
//   UniqueFindHandle    FindFirstFileW 句柄 -> FindClose
//   UniqueRegKey        HKEY         -> RegCloseKey
//   UniqueLocal         LocalAlloc 的内存 -> LocalFree
//   UniqueCoTaskMem<T>  CoTaskMemAlloc 的内存 -> CoTaskMemFree
//   UniqueInternet      HINTERNET    -> WinHttpCloseHandle（需包含 winhttp.h）
//   UniqueFd / UniqueDir（POSIX）    -> close / closedir
//   UniqueSqlite        sqlite3*     -> sqlite3_close
//   UniqueStmt          sqlite3_stmt* -> sqlite3_finalize
//   SqliteText          sqlite3_malloc 的串 -> sqlite3_free
//   UniqueJson          cJSON*       -> cJSON_Delete
//   JsonText            cJSON_Print* 的串 -> cJSON_free
//   Mutex / LockGuard   SRWLOCK / pthread_mutex（不依赖 <mutex>：
//                       mingw-w64 GCC 的 win32 线程模型没有 std::mutex）
//   ScopeExit           作用域退出时执行一段清理
#pragma once

#include <cstdio>
#include <memory>
#include <utility>

#include "cJSON.h"
#include "sqlite3.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace cdc {

// ---------------- C 运行时 ----------------

struct FileCloser {
    void operator()(std::FILE *f) const {
        if (f) std::fclose(f);
    }
};
using UniqueFile = std::unique_ptr<std::FILE, FileCloser>;

// 以 UTF-8 路径打开文件（Windows 走 _wfopen，其它平台 fopen）。失败返回空。
UniqueFile fopen_utf8(const char *path, const char *mode);

// ---------------- 第三方库 ----------------

struct SqliteCloser {
    void operator()(sqlite3 *db) const {
        if (db) sqlite3_close(db);
    }
};
using UniqueSqlite = std::unique_ptr<sqlite3, SqliteCloser>;

struct StmtFinalizer {
    void operator()(sqlite3_stmt *st) const {
        if (st) sqlite3_finalize(st);
    }
};
using UniqueStmt = std::unique_ptr<sqlite3_stmt, StmtFinalizer>;

struct SqliteFree {
    void operator()(void *p) const { sqlite3_free(p); }
};
using SqliteText = std::unique_ptr<char, SqliteFree>;

struct JsonDeleter {
    void operator()(cJSON *j) const {
        if (j) cJSON_Delete(j);
    }
};
using UniqueJson = std::unique_ptr<cJSON, JsonDeleter>;

struct JsonFree {
    void operator()(char *p) const {
        if (p) cJSON_free(p);
    }
};
using JsonText = std::unique_ptr<char, JsonFree>;

// ---------------- 平台资源 ----------------

#ifdef _WIN32

struct HandleCloser {
    using pointer = HANDLE;
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// CreateFileW 等以 INVALID_HANDLE_VALUE 表示失败的 API：归一为空句柄，
// 之后统一用 `if (!h)` 判断。
inline UniqueHandle adopt_handle(HANDLE h) {
    return UniqueHandle(h == INVALID_HANDLE_VALUE ? nullptr : h);
}

struct FindCloser {
    using pointer = HANDLE;
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) FindClose(h);
    }
};
using UniqueFindHandle = std::unique_ptr<void, FindCloser>;
inline UniqueFindHandle adopt_find_handle(HANDLE h) {
    return UniqueFindHandle(h == INVALID_HANDLE_VALUE ? nullptr : h);
}

struct RegKeyCloser {
    using pointer = HKEY;
    void operator()(HKEY k) const {
        if (k) RegCloseKey(k);
    }
};
using UniqueRegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegKeyCloser>;

struct LocalFreer {
    void operator()(void *p) const {
        if (p) LocalFree(p);
    }
};
template <class T>
using UniqueLocal = std::unique_ptr<T, LocalFreer>;

struct ModuleFreer {
    using pointer = HMODULE;
    void operator()(HMODULE m) const {
        if (m) FreeLibrary(m);
    }
};
using UniqueModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleFreer>;

#else  // POSIX

struct FdCloser {
    struct pointer {  // NullablePointer 包装 int fd（-1 为空）
        int fd = -1;
        pointer() = default;
        pointer(std::nullptr_t) {}
        explicit pointer(int f) : fd(f) {}
        explicit operator bool() const { return fd >= 0; }
        friend bool operator==(pointer a, pointer b) { return a.fd == b.fd; }
        friend bool operator!=(pointer a, pointer b) { return a.fd != b.fd; }
    };
    void operator()(pointer p) const {
        if (p.fd >= 0) ::close(p.fd);
    }
};
using UniqueFd = std::unique_ptr<int, FdCloser>;
inline UniqueFd adopt_fd(int fd) { return UniqueFd(FdCloser::pointer(fd)); }
inline int fd_of(const UniqueFd &f) { return f.get().fd; }

struct DirCloser {
    void operator()(DIR *d) const {
        if (d) closedir(d);
    }
};
using UniqueDir = std::unique_ptr<DIR, DirCloser>;

#endif

// ---------------- 同步 ----------------

class Mutex {
public:
    Mutex() = default;
    Mutex(const Mutex &) = delete;
    Mutex &operator=(const Mutex &) = delete;
#ifdef _WIN32
    void lock() { AcquireSRWLockExclusive(&lock_); }
    void unlock() { ReleaseSRWLockExclusive(&lock_); }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
#else
    void lock() { pthread_mutex_lock(&mu_); }
    void unlock() { pthread_mutex_unlock(&mu_); }

private:
    pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
#endif
};

class LockGuard {
public:
    explicit LockGuard(Mutex &m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    Mutex &m_;
};

// ---------------- 作用域清理 ----------------

template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() {
        if (active_) f_();
    }
    void release() { active_ = false; }
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;

private:
    F f_;
    bool active_ = true;
};

}  // namespace cdc
