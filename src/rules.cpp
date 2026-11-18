/* rules.cpp — 规则解析/校验/远程更新，移植自 Rust 版 desktop/src/rules.rs
 * + main.rs 的 Rule/all_rules。
 *
 * 与 Rust 版的差异（有意为之，见头文件注释）：
 *   - config 的第三候选（编译期 CARGO_MANIFEST_DIR）没有等价物：C 版只找
 *     exe 旁与 exe 下 resources/ 里的 rules_update.json。
 *   - 下载走 WinHTTP（仅 Windows）；Linux 下 update() 直接报错。
 */
#include "rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "logs.h"
#include "raii.h"
#include "rules_data.h"
#include "safety.h"

#ifdef _WIN32
#include "blake3/blake3.h"
#include <windows.h>
#include <winhttp.h>
#else
#include <unistd.h>
#endif

namespace cdc {
namespace {

constexpr std::array<std::string_view, 3> kTiers{"conservative", "standard", "deep"};
constexpr std::array<std::string_view, 6> kLibraryParts{"documents",    "desktop", "downloads",
                                                        "my documents", "文档",    "桌面"};

std::string trim_ascii(std::string_view value) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    size_t first = 0, last = value.size();
    while (first < last && is_space(value[first]))
        ++first;
    while (last > first && is_space(value[last - 1]))
        --last;
    return std::string(value.substr(first, last - first));
}

using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

/* ---- JSON 取值小工具（严格：类型错即整文件失败，与 serde 一致） ---- */

std::string json_string(cJSON *obj, const char *key, int *bad) {
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!value)
        return std::string();
    if (!cJSON_IsString(value)) {
        *bad = 1;
        return std::string();
    }
    return value->valuestring ? value->valuestring : "";
}

std::vector<std::string> json_string_array(cJSON *obj, const char *key, int *bad) {
    std::vector<std::string> values;
    cJSON *value = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!value)
        return values;
    if (!cJSON_IsArray(value)) {
        *bad = 1;
        return values;
    }
    const int count = cJSON_GetArraySize(value);
    values.reserve(count > 0 ? static_cast<size_t>(count) : 0);
    for (int i = 0; i < count; ++i) {
        cJSON *element = cJSON_GetArrayItem(value, i);
        if (!cJSON_IsString(element)) {
            *bad = 1;
            return std::vector<std::string>();
        }
        values.emplace_back(element->valuestring ? element->valuestring : "");
    }
    return values;
}

bool json_bool(cJSON *obj, const char *key, int *bad) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v)
        return false;
    if (!cJSON_IsBool(v)) {
        *bad = 1;
        return false;
    }
    return cJSON_IsTrue(v) ? true : false;
}

uint64_t json_u64(cJSON *obj, const char *key, uint64_t def, int *bad) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v)
        return def;
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 18446744073709551615.0) {
        *bad = 1;
        return def;
    }
    return (uint64_t)v->valuedouble;
}

int parse_one_rule(cJSON *item, Rule *out) {
    int bad = 0;
    Rule r;
    r.key = json_string(item, "key", &bad);
    r.name = json_string(item, "name", &bad);
    r.tier = json_string(item, "tier", &bad);
    r.kind = json_string(item, "kind", &bad);
    r.describe = json_string(item, "describe", &bad);
    r.warning = json_string(item, "warning", &bad);
    r.note = json_string(item, "note", &bad);
    r.paths = json_string_array(item, "paths", &bad);
    r.glob_dirs = json_string_array(item, "glob_dirs", &bad);
    r.patterns = json_string_array(item, "patterns", &bad);
    r.exclude = json_string_array(item, "exclude", &bad);
    r.min_age_minutes = json_u64(item, "min_age_minutes", 0, &bad);
    r.requires_confirm = json_bool(item, "requires_confirm", &bad);
    r.prune_dirs = json_bool(item, "prune_dirs", &bad);
    if (bad)
        return kErrInvalidArg;
    *out = std::move(r);
    return kOk;
}

/* ---- 文件读写小工具（UTF-8 路径） ---- */

