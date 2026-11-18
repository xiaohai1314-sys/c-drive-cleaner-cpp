/* shred.cpp — 文件粉碎，移植自 Rust 版 desktop/src/shred.rs。 */
#include "shred.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "safety.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/stat.h>
#endif

namespace cdc {
namespace {

cdc_hooks_t g_hooks{};

void shred_set_hooks_impl(const cdc_hooks_t *hooks) {
    g_hooks = hooks ? *hooks : cdc_hooks_t{};
}

bool cancelled() {
    return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, "shred");
}

void add_done(uint64_t done) {
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "shred", done, 0);
}

void report_error(ShredReport *r, std::string_view msg) {
    r->errors.emplace_back(msg);
}

void report_error_fmt(ShredReport *r, const char *fmt, std::string_view path) {
    report_error(r, format(fmt, std::string(path).c_str()));
}

#ifdef _WIN32
bool fill_random(uint8_t *buf, size_t n) {
    /* BCryptGenRandom（系统首选 RNG），与 Rust getrandom 同源。 */
    return BCryptGenRandom(NULL, buf, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

int shred_file(std::string_view path, uint32_t passes, uint64_t *out_size, Error *err) {
    auto wpath = utf8_to_wide(std::string(path));
    if (!wpath) {
        if (err) err_set(err, kErrUtf8, "invalid UTF-8 path: %s", std::string(path).c_str());
        return kErrUtf8;
    }
    HANDLE h = CreateFileW(
        wpath->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err)
            err_set(err, kErrIo, "无法安全打开文件：%s（Win32 %lu）",
                    std::string(path).c_str(), (unsigned long)GetLastError());
        return kErrIo;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        CloseHandle(h);
        if (err)
            err_set(err, kErrIo, "无法读取文件信息：%s", std::string(path).c_str());
        return kErrIo;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(h);
        if (err)
            err_set(err, kErrInvalidArg,
                    "不能粉碎符号链接或目录联接：%s", std::string(path).c_str());
        return kErrInvalidArg;
    }
    uint64_t size =
        ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    std::vector<uint8_t> buffer(1u << 20);
    int rc = kOk;
    for (uint32_t pass = 0; pass < (passes < 1 ? 1 : passes); pass++) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN)) {
            if (err)
                err_set(err, kErrIo, "%s：设置文件指针失败", std::string(path).c_str());
            rc = kErrIo;
            break;
        }
        uint64_t left = size;
        while (left > 0) {
            size_t block = left > (1u << 20) ? (1u << 20) : (size_t)left;
            if (!fill_random(buffer.data(), block)) {
                if (err)
                    err_set(err, kErrIo, "获取安全随机数失败");
                rc = kErrIo;
                break;
            }
            DWORD written = 0;
            if (!WriteFile(h, buffer.data(), (DWORD)block, &written, NULL) ||
                written != block) {
                if (err)
                    err_set(err, kErrIo, "覆盖写入失败：%s", std::string(path).c_str());
                rc = kErrIo;
                break;
            }
            left -= block;
        }
        if (rc != kOk) break;
        if (!FlushFileBuffers(h)) {
            if (err)
                err_set(err, kErrIo, "%s：刷盘失败", std::string(path).c_str());
            rc = kErrIo;
            break;
        }
    }
    if (rc == kOk && !mark_delete(h)) {
        if (err) err_set(err, kErrIo, "覆盖后无法删除：%s", std::string(path).c_str());
        rc = kErrIo;
    }
    CloseHandle(h);
    if (rc == kOk) *out_size = size;
    return rc;
}
#else
int shred_file(std::string_view path, uint32_t passes, uint64_t *out_size, Error *err) {
    (void)passes;
    (void)out_size;
    if (err)
        err_set(err, kErrIo, "文件粉碎仅支持 Windows：%s", std::string(path).c_str());
    return kErrIo;
}
#endif

