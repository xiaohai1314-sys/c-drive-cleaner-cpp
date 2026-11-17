// safety.cpp — 移植自 Rust 版 desktop/src/safety.rs（逐行对照，行为不变）。
//
// 注意点（对照 Rust 源码）：
//   - 大小写折叠只做 ASCII：Rust 的 to_lowercase()/to_uppercase() 是
//     Unicode 感知的，这里只折叠 A-Z；中文路径不受影响，非 ASCII 字母
//     （如 Ä/ä）的大小写差异不会被折叠——实测中未见此类路径，记为已知
//     微小偏差。
//   - trim 只处理 ASCII 空白（" \t\n\v\f\r"）；Rust trim() 还会去掉
//     Unicode 空白，路径里基本不出现。
//   - glob 匹配是单段匹配：pattern 与 text 都是单个路径段，'*' 不跨 '/'。
#include "safety.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "allowlist_data.h"
#include "cJSON.h"
#include "mft_util.h"
#include "raii.h"

#ifdef _WIN32
#include <windows.h>
typedef LONG NTSTATUS;  // 同类型重复 typedef 在 C++ 中合法（与 ntdef.h / bcrypt.h 兼容）
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cdc {
namespace {

// ---------------- 常量表（与 Rust 版逐字一致） ----------------

constexpr std::array<const char *, 8> kPasswordFilenames = {
    "login data", "login data-journal", "login data for account",
    "login data for account-journal", "logins.json", "logins-backup.json",
    "key4.db", "key3.db",
};
constexpr std::array<const char *, 7> kCriticalFiles = {
    "pagefile.sys", "hiberfil.sys", "swapfile.sys", "bootmgr",
    "ntldr", "ntuser.dat", "ntuser.dat.log",
};
constexpr std::array<const char *, 2> kNeverCleanRoots = {
    "\\recovery", "\\system volume information",
};
constexpr std::array<const char *, 1> kNeverCleanFiles = {"usmt.ppkg"};
constexpr std::array<const char *, 10> kLibraryNames = {
    "documents", "desktop", "downloads", "my documents", "文档", "桌面", "下载",
    "pictures", "music", "videos",
};

// ---------------- 内部字符串小工具 ----------------

constexpr std::string_view kAsciiSpace = " \t\n\v\f\r";

std::string lower_ascii(std::string s) {
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}

std::string_view trim_ascii(std::string_view s) {
    size_t b = s.find_first_not_of(kAsciiSpace);
    if (b == std::string_view::npos) return {};
    size_t e = s.find_last_not_of(kAsciiSpace);
    return s.substr(b, e - b + 1);
}

std::string_view trim_chars(std::string_view s, char c) {
    while (!s.empty() && s.front() == c) s.remove_prefix(1);
    while (!s.empty() && s.back() == c) s.remove_suffix(1);
    return s;
}

std::string_view trim_trailing(std::string_view s, std::string_view chars) {
    while (!s.empty() && chars.find(s.back()) != std::string_view::npos) s.remove_suffix(1);
    return s;
}

std::string slash_to_backslash(std::string_view s) {
    std::string out(s);
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

// 按 '\\' 或 '/' 切分，跳过空段。
std::vector<std::string> split_parts(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == '\\' || s[i] == '/')) i++;
        size_t start = i;
        while (i < s.size() && s[i] != '\\' && s[i] != '/') i++;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

template <size_t N>
bool in_table(std::string_view name, const std::array<const char *, N> &table) {
    return std::any_of(table.begin(), table.end(), [&](const char *t) { return name == t; });
}

// target == root，或 target 以 root + '\\' 开头（root 为空时不算命中）。
bool equal_or_under(std::string_view target, std::string_view root) {
    if (root.empty()) return false;
    if (target == root) return true;
    return target.size() > root.size() && target.substr(0, root.size()) == root &&
           target[root.size()] == '\\';
}

// normalize 后小写、去尾分隔符：大部分"是否在某目录下"判定的统一形式。
std::string folded(PathArg path, std::string_view trailing = "\\/") {
    return std::string(trim_trailing(lower_ascii(normalize(path)), trailing));
}

// ---------------- UTF-8 码点解码（供 glob '?' 与字符类用） ----------------

// 解码 s[i..] 的一个码点；非法字节按一个 U+FFFD 计（与 Rust lossy 策略一致）。
size_t utf8_decode_one(std::string_view s, size_t i, uint32_t &cp) {
    auto at = [&](size_t k) -> unsigned char {
        return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0;
    };
    unsigned char c = at(0);
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && (at(1) & 0xC0) == 0x80) {
        cp = (static_cast<uint32_t>(c & 0x1F) << 6) | (at(1) & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && (at(1) & 0xC0) == 0x80 && (at(2) & 0xC0) == 0x80) {
        cp = (static_cast<uint32_t>(c & 0x0F) << 12) | (static_cast<uint32_t>(at(1) & 0x3F) << 6) |
             (at(2) & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && (at(1) & 0xC0) == 0x80 && (at(2) & 0xC0) == 0x80 &&
        (at(3) & 0xC0) == 0x80) {
        cp = (static_cast<uint32_t>(c & 0x07) << 18) | (static_cast<uint32_t>(at(1) & 0x3F) << 12) |
             (static_cast<uint32_t>(at(2) & 0x3F) << 6) | (at(3) & 0x3F);
        return 4;
    }
    cp = 0xFFFD;
    return 1;
}

// 匹配 "[...]" 字符类：p 指向 '['；返回是否匹配并把 p 移到 ']' 之后。
bool glob_class(std::string_view pat, size_t &p, uint32_t cp) {
    p++;  // 跳过 '['
    bool negate = false;
    if (p < pat.size() && pat[p] == '!') {
        negate = true;
        p++;
    }
    bool matched = false, first = true;
    while (p < pat.size() && (pat[p] != ']' || first)) {
        uint32_t lo;
        p += utf8_decode_one(pat, p, lo);
        first = false;
        if (p + 1 < pat.size() && pat[p] == '-' && pat[p + 1] != ']') {
            uint32_t hi;
            p++;  // 跳过 '-'
            p += utf8_decode_one(pat, p, hi);
            if (lo <= cp && cp <= hi) matched = true;
        } else if (lo == cp) {
            matched = true;
        }
    }
    if (p < pat.size() && pat[p] == ']') p++;
    return negate ? !matched : matched;
}

// tail：normalize 后去盘符、小写、补前导 '\\'。
std::string tail_of(PathArg path) {
    std::string norm = normalize(path);
    std::string_view rest = norm;
    if (rest.size() >= 2 && rest[1] == ':') rest.remove_prefix(2);
    std::string out = lower_ascii(slash_to_backslash(rest));
    if (out.empty() || out[0] != '\\') out.insert(out.begin(), '\\');
    return out;
}

bool is_under(std::string_view tail_text, const char *marker) {
    std::string m = lower_ascii(marker);
    if (m.empty() || m[0] != '\\') m.insert(m.begin(), '\\');
    return tail_text == m || (tail_text.size() > m.size() &&
                              tail_text.substr(0, m.size()) == m && tail_text[m.size()] == '\\');
}

// env_roots：SystemRoot、WINDIR、ProgramFiles、ProgramFiles(x86)、ProgramData。
std::vector<std::string> env_roots() {
    std::vector<std::string> out;
    for (const char *name :
         {"SystemRoot", "WINDIR", "ProgramFiles", "ProgramFiles(x86)", "ProgramData"}) {
        if (std::optional<std::string> v = getenv_utf8(name)) out.push_back(lower_ascii(normalize(*v)));
    }
    return out;
}

// user_library_roots：4 个 base × 10 个库名。
std::vector<std::string> user_library_roots() {
    static constexpr const char *kBases[] = {"USERPROFILE", "HOME", "OneDrive", "OneDriveConsumer"};
    static constexpr const char *kNames[] = {
        "Documents", "Desktop", "Downloads", "My Documents", "文档", "桌面", "下载",
        "Pictures", "Music", "Videos",
    };
    std::vector<std::string> out;
    for (const char *b : kBases) {
        std::optional<std::string> base = getenv_utf8(b);
        if (!base) continue;
        std::string bt(trim_trailing(*base, "\\"));
        for (const char *n : kNames) out.push_back(lower_ascii(normalize(bt + "\\" + n)));
    }
    return out;
}

// ---------------- AppData catalog ----------------

struct CatalogApp {
    std::string app;                                // 小写应用目录名
    std::vector<std::vector<std::string>> patterns;  // 每条：小写 glob 段
};
using CatalogMap = std::vector<CatalogApp>;

constexpr std::array<const char *, 2> kCatalogKeys = {"localappdata", "appdata"};

// 从嵌入的 appdata_path_allowlist.json 建 catalog（与 Rust catalog() 逐行一致：
// 只取 %LOCALAPPDATA% / %APPDATA% 开头的条目；首段含 glob 字符的整条丢弃；
// 其余段逐个解析，非法 pattern 的段丢弃）。
bool catalog_build(std::array<CatalogMap, 2> &maps) {
    UniqueJson root(cJSON_Parse(CDC_ALLOWLIST_JSON));
    if (!root) return false;
    const cJSON *paths = cJSON_GetObjectItemCaseSensitive(root.get(), "paths");
    if (!cJSON_IsArray(paths)) return false;
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, paths) {
        if (!cJSON_IsString(item) || !item->valuestring) continue;
        std::string fold = slash_to_backslash(item->valuestring);
        std::string lower = lower_ascii(fold);
        for (size_t k = 0; k < kCatalogKeys.size(); k++) {
            std::string marker = format("%%%s\\", kCatalogKeys[k]);
            if (lower.compare(0, marker.size(), marker) != 0) continue;
            std::vector<std::string> parts = split_parts(std::string_view(fold).substr(marker.size()));
            // Rust 这里是内层 for 的 continue（另一个 key 不可能匹配）。
            if (parts.empty() || parts[0].find_first_of("*?[") != std::string::npos) continue;
            // patterns = parts[1..]，逐段小写、非法 glob 的段丢弃。
            std::vector<std::string> segs;
            for (size_t i = 1; i < parts.size(); i++) {
                std::string seg = lower_ascii(parts[i]);
                if (glob_valid(seg)) segs.push_back(std::move(seg));
            }
            std::string app = lower_ascii(parts[0]);
            auto it = std::find_if(maps[k].begin(), maps[k].end(),
                                   [&](const CatalogApp &a) { return a.app == app; });
            if (it == maps[k].end()) {
                maps[k].push_back({std::move(app), {}});
                it = std::prev(maps[k].end());
            }
            it->patterns.push_back(std::move(segs));
            break;  // Rust 的 break：命中一个 key 即停
        }
    }
    return true;
}

// 进程级 catalog：首次使用时构建，之后只读。
struct Catalog {
    Mutex mu;
    bool ready = false;
    std::array<CatalogMap, 2> maps;
};
Catalog &catalog() {
    static Catalog c;
    LockGuard g(c.mu);
    if (!c.ready) {
        std::array<CatalogMap, 2> maps;
        if (catalog_build(maps)) {
            c.maps = std::move(maps);
            c.ready = true;
        }
    }
    return c;
}

bool catalog_matches(const std::vector<std::string> &parts, size_t key_idx) {
    if (parts.empty()) return false;
    Catalog &cat = catalog();
    if (!cat.ready) return false;
    std::string app = lower_ascii(parts[0]);
    for (const CatalogApp &a : cat.maps[key_idx]) {
        if (a.app != app) continue;
        for (const std::vector<std::string> &segs : a.patterns) {
            if (parts.size() < segs.size() + 1) continue;
            bool all = true;
            for (size_t s = 0; s < segs.size() && all; s++)
                all = glob_match(segs[s], lower_ascii(parts[1 + s]));
            if (all) return true;
        }
    }
    return false;
}

bool known_appdata_target(PathArg path) {
    std::string raw = slash_to_backslash(trim_ascii(path));
    std::string lower = lower_ascii(raw);
    for (size_t k = 0; k < kCatalogKeys.size(); k++) {
        std::string marker = format("%%%s\\", kCatalogKeys[k]);
        if (lower.compare(0, marker.size(), marker) != 0) continue;
        return catalog_matches(split_parts(std::string_view(raw).substr(marker.size())), k);
    }
    static constexpr const char *kEnvs[2] = {"LOCALAPPDATA", "APPDATA"};
    for (size_t k = 0; k < 2; k++) {
        std::optional<std::string> root_env = getenv_utf8(kEnvs[k]);
        if (!root_env) continue;
        std::string root = lower_ascii(normalize(*root_env));
        std::string target = lower_ascii(normalize(raw));
        bool under = target == root || (target.size() > root.size() &&
                                        target.compare(0, root.size(), root) == 0 &&
                                        target[root.size()] == '\\');
        if (under) {
            std::string_view relative = std::string_view(target).substr(root.size());
            while (!relative.empty() && (relative.front() == '\\' || relative.front() == '/'))
                relative.remove_prefix(1);
            return catalog_matches(split_parts(relative), k);
        }
    }
    return false;
}

}  // namespace

// ---------------- 共享小工具 ----------------

std::optional<std::string> getenv_utf8(const char *name) {
    if (!name) return std::nullopt;
#ifdef _WIN32
    std::optional<std::wstring> wname = utf8_to_wide(name);
    if (!wname) return std::nullopt;
    std::wstring buf(4096, L'\0');
    DWORD n = GetEnvironmentVariableW(wname->c_str(), buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return std::string();  // 存在但为空
    }
    if (n >= buf.size()) {  // 超长：按需分配再读一次
        buf.assign(n + 1, L'\0');
        n = GetEnvironmentVariableW(wname->c_str(), buf.data(), n + 1);
        if (n == 0) return std::nullopt;
    }
    buf.resize(n);
    return wide_to_utf8(buf);
#else
    const char *v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

bool glob_match(std::string_view pat, std::string_view text) {
    size_t p = 0, t = 0;
    size_t star_p = std::string_view::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pat.size() && pat[p] == '*') {
            while (p < pat.size() && pat[p] == '*') p++;  // 连续 '*' 合并
            star_p = p;
            star_t = t;
            if (p == pat.size()) return true;  // 尾 '*' 匹配剩余全部（段内无 '/'）
            continue;
        }
        if (p < pat.size() && pat[p] == '?' && text[t] != '/') {
            uint32_t cp;
            t += utf8_decode_one(text, t, cp);
            p++;
            continue;
        }
        if (p < pat.size() && pat[p] == '[') {
            uint32_t cp;
            size_t adv = utf8_decode_one(text, t, cp);
            size_t save = p;
            if (glob_class(pat, p, cp)) {
                t += adv;
                continue;
            }
            p = save;  // 字符类不匹配：回退到 '*' 回溯
        } else if (p < pat.size() && pat[p] == text[t]) {
            p++;
            t++;
            continue;
        }
        // 不匹配：回溯到最近的 '*'，让它多吃一个字节（最终看整串）。
        if (star_p != std::string_view::npos && star_t < text.size() && text[star_t] != '/') {
            p = star_p;
            t = ++star_t;
            continue;
        }
        return false;
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

// Rust glob::Pattern::new 在字符类未闭合、范围倒置（[z-a]）时返回 Err，
// 调用方用 filter_map 丢弃。本函数做同样判定。
bool glob_valid(std::string_view pat) {
    size_t p = 0;
    while (p < pat.size()) {
        if (pat[p] != '[') {
            p++;
            continue;
        }
        p++;
        if (p < pat.size() && pat[p] == '!') p++;
        if (p < pat.size() && pat[p] == ']') p++;  // 首个 ']' 是字面量
        bool closed = false, have_prev = false;
        uint32_t prev = 0;
        while (p < pat.size()) {
            if (pat[p] == ']') {
                closed = true;
                p++;
                break;
            }
            uint32_t cp;
            size_t adv = utf8_decode_one(pat, p, cp);
            if (pat[p] == '-' && p + adv < pat.size() && pat[p + adv] != ']') {
                uint32_t hi;
                size_t adv2 = utf8_decode_one(pat, p + adv + 1, hi);
                if (have_prev && prev > hi) return false;  // 倒置范围
                p += adv + 1 + adv2;
                have_prev = false;
                continue;
            }
            prev = cp;
            have_prev = true;
            p += adv;
        }
        if (!closed) return false;
    }
    return true;
}

// ---------------- basename / normalize ----------------

std::string basename(PathArg path) {
    std::string text = slash_to_backslash(path.view());
    std::string_view trimmed = trim_trailing(text, "\\");
    size_t last = trimmed.rfind('\\');
    return std::string(last == std::string_view::npos ? trimmed : trimmed.substr(last + 1));
}

std::string normalize(PathArg path) {
    std::string text = slash_to_backslash(trim_chars(trim_ascii(path), '"'));
    if (text.size() >= 2 && text[1] == ':') {
        char drive = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        // 折叠连续 '\\'：Rust 是 while contains("\\\\") replace，效果等同。
        std::string rest;
        for (size_t i = 2; i < text.size(); i++) {
            if (text[i] == '\\' && !rest.empty() && rest.back() == '\\') continue;
            rest += text[i];
        }
        if (rest.empty() || rest == "\\") return std::string{drive, ':', '\\'};
        if (rest[0] != '\\') rest.insert(rest.begin(), '\\');
        return std::string{drive, ':'} + std::string(trim_trailing(rest, "\\"));
    }
    return std::string(trim_trailing(text, "\\/"));
}

// ---------------- 路径判定 ----------------

bool under_marker(PathArg path, std::span<const char *const> markers) {
    std::string text = tail_of(path);
    return std::any_of(markers.begin(), markers.end(),
                       [&](const char *m) { return is_under(text, m); });
}

bool is_drive_root(PathArg path) {
    std::string norm = normalize(path);
    return (norm.size() == 2 && norm[1] == ':') ||
           (norm.size() == 3 && norm[1] == ':' && (norm[2] == '\\' || norm[2] == '/'));
}

bool is_critical_file(PathArg path) { return in_table(lower_ascii(basename(path)), kCriticalFiles); }

bool is_never_clean(PathArg path) {
    if (trim_ascii(path).empty()) return true;
    if (in_table(lower_ascii(basename(path)), kNeverCleanFiles)) return true;
    return under_marker(path, kNeverCleanRoots);
}

bool is_password_store(PathArg path) {
    return in_table(lower_ascii(basename(path)), kPasswordFilenames);
}

bool path_syntax_error(PathArg path) {
    std::string_view text = trim_ascii(path);
    if (text.empty()) return true;
    if (text.substr(0, 2) == "\\\\" || text.substr(0, 2) == "//") return true;
    std::string_view body = text;
    if (text.size() >= 2 && text[1] == ':') body.remove_prefix(2);
    if (body.find(':') != std::string_view::npos) return true;
    // 按 '\\' 或 '/' 切分，任一段为 ".." 即非法。
    for (const std::string &part : split_parts(text))
        if (part == "..") return true;
    return false;
}

bool is_system_path(PathArg path) {
    if (trim_ascii(path).empty() || is_drive_root(path) || is_critical_file(path)) return true;
    static constexpr const char *kMarkers[] = {
        "\\windows", "\\system volume information", "\\recovery", "\\boot", "\\$recycle.bin",
    };
    if (under_marker(path, kMarkers)) return true;
    std::string target = folded(path);
    for (const char *key : {"SystemRoot", "WINDIR"}) {
        std::optional<std::string> value = getenv_utf8(key);
        if (value && equal_or_under(target, folded(*value))) return true;
    }
    return false;
}

bool is_protected_path(PathArg path) {
    if (is_system_path(path)) return true;
    static constexpr const char *kMarkers[] = {
        "\\program files", "\\program files (x86)", "\\programdata", "\\perflogs",
    };
    if (under_marker(path, kMarkers)) return true;
    std::string target = folded(path);
    for (const std::string &root : env_roots())
        if (equal_or_under(target, trim_trailing(root, "\\/"))) return true;
    if (std::optional<std::string> profile = getenv_utf8("USERPROFILE"))
        return target == folded(*profile);
    return false;
}

bool is_user_library(PathArg path) {
    if (trim_ascii(path).empty()) return false;
    std::string target = folded(path, "\\");
    for (const std::string &root : user_library_roots()) {
        std::string_view rt = trim_trailing(root, "\\");
        if (target == rt || equal_or_under(target, rt)) return true;
    }
    // tail 兜底：C:\Users\<anyone>\<库名> 或 C:\Users\<库名>。
    std::vector<std::string> parts = split_parts(tail_of(path));
    if (parts.size() >= 3 && parts[0] == "users" && in_table(parts[2], kLibraryNames)) return true;
    return parts.size() >= 2 && parts[0] == "users" && in_table(parts[1], kLibraryNames);
}

bool is_within(PathArg path, RootList roots) {
    if (roots.empty()) return false;
    std::string target = folded(path);
    return std::any_of(roots.begin(), roots.end(),
                       [&](const char *r) { return equal_or_under(target, folded(r)); });
}

int reparse_point_status(PathArg path, bool &out_is_reparse, Error *err) {
    err_init(err);
    std::string p = path.str();
#ifdef _WIN32
    std::optional<std::wstring> wpath = utf8_to_wide(p);
    if (!wpath) {
        err_set(err, kErrUtf8, "invalid UTF-8 path");
        return kErrUtf8;
    }
    DWORD attrs = GetFileAttributesW(wpath->c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            err_set(err, kErrNotFound, "file not found: %s", p.c_str());
            return kErrNotFound;
        }
        err_set(err, kErrIo, "GetFileAttributesW failed (%lu): %s", static_cast<unsigned long>(code),
                p.c_str());
        return kErrIo;
    }
    out_is_reparse = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return kOk;
#else
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            err_set(err, kErrNotFound, "file not found: %s", p.c_str());
            return kErrNotFound;
        }
        err_set(err, kErrIo, "lstat failed: %s", p.c_str());
        return kErrIo;
    }
    out_is_reparse = S_ISLNK(st.st_mode);
    return kOk;