std::optional<std::string> read_file_utf8(std::string_view path) {
    UniqueFile file = fopen_utf8(std::string(path).c_str(), "rb");
    if (!file)
        return std::nullopt;
    std::vector<unsigned char> bytes;
    std::array<unsigned char, 8192> chunk{};
    for (;;) {
        const size_t got = std::fread(chunk.data(), 1, chunk.size(), file.get());
        if (got != 0)
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + got);
        if (got != chunk.size()) {
            if (std::ferror(file.get()))
                return std::nullopt;
            break;
        }
    }
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

#ifdef _WIN32
int write_file_utf8(std::string_view path, std::string_view data) {
    UniqueFile f = fopen_utf8(std::string(path).c_str(), "wb");
    if (!f)
        return kErrIo;
    size_t wrote = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f.get());
    return (wrote == data.size() && std::fclose(f.release()) == 0) ? kOk : kErrIo;
}
#endif /* _WIN32 — 只在 Windows-only 的 update() 里用 */

int copy_file_utf8(std::string_view from, std::string_view to) {
    UniqueFile input = fopen_utf8(std::string(from).c_str(), "rb");
    UniqueFile output = fopen_utf8(std::string(to).c_str(), "wb");
    if (!input || !output)
        return kErrIo;
    std::array<unsigned char, 65536> chunk{};
    for (;;) {
        const size_t got = std::fread(chunk.data(), 1, chunk.size(), input.get());
        if (got && std::fwrite(chunk.data(), 1, got, output.get()) != got)
            return kErrIo;
        if (got != chunk.size()) {
            if (std::ferror(input.get()))
                return kErrIo;
            break;
        }
    }
    return std::fflush(output.get()) == 0 ? kOk : kErrIo;
}

