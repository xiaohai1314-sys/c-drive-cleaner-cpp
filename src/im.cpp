/* im.cpp — 微信 / QQ 专清，移植自 Rust 版 desktop/src/im.rs。
 * Windows 专属的路径探测（注册表、ini、SHGetFolderPathW）在 Linux 下
 * 返回空/回退值；classify / list_media / thumbnail / preview 的纯逻辑
 * 两平台一致。 */
#include "im.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "cdc_process.h"
#include "logs.h"
#include "raii.h"
#include "recycle.h"
#include "safety.h"
#include "system.h"
#include "timeutil.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cdc {
namespace {

cdc_hooks_t g_hooks{};

bool cancelled() {
    return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, "im_clean");
}

/* Reentrant tokenizer with strtok_r semantics, available on MSVC and POSIX. */
char *im_next_token(char *input, const char *delimiters, char **state) {
    if (!state || !delimiters) return NULL;
    char *cursor = input ? input : *state;
    if (!cursor) return NULL;
    while (*cursor && strchr(delimiters, *cursor)) cursor++;
    if (!*cursor) {
        *state = cursor;
        return NULL;
    }
    char *token = cursor;
    while (*cursor && !strchr(delimiters, *cursor)) cursor++;
    if (*cursor) *cursor++ = '\0';
    *state = cursor;
    return token;
}

/* ---------------- 扩展名表（与 Rust 版逐字一致） ---------------- */

constexpr std::array<std::string_view, 15> kVideoExt = {
    ".mp4", ".avi", ".mov", ".mkv", ".wmv", ".flv", ".m4v", ".3gp",
    ".mpeg", ".mpg", ".ts", ".webm", ".rmvb", ".vob", ".m2ts",
};
constexpr std::array<std::string_view, 14> kImageExt = {
    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".heic", ".tif",
    ".tiff", ".ico", ".raw", ".psd", ".svg", ".jfif",
};
constexpr std::array<std::string_view, 8> kAudioExt = {".silk", ".amr", ".mp3", ".m4a",
                                                       ".wav",  ".ogg", ".aac", ".aud"};
constexpr std::array<std::string_view, 18> kDocExt = {
    ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".txt",
    ".csv", ".rtf", ".ofd",  ".md",  ".wps", ".et",  ".dps",  ".dwg",
    ".dxf", ".xmind",
};
constexpr std::array<std::string_view, 9> kArchiveExt = {".zip", ".rar", ".7z", ".tar", ".gz",
                                                         ".bz2", ".xz",  ".iso", ".cab"};
constexpr std::array<std::string_view, 2> kFileExt = {".psd", ".ai"};
constexpr std::array<std::string_view, 15> kSkipExt = {
    ".db", ".db-wal", ".db-shm", ".sqlite", ".sqlite3", ".ini", ".key",
    ".xml", ".json", ".pfx", ".crt", ".exe", ".dll", ".cfg",
};
constexpr std::array<std::string_view, 8> kSkipDirParts = {
    "db_storage", "config", "account", "apputil",
    "login", "db", "database", "databases",
};

struct MediaDir {
    std::string_view kind;
    std::span<const std::string_view> names;
};
constexpr std::array<std::string_view, 3> kSnsNames = {"sns", "moments", "timeline"};
constexpr std::array<std::string_view, 2> kVideoNames = {"video", "videos"};
constexpr std::array<std::string_view, 7> kImageNames = {"image", "images", "img", "attach",
                                                         "msgattach", "custom_emotion", "emotion"};
constexpr std::array<std::string_view, 4> kAudioNames = {"voice", "audio", "ptt", "sound"};
constexpr std::array<std::string_view, 6> kFileNames = {"file", "files", "filerecv", "file_recv",
                                                        "fav", "favorite"};
constexpr std::array<std::string_view, 5> kLogNames = {"log", "logs", "xlog", "crash", "crashinfo"};
constexpr std::array<std::string_view, 5> kCacheNames = {"cache", "temp", "emoji", "emoticon",
                                                         "cdncomm"};