#endif
}

bool is_reparse_point(PathArg path) {
    bool out = false;
    return reparse_point_status(path, out, nullptr) == kOk && out;
}

bool under_remote_allowlist(PathArg path) {
    if (path_syntax_error(path)) return false;
    std::string normalized = normalize(path);
    if (is_system_path(normalized) || is_user_library(normalized)) return false;
    std::vector<std::string> roots;
    for (const char *key : {"TEMP", "TMP"}) {
        std::optional<std::string> v = getenv_utf8(key);
        if (v && !path_syntax_error(*v)) roots.push_back(std::move(*v));
    }
    if (std::optional<std::string> local = getenv_utf8("LOCALAPPDATA"))
        roots.push_back(std::string(trim_trailing(*local, "\\")) + "\\Temp");
    std::vector<const char *> root_ptrs;
    for (const std::string &r : roots) root_ptrs.push_back(r.c_str());
    if (is_within(normalized, root_ptrs)) return true;
    return known_appdata_target(normalized);
}

bool execution_forbidden(PathArg path, bool untrusted) {
    if (path_syntax_error(path)) return true;
    if (is_never_clean(path)) return true;
    if (is_reparse_point(path)) return true;
    if (is_password_store(path) || is_user_library(path)) return true;
    if (is_drive_root(path) || is_critical_file(path)) return true;
    return untrusted && (is_system_path(path) || !under_remote_allowlist(path));
}