bool file_exists_utf8(std::string_view path) {
#ifdef _WIN32
    auto w = utf8_to_wide(std::string(path));
    if (!w)
        return false;
    DWORD a = GetFileAttributesW(w->c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return access(std::string(path).c_str(), F_OK) == 0;
#endif
}

/* 原子替换：写临时文件后 rename（Windows 上 MoveFileExW 覆盖）。 */
#ifdef _WIN32 /* 只在 Windows-only 的 update() 里用 */
int rename_replace_utf8(std::string_view from, std::string_view to) {
    auto wf = utf8_to_wide(std::string(from));
    auto wt = utf8_to_wide(std::string(to));
    if (!wf || !wt)
        return kErrIo;
    BOOL ok = MoveFileExW(wf->c_str(), wt->c_str(), MOVEFILE_REPLACE_EXISTING);
    return ok ? kOk : kErrIo;
}
#endif /* _WIN32 */

std::optional<std::string> exe_dir_utf8() {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(NULL, buf, 4096);
    if (n == 0 || n >= 4096)
        return std::nullopt;
    auto utf8 = wide_to_utf8(buf);
    if (!utf8)
        return std::nullopt;
    size_t slash = utf8->rfind('\\');
    if (slash == std::string::npos)
        return std::nullopt;
    return utf8->substr(0, slash);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::nullopt;
    buf[n] = '\0';
    std::string s(buf);
    size_t slash = s.rfind('/');
    if (slash == std::string::npos)
        return std::nullopt;
    return s.substr(0, slash);
#endif
}

std::optional<std::string> config_path() {
    auto dir = exe_dir_utf8();
    if (!dir)
        return std::nullopt;
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    const char *suffixes[2] = {"rules_update.json", "resources/rules_update.json"};
    /* resources/ 里的斜杠在 Windows 上同样可用（CreateFile 接受 '/'）。 */
    for (int i = 0; i < 2; i++) {
        std::string p = *dir + sep + suffixes[i];
        if (file_exists_utf8(p))
            return p;
    }
    return std::nullopt;
}

/* 从 cJSON 规则对象里抽出 validate 所需的字段并校验。 */
int validate_json_item(cJSON *item, Error *err) {
    cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
    const char *key_s = (key && cJSON_IsString(key)) ? key->valuestring : "";
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(item, "kind");
    const char *kind_s = (kind && cJSON_IsString(kind)) ? kind->valuestring : nullptr;
    cJSON *tier = cJSON_GetObjectItemCaseSensitive(item, "tier");
    const char *tier_s = (tier && cJSON_IsString(tier)) ? tier->valuestring : nullptr;
    cJSON *command = cJSON_GetObjectItemCaseSensitive(item, "command");
    bool has_command = command != nullptr;
    bool command_non_empty = false;
    if (command) {
        if (cJSON_IsArray(command))
            command_non_empty = cJSON_GetArraySize(command) > 0;
        else
            command_non_empty = true; /* 非数组的 command 同样拒绝 */
    }
    /* paths / glob_dirs：非数组或含非字符串即格式错误。 */
    int bad = 0;
    std::vector<std::string> path_vec, glob_vec;
    for (int f = 0; f < 2 && !bad; f++) {
        cJSON *list = cJSON_GetObjectItemCaseSensitive(item, f == 0 ? "paths" : "glob_dirs");
        if (!list)
            continue;
        if (!cJSON_IsArray(list)) {
            if (err)
                err_set(err, kErrInvalidArg, "规则 %s 的 %s 格式不正确", key_s,
                        f == 0 ? "paths" : "glob_dirs");
            bad = 1;
            break;
        }
        size_t n = (size_t)cJSON_GetArraySize(list);
        for (size_t i = 0; i < n; i++) {
            cJSON *e = cJSON_GetArrayItem(list, (int)i);
            if (!cJSON_IsString(e)) {
                if (err)
                    err_set(err, kErrInvalidArg, "规则 %s 的 %s 含有非字符串", key_s,
                            f == 0 ? "paths" : "glob_dirs");
                bad = 1;
                break;
            }
            (f == 0 ? path_vec : glob_vec)
                .emplace_back(e->valuestring ? e->valuestring : "");
        }
    }
    if (bad)
        return kErrInvalidArg;
    return validate_rule(key_s, kind_s ? kind_s : "", tier_s ? tier_s : "", has_command,
                         command_non_empty, path_vec, glob_vec, err);
}

#ifdef _WIN32
/* WinHTTP GET：返回响应体（UTF-8 文本），读满 MAX_BYTES+1 即停
 * （调用方据此判定超限）。 */
int http_get_https(std::string_view url, std::string *out_body, Error *err) {
    /* 拆 https://host[:port]/path */
    std::string_view rest = url.substr(strlen("https://"));
    size_t slash = rest.find('/');
    std::string_view host = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    std::string_view path = slash == std::string_view::npos ? "/" : rest.substr(slash);
    if (host.empty() || host.size() >= 512) {
        if (err)
            err_set(err, kErrInvalidArg, "下载地址不合法");
        return kErrInvalidArg;
    }
    auto whost = utf8_to_wide(std::string(host));
    auto wpath = utf8_to_wide(std::string(path));
    if (!whost || !wpath) {
        if (err)
            err_set(err, kErrUtf8, "下载地址编码失败");
        return kErrUtf8;
    }
    HINTERNET session = NULL, conn = NULL, req = NULL;
    int rc = kErrIo;
    std::string body;
    session = WinHttpOpen(L"cdc-rules/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        goto done;
    WinHttpSetTimeouts(session, 20000, 20000, 20000, 20000);
    conn = WinHttpConnect(session, whost->c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn)
        goto done;
    req = WinHttpOpenRequest(conn, L"GET", wpath->c_str(), NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req)
        goto done;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                            0))
        goto done;
    if (!WinHttpReceiveResponse(req, NULL))
        goto done;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail))
            goto done;
        if (avail == 0)
            break;
        if (body.size() + avail > kRulesMaxBytes + 1) {
            avail = (DWORD)(kRulesMaxBytes + 1 - body.size());
            if (avail == 0)
                break;
        }
        body.resize(body.size() + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, body.data() + body.size() - avail, avail, &got) || got == 0)
            goto done;
        body.resize(body.size() - avail + got);
        if (body.size() > kRulesMaxBytes)
            break;
    }
    *out_body = std::move(body);
    rc = kOk;
done:
    if (rc != kOk && err && err->code == 0)
        err_set(err, rc, "下载失败：Win32 %lu", (unsigned long)GetLastError());
    if (req)
        WinHttpCloseHandle(req);
    if (conn)
        WinHttpCloseHandle(conn);
    if (session)
        WinHttpCloseHandle(session);
    return rc;
}