void shred_one_file(std::string_view path, uint32_t passes, ShredReport *report,
                    uint64_t *done) {
    (*done)++;
    add_done(*done);
    if (is_protected_path(std::string(path))) {
        report->files_failed++;
        report_error_fmt(report, "为保护系统，拒绝粉碎：%s", path);
        return;
    }
    uint64_t size = 0;
    Error err;
    int rc = shred_file(path, passes, &size, &err);
    if (rc == kOk) {
        report->freed += size;
        report->files_ok++;
    } else {
        report->files_failed++;
        report_error(report, err.message[0] ? err.message : "粉碎失败");
    }
}

void shred_directory(std::string_view root, uint32_t passes, ShredReport *report,
                     uint64_t *done) {
    std::vector<std::string> files;
    std::vector<std::string> dirs;
    Error err;
    if (walk_dir(std::string(root).c_str(), 1,
                 [&](const WalkEntry &e) {
                     if (e.is_symlink) return 0;
                     (e.is_dir ? dirs : files).emplace_back(e.path);
                     return 0;
                 },
                 &err) != kOk) {
        /* 遍历失败：按空目录处理（与 Rust filter_map(ok) 跳过失败项一致，
         * 失败的条目不计入文件数）。 */
    }
    for (auto &f : files) {
        if (cancelled()) {
            report_error(report, "已取消");
            break;
        }
        shred_one_file(f, passes, report, done);
    }
    if (dirs.size() > 1)
        std::sort(dirs.begin(), dirs.end(),
                  [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    for (auto &d : dirs) {
#ifdef _WIN32
        auto w = utf8_to_wide(d);
        if (w)
            RemoveDirectoryW(w->c_str());
#else
        std::remove(d.c_str());
#endif
    }
    if (report->files_failed == 0) {
#ifdef _WIN32
        auto w = utf8_to_wide(std::string(root));
        if (w) {
            if (RemoveDirectoryW(w->c_str())) report->finished++;
        }
#else
        if (std::remove(std::string(root).c_str()) == 0) report->finished++;
#endif
    }
}

}  // namespace

void shred_set_hooks(const cdc_hooks_t *hooks) { shred_set_hooks_impl(hooks); }

int shred_paths(std::span<const std::string> paths, uint32_t passes, ShredReport *out,
                Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "out is NULL");
        return kErrInvalidArg;
    }
    *out = ShredReport{};
    if (passes < 1) passes = 1;
    if (passes > kShredMaxPasses) passes = kShredMaxPasses;
    uint64_t done = 0;
    for (const std::string &path : paths) {
        if (cancelled()) {
            report_error(out, "已取消");
            break;
        }
        if (path.empty()) {
            out->files_failed++;
            report_error_fmt(out, "文件不存在：%s", path);
            continue;
        }
        /* 存在性检查（不跟随链接：lstat / GetFileAttributesW）。 */
        bool is_reparse = false;
        Error e2;
        bool exists = reparse_point_status(path, is_reparse, &e2) == kOk;
        if (!exists) {
            out->files_failed++;
            report_error_fmt(out, "文件不存在：%s", path);
            continue;
        }
        if (is_reparse) {
            out->files_failed++;
            report_error_fmt(out, "不能粉碎符号链接或目录联接：%s", path);
            continue;
        }
        if (is_protected_path(path)) {
            out->files_failed++;
            report_error_fmt(out, "为保护系统，拒绝粉碎：%s", path);
            continue;
        }
        /* 是否目录。 */
        bool is_dir = false;
#ifdef _WIN32
        auto w = utf8_to_wide(path);
        if (w) {
            DWORD a = GetFileAttributesW(w->c_str());
            is_dir = a != INVALID_FILE_ATTRIBUTES &&
                     (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
#else
        struct stat st;
        is_dir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
        if (is_dir) {
            shred_directory(path, passes, out, &done);
        } else {
            shred_one_file(path, passes, out, &done);
        }
    }
    return kOk;
}

}  // namespace cdc