bool explicit_delete_forbidden(PathArg path) {
    if (path_syntax_error(path)) return true;
    if (is_never_clean(path)) return true;
    if (is_reparse_point(path)) return true;
    if (is_password_store(path) || is_drive_root(path) || is_critical_file(path)) return true;
    return is_protected_path(path);
}

// ---------------- 目录遍历 ----------------
//
// 递归目录遍历（单一实现，走 util 的 enum_dir）：Windows 用
// NtQueryDirectoryFile 批量取，POSIX 用 opendir/readdir。不跟随链接、
// 回调失败即停、子目录递归与 min_depth 门限无关（门限只管回调）；
// 打不开的目录按 err 报告（缺失目录两平台都返回 kErrNotFound）。

namespace {

int walk_dir_rec(const std::string &dir, int depth, int min_depth, WalkFn fn, Error *err) {
#ifdef _WIN32
    constexpr char kSep = '\\';
#else
    constexpr char kSep = '/';
#endif
    bool trailing = !dir.empty() && (dir.back() == '\\' || dir.back() == '/');
    std::string full;
    int rc = enum_dir(dir, [&](const DirEntry &e) -> int {
        full.assign(dir);
        if (!trailing) full += kSep;
        full.append(e.name);
        if (depth >= min_depth) {
            int r = fn(WalkEntry{full, e.is_dir, e.is_symlink, e.size, e.mtime, e.attrs});
            if (r != 0) return r;
        }
        // 递归帧有自己的 full；本帧的 full 在子遍历期间不变。
        if (e.is_dir && !e.is_symlink) return walk_dir_rec(full, depth + 1, min_depth, fn, err);
        return 0;
    });
    if (rc == kErrNotFound) {
        err_set(err, kErrNotFound, "directory not found: %s", dir.c_str());
        return kErrNotFound;
    }
    if (rc == kErrIo) {
        err_set(err, kErrIo, "directory enumeration failed: %s", dir.c_str());
        return kErrIo;
    }
    return rc;
}

}  // namespace