constexpr std::array<MediaDir, 7> kMediaDirs = {
    MediaDir{"sns", kSnsNames},   MediaDir{"video", kVideoNames},
    MediaDir{"image", kImageNames}, MediaDir{"audio", kAudioNames},
    MediaDir{"file", kFileNames},   MediaDir{"log", kLogNames},
    MediaDir{"cache", kCacheNames},
};

bool ext_in(std::string_view ext, std::span<const std::string_view> table) {
    for (auto e : table) {
        if (ext == e) return true;
    }
    return false;
}

/* ASCII 小写化（Rust to_lowercase 是 Unicode 全量；路径里的非 ASCII
 * 字母（如 Ä/ä）不会折叠——中文路径不受影响，已知微小偏差）。 */
std::string ascii_lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    }
    return out;
}

bool is_dir_utf8(std::string_view path) {
#ifdef _WIN32
    auto w = utf8_to_wide(std::string(path));
    if (!w) return false;
    DWORD a = GetFileAttributesW(w->c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(std::string(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string path_join_win(std::string_view a, std::string_view b) {
    std::string out(a);
    while (!b.empty() && (b.front() == '\\' || b.front() == '/'))
        b.remove_prefix(1);
    if (!out.empty() && out.back() != '\\' && out.back() != '/')
        out += '\\';
    out += b;
    return out;
}

/* ---------------- 根目录探测 ---------------- */

std::string documents_dir() {
#ifdef _WIN32
    wchar_t buffer[260];
    HRESULT hr = SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, buffer);
    if (hr >= 0) {
        size_t len = 0;
        while (len < 260 && buffer[len] != 0) len++;
        if (len > 0) {
            auto path = wide_to_utf8(std::wstring(buffer, len));
            if (path && is_dir_utf8(*path)) return *path;
        }
    }
#endif
    auto profile = getenv_utf8("USERPROFILE");
    return (profile ? *profile : "") + "\\Documents";
}

std::string trim_copy(std::string_view s) {
    while (!s.empty() && std::strchr(" \t\n\r", s.front()))
        s.remove_prefix(1);
    while (!s.empty() && std::strchr(" \t\n\r", s.back()))
        s.remove_suffix(1);
    return std::string(s);
}

/* reg.exe query HKCU\Software\Tencent\WeChat /v FileSavePath 的解析。
 * 输出经 CP_ACP 转 UTF-8（比 Rust 的 from_utf8_lossy 更准：中文路径
 * 不会变 U+FFFD）。 */
std::optional<std::string> wechat_registry_path_value() {
    std::string reg = system32_path("reg.exe");
    std::string so, se;
    long code = -1;
    Error e;
    int rc = run_command(reg, "query HKCU\\Software\\Tencent\\WeChat /v FileSavePath",
                         true, &so, &se, &code, &e);
    if (rc != kOk || code != 0 || so.empty())
        return std::nullopt;
    char *save = NULL;
    for (char *line = im_next_token(so.data(), "\n", &save); line;
         line = im_next_token(NULL, "\n", &save)) {
        /* 去行尾 \r。 */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
        if (!strstr(line, "FileSavePath")) continue;
        /* split_whitespace：parts[2..].join(" ")。 */
        std::vector<std::string> parts;
        char *p = line;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            parts.push_back(trim_copy(std::string_view(start, (size_t)(p - start))));
        }
        if (parts.size() >= 3) {
            std::string val;
            for (size_t i = 2; i < parts.size(); i++) {
                if (i > 2) val += " ";
                val += parts[i];
            }
            if (val != "MyDocument:")
                return val;
        }
        break;
    }
    return std::nullopt;
}

std::optional<std::string> resolve_wechat_path(std::optional<std::string> value) {
    if (!value) return std::nullopt;
    if (is_dir_utf8(*value)) return value;
    std::string sub = path_join_win(*value, "WeChat Files");
    if (is_dir_utf8(sub)) return sub;
    return std::nullopt;
}

bool utf8_valid_simple(const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t extra = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= n) return false;
        for (size_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += extra + 1;
    }
    return true;
}

std::optional<std::string> wechat_ini_path_value() {
    auto roaming = getenv_utf8("APPDATA");
    std::string dir = path_join_win(roaming ? *roaming : "",
                                    "Tencent\\WeChat\\All Users\\config");
    std::string ini = path_join_win(dir, "3ebffe94.ini");
    auto f = fopen_utf8(ini.c_str(), "rb");
    if (!f) return std::nullopt;
    std::string content;
    uint8_t chunk[8192];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f.get())) > 0)
        content.append((const char *)chunk, got);
    /* Rust read_to_string 要求合法 UTF-8，否则整个文件读不到。 */
    if (!utf8_valid_simple((const uint8_t *)content.data(), content.size()))
        return std::nullopt;
    char *save = NULL;
    for (char *line = im_next_token(content.data(), "\n", &save); line;
         line = im_next_token(NULL, "\n", &save)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        std::string key = trim_copy(std::string_view(line, (size_t)(eq - line)));
        std::string val = trim_copy(eq + 1);
        if (ascii_lower(key) == "filesavepath" && val != "MyDocument:")
            return val;
    }
    return std::nullopt;
}

