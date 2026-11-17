// privacy.cpp — 移植自 Rust 版 desktop/src/privacy.rs。
// SQLite 通过 third_party/sqlite3。Windows 专属部分（注册表、
// %LOCALAPPDATA% 下的浏览器目录）在 Linux 下返回空结果：scan() 只有最近
// 文件/媒体等纯文件逻辑可跑，注册表项计数为 0。
#include "privacy.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>

#include "cdc_process.h"
#include "logs.h"
#include "raii.h"
#include "recycle.h"
#include "safety.h"
#include "sqlite3.h"
#include "system.h"
#include "timeutil.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <unistd.h>
#endif

namespace cdc {
namespace {

// ---------------- 进度 / 取消 ----------------

cdc_hooks_t g_hooks = {};
uint64_t g_done = 0;
uint64_t g_total = 0;

bool cancelled() { return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, "privacy"); }

void add_done() {
    g_done++;
    if (g_hooks.progress) g_hooks.progress(g_hooks.ctx, "privacy", g_done, g_total);
}

void set_total(uint64_t total) {
    g_total = total;
    g_done = 0;
    if (g_hooks.progress) g_hooks.progress(g_hooks.ctx, "privacy", 0, total);
}

// ---------------- 常量表（与 Rust 版逐字一致） ----------------

using Words = std::span<const char *const>;

constexpr const char *kShopDomains[] = {
    "taobao.com", "tmall.com",  "jd.com",    "yangkeduo.com", "pinduoduo.com",
    "amazon.com", "amazon.cn",  "smzdm.com", "suning.com",    "dangdang.com",
};
constexpr const char *kFinanceDomains[] = {
    "alipay.com", "paypal.com", "icbc.com.cn", "ccb.com", "cmbchina.com", "unionpay.com", "abchina.com",
};
constexpr const char *kMedicalDomains[] = {
    "haodf.com", "dxy.cn", "xywy.com", "chunyuyisheng.com", "guahao.com",
};
constexpr const char *kSensitiveWords[] = {"身份证", "密码", "银行卡", "合同", "护照", "工资"};
constexpr const char *kRecentMediaExt[] = {".mp4", ".mkv", ".avi", ".mp3", ".flac", ".wav", ".mov"};

constexpr const char *kRecentDocs = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RecentDocs";
#ifdef _WIN32
constexpr const char *kUserAssist = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist";
#endif
constexpr const char *kWmpRecent = "Software\\Microsoft\\MediaPlayer\\Player\\RecentFileList";
constexpr const char *kUsbStorage = "SYSTEM\\CurrentControlSet\\Enum\\USBSTOR";

// ---------------- 小工具 ----------------

// 尚未迁移模块返回的 malloc 串：接管并释放。
std::string take(char *s) {
    std::string out = s ? s : "";
    std::free(s);
    return out;
}

std::string env_or_empty(const char *name) { return getenv_utf8(name).value_or(std::string()); }

bool is_dir_utf8(const std::string &path) {
#ifdef _WIN32
    std::optional<std::wstring> w = utf8_to_wide(path);
    if (!w) return false;
    DWORD a = GetFileAttributesW(w->c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool is_file_utf8(const std::string &path) {
#ifdef _WIN32
    std::optional<std::wstring> w = utf8_to_wide(path);
    if (!w) return false;
    DWORD a = GetFileAttributesW(w->c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

// b 可能以 '\' 开头（如 "\Google\..."），a 末尾可能已有分隔符：只保留一个分隔符。
std::string path_join(std::string_view a, std::string_view b) {
    while (!b.empty() && (b.front() == '\\' || b.front() == '/')) b.remove_prefix(1);
    std::string out(a);
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out += '\\';
    out.append(b);
    return out;
}

// 目录下的直接子项名（不递归）；不存在/不可读 = 空。
std::vector<std::string> list_dir_names(const std::string &dir) {
    std::vector<std::string> names;
    enum_dir(dir, [&](const DirEntry &e) {
        names.emplace_back(e.name);
        return 0;
    });
    return names;
}

// 纳秒时间戳（connect_copy 的临时文件名 nonce）。
uint64_t now_nanos() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (t < 116444736000000000ULL) return 0;  // 1601 → 1970 共 11644473600 秒
    return (t - 116444736000000000ULL) * 100ULL;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

bool copy_file_utf8(const std::string &from, const std::string &to) {
#ifdef _WIN32
    std::optional<std::wstring> ws = utf8_to_wide(from), wd = utf8_to_wide(to);
    return ws && wd && CopyFileW(ws->c_str(), wd->c_str(), FALSE) != 0;
#else
    UniqueFile in = fopen_utf8(from.c_str(), "rb");
    if (!in) return false;
    UniqueFile out = fopen_utf8(to.c_str(), "wb");
    if (!out) return false;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, in.get())) > 0)
        if (std::fwrite(chunk, 1, got, out.get()) != got) return false;
    return true;
#endif
}

void delete_file_utf8(const std::string &path) {
#ifdef _WIN32
    if (std::optional<std::wstring> w = utf8_to_wide(path)) DeleteFileW(w->c_str());
#else
    std::remove(path.c_str());
#endif
}

// ---------------- 浏览器 ----------------

struct BrowserSpec {
    std::string id;    // chrome / edge / qqbrowser / quark / brave / firefox
    std::string name;  // Chrome / Edge / QQ浏览器 / 夸克浏览器 / Brave / Firefox
    std::string root;
    bool firefox = false;
};

std::vector<BrowserSpec> browser_specs() {
    struct Chromium {
        const char *id, *name, *suffix;
    };
    static constexpr Chromium kChromium[] = {
        {"chrome", "Chrome", "\\Google\\Chrome\\User Data"},
        {"edge", "Edge", "\\Microsoft\\Edge\\User Data"},
        {"qqbrowser", "QQ浏览器", "\\Tencent\\QQBrowser\\User Data"},
        {"quark", "夸克浏览器", "\\Quark\\User Data"},
        {"brave", "Brave", "\\BraveSoftware\\Brave-Browser\\User Data"},
    };
    std::vector<BrowserSpec> specs;
    std::string local = env_or_empty("LOCALAPPDATA");
    for (const Chromium &c : kChromium) {
        std::string root = path_join(local, c.suffix);
        if (is_dir_utf8(root)) specs.push_back({c.id, c.name, std::move(root), false});
    }
    std::string ff = path_join(env_or_empty("APPDATA"), "\\Mozilla\\Firefox\\Profiles");
    if (is_dir_utf8(ff)) specs.push_back({"firefox", "Firefox", std::move(ff), true});
    return specs;
}

// 配置目录：Chromium 只取 Default / Profile* / Guest Profile；Firefox 取全部子目录。
std::vector<std::string> profiles_of(const BrowserSpec &spec) {
    std::vector<std::string> found;
    for (const std::string &name : list_dir_names(spec.root)) {
        std::string full = path_join(spec.root, name);
        if (!is_dir_utf8(full)) continue;
        if (spec.firefox || name == "Default" || name.compare(0, 7, "Profile") == 0 ||
            name == "Guest Profile")
            found.push_back(std::move(full));
    }
    return found;
}

std::optional<std::string> history_file(const BrowserSpec &spec, const std::string &profile) {
    std::string p = path_join(profile, spec.firefox ? "places.sqlite" : "History");
    if (is_file_utf8(p)) return p;
    return std::nullopt;
}

std::vector<std::string> cookie_files(const BrowserSpec &spec, const std::string &profile) {
    std::vector<std::string> files;
    if (spec.firefox) {
        std::string p = path_join(profile, "cookies.sqlite");
        if (is_file_utf8(p)) files.push_back(std::move(p));
        return files;
    }
    for (std::string p : {path_join(profile, "Cookies"), path_join(path_join(profile, "Network"), "Cookies")})
        if (is_file_utf8(p) && !is_password_store(p)) files.push_back(std::move(p));
    return files;
}

// ---------------- SQLite 副本 ----------------

// 临时副本：析构时关库并删除临时文件。
class DbCopy {
public:
    DbCopy() = default;
    DbCopy(const DbCopy &) = delete;
    DbCopy &operator=(const DbCopy &) = delete;
    ~DbCopy() { close(); }

    // 拷一份到 %TEMP%\cdc-privacy-{stamp}-{pid}-{nanos}.db 再打开
    // （浏览器可能正占用原文件；密码库一律不碰）。
    bool open(const std::string &path) {
        if (!is_file_utf8(path) || is_password_store(path)) return false;
        std::string tmpdir = env_or_empty("TEMP");
#ifdef _WIN32
        if (tmpdir.empty()) tmpdir = env_or_empty("TMP");
        if (tmpdir.empty()) tmpdir = "C:\\Windows\\Temp";
        unsigned long pid = GetCurrentProcessId();
#else
        if (tmpdir.empty()) tmpdir = "/tmp";
        unsigned long pid = static_cast<unsigned long>(getpid());
#endif
        std::string name = format("cdc-privacy-%s-%lu-%llu.db", take(cdc_stamp()).c_str(), pid,
                                  static_cast<unsigned long long>(now_nanos()));
        if (tmpdir.back() != '\\' && tmpdir.back() != '/') tmpdir += '\\';
        std::string tmp = tmpdir + name;
        if (!copy_file_utf8(path, tmp)) return false;
        tmp_path_ = std::move(tmp);
        sqlite3 *db = nullptr;
        int rc = sqlite3_open(tmp_path_.c_str(), &db);
        db_.reset(db);
        return rc == SQLITE_OK;
    }
    sqlite3 *db() const { return db_.get(); }
    const std::string &tmp_path() const { return tmp_path_; }
    void close_db() { db_.reset(); }
    void close() {
        db_.reset();
        if (!tmp_path_.empty()) delete_file_utf8(tmp_path_);
        tmp_path_.clear();
    }

private:
    UniqueSqlite db_;
    std::string tmp_path_;
};

// COUNT(*) 查询：成功返回值；表不存在等失败返回 nullopt（调用方按 Rust 的
// or_else 换备用表）。
std::optional<int64_t> sql_count(sqlite3 *db, const std::string &sql,
                                 std::span<const std::string> params = {}) {
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        if (raw) sqlite3_finalize(raw);
        return std::nullopt;
    }
    UniqueStmt st(raw);
    for (size_t i = 0; i < params.size(); i++)
        sqlite3_bind_text(raw, static_cast<int>(i) + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_int64(raw, 0);
}

// SELECT COUNT(*) FROM <table> WHERE url LIKE ? OR ...（域名固定常量，最多 32 个）。
std::optional<int64_t> domain_count(sqlite3 *db, const char *table, Words domains) {
    std::string sql = std::string("SELECT COUNT(*) FROM ") + table + " WHERE ";
    std::vector<std::string> patterns;
    for (size_t i = 0; i < domains.size(); i++) {
        if (i > 0) sql += " OR ";
        sql += "url LIKE ?";
        if (i < 32) patterns.push_back(std::string("%") + domains[i] + "%");
    }
    return sql_count(db, sql, patterns);
}

// 先查 Chromium 表，失败再查 Firefox 表。
std::optional<int64_t> domain_count_any(sqlite3 *db, Words domains) {
    if (std::optional<int64_t> v = domain_count(db, "urls", domains)) return v;
    return domain_count(db, "moz_places", domains);
}

struct HistoryStats {
    int64_t urls = 0, downloads = 0, shop = 0, money = 0, medical = 0;
    bool urls_available = false;
};

HistoryStats history_stats(const std::string &path) {
    HistoryStats s;
    DbCopy c;
    if (!c.open(path)) return s;
    sqlite3 *db = c.db();
    std::optional<int64_t> v = sql_count(db, "SELECT COUNT(*) FROM urls");
    if (!v) v = sql_count(db, "SELECT COUNT(*) FROM moz_places");
    if (v) {
        s.urls = *v;
        s.urls_available = true;
    }
    v = sql_count(db, "SELECT COUNT(*) FROM downloads");
    if (!v)
        v = sql_count(db,
                      "SELECT COUNT(*) FROM moz_annos a JOIN moz_anno_attributes n ON "
                      "a.anno_attribute_id = n.id WHERE n.name LIKE 'downloads/%'");
    s.downloads = v.value_or(0);
    s.shop = domain_count_any(db, kShopDomains).value_or(0);
    s.money = domain_count_any(db, kFinanceDomains).value_or(0);
    s.medical = domain_count_any(db, kMedicalDomains).value_or(0);
    return s;
}

int64_t cookie_count(const std::string &path) {
    DbCopy c;
    if (!c.open(path)) return 0;
    std::optional<int64_t> v = sql_count(c.db(), "SELECT COUNT(*) FROM cookies");
    if (!v) v = sql_count(c.db(), "SELECT COUNT(*) FROM moz_cookies");
    return v.value_or(0);
}

// ---------------- 清理用 SQL ----------------

std::vector<std::string> sql_statements(std::string_view kind) {
    if (kind == "history")
        return {"DELETE FROM visits", "DELETE FROM urls", "DELETE FROM moz_historyvisits",
                "DELETE FROM moz_places"};
    if (kind == "cookies") return {"DELETE FROM cookies", "DELETE FROM moz_cookies"};
    if (kind == "downloads")
        return {"DELETE FROM downloads_url_chains", "DELETE FROM downloads",
                "DELETE FROM moz_annos WHERE anno_attribute_id IN (SELECT id FROM "
                "moz_anno_attributes WHERE name LIKE 'downloads/%')"};
    Words domains;
    if (kind == "shop")
        domains = kShopDomains;
    else if (kind == "money")
        domains = kFinanceDomains;
    else if (kind == "medical")
        domains = kMedicalDomains;
    else
        return {};  // 未知 kind：空列表
    std::string likes;
    for (size_t i = 0; i < domains.size(); i++) {
        if (i > 0) likes += " OR ";
        likes += std::string("url LIKE '%") + domains[i] + "%'";
    }
    return {"DELETE FROM visits WHERE url IN (SELECT id FROM urls WHERE " + likes + ")",
            "DELETE FROM urls WHERE " + likes, "DELETE FROM moz_places WHERE " + likes};
}

bool apply_sql(const std::string &path, const std::vector<std::string> &statements) {
    if (!is_file_utf8(path) || is_password_store(path)) return false;
    DbCopy c;
    if (!c.open(path)) return false;
    bool applied = false;
    for (const std::string &sql : statements) {
        char *emsg = nullptr;
        if (sqlite3_exec(c.db(), sql.c_str(), nullptr, nullptr, &emsg) == SQLITE_OK) applied = true;
        sqlite3_free(emsg);
    }
    if (!applied) return false;
    c.close_db();
    return copy_file_utf8(c.tmp_path(), path);  // 把改好的副本拷回原文件
}

// ---------------- 注册表 ----------------

#ifdef _WIN32

HKEY hive_root(std::string_view hive) { return hive == "HKLM" ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }

UniqueRegKey reg_open(HKEY root, const std::string &path, REGSAM access) {
    std::optional<std::wstring> w = utf8_to_wide(path);
    if (!w) return nullptr;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, w->c_str(), 0, access, &key) != ERROR_SUCCESS) return nullptr;
    return UniqueRegKey(key);
}