void blake3_hex(const uint8_t *data, size_t len, char out_hex[65]) {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    uint8_t digest[32];
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = hexd[digest[i] >> 4];
        out_hex[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}
#endif /* _WIN32 */

}  // namespace

std::string expand_env(std::string_view raw) {
    std::string result;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '%') {
            const size_t close = raw.find('%', i + 1);
            if (close != std::string_view::npos) {
                std::string name(raw.substr(i + 1, close - i - 1));
                auto value = getenv_utf8(name.c_str());
                if (value) {
                    result += *value;
                    i = close + 1;
                    continue;
                }
            }
        }
        result.push_back(raw[i++]);
    }
    return result;
}

int parse_rules_text(std::string_view text, std::vector<Rule> *out_rules,
                     uint64_t *out_version, Error *err) {
    if (err)
        err_init(err);
    if (!out_rules) {
        if (err)
            err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out_rules->clear();
    if (out_version)
        *out_version = 0;
    JsonPtr root(cJSON_Parse(std::string(text).c_str()));
    if (!root) {
        if (err)
            err_set(err, kErrInvalidArg, "规则格式不正确");
        return kErrInvalidArg;
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root.get(), "version");
    if (out_version && cJSON_IsNumber(version) && version->valuedouble >= 0)
        *out_version = (uint64_t)version->valuedouble;
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(root.get(), "categories");
    if (!cJSON_IsArray(cats)) {
        if (err)
            err_set(err, kErrInvalidArg, "规则文件缺少 categories");
        return kErrInvalidArg;
    }
    size_t n = (size_t)cJSON_GetArraySize(cats);
    out_rules->reserve(n);
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(cats, (int)i);
        Rule r;
        if (!cJSON_IsObject(item) || parse_one_rule(item, &r) != kOk) {
            out_rules->clear();
            if (err)
                err_set(err, kErrInvalidArg, "规则格式不正确（第 %zu 条）", i + 1);
            return kErrInvalidArg;
        }
        out_rules->push_back(std::move(r));
    }
    return kOk;
}

std::optional<std::string> remote_rules_path() {
    auto app = app_data_dir();
    std::string base = app ? *app : ".";
#ifdef _WIN32
    return base + "\\rules_remote.json";
#else
    return base + "/rules_remote.json";
#endif
}

std::optional<std::string> remote_rules_backup_path() {
    auto path = remote_rules_path();
    if (!path)
        return std::nullopt;
    /* "rules_remote.json" → "rules_remote.previous.json" */
    size_t dot = path->rfind('.');
    if (dot == std::string::npos)
        return *path + ".previous";
    return path->substr(0, dot) + ".previous" + path->substr(dot);
}

int load_all_rules(std::vector<Rule> *out, Error *err) {
    if (err)
        err_init(err);
    if (!out) {
        if (err)
            err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
    Error e2;
    if (parse_rules_text(CDC_RULES_JSON, out, nullptr, &e2) != kOk) {
        if (err)
            *err = e2;
        return kErrInvalidArg;
    }
    /* 远程规则：读不到/解析失败时静默跳过（与 Rust 版一致）。 */
    auto remote_path = remote_rules_path();
    if (remote_path) {
        auto text = read_file_utf8(*remote_path);
        if (text) {
            std::vector<Rule> extra;
            if (parse_rules_text(*text, &extra, nullptr, nullptr) == kOk) {
                for (auto &r : extra) {
                    if (r.key.empty())
                        continue;
                    bool known = false;
                    for (auto &have : *out) {
                        if (have.key == r.key) {
                            known = true;
                            break;
                        }
                    }
                    if (known)
                        continue;
                    out->push_back(std::move(r));
                }
            }
        }
    }
    return kOk;
}

RulesConfig rules_config() {
    RulesConfig cfg;
    auto path = config_path();
    if (!path)
        return cfg; /* 无配置 = url 为空（与 Rust 版一致） */
    auto text = read_file_utf8(*path);
    if (!text)
        return cfg;
    JsonPtr root(cJSON_Parse(text->c_str()));
    if (!root)
        return cfg;
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root.get(), "url");
    if (cJSON_IsString(url) && url->valuestring)
        cfg.url = trim_ascii(url->valuestring);
    cJSON *hash = cJSON_GetObjectItemCaseSensitive(root.get(), "blake3");
    if (cJSON_IsString(hash) && hash->valuestring) {
        std::string trimmed = trim_ascii(hash->valuestring);
        if (trimmed.size() == 64) {
            for (char &c : trimmed) {
                if (c >= 'A' && c <= 'F')
                    c = (char)(c + ('a' - 'A'));
            }
            cfg.blake3 = trimmed;
        }
    }
    return cfg;
}