std::vector<std::string> default_root_candidates(std::string_view kind) {
    std::vector<std::string> cands;
    auto roaming = getenv_utf8("APPDATA");
    std::string docs = documents_dir();
    if (kind == "wechat") {
        cands.push_back(path_join_win(docs, "WeChat Files"));
        cands.push_back(path_join_win(docs, "xwechat_files"));
        cands.push_back(path_join_win(roaming ? *roaming : "", "Tencent\\WeChat"));
        cands.push_back(path_join_win(roaming ? *roaming : "", "Tencent\\xwechat"));
    } else {
        cands.push_back(path_join_win(docs, "Tencent Files"));
        cands.push_back(path_join_win(roaming ? *roaming : "", "Tencent\\QQ"));
        cands.push_back(path_join_win(roaming ? *roaming : "", "Tencent\\QQNT"));
    }
    return cands;
}

void append_wechat_migration_candidate(std::vector<std::string> &cands,
                                       const std::string &value) {
    cands.push_back(value);
    cands.push_back(path_join_win(value, "WeChat Files"));
}

int im_migration_roots_impl(std::string_view kind, std::vector<std::string> *out) {
    out->clear();
    if (kind != "wechat" && kind != "qq") return kOk;
    *out = default_root_candidates(kind);
    if (kind == "wechat") {
        if (auto rv = wechat_registry_path_value())
            append_wechat_migration_candidate(*out, *rv);
        if (auto iv = wechat_ini_path_value())
            append_wechat_migration_candidate(*out, *iv);
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return kOk;
}

/* ---------------- 分类 ---------------- */

/* parts_of：'/' → '\'，小写，分段，去空。 */
std::vector<std::string> parts_of(std::string_view path) {
    std::string lower = ascii_lower(path);
    for (char &c : lower)
        if (c == '/') c = '\\';
    std::vector<std::string> parts;
    char *save = NULL;
    for (char *tok = im_next_token(lower.data(), "\\", &save); tok;
         tok = im_next_token(NULL, "\\", &save)) {
        if (tok[0] == '\0') continue;
        parts.emplace_back(tok);
    }
    return parts;
}

bool parts_start_with(const std::vector<std::string> &full,
                      const std::vector<std::string> &base) {
    if (full.size() <= base.size()) return false;
    for (size_t i = 0; i < base.size(); i++) {
        if (full[i] != base[i]) return false;
    }
    return true;
}

/* relative_parts：path 去掉 root 前缀（前缀须完整匹配）。 */
std::vector<std::string> relative_parts(std::string_view path, std::string_view root) {
    std::vector<std::string> full = parts_of(path);
    if (!root.empty()) {
        std::vector<std::string> base = parts_of(root);
        if (parts_start_with(full, base))
            full.erase(full.begin(), full.begin() + (ptrdiff_t)base.size());
    }
    return full;
}

/* basename 的扩展名（含 '.'，小写）；无 '.' 返回 ""。 */
std::string ext_of(std::string_view path) {
    std::string_view base = path;
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string_view::npos)
        base = path.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot == std::string_view::npos) return std::string();
    return ascii_lower(base.substr(dot));
}