std::vector<std::string> reg_list_values(HKEY key) {
    std::vector<std::string> names;
    for (DWORD index = 0;; index++) {
        wchar_t buf[512];
        DWORD len = 512;
        if (RegEnumValueW(key, index, buf, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        if (std::optional<std::string> u = wide_to_utf8(buf)) names.push_back(std::move(*u));
    }
    return names;
}

std::vector<std::string> reg_list_subkeys(HKEY key) {
    std::vector<std::string> names;
    for (DWORD index = 0;; index++) {
        wchar_t buf[512];
        DWORD len = 512;
        FILETIME ft;
        if (RegEnumKeyExW(key, index, buf, &len, nullptr, nullptr, nullptr, &ft) != ERROR_SUCCESS)
            break;
        if (std::optional<std::string> u = wide_to_utf8(buf)) names.push_back(std::move(*u));
    }
    return names;
}

// 删除前先用 reg.exe 导出备份到 logs\backups\reg\{label}_{stamp}.reg。
bool backup_key(const char *hive, const std::string &path, const char *label) {
    std::string logs = take(cdc_logs_dir());
    if (logs.empty()) return false;
    std::string dir = logs + "\\backups\\reg";
    if (std::optional<std::wstring> wd = utf8_to_wide(dir)) {
        for (size_t i = 1; i < wd->size(); i++)
            if ((*wd)[i] == L'\\') CreateDirectoryW(wd->substr(0, i).c_str(), nullptr);
        CreateDirectoryW(wd->c_str(), nullptr);
    }
    std::string file = dir + "\\" + label + "_" + take(cdc_stamp()) + ".reg";
    std::string reg_exe = take(cdc_system32_path("reg.exe"));
    if (reg_exe.empty()) reg_exe = "reg.exe";
    std::string args = std::string("export \"") + hive + "\\" + path + "\" \"" + file + "\" /y";
    long code = -1;
    int rc = cdc_run_command(reg_exe.c_str(), args.c_str(), 0, nullptr, nullptr, &code, nullptr);
    return rc == kOk && code == 0;
}

int64_t clear_values(const char *hive, const std::string &path, const char *label) {
    if (!backup_key(hive, path, label)) return 0;
    UniqueRegKey key = reg_open(hive_root(hive), path, KEY_ALL_ACCESS);
    if (!key) return 0;
    int64_t deleted = 0;
    for (const std::string &name : reg_list_values(key.get())) {
        std::optional<std::wstring> w = utf8_to_wide(name);
        if (w && RegDeleteValueW(key.get(), w->c_str()) == ERROR_SUCCESS) deleted++;
    }
    return deleted;
}

std::optional<int64_t> count_values(const char *hive, const std::string &path) {
    UniqueRegKey key = reg_open(hive_root(hive), path, KEY_READ);
    if (!key) return std::nullopt;
    return static_cast<int64_t>(reg_list_values(key.get()).size());
}

std::optional<int64_t> count_subkeys(const char *hive, const std::string &path) {
    UniqueRegKey key = reg_open(hive_root(hive), path, KEY_READ);
    if (!key) return std::nullopt;
    return static_cast<int64_t>(reg_list_subkeys(key.get()).size());
}

// 键下每个子键的完整路径（+ suffix）。
std::vector<std::string> subkey_paths(const char *base, const char *suffix) {
    std::vector<std::string> out;
    if (UniqueRegKey key = reg_open(HKEY_CURRENT_USER, base, KEY_READ))
        for (const std::string &sub : reg_list_subkeys(key.get()))
            out.push_back(std::string(base) + "\\" + sub + suffix);
    return out;
}

#else

int64_t clear_values(const char *, const std::string &, const char *) { return 0; }
std::optional<int64_t> count_values(const char *, const std::string &) { return std::nullopt; }
std::optional<int64_t> count_subkeys(const char *, const std::string &) { return std::nullopt; }

#endif

std::vector<std::string> recent_docs_paths() {
    std::vector<std::string> paths = {kRecentDocs};
#ifdef _WIN32
    std::vector<std::string> subs = subkey_paths(kRecentDocs, "");
    paths.insert(paths.end(), subs.begin(), subs.end());
#endif
    return paths;
}

std::vector<std::string> userassist_count_keys() {
#ifdef _WIN32
    return subkey_paths(kUserAssist, "\\Count");
#else
    return {};
#endif
}

// ---------------- 最近文件 / 影音 ----------------

std::vector<std::string> recent_files() {
    std::string dir = path_join(env_or_empty("APPDATA"), "\\Microsoft\\Windows\\Recent");
    std::vector<std::string> found;
    for (const std::string &name : list_dir_names(dir)) {
        std::string full = path_join(dir, name);
        if (is_file_utf8(full)) found.push_back(std::move(full));
    }
    return found;
}

std::string_view file_name_of(std::string_view path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string_view::npos ? path : path.substr(p + 1);
}

bool contains_any(std::string_view haystack, Words words) {
    return std::any_of(words.begin(), words.end(),
                       [&](const char *w) { return haystack.find(w) != std::string_view::npos; });
}

// 最近打开的快捷方式：取 file_stem 再取扩展名，与 kRecentMediaExt 比对。
// 如 "movie.mp4.lnk" → stem "movie.mp4" → ext ".mp4"。
bool recent_is_media(std::string_view path) {
    std::string_view base = file_name_of(path);
    size_t dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    std::string_view stem = base.substr(0, dot);
    size_t dot2 = stem.rfind('.');
    if (dot2 == std::string_view::npos) return false;
    std::string ext(stem.substr(dot2));
    if (ext.size() >= 16) return false;
    for (char &ch : ext)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return std::any_of(std::begin(kRecentMediaExt), std::end(kRecentMediaExt),
                       [&](const char *e) { return ext == e; });
}

bool utf8_valid(std::string_view s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        auto c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t extra;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
            if (cp == 0) return false;  // 过短编码
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
            auto b = static_cast<uint8_t>(s[i + k]);
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += extra + 1;
    }
    return true;
}

// VLC 最近播放：%APPDATA%\vlc\vlc-qt-interface.ini 里含 file:/// 的行数
// （行内小写化后匹配）。文件不是合法 UTF-8 时 Rust 的 read_to_string 失败 → 计 0。
int64_t vlc_recent_count() {
    std::string file = path_join(path_join(env_or_empty("APPDATA"), "\\vlc"), "vlc-qt-interface.ini");
    UniqueFile f = fopen_utf8(file.c_str(), "rb");
    if (!f) return 0;
    std::string content;
    char chunk[8192];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f.get())) > 0) content.append(chunk, got);
    if (!utf8_valid(content)) return 0;
    int64_t count = 0;
    size_t start = 0;
    while (start <= content.size()) {
        size_t end = content.find('\n', start);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        for (char &ch : line)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        if (line.find("file:///") != std::string::npos) count++;
        start = end + 1;
    }
    return count;
}