int walk_dir(const char *root, int min_depth, WalkFn fn, Error *err) {
    err_init(err);
    if (!root || !fn) {
        err_set(err, kErrInvalidArg, "root/fn is NULL");
        return kErrInvalidArg;
    }
    if (min_depth <= 0) {
        // 先回调 root 本身（Rust WalkDir min_depth(0) 的行为）。
        bool is_reparse = false;
        Error e2;
        int rc2 = reparse_point_status(root, is_reparse, &e2);
        if (rc2 != kOk) {
            if (err) *err = e2;
            return rc2;
        }
        WalkEntry e;
        e.path = root;
        e.is_symlink = is_reparse;
#ifdef _WIN32
        if (std::optional<std::wstring> wroot = utf8_to_wide(root)) {
            DWORD a = GetFileAttributesW(wroot->c_str());
            if (a != INVALID_FILE_ATTRIBUTES) {
                e.attrs = a;
                e.is_dir = (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(wroot->c_str(), GetFileExInfoStandard, &fad)) {
                uint64_t ft = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                              fad.ftLastWriteTime.dwLowDateTime;
                e.mtime = mft_filetime_to_unix(ft);
                if (!e.is_dir)
                    e.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            }
        }
#else
        struct stat st;
        if (lstat(root, &st) == 0) {
            e.attrs = static_cast<uint32_t>(st.st_mode);
            e.is_dir = S_ISDIR(st.st_mode);
            if (S_ISREG(st.st_mode)) e.size = static_cast<uint64_t>(st.st_size);
            e.mtime = static_cast<uint64_t>(st.st_mtime);
        }
#endif
        int rc = fn(e);
        if (rc != 0) return rc;
    }
    return walk_dir_rec(root, 1, min_depth, fn, err);
}

// ---------------- 句柄与句柄 I/O ----------------

OwnedHandle &OwnedHandle::operator=(OwnedHandle &&o) noexcept {
    if (this != &o) {
        reset(o.h_);
        o.h_ = nullptr;
    }
    return *this;
}

void OwnedHandle::reset(RawHandle h) {
#ifdef _WIN32
    if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(h_));
#endif
    h_ = h;
}

#ifdef _WIN32

int handle_file_size(RawHandle h, uint64_t &out_size, Error *err) {
    err_init(err);
    if (!h) {
        err_set(err, kErrInvalidArg, "handle is NULL");
        return kErrInvalidArg;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(static_cast<HANDLE>(h), &size)) {
        err_set(err, kErrIo, "GetFileSizeEx failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return kErrIo;
    }
    out_size = static_cast<uint64_t>(size.QuadPart);
    return kOk;
}

int handle_read(RawHandle h, uint8_t *buf, size_t want, size_t &out_got, Error *err) {
    err_init(err);
    if (!h || !buf) {
        err_set(err, kErrInvalidArg, "NULL argument");
        return kErrInvalidArg;
    }
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(h), buf, static_cast<DWORD>(want), &got, nullptr)) {
        err_set(err, kErrIo, "ReadFile failed (%lu)", static_cast<unsigned long>(GetLastError()));
        return kErrIo;
    }
    out_got = got;
    return kOk;
}

int handle_write_all(RawHandle h, const uint8_t *buf, size_t len, Error *err) {
    err_init(err);
    if (!h || (!buf && len > 0)) {
        err_set(err, kErrInvalidArg, "NULL argument");
        return kErrInvalidArg;
    }
    size_t off = 0;
    while (off < len) {
        DWORD chunk = len - off > 0x40000000u ? 0x40000000u : static_cast<DWORD>(len - off);
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(h), buf + off, chunk, &written, nullptr)) {
            err_set(err, kErrIo, "WriteFile failed (%lu)", static_cast<unsigned long>(GetLastError()));
            return kErrIo;
        }
        off += written;
    }
    return kOk;
}

int handle_sync(RawHandle h, Error *err) {
    err_init(err);
    if (!h) {
        err_set(err, kErrInvalidArg, "handle is NULL");
        return kErrInvalidArg;
    }
    if (!FlushFileBuffers(static_cast<HANDLE>(h))) {
        err_set(err, kErrIo, "FlushFileBuffers failed (%lu)",
                static_cast<unsigned long>(GetLastError()));
        return kErrIo;
    }
    return kOk;
}