std::string_view dir_kind_of(const std::vector<std::string> &dirs, std::string_view ext) {
    for (auto &g : kMediaDirs) {
        bool any = false;
        for (auto &d : dirs) {
            for (auto name : g.names) {
                if (d == name) {
                    any = true;
                    break;
                }
            }
            if (any) break;
        }
        if (!any) continue;
        /* 微信把视频封面图（.jpg）和视频放在同一个 Video 目录里，
         * 不能因为目录叫 Video 就把封面图也算成视频。 */
        if (g.kind == "video" && !ext_in(ext, kVideoExt))
            continue;
        return g.kind;
    }
    return std::string_view();
}

std::string_view received_subkind(std::string_view path) {
    std::string ext = ext_of(path);
    if (ext_in(ext, kDocExt)) return "doc";
    if (ext_in(ext, kVideoExt)) return "video";
    if (ext_in(ext, kImageExt)) return "image";
    if (ext_in(ext, kAudioExt)) return "music";
    if (ext_in(ext, kArchiveExt)) return "archive";
    return "other";
}

/* ---------------- 已扫描集合（缩略图协议白名单） ---------------- */

std::mutex g_scanned_lock;
std::vector<std::string> g_scanned; /* 只增不减 */

/* ---------------- 缩略图 ---------------- */