/* ---- 校验 ---- */

bool rule_key_ok(std::string_view key) {
    if (key.empty() || key.size() > 80)
        return false;
    for (char c : key) {
        unsigned char u = (unsigned char)c;
        if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
              u == '_' || u == '-'))
            return false;
    }
    return true;
}

int rule_path_allowed(std::string_view raw, std::string *out_err) {
    auto fail = [&](const char *reason) {
        if (out_err)
            *out_err = reason;
        return kErrInvalidArg;
    };
    if (raw.empty())
        return fail("路径为空");
    std::string_view t = raw;
    while (!t.empty() && std::strchr(" \t\n\r", t.front()))
        t.remove_prefix(1);
    if (t.empty())
        return fail("路径为空");
    if (raw.size() > 500)
        return fail("路径含有非法字符");
    for (unsigned char c : raw) {
        if (c < 32)
            return fail("路径含有非法字符");
    }
    std::string expanded = expand_env(raw);
    if (path_syntax_error(expanded))
        return fail("路径格式不合法");
    /* 按 '\\' 或 '/' 切分、小写，任一段命中 LIBRARY_PARTS 即拒绝。 */
    {
        std::string lower = expanded;
        for (char &c : lower) {
            if (c == '/')
                c = '\\';
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + ('a' - 'A'));
        }
        std::string_view p = lower;
        bool bad = false;
        while (!p.empty()) {
            while (!p.empty() && p.front() == '\\')
                p.remove_prefix(1);
            if (p.empty())
                break;
            size_t len = p.find('\\');
            std::string_view seg = len == std::string_view::npos ? p : p.substr(0, len);
            for (std::string_view part : kLibraryParts) {
                if (part == seg) {
                    bad = true;
                    break;
                }
            }
            if (bad)
                break;
            p = len == std::string_view::npos ? std::string_view() : p.substr(len);
        }
        if (bad)
            return fail("远程规则不能指向文档、桌面或下载");
    }
    std::string name = basename(expanded);
    for (char &c : name) {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + ('a' - 'A'));
    }
    if (is_password_store(name) || name.find("login data") != std::string::npos)
        return fail("远程规则不能删除浏览器密码库");
    if (!under_remote_allowlist(expanded))
        return fail("远程规则只能指向临时目录或固定 catalog 中的目录");
    return kOk;
}

int validate_rule(std::string_view key, std::string_view kind, std::string_view tier,
                  bool has_command, bool command_non_empty,
                  std::span<const std::string> paths,
                  std::span<const std::string> glob_dirs, Error *err) {
    if (err)
        err_init(err);
    if (!rule_key_ok(key)) {
        if (err)
            err_set(err, kErrInvalidArg, "规则 key 不合法");
        return kErrInvalidArg;
    }
    std::string_view k = kind.empty() ? "files" : kind;
    if (k != "files") {
        if (err)
            err_set(err, kErrInvalidArg, "规则 %s 不是文件清理类型", std::string(key).c_str());
        return kErrInvalidArg;
    }
    std::string_view t = tier.empty() ? "standard" : tier;
    bool tier_ok = false;
    for (std::string_view allowed : kTiers) {
        if (t == allowed) {
            tier_ok = true;
            break;
        }
    }
    if (!tier_ok) {
        if (err)
            err_set(err, kErrInvalidArg, "规则 %s 的档位不合法", std::string(key).c_str());
        return kErrInvalidArg;
    }
    if (has_command && command_non_empty) {
        if (err)
            err_set(err, kErrInvalidArg, "规则 %s 不能带命令", std::string(key).c_str());
        return kErrInvalidArg;
    }
    for (int f = 0; f < 2; f++) {
        std::span<const std::string> list = f == 0 ? paths : glob_dirs;
        for (const std::string &p : list) {
            std::string why;
            if (rule_path_allowed(p, &why) != kOk) {
                if (err)
                    err_set(err, kErrInvalidArg, "规则 %s：%s", std::string(key).c_str(),
                            why.c_str());
                return kErrInvalidArg;
            }
        }
    }
    return kOk;
}