int handle_seek_start(RawHandle h, Error *err) {
    err_init(err);
    if (!h) {
        err_set(err, kErrInvalidArg, "handle is NULL");
        return kErrInvalidArg;
    }
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (!SetFilePointerEx(static_cast<HANDLE>(h), zero, nullptr, FILE_BEGIN)) {
        err_set(err, kErrIo, "SetFilePointerEx failed (%lu)",
                static_cast<unsigned long>(GetLastError()));
        return kErrIo;
    }
    return kOk;
}

#else  // 句柄 I/O 在 Linux 下不可用

namespace {
int windows_only(Error *err) {
    err_init(err);
    err_set(err, kErrIo, "file handles are Windows-only");
    return kErrIo;
}
}  // namespace

int handle_file_size(RawHandle, uint64_t &, Error *err) { return windows_only(err); }
int handle_read(RawHandle, uint8_t *, size_t, size_t &, Error *err) { return windows_only(err); }
int handle_write_all(RawHandle, const uint8_t *, size_t, Error *err) { return windows_only(err); }
int handle_sync(RawHandle, Error *err) { return windows_only(err); }
int handle_seek_start(RawHandle, Error *err) { return windows_only(err); }

#endif

// ---------------- Win32 句柄删除 ----------------

#ifdef _WIN32

namespace {

constexpr DWORD kDeleteAccess = 0x00010000u;
constexpr ULONG kFileRenameInfoClass = 10;
constexpr DWORD kFileReadAttributes = 0x00000080u;
constexpr DWORD kFileReadData = 0x00000001u;
constexpr DWORD kFileWriteAttributes = 0x00000100u;
constexpr DWORD kFileWriteData = 0x00000002u;
constexpr DWORD kFileAddFile = 0x00000002u;
constexpr DWORD kFileAddSubdirectory = 0x00000004u;
constexpr DWORD kFileTraverse = 0x00000020u;
constexpr DWORD kSynchronizeAccess = 0x00100000u;
constexpr DWORD kShareRead = 0x00000001u;
constexpr DWORD kShareWrite = 0x00000002u;
constexpr DWORD kShareAll = 0x00000007u;
constexpr ULONG kFileOpen = 1u;
constexpr ULONG kFileCreate = 2u;
constexpr ULONG kFileDirectoryFile = 0x00000001u;
constexpr ULONG kFileNonDirectoryFile = 0x00000040u;
constexpr ULONG kFileOpenReparsePoint = 0x00200000u;
constexpr ULONG kFileSynchronousIoNonalert = 0x00000020u;
constexpr ULONG kObjCaseInsensitive = 0x00000040u;
constexpr DWORD kFileFlagBackupSemantics = 0x02000000u;
constexpr DWORD kBaseDirAccess = kFileReadAttributes | kFileTraverse | kSynchronizeAccess;

}  // namespace
}  // namespace cdc

// 与 Rust extern "system" 块逐项对应（ntdll，未经 windows_sys 封装）。
struct CdcNtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
struct CdcNtObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    CdcNtUnicodeString *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};
struct CdcNtIoStatusBlock {
    union {
        LONG Status;
        PVOID Pointer;
    } u;
    ULONG_PTR Information;
};

extern "C" NTSTATUS NTAPI NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess,
                                       CdcNtObjectAttributes *ObjectAttributes,
                                       CdcNtIoStatusBlock *IoStatusBlock,
                                       PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                       ULONG ShareAccess, ULONG CreateDisposition,
                                       ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);
extern "C" NTSTATUS NTAPI NtSetInformationFile(HANDLE FileHandle,
                                               CdcNtIoStatusBlock *IoStatusBlock,
                                               PVOID FileInformation, ULONG Length,
                                               ULONG FileInformationClass);
extern "C" ULONG NTAPI RtlNtStatusToDosError(NTSTATUS Status);

namespace cdc {
namespace {

// 内部 io 种类：只为 delete_file_checked 的 Skipped/Failed 分流服务。
// NotPlain：目标存在但不是要求的"普通目录/普通文件"（重解析点或类型不符）。
enum class IoKind { Ok, NotFound, Denied, Invalid, Exists, NotPlain, Other };

// 父链中途的"不是普通目录"按拒绝处理（与 C 版 IO_DENIED 同义）。
IoKind as_denied(IoKind k) { return k == IoKind::NotPlain ? IoKind::Denied : k; }

IoKind io_kind_from_win32(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return IoKind::NotFound;
    case ERROR_ACCESS_DENIED:
        return IoKind::Denied;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
        return IoKind::Invalid;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return IoKind::Exists;
    default:
        return IoKind::Other;
    }
}

int kind_to_code(IoKind k) {
    switch (k) {
    case IoKind::Ok:
        return kOk;
    case IoKind::NotFound:
        return kErrNotFound;
    case IoKind::Invalid:
        return kErrInvalidArg;
    case IoKind::Exists:
        return kErrExists;
    default:
        return kErrIo;
    }
}

IoKind nt_kind(NTSTATUS status, Error *err, const char *what) {
    ULONG code = RtlNtStatusToDosError(status);
    IoKind k = io_kind_from_win32(code);
    err_set(err, kind_to_code(k), "%s failed: NTSTATUS 0x%08lX (Win32 %lu)", what,
            static_cast<unsigned long>(status), static_cast<unsigned long>(code));
    return k;
}

IoKind win32_kind(DWORD code, Error *err, const char *what) {
    IoKind k = io_kind_from_win32(code);
    err_set(err, kind_to_code(k), "%s failed (Win32 %lu)", what, static_cast<unsigned long>(code));
    return k;
}

// 取句柄元数据：是否目录 / 是否重解析点。
IoKind handle_is_dir_reparse(HANDLE h, bool &is_dir, bool &is_reparse, Error *err) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info))
        return win32_kind(GetLastError(), err, "GetFileInformationByHandle");
    is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    is_reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return IoKind::Ok;
}

// 句柄必须是"普通目录"（want_dir）或"普通文件"（!want_dir）；否则关掉并返回 Denied。
IoKind expect_plain(OwnedHandle &h, bool want_dir, Error *err, const char *fmt,
                    std::string_view what) {
    bool is_dir = false, is_reparse = false;
    IoKind k = handle_is_dir_reparse(static_cast<HANDLE>(h.get()), is_dir, is_reparse, err);
    if (k != IoKind::Ok) {
        h.reset();
        return k;
    }
    if (is_reparse || is_dir != want_dir) {
        h.reset();
        err_set(err, kErrWin32, fmt, std::string(what).c_str());
        return IoKind::NotPlain;
    }
    return IoKind::Ok;
}

// win32_path_parts：纯字符串解析（与 Rust 版逐行一致）。
IoKind win32_path_parts(PathArg path, std::string &root, std::vector<std::string> &parts,
                        Error *err) {
    std::string raw = slash_to_backslash(path.view());
    if (raw.size() < 3 || !std::isalpha(static_cast<unsigned char>(raw[0])) || raw[1] != ':' ||
        raw[2] != '\\' || path_syntax_error(raw)) {
        err_set(err, kErrInvalidArg, "expected a normalized absolute drive path: %s",
                path.str().c_str());
        return IoKind::Invalid;
    }
    std::string normalized = normalize(raw);
    root = normalized.substr(0, 3);
    parts = split_parts(std::string_view(normalized).substr(3));
    return IoKind::Ok;
}

IoKind open_drive_root(const std::string &root, DWORD access, OwnedHandle &out, Error *err) {
    std::optional<std::wstring> wide = utf8_to_wide(root);
    if (!wide) {
        err_set(err, kErrUtf8, "invalid UTF-8 path: %s", root.c_str());
        return IoKind::Invalid;
    }
    HANDLE h = CreateFileW(wide->c_str(), access, kShareAll, nullptr, OPEN_EXISTING,
                           kFileFlagBackupSemantics | kFileOpenReparsePoint, nullptr);
    if (h == INVALID_HANDLE_VALUE) return win32_kind(GetLastError(), err, "CreateFileW(drive root)");
    out.reset(h);
    return as_denied(expect_plain(out, true, err, "drive root is not a plain directory: %s", root));
}