const char *image_mime(const uint8_t *head, size_t n) {
    if (n >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
        return "image/jpeg";
    if (n >= 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (n >= 6 && (memcmp(head, "GIF87a", 6) == 0 ||
                   memcmp(head, "GIF89a", 6) == 0))
        return "image/gif";
    if (n >= 12 && memcmp(head, "RIFF", 4) == 0 &&
        memcmp(head + 8, "WEBP", 4) == 0)
        return "image/webp";
    if (n >= 2 && head[0] == 'B' && head[1] == 'M') return "image/bmp";
    return nullptr;
}

int read_image(std::string_view path, const char **out_mime,
               std::vector<uint8_t> *out_data) {
    const uint64_t MAX = 64ULL * 1024ULL * 1024ULL;
    auto f = fopen_utf8(std::string(path).c_str(), "rb");
    if (!f) return 0;
    /* 文件大小（fseek/ftell 在 64MB 上限内够用；Linux 上用 stat 更稳）。 */
    uint64_t size = 0;
    bool size_ok = false;
#ifdef _WIN32
    auto w = utf8_to_wide(std::string(path));
    if (w) {
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &attr)) {
            DWORD a = attr.dwFileAttributes;
            if ((a & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
                size_ok = true;
            }
        }
    }
#else
    struct stat st;
    if (stat(std::string(path).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        size = (uint64_t)st.st_size;
        size_ok = true;
    }
#endif
    if (!size_ok || size > MAX)
        return 0;
    uint8_t head[16];
    size_t nread = fread(head, 1, sizeof(head), f.get());
    if (nread < 16)
        return 0;
    bool encrypted = false;
    if (image_mime(head, 16)) {
        encrypted = false;
    } else {
        uint8_t probe[16];
        for (int i = 0; i < 16; i++) probe[i] = (uint8_t)(head[i] ^ 0x05);
        if (!image_mime(probe, 16))
            return 0;
        encrypted = true;
    }
    if (fseek(f.get(), 0, SEEK_SET) != 0)
        return 0;
    std::vector<uint8_t> data(size > 0 ? (size_t)size : 1);
    size_t got = size > 0 ? fread(data.data(), 1, (size_t)size, f.get()) : 0;
    if (got != size)
        return 0;
    if (encrypted) {
        for (uint64_t i = 0; i < size; i++) data[i] ^= 0x05;
    }
    const char *mime = image_mime(data.data(), data.size());
    if (!mime)
        return 0;
    if (out_mime) *out_mime = mime;
    if (out_data) *out_data = std::move(data);
    return 1;
}

/* ---------------- 预览 ---------------- */

bool looks_binary(const uint8_t *data, size_t n) {
    if (n == 0) return false;
    size_t sample = n < 4096 ? n : 4096;
    size_t odd = 0;
    for (size_t i = 0; i < sample; i++) {
        uint8_t b = data[i];
        if (b < 0x09 || (b > 0x0D && b < 0x20)) odd++;
    }
    return odd * 100 / sample > 10;
}

void hex_preview(const uint8_t *data, size_t n, std::string &out) {
    size_t total = n < 512 ? n : 512; /* 32 行 × 16 字节 */
    size_t offset = 0;
    bool first = true;
    while (offset < total) {
        size_t chunk = total - offset < 16 ? total - offset : 16;
        char hex[48] = {};
        size_t hp = 0;
        for (size_t i = 0; i < chunk; i++) {
            if (i > 0) hex[hp++] = ' ';
            hp += (size_t)snprintf(hex + hp, sizeof(hex) - hp, "%02x",
                                   data[offset + i]);
        }
        char ascii[17];
        for (size_t i = 0; i < chunk; i++) {
            uint8_t b = data[offset + i];
            ascii[i] = (b >= 32 && b < 127) ? (char)b : '.';
        }
        ascii[chunk] = '\0';
        if (!first) out += "\n";
        first = false;
        char line[128];
        /* Rust: format!("{:<47}  {ascii}")——hex 段左对齐补到 47 列。 */
        snprintf(line, sizeof(line), "%-47s  %s", hex, ascii);
        out += line;
        offset += chunk;
    }
}

/* UTF-8 lossy 解码：非法序列按 U+FFFD 计（与 Rust from_utf8_lossy 的
 * maximal-subpart 替换基本一致：每段非法子序列一个 U+FFFD）。 */
void utf8_lossy_append(std::string &out, const uint8_t *s, size_t n) {
    static const unsigned char REPL[3] = {0xEF, 0xBF, 0xBD};
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) {
            out.push_back((char)c);
            i++;
            continue;
        }
        size_t extra = 0;
        uint32_t cp = 0;
        uint32_t min_cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
            min_cp = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
            min_cp = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
            min_cp = 0x10000;
        } else {
            out.append(reinterpret_cast<const char *>(REPL), 3);
            i++;
            continue;
        }
        /* 统计连续的后续字节数（maximal subpart）。 */
        size_t valid = 0;
        uint32_t vcp = cp;
        while (valid < extra && i + 1 + valid < n &&
               (s[i + 1 + valid] & 0xC0) == 0x80) {
            vcp = (vcp << 6) | (s[i + 1 + valid] & 0x3F);
            valid++;
        }
        if (valid == extra && vcp >= min_cp && vcp <= 0x10FFFF &&
            !(vcp >= 0xD800 && vcp <= 0xDFFF)) {
            out.append((const char *)s + i, extra + 1);
            i += extra + 1;
        } else {
            /* 非法：整个 maximal subpart（lead + 已有的后续字节）换一个
             * U+FFFD；若一个后续字节都没有，只消费 lead 字节。 */
            out.append(reinterpret_cast<const char *>(REPL), 3);
            i += 1 + valid;
        }
    }
}

int file_mtime_size(std::string_view path, uint64_t *out_size, uint64_t *out_mtime) {
#ifdef _WIN32
    auto w = utf8_to_wide(std::string(path));
    if (!w) return kErrUtf8;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &attr))
        return kErrNotFound;
    if ((attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return kErrInvalidArg;
    *out_size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
    uint64_t ft = ((uint64_t)attr.ftLastWriteTime.dwHighDateTime << 32) |
                  attr.ftLastWriteTime.dwLowDateTime;
    *out_mtime = ft >= 116444736000000000ULL
                     ? (ft - 116444736000000000ULL) / 10000000ULL
                     : 0;
    return kOk;
#else
    struct stat st;
    if (stat(std::string(path).c_str(), &st) != 0) return kErrNotFound;
    if (!S_ISREG(st.st_mode)) return kErrInvalidArg;
    *out_size = (uint64_t)st.st_size;
    *out_mtime = (uint64_t)st.st_mtime;
    return kOk;
#endif
}

bool kind_wanted(std::string_view kind, std::span<const std::string> kinds) {
    for (auto &k : kinds) {
        if (kind == k) return true;
    }
    return false;
}

}  // namespace