PrivacyItem make_item(std::string id, const char *section, std::string name, int64_t count,
                      bool available, std::vector<std::string> files, const char *action,
                      const char *note) {
    return PrivacyItem{std::move(id), section, std::move(name), count, available, std::move(files),
                       action, note ? note : ""};
}

}  // namespace

void privacy_set_hooks(const cdc_hooks_t *hooks) { g_hooks = hooks ? *hooks : cdc_hooks_t{}; }

// ---------------- scan ----------------

std::vector<PrivacyItem> privacy_scan() {
    std::vector<PrivacyItem> items;
    // 每浏览器的 history / download / shop 三项（先收集，最后拼顺序：
    // Rust 版是 high（风险项在前）+ history + download + shop）。
    std::vector<PrivacyItem> hist_items, down_items, shop_items;
    int64_t money_total = 0, medical_total = 0, shop_total = 0, cookie_total = 0;
    bool money_available = false, medical_available = false, shop_available = false;
    bool cookie_available = false;
    std::vector<std::string> cookie_files_all;

    for (const BrowserSpec &spec : browser_specs()) {
        HistoryStats sum;
        for (const std::string &prof : profiles_of(spec)) {
            if (std::optional<std::string> hf = history_file(spec, prof)) {
                HistoryStats st = history_stats(*hf);
                sum.urls += st.urls;
                sum.downloads += st.downloads;
                sum.shop += st.shop;
                sum.money += st.money;
                sum.medical += st.medical;
                sum.urls_available |= st.urls_available;
            }
            for (std::string &ck : cookie_files(spec, prof)) {
                int64_t cnt = cookie_count(ck);
                if (cnt > 0 || cookie_available) cookie_available = true;  // 与 Rust 版同一写法
                cookie_total += cnt;
                cookie_files_all.push_back(std::move(ck));
            }
        }
        money_total += sum.money;
        medical_total += sum.medical;
        shop_total += sum.shop;
        money_available |= sum.money > 0;
        medical_available |= sum.medical > 0;
        shop_available |= sum.urls_available;
        hist_items.push_back(make_item("hist_" + spec.id, "history", spec.name, sum.urls,
                                       sum.urls_available, {}, "sql", ""));
        down_items.push_back(make_item("down_" + spec.id, "download", spec.name, sum.downloads,
                                       sum.urls_available, {}, "sql",
                                       "只删浏览记录，不删除已下载的文件"));
        shop_items.push_back(make_item("shop_" + spec.id, "shop", spec.name, sum.shop,
                                       sum.urls_available, {}, "sql", ""));
    }

    std::vector<std::string> recent = recent_files();
    // 敏感文件名（最近打开的文件名含敏感词）。
    std::vector<std::string> sensitive;
    for (const std::string &r : recent)
        if (contains_any(file_name_of(r), kSensitiveWords)) sensitive.push_back(r);
    int64_t nsensitive = static_cast<int64_t>(sensitive.size());
    items.push_back(make_item("risk_file", "high", "文件敏感隐私", nsensitive, !recent.empty(),
                              std::move(sensitive), "files",
                              "最近打开的文件名里含身份证、密码、合同等字样"));
    items.push_back(make_item("risk_money", "high", "财产安全隐私", money_total, money_available, {},
                              "sql", ""));
    items.push_back(make_item("risk_shop", "high", "网络购物痕迹", shop_total, shop_available, {},
                              "sql", ""));
    items.push_back(make_item("risk_medical", "high", "医疗信息隐私", medical_total,
                              medical_available, {}, "sql", ""));
    items.push_back(make_item("cookies", "high", "Cookie", cookie_total, cookie_available,
                              std::move(cookie_files_all), "sql", "清理后部分网站需要重新登录"));
    for (auto *group : {&hist_items, &down_items, &shop_items})
        for (PrivacyItem &it : *group) items.push_back(std::move(it));

    // 电脑：文件访问记录 + USB（只统计）。
    int64_t recent_count = 0;
    bool recent_available = false;
    for (const std::string &p : recent_docs_paths()) {
        if (std::optional<int64_t> v = count_values("HKCU", p)) {
            recent_available = true;
            recent_count += *v;
        }
    }
    items.push_back(make_item("reg_file", "computer", "文件访问记录", recent_count, recent_available,
                              {}, "registry", "最近文档列表，删除前会导出备份"));
    int64_t usb = count_subkeys("HKLM", kUsbStorage).value_or(0);
    items.push_back(make_item("reg_usb", "computer", "USB 使用痕迹", usb, usb > 0, {}, "count",
                              "只统计；清理 USB 记录需要改系统设备记录，Rust 版暂不执行"));

    // 软件：UserAssist + Prefetch .pf 计数（Prefetch 文件保留）。
    int64_t userassist = 0;
    for (const std::string &k : userassist_count_keys()) userassist += count_values("HKCU", k).value_or(0);
    int64_t prefetch_count = 0;
    for (const std::string &name : list_dir_names(path_join(env_or_empty("SystemRoot"), "\\Prefetch"))) {
        size_t n = name.size();
        if (n >= 3 && name[n - 3] == '.' && (name[n - 2] == 'p' || name[n - 2] == 'P') &&
            (name[n - 1] == 'f' || name[n - 1] == 'F'))
            prefetch_count++;
    }
    items.push_back(make_item("software", "software", "软件使用痕迹", userassist + prefetch_count,
                              true, {}, "userassist", "清理 UserAssist 记录；Prefetch 文件保留"));

    // 影音：最近打开的音视频快捷方式 + WMP + VLC。
    std::vector<std::string> media_files;
    for (const std::string &r : recent)
        if (recent_is_media(r)) media_files.push_back(r);
    int64_t wmp = count_values("HKCU", kWmpRecent).value_or(0);
    int64_t media_total = static_cast<int64_t>(media_files.size()) + wmp + vlc_recent_count();
    items.push_back(make_item("media", "media", "影音播放痕迹", media_total, true,
                              std::move(media_files), "media",
                              "清理最近播放列表和最近打开的音视频快捷方式"));
    return items;
}