IoKind open_relative(RawHandle parent, std::string_view name, DWORD access, ULONG options,
                     ULONG disposition, DWORD share, OwnedHandle &out, Error *err) {
    if (name.empty() || name.find_first_of("\\/:") != std::string_view::npos || name == "." ||
        name == "..") {
        err_set(err, kErrInvalidArg, "invalid relative name: %s", std::string(name).c_str());
        return IoKind::Invalid;
    }
    std::optional<std::wstring> wide = utf8_to_wide(std::string(name));
    if (!wide) {
        err_set(err, kErrUtf8, "invalid UTF-8 name: %s", std::string(name).c_str());
        return IoKind::Invalid;
    }
    if (wide->size() > (0xFFFFu - 2) / 2) {
        err_set(err, kErrInvalidArg, "path component too long: %s", std::string(name).c_str());
        return IoKind::Invalid;
    }
    CdcNtUnicodeString object_name{static_cast<USHORT>(wide->size() * 2),
                                static_cast<USHORT>(wide->size() * 2 + 2), wide->data()};
    CdcNtObjectAttributes oa{sizeof(CdcNtObjectAttributes), static_cast<HANDLE>(parent), &object_name,
                          kObjCaseInsensitive, nullptr, nullptr};
    CdcNtIoStatusBlock iosb{};
    HANDLE handle = nullptr;
    NTSTATUS status =
        NtCreateFile(&handle, access, &oa, &iosb, nullptr, 0, share, disposition,
                     options | kFileOpenReparsePoint | kFileSynchronousIoNonalert, nullptr, 0);
    if (status < 0) return nt_kind(status, err, "NtCreateFile");
    out.reset(handle);
    return IoKind::Ok;
}

IoKind open_directory_chain(PathArg path, DWORD final_access, HandleChain &out, Error *err) {
    std::string root;
    std::vector<std::string> components;
    IoKind k = win32_path_parts(path, root, components, err);
    if (k != IoKind::Ok) return k;
    HandleChain chain;
    OwnedHandle root_handle;
    k = open_drive_root(root, kBaseDirAccess | (components.empty() ? final_access : 0), root_handle,
                        err);
    if (k != IoKind::Ok) return k;
    chain.push(std::move(root_handle));
    for (size_t i = 0; i < components.size(); i++) {
        DWORD access = kBaseDirAccess | (i + 1 == components.size() ? final_access : 0);
        OwnedHandle child;
        k = open_relative(chain.back(), components[i], access, kFileDirectoryFile, kFileOpen,
                          kShareRead | kShareWrite, child, err);
        if (k != IoKind::Ok) return k;
        k = as_denied(expect_plain(child, true, err,
                                   "reparse point or non-directory in parent path: %s",
                                   components[i]));
        if (k != IoKind::Ok) return k;
        chain.push(std::move(child));
    }
    out = std::move(chain);
    return IoKind::Ok;
}

// parent 路径：normalize 后去掉最后一个段（"C:\foo" → "C:\"）。
std::optional<std::string> parent_of(PathArg path) {
    std::string norm = normalize(path);
    size_t last = norm.rfind('\\');
    if (last == std::string::npos) return std::nullopt;
    if (last == 2 && norm[1] == ':') return norm.substr(0, 3);
    return norm.substr(0, last);
}

IoKind open_parent_chain(PathArg path, RootList roots, DWORD parent_rights, HandleChain &out_chain,
                         std::string &out_name, Error *err) {
    if (!is_within(path, roots)) {
        err_set(err, kErrWin32, "path is outside the allowed roots: %s", path.str().c_str());
        return IoKind::Denied;
    }
    std::string name = basename(path);
    if (name.empty()) {
        err_set(err, kErrInvalidArg, "invalid file name: %s", path.str().c_str());
        return IoKind::Invalid;
    }
    std::optional<std::string> parent = parent_of(path);
    if (!parent) {
        err_set(err, kErrInvalidArg, "path has no parent: %s", path.str().c_str());
        return IoKind::Invalid;
    }
    IoKind k = open_directory_chain(*parent, parent_rights, out_chain, err);
    if (k == IoKind::Ok) out_name = std::move(name);
    return k;
}

bool clear_readonly_and_delete(HANDLE file) {
    FILE_BASIC_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &info, sizeof(info))) return false;
    if ((info.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0) return false;
    info.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
    if (!SetFileInformationByHandle(file, FileBasicInfo, &info, sizeof(info))) return false;
    return mark_delete(file);
}

// 在 roots 内经父链打开 name：dir/file、access、disposition、share 由调用方给，
// 打开后校验是普通目录（want_dir）或普通文件。
IoKind open_child_checked(PathArg path, RootList roots, DWORD parent_rights, DWORD access,
                          bool want_dir, ULONG disposition, DWORD share, HandleChain &out_chain,
                          OwnedHandle &out, const char *mismatch_fmt, Error *err) {
    HandleChain chain;
    std::string name;
    IoKind k = open_parent_chain(path, roots, parent_rights, chain, name, err);
    if (k != IoKind::Ok) return k;
    OwnedHandle h;
    k = open_relative(chain.back(), name, access, want_dir ? kFileDirectoryFile : kFileNonDirectoryFile,
                      disposition, share, h, err);
    if (k != IoKind::Ok) return k;
    k = expect_plain(h, want_dir, err, mismatch_fmt, path.view());
    if (k != IoKind::Ok) return k;
    out_chain = std::move(chain);
    out = std::move(h);
    return IoKind::Ok;
}

IoKind delete_plain_file(PathArg path, RootList roots, bool &out_deleted, Error *err) {
    HandleChain chain;
    OwnedHandle file;
    IoKind k = open_child_checked(path, roots, 0,
                                  kDeleteAccess | kFileReadAttributes | kFileWriteAttributes |
                                      kSynchronizeAccess,
                                  false, kFileOpen, kShareRead | kShareWrite, chain, file,
                                  "target is a reparse point or directory: %s", err);
    if (k != IoKind::Ok) return k;
    HANDLE h = static_cast<HANDLE>(file.get());
    out_deleted = mark_delete(h) || clear_readonly_and_delete(h);
    return IoKind::Ok;
}

// "父路径 + 段"（处理根目录尾 '\\'）。
std::string join_child(const std::string &parent, std::string_view name) {
    std::string out = parent;
    if (out.empty() || out.back() != '\\') out += '\\';
    out.append(name);
    return out;
}