void im_set_hooks(const cdc_hooks_t *hooks) {
    g_hooks = hooks ? *hooks : cdc_hooks_t{};
}

int im_roots(std::string_view kind, std::vector<std::string> *out, Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
    if (kind != "wechat" && kind != "qq") return kOk;
    *out = default_root_candidates(kind);
    if (kind == "wechat") {
        if (auto rp = resolve_wechat_path(wechat_registry_path_value()))
            out->push_back(*rp);
        if (auto ip = resolve_wechat_path(wechat_ini_path_value()))
            out->push_back(*ip);
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    /* 只保留存在的目录。 */
    out->erase(std::remove_if(out->begin(), out->end(),
                              [](const std::string &p) { return !is_dir_utf8(p); }),
               out->end());
    return kOk;
}

int im_migration_roots(std::string_view kind, std::vector<std::string> *out, Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    return im_migration_roots_impl(kind, out);
}

bool im_is_program_root(std::string_view root) {
    if (root.empty()) return false;
    std::string r = ascii_lower(root);
    while (!r.empty() && (r.back() == '\\' || r.back() == '/'))
        r.pop_back();
    if (r.empty()) return false;
    for (const char *key : {"APPDATA", "LOCALAPPDATA"}) {
        auto base = getenv_utf8(key);
        if (!base) continue;
        std::string b = ascii_lower(*base);
        while (!b.empty() && (b.back() == '\\' || b.back() == '/'))
            b.pop_back();
        if (b.empty()) continue;
        if (r == b) return true;
        if (r.size() > b.size() && r.compare(0, b.size(), b) == 0 && r[b.size()] == '\\')
            return true;
    }
    return false;
}

bool im_classify(std::string_view path, std::string_view root, std::string *out_kind,
                 std::string *out_sub) {
    if (out_kind) out_kind->clear();
    if (out_sub) out_sub->clear();
    if (path.empty()) return false;
    std::vector<std::string> parts = relative_parts(path, root);
    if (parts.empty()) return false;
    std::string ext = ext_of(parts.back());
    if (ext_in(ext, kSkipExt))
        return false;
    /* 目录段命中跳过名单。 */
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        if (ext_in(parts[i], kSkipDirParts))
            return false;
    }
    if (im_is_program_root(root)) {
        std::string_view kind =
            (ext == ".log" || ext == ".xlog" || ext == ".dmp") ? "log" : "cache";
        if (out_kind) *out_kind = kind;
        if (out_sub) *out_sub = "";
        return true;
    }
    std::vector<std::string> dirs(parts.begin(), parts.end() - 1);
    std::string_view kind = dir_kind_of(dirs, ext);
    std::string kind_s;
    if (!kind.empty()) {
        kind_s = kind;
    } else if (ext.empty() || ext == ".dat" || ext == ".tmp") {
        return false;
    } else if (ext_in(ext, kVideoExt)) {
        kind_s = "video";
    } else if (ext_in(ext, kImageExt)) {
        kind_s = "image";
    } else if (ext_in(ext, kAudioExt)) {
        kind_s = "audio";
    } else if (ext_in(ext, kFileExt) || ext_in(ext, kArchiveExt) || ext_in(ext, kDocExt)) {
        kind_s = "file";
    } else if (ext == ".log" || ext == ".xlog" || ext == ".dmp") {
        kind_s = "log";
    } else {
        return false;
    }
    bool is_file_kind = kind_s == "file";
    if (out_kind) *out_kind = kind_s;
    if (out_sub) *out_sub = is_file_kind ? received_subkind(path) : "";
    return true;
}