int validate_rules_text(std::string_view text, Error *err) {
    if (err)
        err_init(err);
    JsonPtr root(cJSON_Parse(std::string(text).c_str()));
    if (!root) {
        if (err)
            err_set(err, kErrInvalidArg, "规则格式不正确");
        return kErrInvalidArg;
    }
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(root.get(), "categories");
    if (!cJSON_IsArray(cats)) {
        if (err)
            err_set(err, kErrInvalidArg, "规则文件缺少 categories");
        return kErrInvalidArg;
    }
    if ((size_t)cJSON_GetArraySize(cats) > kRulesMaxCount) {
        if (err)
            err_set(err, kErrInvalidArg, "远程规则过多（上限 %u 条）", kRulesMaxCount);
        return kErrInvalidArg;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cats) {
        if (!cJSON_IsObject(item)) {
            if (err)
                err_set(err, kErrInvalidArg, "规则格式不正确");
            return kErrInvalidArg;
        }
        if (validate_json_item(item, err) != kOk)
            return kErrInvalidArg;
    }
    return kOk;
}

/* ---- 远程更新 ---- */

int update_rules(RulesUpdateResult *out, Error *err) {
    if (err)
        err_init(err);
    if (!out) {
        if (err)
            err_set(err, kErrInvalidArg, "out is NULL");
        return kErrInvalidArg;
    }
    *out = RulesUpdateResult{};
#ifdef _WIN32
    RulesConfig cfg = rules_config();
    if (cfg.url.rfind("https://", 0) != 0) {
        if (err)
            err_set(err, kErrInvalidArg,
                    "未配置规则更新地址（在 rules_update.json 里填写 "
                    "https 地址）");
        return kErrInvalidArg;
    }
    std::string body;
    int rc = http_get_https(cfg.url, &body, err);
    if (rc != kOk)
        return rc;
    if (body.size() > kRulesMaxBytes) {
        if (err)
            err_set(err, kErrInvalidArg, "规则文件过大");
        return kErrInvalidArg;
    }
    if (cfg.blake3) {
        char hex[65];
        blake3_hex((const uint8_t *)body.data(), body.size(), hex);
        if (hex != *cfg.blake3) {
            if (err)
                err_set(err, kErrInvalidArg, "规则完整性校验失败，未保存更新");
            return kErrInvalidArg;
        }
    }
    JsonPtr parsed(cJSON_Parse(body.c_str()));
    if (!parsed) {
        if (err)
            err_set(err, kErrInvalidArg, "规则格式不正确");
        return kErrInvalidArg;
    }
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(parsed.get(), "categories");
    if (!cJSON_IsArray(cats)) {
        if (err)
            err_set(err, kErrInvalidArg, "规则文件缺少 categories");
        return kErrInvalidArg;
    }
    if ((size_t)cJSON_GetArraySize(cats) > kRulesMaxCount) {
        if (err)
            err_set(err, kErrInvalidArg, "远程规则过多（上限 %u 条）", kRulesMaxCount);
        return kErrInvalidArg;
    }
    /* 逐条校验（与 Rust validate 逐项一致）。 */
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cats) {
        if (!cJSON_IsObject(item) || validate_json_item(item, err) != kOk)
            return kErrInvalidArg;
    }
    /* 版本比较：incoming < old → 拒绝。 */
    auto path = remote_rules_path();
    if (!path) {
        if (err)
            err_set(err, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    uint64_t old_version = 0;
    {
        auto old_text = read_file_utf8(*path);
        if (old_text) {
            JsonPtr old(cJSON_Parse(old_text->c_str()));
            if (old) {
                cJSON *v = cJSON_GetObjectItemCaseSensitive(old.get(), "version");
                if (cJSON_IsNumber(v) && v->valuedouble >= 0)
                    old_version = (uint64_t)v->valuedouble;
            }
        }
    }
    uint64_t incoming_version = 1;
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(parsed.get(), "version");
        if (cJSON_IsNumber(v) && v->valuedouble >= 0)
            incoming_version = (uint64_t)v->valuedouble;
    }
    if (incoming_version < old_version) {
        if (err)
            err_set(err, kErrInvalidArg, "规则版本过旧：当前 %llu，收到 %llu",
                    (unsigned long long)old_version, (unsigned long long)incoming_version);
        return kErrInvalidArg;
    }
    /* 拼 payload {"version": N, "categories": [...]}（保留原始条目字段）。 */
    size_t accepted = (size_t)cJSON_GetArraySize(cats);
    cJSON *payload = cJSON_CreateObject();
    cJSON *cats_copy = cJSON_Duplicate(cats, 1);
    if (!payload || !cats_copy) {
        cJSON_Delete(payload);
        cJSON_Delete(cats_copy);
        if (err)
            err_set(err, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    cJSON_AddNumberToObject(payload, "version", (double)incoming_version);
    cJSON_AddItemToObject(payload, "categories", cats_copy);
    std::unique_ptr<char, JsonFree> encoded(cJSON_Print(payload));
    cJSON_Delete(payload);
    if (!encoded) {
        if (err)
            err_set(err, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    /* 原子写：temp → 备份旧文件 → rename。 */
    std::string temp = *path + ".tmp";
    if (write_file_utf8(temp, encoded.get()) != kOk) {
        if (err)
            err_set(err, kErrIo, "写入临时文件失败");
        return kErrIo;
    }
    if (file_exists_utf8(*path)) {
        auto backup = remote_rules_backup_path();
        if (backup)
            copy_file_utf8(*path, *backup); /* 备份失败不致命（与 Rust let _ = 一致） */
    }
    if (rename_replace_utf8(temp, *path) != kOk) {
        if (err)
            err_set(err, kErrIo, "替换规则文件失败");
        return kErrIo;
    }
    /* total = all_rules 总条数（与 Rust update() 返回值一致）。 */
    std::vector<Rule> all;
    size_t total = 0;
    if (load_all_rules(&all, nullptr) == kOk)
        total = all.size();
    out->accepted = accepted;
    out->total = total;
    out->message = "已保存到 " + *path + "，版本 " + std::to_string(incoming_version);
    return kOk;
#else
    if (err)
        err_set(err, kErrIo, "规则更新仅支持 Windows（WinHTTP）");
    return kErrIo;
#endif
}

int rollback_rules(std::string *out_message, Error *err) {
    if (err)
        err_init(err);
    auto path = remote_rules_path();
    auto backup = remote_rules_backup_path();
    if (!path || !backup) {
        if (err)
            err_set(err, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    if (!file_exists_utf8(*backup)) {
        if (err)
            err_set(err, kErrNotFound, "没有可回滚的规则版本");
        return kErrNotFound;
    }
    auto text = read_file_utf8(*backup);
    if (!text) {
        if (err)
            err_set(err, kErrIo, "读取备份规则失败");
        return kErrIo;
    }
    JsonPtr value(cJSON_Parse(text->c_str()));
    if (!value) {
        if (err)
            err_set(err, kErrInvalidArg, "备份规则损坏");
        return kErrInvalidArg;
    }
    cJSON *cats = cJSON_GetObjectItemCaseSensitive(value.get(), "categories");
    if (!cJSON_IsArray(cats)) {
        if (err)
            err_set(err, kErrInvalidArg, "备份规则缺少 categories");
        return kErrInvalidArg;
    }
    if ((size_t)cJSON_GetArraySize(cats) > kRulesMaxCount) {
        if (err)
            err_set(err, kErrInvalidArg, "备份规则数量超限");
        return kErrInvalidArg;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, cats) {
        if (!cJSON_IsObject(item) || validate_json_item(item, err) != kOk)
            return kErrInvalidArg;
    }
    uint64_t version = 0;
    {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(value.get(), "version");
        if (cJSON_IsNumber(v) && v->valuedouble >= 0)
            version = (uint64_t)v->valuedouble;
    }
    if (copy_file_utf8(*backup, *path) != kOk) {
        if (err)
            err_set(err, kErrIo, "回滚失败");
        return kErrIo;
    }
    if (out_message)
        *out_message = "已回滚到规则版本 " + std::to_string(version);
    return kOk;
}

}  // namespace cdc