// remove_tree_checked 的递归体：枚举用路径式（已 pin 住父链），删除一律重新走句柄。
IoKind remove_contents(const std::string &dir, RootList roots, uint64_t &removed, Error *err) {
    HandleChain chain;
    IoKind k = open_directory_chain(dir, 0, chain, err);
    if (k != IoKind::Ok) return k;
    bool is_dir = false, is_reparse = false;
    k = handle_is_dir_reparse(static_cast<HANDLE>(chain.back()), is_dir, is_reparse, err);
    if (k != IoKind::Ok) return k;
    if (is_reparse || !is_dir) {
        err_set(err, kErrWin32, "tree root is not a plain directory: %s", dir.c_str());
        return IoKind::Denied;
    }
    // 链在整个路径式枚举期间保持打开（防止路径段在枚举时被改名）；
    // 删除操作各自重新 pin 父链。
    std::optional<std::wstring> wpattern = utf8_to_wide(join_child(dir, "*"));
    if (!wpattern) {
        err_set(err, kErrUtf8, "invalid UTF-8 path: %s", dir.c_str());
        return IoKind::Invalid;
    }
    WIN32_FIND_DATAW fd;
    UniqueFindHandle find = adopt_find_handle(FindFirstFileW(wpattern->c_str(), &fd));
    if (!find) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) return IoKind::Ok;  // 空目录
        return win32_kind(code, err, "FindFirstFileW");
    }
    do {
        if (std::wcscmp(fd.cFileName, L".") == 0 || std::wcscmp(fd.cFileName, L"..") == 0) continue;
        std::optional<std::string> name = wide_to_utf8(fd.cFileName);
        if (!name) {
            err_set(err, kErrUtf8, "invalid file name");
            return IoKind::Other;
        }
        std::string child = join_child(dir, *name);
        DWORD attrs = fd.dwFileAttributes;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // 链接子项故意保留
        bool deleted = false;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            uint64_t sub = 0;
            k = remove_contents(child, roots, sub, err);
            if (k != IoKind::Ok) return k;
            removed += sub;
            Error e2;
            if (remove_directory_checked(child, roots, deleted, &e2) != kOk) {
                if (err) *err = e2;
                return IoKind::Other;
            }
        } else {
            Error e2;
            k = delete_plain_file(child, roots, deleted, &e2);
            if (k != IoKind::Ok) {
                if (err) *err = e2;
                return k;
            }
        }
        if (deleted) removed++;
    } while (FindNextFileW(find.get(), &fd));
    return IoKind::Ok;
}

}  // namespace

bool mark_delete(RawHandle file) {
    if (!file) return false;
    uint8_t info = 1;  // FILE_DISPOSITION_INFO{ DeleteFile: TRUE }，1 字节
    return SetFileInformationByHandle(static_cast<HANDLE>(file), FileDispositionInfo, &info, 1) != 0;
}

DeleteOutcome delete_file_checked(PathArg path, RootList roots, bool untrusted, Error *err) {
    err_init(err);
    if (execution_forbidden(path, untrusted)) return DeleteOutcome::Skipped;
    bool deleted = false;
    Error e2;
    IoKind k = delete_plain_file(path, roots, deleted, &e2);
    if (k == IoKind::Ok) {
        if (deleted) return DeleteOutcome::Deleted;
        err_set(err, kErrIo, "无法删除：%s", path.str().c_str());
        return DeleteOutcome::Failed;
    }
    if (k == IoKind::NotFound || k == IoKind::Denied || k == IoKind::NotPlain ||
        k == IoKind::Invalid)
        return DeleteOutcome::Skipped;
    err_set(err, kind_to_code(k), "%s：%s", path.str().c_str(), e2.message);
    return DeleteOutcome::Failed;
}

int pin_directory_checked(PathArg path, RootList roots, HandleChain &out_chain, Error *err) {
    err_init(err);
    if (path_syntax_error(path) || !is_within(path, roots)) {
        err_set(err, kErrWin32,
                "directory is outside the allowed roots or has invalid syntax: %s",
                path.str().c_str());
        return kErrWin32;
    }
    return kind_to_code(open_directory_chain(path, 0, out_chain, err));
}