int im_list_media(std::span<const std::string> roots, std::vector<ImRecord> *out,
                  Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
    uint64_t seq = 0;
    for (const std::string &root : roots) {
        if (!is_dir_utf8(root)) continue;
        walk_dir(root.c_str(), 1,
                 [&](const WalkEntry &e) {
                     if (e.is_dir || e.is_symlink) return 0;
                     std::string kind, sub;
                     if (!im_classify(e.path, root, &kind, &sub)) return 0;
                     ImRecord rec;
                     rec.path = e.path;
                     rec.size = e.size;
                     rec.mtime = e.mtime;
                     rec.kind = std::move(kind);
                     rec.sub = std::move(sub);
                     rec.month = month_label(e.mtime);
                     rec.name = basename(e.path);
                     rec.seq = seq++;
                     out->push_back(std::move(rec));
                     return 0;
                 },
                 nullptr);
    }
    std::stable_sort(out->begin(), out->end(), [](const ImRecord &a, const ImRecord &b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.seq < b.seq;
    });
    return kOk;
}

void im_remember_scanned(std::span<const ImRecord> records) {
    std::lock_guard<std::mutex> lock(g_scanned_lock);
    for (auto &r : records) {
        if (std::find(g_scanned.begin(), g_scanned.end(), r.path) == g_scanned.end())
            g_scanned.push_back(r.path);
    }
}

bool im_is_scanned(std::string_view path) {
    std::lock_guard<std::mutex> lock(g_scanned_lock);
    return std::find(g_scanned.begin(), g_scanned.end(), path) != g_scanned.end();
}

int im_thumbnail(std::string_view path, const char **out_mime,
                 std::vector<uint8_t> *out_data, Error *err) {
    if (err) err_init(err);
    if (out_mime) *out_mime = nullptr;
    if (path.empty()) return 0;
    int rc = read_image(path, out_mime, out_data);
    if (rc != 0) return rc;
    /* 朋友圈视频没有封面扩展名，同名的 `_t` 文件就是它的封面图。 */
    std::string alt = std::string(path) + "_t";
    return read_image(alt, out_mime, out_data);
}

int im_preview(std::string_view path, ImPreview *out, Error *err) {
    if (err) err_init(err);
    if (!out || path.empty()) {
        if (err) err_set(err, kErrInvalidArg, "NULL param");
        return kErrInvalidArg;
    }
    *out = ImPreview{};
    const uint64_t LIMIT = 64ULL * 1024ULL;
    uint64_t size = 0;
    bool meta_ok = false, is_file = false;
#ifdef _WIN32
    auto w = utf8_to_wide(std::string(path));
    if (w) {
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &attr)) {
            meta_ok = true;
            is_file = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
            size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
        }
    }
#else
    struct stat st;
    if (stat(std::string(path).c_str(), &st) == 0) {
        meta_ok = true;
        is_file = S_ISREG(st.st_mode);
        size = (uint64_t)st.st_size;
    }
#endif
    /* Rust 顺序：metadata 失败 → "读取失败"；不是文件 → "不是文件"。 */
    if (meta_ok && !is_file) {
        if (err) err_set(err, kErrInvalidArg, "不是文件");
        return kErrInvalidArg;
    }
    auto f = fopen_utf8(std::string(path).c_str(), "rb");
    if (!f) {
        if (err) err_set(err, kErrIo, "读取失败：%s", std::string(path).c_str());
        return kErrIo;
    }
    std::vector<uint8_t> buf((size_t)LIMIT);
    size_t got = fread(buf.data(), 1, (size_t)LIMIT, f.get());
    buf.resize(got);
    out->binary = looks_binary(buf.data(), got);
    if (out->binary) {
        hex_preview(buf.data(), got, out->text);
    } else {
        utf8_lossy_append(out->text, buf.data(), got);
    }
    out->truncated = size > LIMIT;
    out->size = size;
    return kOk;
}

