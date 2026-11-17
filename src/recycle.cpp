// recycle.cpp — 移植自 Rust 版 desktop/src/recycle.rs。
#include "recycle.h"

#include <algorithm>
#include <optional>

#include "safety.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#endif

namespace cdc {
namespace {

bool is_regular_file(const char *path) {
    if (!path) return false;
#ifdef _WIN32
    std::optional<std::wstring> w = utf8_to_wide(path);
    if (!w) return false;
    DWORD a = GetFileAttributesW(w->c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

#ifdef _WIN32
constexpr size_t kBatch = 40;

// 一批路径 → 双 NUL 结尾的多串缓冲；任一路径非法 UTF-8 返回 nullopt。
std::optional<std::wstring> multi_sz(std::span<const std::string> batch) {
    std::wstring buf;
    for (const std::string &p : batch) {
        std::optional<std::wstring> w = utf8_to_wide(p);
        if (!w) return std::nullopt;
        buf += *w;
        buf += L'\0';
    }
    buf += L'\0';
    return buf;
}
#endif

}  // namespace

std::vector<std::string> send_to_recycle(std::span<const char *const> paths) {
    std::vector<std::string> failed, eligible;
    for (const char *path : paths) {
        std::string p = path ? path : "";
        if (!is_regular_file(path) || explicit_delete_forbidden(p))
            failed.push_back(std::move(p));
        else
            eligible.push_back(std::move(p));
    }
#ifdef _WIN32
    for (size_t base = 0; base < eligible.size(); base += kBatch) {
        std::span<const std::string> batch(eligible.data() + base,
                                           std::min(kBatch, eligible.size() - base));
        // 本批转换失败或 SHFileOperation 失败：整批计入失败（与 Rust 语义一致）。
        std::optional<std::wstring> from = multi_sz(batch);
        bool ok = false;
        if (from) {
            SHFILEOPSTRUCTW op{};
            op.wFunc = FO_DELETE;
            op.pFrom = from->c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;
            ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
        }
        if (!ok) failed.insert(failed.end(), batch.begin(), batch.end());
    }
#else
    // Linux 下没有回收站 API：全部计入失败，不做永久删除。
    for (std::string &p : eligible) failed.push_back(std::move(p));
#endif
    return failed;
}

}  // namespace cdc