int create_directory_tree_checked(PathArg path, HandleChain &out_chain, Error *err) {
    err_init(err);
    if (path_syntax_error(path)) {
        err_set(err, kErrInvalidArg, "invalid directory path: %s", path.str().c_str());
        return kErrInvalidArg;
    }
    std::string parent_path;
    std::vector<std::string> components;
    IoKind k = win32_path_parts(path, parent_path, components, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    if (components.empty()) {
        err_set(err, kErrExists, "a volume root cannot be used as a new migration target");
        return kErrExists;
    }
    HandleChain chain;
    OwnedHandle root_handle;
    k = open_drive_root(parent_path, kBaseDirAccess, root_handle, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    chain.push(std::move(root_handle));
    // 缺失的段：带 ADD_SUBDIRECTORY 重新打开父链，在其末端 FILE_CREATE。
    auto create_child = [&](const std::string &name, OwnedHandle &child) -> IoKind {
        HandleChain create_parent;
        IoKind ck = open_directory_chain(parent_path, kFileAddSubdirectory, create_parent, err);
        if (ck != IoKind::Ok) return ck;
        return open_relative(create_parent.back(), name, kBaseDirAccess, kFileDirectoryFile,
                             kFileCreate, kShareRead | kShareWrite, child, err);
    };
    for (size_t i = 0; i < components.size(); i++) {
        OwnedHandle child;
        if (i + 1 == components.size()) {
            k = create_child(components[i], child);  // 末段必须不存在
        } else {
            k = open_relative(chain.back(), components[i], kBaseDirAccess, kFileDirectoryFile,
                              kFileOpen, kShareRead | kShareWrite, child, err);
            if (k == IoKind::NotFound) k = create_child(components[i], child);
        }
        if (k != IoKind::Ok) return kind_to_code(k);
        k = expect_plain(child, true, err,
                         "directory component is a reparse point or non-directory: %s",
                         components[i]);
        if (k != IoKind::Ok) return kind_to_code(k);
        chain.push(std::move(child));
        parent_path = join_child(parent_path, components[i]);
    }
    out_chain = std::move(chain);
    return kOk;
}

int create_directory_checked(PathArg path, RootList roots, HandleChain &out_chain,
                             OwnedHandle &out_dir, Error *err) {
    err_init(err);
    IoKind k = open_child_checked(path, roots, kFileAddSubdirectory, kBaseDirAccess, true,
                                  kFileCreate, kShareRead | kShareWrite, out_chain, out_dir,
                                  "created directory is not an ordinary directory: %s", err);
    return k == IoKind::NotPlain ? kErrWin32 : kind_to_code(k);
}

int create_new_file_checked(PathArg path, RootList roots, HandleChain &out_chain,
                            OwnedHandle &out_file, Error *err) {
    err_init(err);
    IoKind k = open_child_checked(path, roots, kFileAddFile,
                                  kFileWriteData | kFileReadAttributes | kSynchronizeAccess, false,
                                  kFileCreate, kShareRead, out_chain, out_file,
                                  "created file is not an ordinary file: %s", err);
    return k == IoKind::NotPlain ? kErrWin32 : kind_to_code(k);
}

int open_read_file_checked(PathArg path, RootList roots, HandleChain &out_chain,
                           OwnedHandle &out_file, Error *err) {
    err_init(err);
    IoKind k = open_child_checked(path, roots, 0,
                                  kFileReadData | kFileReadAttributes | kSynchronizeAccess, false,
                                  kFileOpen, kShareRead, out_chain, out_file,
                                  "opened file is not an ordinary file: %s", err);
    return k == IoKind::NotPlain ? kErrWin32 : kind_to_code(k);
}

int remove_directory_checked(PathArg path, RootList roots, bool &out_deleted, Error *err) {
    err_init(err);
    out_deleted = false;
    HandleChain chain;
    std::string name;
    IoKind k = open_parent_chain(path, roots, 0, chain, name, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    OwnedHandle dir;
    k = open_relative(chain.back(), name, kDeleteAccess | kFileReadAttributes | kSynchronizeAccess,
                      kFileDirectoryFile, kFileOpen, kShareRead | kShareWrite, dir, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    bool is_dir = false, is_reparse = false;
    k = handle_is_dir_reparse(static_cast<HANDLE>(dir.get()), is_dir, is_reparse, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    if (is_reparse || !is_dir) return kOk;  // Rust 这里返回 Ok(false)，不是错误
    out_deleted = mark_delete(dir.get());
    return kOk;
}

int remove_tree_checked(PathArg root, RootList allowed_roots, uint64_t &out_removed, Error *err) {
    err_init(err);
    out_removed = 0;
    if (!is_within(root, allowed_roots)) {
        err_set(err, kErrWin32, "tree root is outside the allowed roots: %s", root.str().c_str());
        return kErrWin32;
    }
    uint64_t removed = 0;
    IoKind k = remove_contents(root.str(), allowed_roots, removed, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    bool deleted = false;
    int rc = remove_directory_checked(root, allowed_roots, deleted, err);
    if (rc != kOk) return rc;
    if (!deleted) {
        err_set(err, kErrIo, "tree could not be fully removed: %s", root.str().c_str());
        return kErrIo;
    }
    out_removed = removed + 1;
    return kOk;
}

int rename_directory_sibling_checked(PathArg from, PathArg to, RootList allowed_roots, Error *err) {
    err_init(err);
    auto not_siblings = [&] {
        err_set(err, kErrWin32, "rename endpoints are not permitted siblings: %s -> %s",
                from.str().c_str(), to.str().c_str());
        return kErrWin32;
    };
    if (path_syntax_error(from) || path_syntax_error(to) ||
        (!is_within(from, allowed_roots) && !is_within(to, allowed_roots)))
        return not_siblings();
    std::optional<std::string> from_parent = parent_of(from), to_parent = parent_of(to);
    if (!from_parent || !to_parent) {
        err_set(err, kErrInvalidArg, "rename endpoints have no parent: %s -> %s", from.str().c_str(),
                to.str().c_str());
        return kErrInvalidArg;
    }
    // Rust 这里是大小写敏感的 normalize 比较（不 lower）。
    std::string parent_n = normalize(*from_parent);
    if (parent_n != normalize(*to_parent)) return not_siblings();
    std::string from_name = basename(from), to_name = basename(to);
    if (from_name.empty() || to_name.empty()) {
        err_set(err, kErrInvalidArg, "invalid rename name");
        return kErrInvalidArg;
    }
    HandleChain chain;
    IoKind k = open_directory_chain(parent_n, kFileAddSubdirectory, chain, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    RawHandle parent_handle = chain.back();
    OwnedHandle source;
    k = open_relative(parent_handle, from_name, kDeleteAccess | kFileReadAttributes | kSynchronizeAccess,
                      kFileDirectoryFile, kFileOpen, kShareRead | kShareWrite, source, err);
    if (k != IoKind::Ok) return kind_to_code(k);
    k = expect_plain(source, true, err, "rename source is not a plain directory: %s", from.view());
    if (k == IoKind::NotPlain) return kErrWin32;
    if (k != IoKind::Ok) return kind_to_code(k);
    OwnedHandle probe;
    k = open_relative(parent_handle, to_name, kFileReadAttributes | kSynchronizeAccess, 0, kFileOpen,
                      kShareAll, probe, err);
    if (k == IoKind::Ok) {
        err_set(err, kErrExists, "rename target exists: %s", to.str().c_str());
        return kErrExists;
    }
    if (k != IoKind::NotFound) return kind_to_code(k);
    // FILE_RENAME_INFORMATION：变长 WCHAR 尾，偏移与 Rust 手工布局一致。
    struct RenamePrefix {
        uint8_t replace_if_exists;
        void *root_directory;
        uint32_t file_name_length;
    };
    std::optional<std::wstring> wide = utf8_to_wide(to_name);
    if (!wide) {
        err_set(err, kErrUtf8, "invalid UTF-8 name: %s", to.str().c_str());
        return kErrUtf8;
    }
    size_t name_offset = offsetof(RenamePrefix, file_name_length) + sizeof(uint32_t);
    size_t name_bytes = wide->size() * sizeof(wchar_t);
    size_t total_len = name_offset + name_bytes;
    std::vector<uint8_t> buffer(std::max(total_len, sizeof(RenamePrefix)), 0);
    RenamePrefix prefix{0, parent_handle, static_cast<uint32_t>(name_bytes)};
    std::memcpy(buffer.data(), &prefix, sizeof(prefix));
    std::memcpy(buffer.data() + name_offset, wide->data(), name_bytes);
    CdcNtIoStatusBlock iosb{};
    NTSTATUS status = NtSetInformationFile(static_cast<HANDLE>(source.get()), &iosb, buffer.data(),
                                           static_cast<ULONG>(total_len), kFileRenameInfoClass);
    if (status < 0) return kind_to_code(nt_kind(status, err, "NtSetInformationFile(rename)"));
    return kOk;
}

#else  // !_WIN32 — 句柄删除在 Linux 下不可用，返回错误，不静默成功

namespace {
int chains_windows_only(Error *err) {
    err_init(err);
    err_set(err, kErrIo, "handle chains are Windows-only");
    return kErrIo;
}
}  // namespace

bool mark_delete(RawHandle) { return false; }

DeleteOutcome delete_file_checked(PathArg path, RootList, bool untrusted, Error *err) {
    err_init(err);
    if (execution_forbidden(path, untrusted)) return DeleteOutcome::Skipped;
    err_set(err, kErrIo, "handle-relative delete is Windows-only: %s", path.str().c_str());
    return DeleteOutcome::Failed;
}

int pin_directory_checked(PathArg, RootList, HandleChain &, Error *err) {
    return chains_windows_only(err);
}
int create_directory_tree_checked(PathArg, HandleChain &, Error *err) {
    return chains_windows_only(err);
}
int create_directory_checked(PathArg, RootList, HandleChain &, OwnedHandle &, Error *err) {
    return chains_windows_only(err);
}
int create_new_file_checked(PathArg, RootList, HandleChain &, OwnedHandle &, Error *err) {
    return chains_windows_only(err);
}
int open_read_file_checked(PathArg, RootList, HandleChain &, OwnedHandle &, Error *err) {
    return chains_windows_only(err);
}
int remove_directory_checked(PathArg, RootList, bool &out_deleted, Error *err) {
    out_deleted = false;
    return chains_windows_only(err);
}
int remove_tree_checked(PathArg, RootList, uint64_t &out_removed, Error *err) {
    out_removed = 0;
    return chains_windows_only(err);
}
int rename_directory_sibling_checked(PathArg, PathArg, RootList, Error *err) {
    return chains_windows_only(err);
}

#endif  // _WIN32

// ---------------- remove_empty_dirs（两平台共用遍历，删除走句柄） ----------------

int remove_empty_dirs(const char *root) {
    std::vector<std::string> dirs;
    Error err;
    int rc = walk_dir(root, 1, [&](const WalkEntry &e) {
        if (e.is_dir && !e.is_symlink) dirs.emplace_back(e.path);
        return 0;
    }, &err);
    if (rc != kOk) return 0;
    // 深的先删：按路径长度降序（C 版 qsort 不稳定，这里稳定排序，结果相同）。
    std::stable_sort(dirs.begin(), dirs.end(),
                     [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
    std::string root_norm = normalize(root);
    const char *roots_arr[1] = {root};
    int removed = 0;
    for (const std::string &path : dirs) {
        if (normalize(path) == root_norm) continue;
        if (is_never_clean(path)) continue;
        bool deleted = false;
        if (remove_directory_checked(path, roots_arr, deleted, nullptr) == kOk && deleted) removed++;
    }
    return removed;
}

}  // namespace cdc