int im_clean(std::string_view kind, std::span<const std::string> kinds,
             std::span<const std::string> paths, bool has_paths, DeleteReport *out,
             Error *err) {
    if (err) err_init(err);
    if (!out || kind.empty()) {
        if (err) err_set(err, kErrInvalidArg, "NULL param");
        return kErrInvalidArg;
    }
    *out = DeleteReport{};
    std::vector<std::string> roots;
    if (im_roots(kind, &roots, nullptr) != kOk) {
        if (err) err_set(err, kErrIo, "无法探测 %s 数据目录", std::string(kind).c_str());
        return kErrIo;
    }
    std::vector<ImRecord> chosen;
    if (!has_paths) {
        std::vector<ImRecord> all;
        if (im_list_media(roots, &all, nullptr) == kOk) {
            for (auto &r : all) {
                if (kind_wanted(r.kind, kinds))
                    chosen.push_back(std::move(r));
            }
        }
    } else {
        std::vector<const char *> root_ptrs;
        root_ptrs.reserve(roots.size());
        for (auto &r : roots) root_ptrs.push_back(r.c_str());
        for (const std::string &path : paths) {
            std::string p = trim_copy(path);
            if (p.empty()) continue;
            if (!is_within(p, root_ptrs)) continue;
            if (is_password_store(p) || is_protected_path(p))
                continue;
            /* 取包含该路径的最长根目录（与 Rust max_by_key(root.len()) 一致）。 */
            std::string_view best_root;
            size_t best_len = 0;
            for (size_t r = 0; r < roots.size(); r++) {
                if (is_within(p, std::span<const char *const>(&root_ptrs[r], 1))) {
                    if (roots[r].size() > best_len) {
                        best_len = roots[r].size();
                        best_root = roots[r];
                    }
                }
            }
            std::string rk, rs;
            if (!im_classify(p, best_root, &rk, &rs)) continue;
            if (!kind_wanted(rk, kinds)) continue;
            uint64_t size = 0, mtime = 0;
            if (file_mtime_size(p, &size, &mtime) != kOk) continue;
            ImRecord rec;
            rec.path = p;
            rec.size = size;
            rec.mtime = mtime;
            rec.kind = std::move(rk);
            rec.sub = std::move(rs);
            rec.month = month_label(mtime);
            rec.name = basename(p);
            chosen.push_back(std::move(rec));
        }
    }
    if (chosen.empty())
        return kOk;
    uint64_t total = (uint64_t)chosen.size();
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "im_clean", 0, total);
    if (cancelled()) {
        out->reasons.push_back("已取消");
        return kOk;
    }
    /* 批量送回收站（与 Rust 的 send_to_recycle(&batch) 一致：整批调用，
     * 失败的路径后续按“仍存在”判定）。 */
    std::vector<std::string> batch;
    batch.reserve(chosen.size());
    for (auto &r : chosen) batch.push_back(r.path);
    std::vector<const char *> batch_ptrs;
    batch_ptrs.reserve(batch.size());
    for (auto &b : batch) batch_ptrs.push_back(b.c_str());
    send_to_recycle(batch_ptrs);
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "im_clean", total, total);
    for (auto &rec : chosen) {
        bool still_exists = false;
#ifdef _WIN32
        auto w = utf8_to_wide(rec.path);
        if (w) {
            DWORD a = GetFileAttributesW(w->c_str());
            still_exists = a != INVALID_FILE_ATTRIBUTES;
        }
#else
        struct stat st;
        still_exists = stat(rec.path.c_str(), &st) == 0;
#endif
        if (still_exists) {
            out->failed.push_back(rec.path);
            out->reasons.push_back(rec.path + "：送回收站失败（可能被占用）");
        } else {
            out->freed += rec.size;
            out->deleted++;
        }
    }
    /* 重复文件缓存失效（Rust 的 crate::invalidate_dup_cache()；
     * 本模块只通过 hooks 触发，由 bridge 注册实现）。 */
    if (g_hooks.invalidate_dup_cache)
        g_hooks.invalidate_dup_cache(g_hooks.ctx);
    {
        std::string label = kind == "wechat" ? "微信" : "QQ";
        std::string summary = format("完成：移到回收站 %llu 个，释放 %llu 字节",
                                     (unsigned long long)out->deleted,
                                     (unsigned long long)out->freed);
        std::string lines[3] = {"开始清理", "清理：" + label, summary};
        write_log(lines, nullptr, nullptr);
    }
    return kOk;
}

}  // namespace cdc