// ---------------- clean ----------------

PrivacyReport privacy_clean(std::span<const char *const> ids) {
    PrivacyReport out;
    struct SqlJob {
        std::string path;  // 数据库文件
        std::vector<std::string> stmts;
    };
    std::vector<std::string> files;
    std::vector<SqlJob> sql_jobs;
    std::vector<std::string> registry_jobs;
    bool userassist = false, media = false;

    auto selected = [&](const std::string &id) {
        return std::any_of(ids.begin(), ids.end(), [&](const char *s) { return s && id == s; });
    };
    auto starts = [](const std::string &s, const char *p) { return s.compare(0, std::strlen(p), p) == 0; };
    auto ends = [](const std::string &s, const std::string &suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };

    for (PrivacyItem &item : privacy_scan()) {
        if (!selected(item.id) || item.action == "count") continue;
        if (item.action == "sql") {
            const char *kind = nullptr;
            if (item.id == "risk_money")
                kind = "money";
            else if (item.id == "risk_shop")
                kind = "shop";
            else if (item.id == "risk_medical")
                kind = "medical";
            else if (starts(item.id, "hist_"))
                kind = "history";
            else if (starts(item.id, "down_"))
                kind = "downloads";
            else if (starts(item.id, "shop_"))
                kind = "shop";
            else if (item.id == "cookies")
                kind = "cookies";
            if (!kind) continue;
            std::vector<std::string> stmts = sql_statements(kind);
            if (stmts.empty()) continue;
            bool all_browsers = item.id == "cookies" || item.id == "risk_money" ||
                                item.id == "risk_shop" || item.id == "risk_medical";
            for (const BrowserSpec &spec : browser_specs()) {
                if (!all_browsers && !ends(item.id, spec.id)) continue;
                for (const std::string &prof : profiles_of(spec)) {
                    if (item.id == "cookies") {
                        for (std::string &ck : cookie_files(spec, prof)) sql_jobs.push_back({std::move(ck), stmts});
                    } else if (std::optional<std::string> hf = history_file(spec, prof)) {
                        sql_jobs.push_back({std::move(*hf), stmts});
                    }
                }
            }
        } else if (item.action == "registry") {
            registry_jobs.push_back(item.id);
        } else if (item.action == "userassist") {
            userassist = true;
        } else {
            for (std::string &f : item.files) files.push_back(std::move(f));
            if (item.action == "media") media = true;
        }
    }

    // 去重：去空白串、去密码库、排序、去重。
    std::vector<std::string> kept;
    for (std::string &p : files) {
        bool empty = p.find_first_not_of(" \t\n\r") == std::string::npos;
        if (!empty && !is_password_store(p)) kept.push_back(std::move(p));
    }
    std::sort(kept.begin(), kept.end());
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());

    set_total(kept.size() + sql_jobs.size() + registry_jobs.size() + (userassist ? 1 : 0) +
              (media ? 1 : 0));
    auto bail = [&]() -> PrivacyReport & {
        out.failed.push_back("已取消");
        return out;
    };
    if (cancelled()) return bail();
    if (!kept.empty()) {
        std::vector<const char *> ptrs;
        for (const std::string &p : kept) ptrs.push_back(p.c_str());
        std::vector<std::string> failed = send_to_recycle(ptrs);
        for (const std::string &p : kept) {
            add_done();
            bool is_failed = std::find(failed.begin(), failed.end(), p) != failed.end();
            if (is_failed || is_file_utf8(p))
                out.failed.push_back("文件未能移入回收站：" + p);
            else
                out.files++;
        }
    }
    if (cancelled()) return bail();
    for (const SqlJob &job : sql_jobs) {
        add_done();
        if (apply_sql(job.path, job.stmts))
            out.sql++;
        else
            out.failed.push_back("数据库清理失败或数据源不可用：" + job.path);
    }
    if (cancelled()) return bail();
    for (const std::string &job : registry_jobs) {
        add_done();
        if (job != "reg_file") continue;
        int64_t deleted = 0;
        for (const std::string &p : recent_docs_paths()) deleted += clear_values("HKCU", p, "recentdocs");
        out.registry += deleted;
        if (deleted == 0) out.failed.push_back("文件访问记录清理未完成：没有记录被清除");
    }
    if (userassist) {
        add_done();
        int64_t deleted = 0;
        for (const std::string &k : userassist_count_keys()) deleted += clear_values("HKCU", k, "userassist");
        out.registry += deleted;
        if (deleted == 0) out.failed.push_back("软件使用痕迹清理未完成：没有记录被清除");
    }
    if (media) {
        add_done();
        out.registry += clear_values("HKCU", kWmpRecent, "wmp");
    }
    std::string summary = format("完成：文件 %lld 个，数据库 %lld 个，注册表记录 %lld 条",
                                 static_cast<long long>(out.files), static_cast<long long>(out.sql),
                                 static_cast<long long>(out.registry));
    const char *lines[3] = {"开始清理", "清理：隐私痕迹", summary.c_str()};
    cdc_write_log(lines, 3, nullptr, nullptr);
    return out;
}

}  // namespace cdc
