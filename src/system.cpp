/* system.cpp — 移植自 Rust 版 desktop/src/system.rs + main.rs 的
 * system32 / fixed_drives / get_drive_free_space。
 * 句柄删除/固定全部走 safety.h 的句柄 API；Linux 下 Windows 专属
 * 功能返回 kErrIo。 */
#include "system.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cJSON.h"
#include "cdc_process.h"
#include "im.h"
#include "logs.h"
#include "raii.h"
#include "safety.h"
#include "timeutil.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
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

bool cancelled(std::string_view task) {
    return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, std::string(task).c_str());
}

void progress(std::string_view task, uint64_t done, uint64_t total) {
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, std::string(task).c_str(), done, total);
}

void stage(std::string_view task, std::string_view s) {
    if (g_hooks.stage)
        g_hooks.stage(g_hooks.ctx, std::string(task).c_str(),
                      std::string(s).c_str());
}

void unit(std::string_view task, std::string_view u) {
    if (g_hooks.unit)
        g_hooks.unit(g_hooks.ctx, std::string(task).c_str(), std::string(u).c_str());
}

/* ---------------- 路径辅助（main.rs） ---------------- */

std::string env_or_string(const char *name, std::string_view fallback) {
    if (std::optional<std::string> value = getenv_utf8(name);
        value && !value->empty())
        return std::move(*value);
    return std::string(fallback);
}

std::string windows_dir() { return env_or_string("SystemRoot", "C:\\Windows"); }

std::string programdata_dir() {
    return env_or_string("ProgramData", "C:\\ProgramData");
}

std::string hiber_path() {
    std::string drive = env_or_string("SystemDrive", "C:");
    while (!drive.empty() && drive.back() == '\\') drive.pop_back();
    drive += "\\hiberfil.sys";
    return drive;
}

std::string path_join_win(std::string_view a, std::string_view b) {
    std::string out(a);
    while (!b.empty() && (b.front() == '\\' || b.front() == '/'))
        b.remove_prefix(1);
    if (!out.empty() && out.back() != '\\' && out.back() != '/')
        out.push_back('\\');
    out += b;
    return out;
}

bool path_exists_utf8(std::string_view path) {
#ifdef _WIN32
    auto w = utf8_to_wide(std::string(path));
    if (!w) return false;
    DWORD a = GetFileAttributesW(w->c_str());
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(std::string(path).c_str(), &st) == 0;
#endif
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

std::string trim_copy(std::string_view s) {
    while (!s.empty() && std::strchr(" \t\n\r", s.front()))
        s.remove_prefix(1);
    while (!s.empty() && std::strchr(" \t\n\r", s.back()))
        s.remove_suffix(1);
    return std::string(s);
}

/* ---------------- 资源管理器联动 ---------------- */

#ifdef _WIN32
std::string explorer_exe() {
    std::string root = env_or_string("SystemRoot", "C:\\Windows");
    while (!root.empty() && root.back() == '\\') root.pop_back();
    return root + "\\explorer.exe";
}

/* spawn 不等待：CreateProcessW 后立即关闭句柄（与 Rust
 * Command::spawn 一致，子进程独立运行）。 */
int spawn_no_wait(std::string_view exe, std::string_view args, Error *err) {
    std::string cmdline = "\"" + std::string(exe) + "\"";
    if (!args.empty()) {
        cmdline += " ";
        cmdline += args;
    }
    auto wcmd = utf8_to_wide(cmdline);
    if (!wcmd) {
        if (err) err_set(err, kErrUtf8, "invalid UTF-8 command");
        return kErrUtf8;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(NULL, wcmd->data(), NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        if (err)
            err_set(err, kErrIo, "启动失败（Win32 %lu）",
                    (unsigned long)GetLastError());
        return kErrIo;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return kOk;
}
#endif

/* ---------------- 系统清理项 ---------------- */

/* system 任务的已完成计数（每次 clean_system_item 入口清零；
 * Rust 的 task::add_done 是按任务累计的，这里用同一语义）。 */
uint64_t g_system_done = 0;

void system_add_done() {
    g_system_done++;
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "system", g_system_done, 0);
}

/* im_move 复制阶段的已复制字节数（每次搬家入口清零）。 */
uint64_t g_im_move_bytes_done = 0;

/* ASCII 不区分大小写比较（只折叠 A-Z；中文路径不受影响）。 */
bool ascii_iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

struct SizeCount {
    uint64_t size = 0;
    uint64_t count = 0;
};

SizeCount dir_size_count(std::string_view dir) {
    SizeCount result;
    Error ignored;
    walk_dir(std::string(dir).c_str(), 1,
             [&](const WalkEntry &entry) {
                 if (!entry.is_dir && !entry.is_symlink) {
                     result.size += entry.size;
                     result.count++;
                 }
                 return 0;
             },
             &ignored);
    return result;
}

std::optional<std::string> parent_of(std::string_view path) {
    /* 去尾分隔符后取最后一个分隔符之前；"C:\" 无父目录。 */
    size_t n = path.size();
    while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) n--;
    if (n == 0) return std::nullopt;
    size_t cut = n;
    while (cut > 0 && path[cut - 1] != '\\' && path[cut - 1] != '/') cut--;
    if (cut == 0) return std::nullopt;
    size_t plen = cut;
    while (plen > 1 && (path[plen - 1] == '\\' || path[plen - 1] == '/'))
        plen--;
    if (plen == 0) return std::nullopt;
    return std::string(path.substr(0, plen));
}

/* 祖先链上任一组件是重解析点，或某一级元数据读失败（不存在除外），
 * 都视为不安全（Rust path_has_reparse_ancestor 的语义）。 */
bool path_has_reparse_ancestor(std::string_view path) {
    std::string cur(path);
    for (;;) {
        bool is_reparse = false;
        Error e;
        int rc = reparse_point_status(cur, is_reparse, &e);
        if (rc == kOk) {
            if (is_reparse) return true;
        } else if (rc == kErrNotFound) {
            /* 不存在：继续向上检查祖先。 */
        } else {
            return true;
        }
        auto par = parent_of(cur);
        if (!par) return false;
        cur = std::move(*par);
    }
}

/* 清空目录内容（保留目录本身）：不跟随任何重解析点，逐个文件走
 * delete_file_checked（untrusted=false——目录本身是已知系统目录，
 * 不在远程 allowlist 内，Rust 版同样传 false），再自底向上删空目录。
 * 先收集文件列表再删除，避免在枚举过程中删除扰乱遍历。 */
void clean_dir_contents(std::string_view dir, uint64_t *out_freed, uint64_t *out_count) {
    *out_freed = 0;
    *out_count = 0;
    if (!path_exists_utf8(dir)) return;
    if (is_reparse_point(dir)) return;
    if (!is_dir_utf8(dir)) return;
    std::vector<std::string> files;
    Error ignored;
    walk_dir(std::string(dir).c_str(), 1,
             [&](const WalkEntry &entry) {
                 if (!entry.is_dir && !entry.is_symlink)
                     files.emplace_back(entry.path);
                 return 0;
             },
             &ignored);
    std::string dir_s(dir);
    std::vector<const char *> roots;
    roots.push_back(dir_s.c_str());
    for (const std::string &p : files) {
        if (cancelled("system")) break;
        /* 删除前先 stat 取大小（Rust 取 entry.metadata() 的 len）。 */
        uint64_t len = 0;
#ifdef _WIN32
        auto w = utf8_to_wide(p);
        if (w) {
            WIN32_FILE_ATTRIBUTE_DATA attr;
            if (GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &attr))
                len = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
        }
#else
        struct stat st;
        if (stat(p.c_str(), &st) == 0) len = (uint64_t)st.st_size;
#endif
        Error e;
        if (delete_file_checked(p, roots, false, &e) == DeleteOutcome::Deleted) {
            *out_freed += len;
            *out_count += 1;
            system_add_done();
        }
    }
    remove_empty_dirs(std::string(dir).c_str());
}

/* ---------------- 服务 ---------------- */

bool service_running(std::string_view name) {
    std::string sc = system32_path("sc.exe");
    std::string so, se;
    long code = -1;
    Error e;
    int rc = run_command(sc, "query " + std::string(name), true, &so, &se, &code, &e);
    return rc == kOk && so.find("RUNNING") != std::string::npos;
}

void start_services(std::span<const std::string> names) {
    /* 倒序启动（与 Rust start_services 的 .rev() 一致）。 */
    for (size_t ri = 0; ri < names.size(); ri++) {
        const std::string &name = names[names.size() - 1 - ri];
        std::string net = system32_path("net.exe");
        run_command(net, "start " + name, true, nullptr, nullptr, nullptr, nullptr);
    }
}

/* 停止仍在运行的服务，返回本次停掉的服务名；任何一个停不下来就把
 * 已停的恢复并报错。 */
int stop_services(std::span<const std::string> names, std::vector<std::string> *out_stopped,
                  Error *err) {
    out_stopped->clear();
    for (const std::string &name : names) {
        if (!service_running(name)) continue;
        std::string net = system32_path("net.exe");
        run_command(net, "stop " + name + " /y", true, nullptr, nullptr, nullptr,
                    nullptr);
        if (service_running(name)) {
            start_services(*out_stopped);
            if (err)
                err_set(err, kErrIo,
                        "%s 服务正在工作且无法暂停，请等待 Windows 更新完成后再清理",
                        name.c_str());
            return kErrIo;
        }
        out_stopped->push_back(name);
    }
    return kOk;
}

std::string mb_string(double mb) { return format("%.2f", mb); }

/* ---------------- 搬家恢复日志 ---------------- */

std::string journal_path() {
    auto cache = cache_dir();
    if (!cache) return std::string();
    return path_join_win(*cache, "im_move_pending.json");
}

struct MoveJournal {
    std::string source;
    std::string backup;
    std::string kind; /* 可能为空串（旧版日志无 kind 字段） */
};

int write_journal(std::string_view source, std::string_view backup, std::string_view kind,
                  Error *err) {
    if (err) err_init(err);
    auto cache = cache_dir();
    if (!cache) {
        if (err) err_set(err, kErrIo, "无法创建缓存目录");
        return kErrIo;
    }
    std::string cache_s = *cache;
    std::vector<const char *> roots;
    roots.push_back(cache_s.c_str());
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "source", std::string(source).c_str());
    cJSON_AddStringToObject(obj, "backup", std::string(backup).c_str());
    cJSON_AddStringToObject(obj, "kind", std::string(kind).c_str());
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!text) {
        if (err) err_set(err, kErrIo, "序列化搬家日志失败");
        return kErrIo;
    }
    std::string path = journal_path();
    HandleChain chain;
    OwnedHandle file;
    Error e;
    int rc = create_new_file_checked(path, roots, chain, file, &e);
    if (rc != kOk) {
        /* create_new 失败：Rust 映射 AlreadyExists（文件已存在时
         * create_new 返回 AlreadyExists）。 */
        /* text 是 cJSON_PrintUnformatted 的返回值，用 cJSON_free 释放。 */
        cJSON_free(text);
        if (err) *err = e;
        return rc;
    }
    int wrc = handle_write_all(file.get(), (const uint8_t *)text, strlen(text), &e);
    if (wrc == kOk) wrc = handle_sync(file.get(), &e);
    cJSON_free(text);
    if (wrc != kOk) {
        delete_file_checked(path, roots, false, nullptr);
        if (err) *err = e;
        return wrc;
    }
    return kOk;
}

void clear_journal() {
    std::string path = journal_path();
    auto cache = cache_dir();
    if (path.empty() || !cache) return;
    std::string cache_s = *cache;
    std::vector<const char *> roots;
    roots.push_back(cache_s.c_str());
    delete_file_checked(path, roots, false, nullptr);
}

int read_journal_text(std::string *out_text, Error *err) {
    if (err) err_init(err);
    std::string path = journal_path();
    auto cache = cache_dir();
    if (path.empty() || !cache) {
        if (err) err_set(err, kErrIo, "无法定位搬家日志");
        return kErrIo;
    }
    std::string cache_s = *cache;
    std::vector<const char *> roots;
    roots.push_back(cache_s.c_str());
    HandleChain chain;
    OwnedHandle file;
    Error e;
    int rc = open_read_file_checked(path, roots, chain, file, &e);
    if (rc != kOk) {
        if (err) *err = e;
        return rc;
    }
    std::string buf;
    uint8_t chunk[8192];
    for (;;) {
        size_t got = 0;
        if (handle_read(file.get(), chunk, sizeof(chunk), got, &e) != kOk || got == 0)
            break;
        buf.append((const char *)chunk, got);
    }
    *out_text = std::move(buf);
    return kOk;
}

int parse_journal(std::string_view text, MoveJournal *out) {
    *out = MoveJournal{};
    cJSON *obj = cJSON_Parse(std::string(text).c_str());
    if (!obj || !cJSON_IsObject(obj)) {
        if (obj) cJSON_Delete(obj);
        return kErrInvalidArg;
    }
    cJSON *s = cJSON_GetObjectItemCaseSensitive(obj, "source");
    cJSON *b = cJSON_GetObjectItemCaseSensitive(obj, "backup");
    cJSON *k = cJSON_GetObjectItemCaseSensitive(obj, "kind");
    if (!cJSON_IsString(s) || !cJSON_IsString(b)) {
        cJSON_Delete(obj);
        return kErrInvalidArg;
    }
    out->source = s->valuestring ? s->valuestring : "";
    out->backup = b->valuestring ? b->valuestring : "";
    out->kind = cJSON_IsString(k) && k->valuestring ? k->valuestring : "";
    cJSON_Delete(obj);
    return kOk;
}

bool path_has_reparse_parent(std::string_view path) {
    auto par = parent_of(path);
    if (!par) return true; /* 无父目录视为不安全（Rust 的 unwrap_or(true)） */
    return path_has_reparse_ancestor(*par);
}

/* ---------------- 搬家执行 ---------------- */

/* 列出目录的直接子项（名 + 是否目录，不跟随链接）。 */
struct DirEntry {
    std::string name;
    bool is_dir = false;
};

int list_entries(std::string_view dir, std::vector<DirEntry> *out) {
    out->clear();
#ifdef _WIN32
    std::string pat = path_join_win(dir, "*");
    auto wpat = utf8_to_wide(pat);
    if (!wpat) return kErrUtf8;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat->c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return kOk;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        auto u = wide_to_utf8(fd.cFileName);
        if (!u) continue;
        DirEntry e;
        e.name = std::move(*u);
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(std::string(dir).c_str());
    if (!d) return kOk;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        std::string full = path_join_win(dir, e->d_name);
        struct stat st;
        DirEntry de;
        de.name = e->d_name;
        de.is_dir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        out->push_back(std::move(de));
    }
    closedir(d);
#endif
    return kOk;
}

/* 递归复制目录内容（句柄相对打开；重解析点跳过；取消时返回
 * kErrIo 且 err 文案为“已取消”——调用方按失败处理）。 */
int copy_dir_contents(std::string_view src, std::string_view dst, RootList source_root,
                      RootList target_root, Error *err);

int copy_dir_recursive(std::string_view src, std::string_view dst, HandleChain *out_source,
                       HandleChain *out_target, Error *err);

int verify_copy(std::string_view src, std::string_view dst, Error *err);

bool cleanup_target(const std::string &target, HandleChain chain) {
    std::vector<const char *> roots;
    roots.push_back(target.c_str());
    uint64_t removed = 0;
    Error e;
    bool first = remove_tree_checked(target, roots, removed, &e) == kOk;
    chain.clear();
    if (first) return true;
    removed = 0;
    return remove_tree_checked(target, roots, removed, nullptr) == kOk;
}

}  // namespace

void system_set_hooks(const cdc_hooks_t *hooks) {
    g_hooks = hooks ? *hooks : cdc_hooks_t{};
}

std::string system32_path(std::string_view name) {
    std::string root = env_or_string("SystemRoot", "C:\\Windows");
    while (!root.empty() && root.back() == '\\') root.pop_back();
    root += "\\System32\\";
    root += name;
    return root;
}

int fixed_drives(std::vector<std::string> *out, Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (uint32_t i = 0; i < 26; i++) {
        if ((mask & (1u << i)) == 0) continue;
        std::string root = {static_cast<char>('A' + i), ':', '\\'};
        auto wide = utf8_to_wide(root);
        if (!wide) continue;
        if (GetDriveTypeW(wide->c_str()) == DRIVE_FIXED)
            out->push_back(root);
    }
#endif
    if (out->empty()) out->emplace_back("C:\\");
    return kOk;
}

int drive_free_space(std::string_view drive, uint64_t *out_free, uint64_t *out_total,
                     Error *err) {
    if (err) err_init(err);
    if (!out_free || !out_total) {
        if (err) err_set(err, kErrInvalidArg, "NULL param");
        return kErrInvalidArg;
    }
#ifdef _WIN32
    auto wide = utf8_to_wide(std::string(drive));
    if (!wide) {
        if (err) err_set(err, kErrUtf8, "invalid UTF-8 drive");
        return kErrUtf8;
    }
    ULARGE_INTEGER free_avail{}, total{}, total_free{};
    if (!GetDiskFreeSpaceExW(wide->c_str(), &free_avail, &total, &total_free)) {
        if (err) err_set(err, kErrIo, "获取磁盘空间失败");
        return kErrIo;
    }
    *out_free = free_avail.QuadPart;
    *out_total = total.QuadPart;
    return kOk;
#else
    (void)drive;
    if (err) err_set(err, kErrIo, "仅支持 Windows");
    return kErrIo;
#endif
}

int reveal_in_explorer(std::string_view path, Error *err) {
    if (err) err_init(err);
    if (path.empty() || !path_exists_utf8(path)) {
        if (err) err_set(err, kErrNotFound, "文件不存在：%s", std::string(path).c_str());
        return kErrNotFound;
    }
#ifdef _WIN32
    /* /select,"C:\..."：'/' 换 '\'（与 Rust .replace('/', "\\") 一致）。 */
    std::string arg = "/select,\"";
    for (char c : path) arg += (c == '/') ? '\\' : c;
    arg += "\"";
    int rc = spawn_no_wait(explorer_exe(), arg, err);
    if (rc != kOk && err && err->code == kErrIo)
        err_set(err, kErrIo, "启动资源管理器失败：%s", std::string(path).c_str());
    return rc;
#else
    if (err) err_set(err, kErrIo, "仅支持 Windows");
    return kErrIo;
#endif
}

int open_path(std::string_view path, Error *err) {
    if (err) err_init(err);
    if (path.empty() || !path_exists_utf8(path)) {
        if (err) err_set(err, kErrNotFound, "路径不存在：%s", std::string(path).c_str());
        return kErrNotFound;
    }
#ifdef _WIN32
    std::string arg = "\"";
    for (char c : path) arg += (c == '/') ? '\\' : c;
    arg += "\"";
    int rc = spawn_no_wait(explorer_exe(), arg, err);
    if (rc != kOk && err && err->code == kErrIo)
        err_set(err, kErrIo, "打开路径失败：%s", std::string(path).c_str());
    return rc;
#else
    if (err) err_set(err, kErrIo, "仅支持 Windows");
    return kErrIo;
#endif
}

int query_hiber(HiberInfo *out, Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "out is NULL");
        return kErrInvalidArg;
    }
    *out = HiberInfo{};
    std::string path = hiber_path();
    uint64_t size = 0;
    bool exists = false;
#ifdef _WIN32
    auto w = utf8_to_wide(path);
    if (w) {
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &attr)) {
            size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
            exists = true;
        }
    }
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        size = (uint64_t)st.st_size;
        exists = true;
    }
#endif
    out->enabled = exists;
    out->size = size;
    out->path = path;
    if (exists) {
        out->description = format(
            "休眠文件正在占用 %.2f GB 空间。若无需使用系统休眠（如只用睡眠），可一键关闭释放空间。",
            (double)size / (1024.0 * 1024.0 * 1024.0));
    } else {
        out->description = "系统休眠未开启，未占用磁盘空间。";
    }
    return kOk;
}

int set_hiber(std::string_view mode, std::string *out_msg, Error *err) {
    if (err) err_init(err);
    if (out_msg) out_msg->clear();
    std::string args;
    std::string ok_msg;
    if (mode == "off") {
        args = "/h off";
        ok_msg = "休眠功能已彻底关闭，hiberfil.sys 已移除并释放全部空间。";
    } else if (mode == "reduced") {
        args = "/h /type reduced";
        ok_msg = "休眠文件已精简为 Reduced 模式（体积缩减约 50%）。";
    } else if (mode == "full" || mode == "on") {
        args = "/h on";
        ok_msg = "休眠功能已开启。";
    } else {
        if (err) err_set(err, kErrInvalidArg, "无效的休眠模式参数");
        return kErrInvalidArg;
    }
    std::string powercfg = system32_path("powercfg.exe");
    std::string so, se;
    long code = -1;
    Error e2;
    int rc = run_command(powercfg, args, true, &so, &se, &code, &e2);
    if (rc != kOk || code != 0) {
        /* Rust 取 stderr 的 lossy 文本 trim 后拼进错误。 */
        std::string detail = trim_copy(se);
        if (err) err_set(err, kErrIo, "设置休眠失败：%s", detail.c_str());
        return kErrIo;
    }
    if (out_msg) *out_msg = ok_msg;
    return kOk;
}

int empty_all_recycle_bins(Error *err) {
    if (err) err_init(err);
#ifdef _WIN32
    HRESULT code = SHEmptyRecycleBinW(NULL, NULL,
                                      SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI |
                                          SHERB_NOSOUND);
    /* S_OK = 0, S_FALSE = 1。 */
    if (code == 0 || code == 1) return kOk;
    if (err)
        err_set(err, kErrIo, "清空回收站返回错误码：0x%lX", (unsigned long)code);
    return kErrIo;
#else
    if (err) err_set(err, kErrIo, "仅支持 Windows");
    return kErrIo;
#endif
}

int scan_system_deep(std::vector<SystemItem> *out, Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
    auto push = [&](std::string key, std::string name, std::string describe, uint64_t size,
                    uint64_t count, bool safe, bool is_command) {
        SystemItem it;
        it.key = std::move(key);
        it.name = std::move(name);
        it.describe = std::move(describe);
        it.size = size;
        it.count = count;
        it.safe_to_clean = safe;
        it.is_command = is_command;
        out->push_back(std::move(it));
    };
    std::string win = windows_dir();
    std::string upd = path_join_win(win, "SoftwareDistribution\\Download");
    if (path_exists_utf8(upd) && is_dir_utf8(upd)) {
        SizeCount s = dir_size_count(upd);
        push("sys_update_dl", "Windows 更新下载安装包",
             "系统更新完成后的旧安装包和补丁残留，清理后不影响已安装的更新", s.size, s.count,
             true, false);
    }
    std::string dopt = path_join_win(win, "SoftwareDistribution\\DeliveryOptimization");
    if (path_exists_utf8(dopt) && is_dir_utf8(dopt)) {
        SizeCount s = dir_size_count(dopt);
        push("sys_delivery_opt", "Windows 传递优化网络缓存",
             "微软更新与其他电脑局域网共享的下载分片缓存", s.size, s.count, true, false);
    }
    /* 崩溃转储：%SystemRoot%\Minidump + %ProgramData%\Microsoft\Windows\WER
     * + %LOCALAPPDATA%\CrashDumps。 */
    uint64_t dump_size = 0, dump_count = 0;
    std::string md = path_join_win(win, "Minidump");
    std::string wer = path_join_win(programdata_dir(), "Microsoft\\Windows\\WER");
    std::string cd;
    if (auto local = getenv_utf8("LOCALAPPDATA"))
        cd = path_join_win(*local, "CrashDumps");
    const std::string *dump_dirs[3] = {&md, &wer, &cd};
    for (int i = 0; i < 3; i++) {
        if (dump_dirs[i]->empty()) continue;
        if (path_exists_utf8(*dump_dirs[i])) {
            SizeCount s = dir_size_count(*dump_dirs[i]);
            dump_size += s.size;
            dump_count += s.count;
        }
    }
    push("sys_crash_dumps", "系统崩溃转储与错误报告",
         "软件或系统蓝屏崩溃时记录的 .dmp 文件与诊断日志", dump_size, dump_count, true,
         false);
    push("sys_dism_winsxs", "WinSxS 系统组件库精简 (DISM)",
         "执行 DISM 原生命令精简 WinSxS 目录下的历史备份更新，通常可释放 3~15GB（耗时约 1~3 分钟）",
         0, 0, true, true);
    return kOk;
}

int clean_system_item(std::string_view key, uint64_t *out_freed, uint64_t *out_count,
                      std::string *out_msg, Error *err) {
    if (err) err_init(err);
    g_system_done = 0;
    if (out_freed) *out_freed = 0;
    if (out_count) *out_count = 0;
    if (out_msg) out_msg->clear();
    if (key == "sys_update_dl") {
        stage("system", "停止更新服务");
        std::vector<std::string> svc = {"wuauserv", "bits"};
        std::vector<std::string> stopped;
        Error e;
        if (stop_services(svc, &stopped, &e) != kOk) {
            if (err) *err = e;
            return kErrIo;
        }
        stage("system", "清理更新安装包");
        std::string dir = path_join_win(windows_dir(), "SoftwareDistribution\\Download");
        uint64_t freed = 0, count = 0;
        clean_dir_contents(dir, &freed, &count);
        stage("system", "恢复更新服务");
        start_services(stopped);
        if (out_freed) *out_freed = freed;
        if (out_count) *out_count = count;
        if (out_msg)
            *out_msg = format("已清理更新安装包 %llu 个，释放 %s MB",
                              (unsigned long long)count, mb_string((double)freed / 1048576.0).c_str());
        return kOk;
    }
    if (key == "sys_delivery_opt") {
        stage("system", "调用系统清理传递优化缓存");
        std::string dir =
            path_join_win(windows_dir(), "SoftwareDistribution\\DeliveryOptimization");
        SizeCount before = dir_size_count(dir);
        std::string ps =
            system32_path("WindowsPowerShell\\v1.0\\powershell.exe");
        const char *args =
            "-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
            "\"Delete-DeliveryOptimizationCache -Force -ErrorAction Stop\"";
        std::string so, se;
        long code = -1;
        Error e2;
        int rc = run_command(ps, args, true, &so, &se, &code, &e2);
        if (rc != kOk || code != 0) {
            std::string detail = trim_copy(se);
            if (err) err_set(err, kErrIo, "传递优化缓存清理失败：%s", detail.c_str());
            return kErrIo;
        }
        SizeCount after = dir_size_count(dir);
        uint64_t freed = before.size >= after.size ? before.size - after.size : 0;
        uint64_t count = before.count >= after.count ? before.count - after.count : 0;
        if (out_freed) *out_freed = freed;
        if (out_count) *out_count = count;
        if (out_msg)
            *out_msg = format("已清理传递优化分片 %llu 个，释放 %s MB",
                              (unsigned long long)count, mb_string((double)freed / 1048576.0).c_str());
        return kOk;
    }
    if (key == "sys_crash_dumps") {
        stage("system", "清理崩溃转储与错误报告");
        std::string md = path_join_win(windows_dir(), "Minidump");
        std::string wer = path_join_win(programdata_dir(), "Microsoft\\Windows\\WER");
        std::string cd;
        if (auto local = getenv_utf8("LOCALAPPDATA"))
            cd = path_join_win(*local, "CrashDumps");
        const std::string *dirs[3] = {&md, &wer, &cd};
        uint64_t freed = 0, count = 0;
        for (int i = 0; i < 3; i++) {
            if (dirs[i]->empty()) continue;
            uint64_t f = 0, c = 0;
            clean_dir_contents(*dirs[i], &f, &c);
            freed += f;
            count += c;
        }
        if (out_freed) *out_freed = freed;
        if (out_count) *out_count = count;
        if (out_msg)
            *out_msg = format("已清理转储文件 %llu 个，释放 %s MB",
                              (unsigned long long)count, mb_string((double)freed / 1048576.0).c_str());
        return kOk;
    }
    if (key == "sys_dism_winsxs") {
        stage("system", "DISM 组件库精简（约 1~3 分钟）");
        std::string dism = system32_path("Dism.exe");
        std::string so, se;
        long code = -1;
        Error e2;
        int rc = run_command(dism, "/Online /Cleanup-Image /StartComponentCleanup", true,
                             &so, &se, &code, &e2);
        if (rc == kOk && code == 0) {
            if (out_msg) *out_msg = "DISM WinSxS 组件库精简优化已成功完成！";
            return kOk;
        }
        std::string detail = trim_copy(se);
        if (err) err_set(err, kErrIo, "DISM 组件精简失败：%s", detail.c_str());
        return kErrIo;
    }
    if (err) err_set(err, kErrInvalidArg, "未知系统清理项");
    return kErrInvalidArg;
}

/* ---------------- IM 迁移 ---------------- */

int analyze_im_migration(std::string_view kind, MoveAnalysis *out, Error *err) {
    if (err) err_init(err);
    if (!out || kind.empty()) {
        if (err) err_set(err, kErrInvalidArg, "NULL param");
        return kErrInvalidArg;
    }
    *out = MoveAnalysis{};
    std::vector<std::string> roots;
    if (im_roots(kind, &roots, nullptr) != kOk) {
        if (err) err_set(err, kErrIo, "无法探测 %s 数据目录", std::string(kind).c_str());
        return kErrIo;
    }
    std::string best;
    uint64_t max_size = 0, max_count = 0;
    for (const std::string &root : roots) {
        std::string norm = normalize(root);
        if (im_is_program_root(norm)) continue;
        /* 以 C: 开头（Rust 用 to_uppercase().starts_with("C:")）。 */
        bool is_c = norm.size() >= 2 && (norm[0] == 'C' || norm[0] == 'c') &&
                    norm[1] == ':';
        if (is_c) {
            SizeCount s = dir_size_count(norm);
            if (s.size > max_size) {
                max_size = s.size;
                max_count = s.count;
                best = norm;
            }
        }
    }
    out->source = best;
    out->exists = !best.empty();
    out->size = max_size;
    out->file_count = max_count;
    out->is_junction = !best.empty() && is_reparse_point(best);
    std::vector<std::string> drives;
    if (fixed_drives(&drives, nullptr) == kOk) {
        for (const std::string &d : drives) {
            bool is_c = d.size() >= 2 && (d[0] == 'C' || d[0] == 'c') && d[1] == ':';
            if (is_c) continue;
            uint64_t free_b = 0, total_b = 0;
            if (drive_free_space(d, &free_b, &total_b, nullptr) == kOk) {
                DriveTarget t;
                t.letter = d;
                t.free_bytes = free_b;
                t.total_bytes = total_b;
                out->targets.push_back(std::move(t));
            }
        }
    }
    return kOk;
}

bool safe_for_quoted_cmd_path(std::string_view path) {
    if (path.empty()) return false;
    for (unsigned char c : path) {
        if (c == '"' || c == '%' || c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

bool valid_recovery_paths(std::string_view source, std::string_view backup,
                          std::span<const std::string> allowed_roots) {
    if (source.empty() || backup.empty()) return false;
    if (path_syntax_error(source) || path_syntax_error(backup) ||
        is_protected_path(source) || is_never_clean(source) || is_drive_root(source) ||
        is_critical_file(source) || is_password_store(source))
        return false;
    /* 必须都是绝对路径：盘符 X:\ 或 UNC \\ 开头。 */
    auto is_abs = [](std::string_view p) {
        return (p.size() >= 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) ||
               (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
    };
    if (!is_abs(source) || !is_abs(backup)) return false;
    std::string source_norm = normalize(source);
    bool allowed = false;
    for (const std::string &r : allowed_roots) {
        if (ascii_iequal(normalize(r), source_norm)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) return false;
    auto sp = parent_of(source);
    auto bp = parent_of(backup);
    std::string sn = basename(source);
    std::string bn = basename(backup);
    if (!sp || !bp || sn.empty() || bn.empty()) return false;
    if (!ascii_iequal(normalize(*sp), normalize(*bp))) return false;
    /* backup 名 = source 名小写 + "_backup_" + 15 字符戳
     *（YYYYMMDD_HHMMSS：8 位数字 + '_' + 6 位数字）。 */
    size_t sl = sn.size(), bl = bn.size();
    if (bl <= sl + 8) return false;
    for (size_t i = 0; i < sl; i++) {
        char a = sn[i], b = bn[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    if (bn.compare(sl, 8, "_backup_") != 0) return false;
    std::string_view stamp = std::string_view(bn).substr(sl + 8);
    if (stamp.size() != 15 || stamp[8] != '_') return false;
    for (int i = 0; i < 15; i++) {
        if (i == 8) continue;
        if (stamp[i] < '0' || stamp[i] > '9') return false;
    }
    return true;
}

void recover_pending_move() {
    std::string text;
    Error e;
    if (read_journal_text(&text, &e) != kOk) return; /* 不存在或读失败：静默返回 */
    MoveJournal j;
    if (parse_journal(text, &j) != kOk) {
        /* 格式错误的日志保留，供诊断；不删除。 */
        return;
    }
    std::vector<std::string> allowed;
    if (j.kind == "wechat") {
        im_migration_roots("wechat", &allowed, nullptr);
    } else if (j.kind == "qq") {
        im_migration_roots("qq", &allowed, nullptr);
    } else if (j.kind.empty()) {
        std::vector<std::string> w, q;
        im_migration_roots("wechat", &w, nullptr);
        im_migration_roots("qq", &q, nullptr);
        allowed.insert(allowed.end(), w.begin(), w.end());
        allowed.insert(allowed.end(), q.begin(), q.end());
    } else {
        return;
    }
    if (!valid_recovery_paths(j.source, j.backup, allowed) ||
        path_has_reparse_parent(j.source) || path_has_reparse_parent(j.backup)) {
        return;
    }
    /* 源路径已存在：绝不覆盖；备份不存在时清日志，否则保留日志并记一笔。 */
    if (path_exists_utf8(j.source)) {
        if (!path_exists_utf8(j.backup)) {
            clear_journal();
        } else {
            std::string line = "搬家恢复日志保留：源路径已存在，但备份目录仍在或状态不确定：" + j.backup;
            std::string lines[1] = {line};
            write_log(lines, nullptr, nullptr);
        }
        return;
    }
    /* 备份必须是普通目录（非重解析点）。 */
    bool backup_ok = false;
#ifdef _WIN32
    auto w = utf8_to_wide(j.backup);
    if (w) {
        DWORD a = GetFileAttributesW(w->c_str());
        backup_ok = a != INVALID_FILE_ATTRIBUTES &&
                    (a & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
#else
    struct stat st;
    backup_ok = stat(j.backup.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
    if (!backup_ok) return;
    std::vector<const char *> root_ptrs;
    root_ptrs.reserve(allowed.size());
    for (auto &r : allowed) root_ptrs.push_back(r.c_str());
    Error e2;
    if (rename_directory_sibling_checked(j.backup, j.source, root_ptrs, &e2) == kOk) {
        std::string line =
            "微信/QQ 搬家中断恢复：" + j.backup + " 已还原为 " + j.source;
        std::string lines[1] = {line};
        write_log(lines, nullptr, nullptr);
        clear_journal();
    }
}

namespace {

int copy_dir_contents(std::string_view src, std::string_view dst, RootList source_root,
                      RootList target_root, Error *err) {
    HandleChain chain;
    Error e;
    if (pin_directory_checked(src, source_root, chain, &e) != kOk) {
        if (err) *err = e;
        return kErrIo;
    }
    std::vector<DirEntry> entries;
    list_entries(src, &entries);
    int rc = kOk;
    for (size_t i = 0; i < entries.size() && rc == kOk; i++) {
        if (cancelled("im_move")) {
            if (err) err_set(err, kErrIo, "已取消");
            rc = kErrIo;
            break;
        }
        std::string from = path_join_win(src, entries[i].name);
        std::string to = path_join_win(dst, entries[i].name);
        bool is_reparse = false;
        Error e2;
        if (reparse_point_status(from, is_reparse, &e2) != kOk) {
            if (err) *err = e2;
            rc = kErrIo;
            break;
        }
        if (is_reparse) continue;
        if (entries[i].is_dir) {
            {
                HandleChain dc;
                OwnedHandle dirh;
                if (create_directory_checked(to, target_root, dc, dirh, &e2) != kOk) {
                    if (err) *err = e2;
                    rc = kErrIo;
                    break;
                }
            }
            rc = copy_dir_contents(from, to, source_root, target_root, err);
            continue;
        }
        /* 普通文件：句柄相对打开源、创建目标，流式复制后 sync。 */
        HandleChain sc, tc;
        OwnedHandle sf, tf;
        if (open_read_file_checked(from, source_root, sc, sf, &e2) != kOk) {
            if (err) *err = e2;
            rc = kErrIo;
            break;
        }
        if (create_new_file_checked(to, target_root, tc, tf, &e2) != kOk) {
            if (err) *err = e2;
            rc = kErrIo;
            break;
        }
        uint8_t buf[65536];
        uint64_t copied = 0;
        bool io_err = false;
        for (;;) {
            size_t got = 0;
            if (handle_read(sf.get(), buf, sizeof(buf), got, &e2) != kOk) {
                if (err) *err = e2;
                io_err = true;
                break;
            }
            if (got == 0) break;
            if (handle_write_all(tf.get(), buf, got, &e2) != kOk) {
                if (err) *err = e2;
                io_err = true;
                break;
            }
            copied += got;
        }
        if (!io_err && handle_sync(tf.get(), &e2) != kOk) {
            if (err) *err = e2;
            io_err = true;
        }
        if (io_err) {
            rc = kErrIo;
            break;
        }
        /* Rust 按字节累计进度（set_unit "字节"）。 */
        g_im_move_bytes_done += copied;
        if (g_hooks.progress)
            g_hooks.progress(g_hooks.ctx, "im_move", g_im_move_bytes_done, 0);
    }
    return rc;
}

int copy_dir_recursive(std::string_view src, std::string_view dst, HandleChain *out_source,
                       HandleChain *out_target, Error *err) {
    std::string src_s(src), dst_s(dst);
    std::vector<const char *> source_root{src_s.c_str()};
    std::vector<const char *> target_root{dst_s.c_str()};
    HandleChain source_chain;
    Error e;
    if (pin_directory_checked(src, source_root, source_chain, &e) != kOk) {
        if (err) *err = e;
        return kErrIo;
    }
    HandleChain target_chain;
    if (create_directory_tree_checked(dst, target_chain, &e) != kOk) {
        if (err) *err = e;
        return kErrIo;
    }
    if (copy_dir_contents(src, dst, source_root, target_root, err) != kOk) {
        uint64_t removed = 0;
        remove_tree_checked(dst, target_root, removed, nullptr);
        cleanup_target(dst_s, std::move(target_chain));
        return kErrIo;
    }
    if (out_source) *out_source = std::move(source_chain);
    if (out_target) *out_target = std::move(target_chain);
    return kOk;
}

/* 逐文件校验：大小 + 64KB 分块内容比对（句柄相对打开）。 */
int verify_copy(std::string_view src, std::string_view dst, Error *err) {
    std::string src_s(src), dst_s(dst);
    std::vector<const char *> source_root{src_s.c_str()};
    std::vector<const char *> target_root{dst_s.c_str()};
    HandleChain sc, tc;
    Error e;
    if (pin_directory_checked(src, source_root, sc, &e) != kOk) {
        if (err) *err = e;
        return kErrIo;
    }
    if (pin_directory_checked(dst, target_root, tc, &e) != kOk) {
        if (err) *err = e;
        return kErrIo;
    }
    /* 收集源目录下的普通文件（相对路径）。 */
    std::vector<std::string> files;
    walk_dir(src_s.c_str(), 1,
             [&](const WalkEntry &entry) {
                 if (entry.is_dir || entry.is_symlink) return 0;
                 std::string_view rel = entry.path;
                 rel.remove_prefix(src_s.size());
                 while (!rel.empty() && (rel.front() == '\\' || rel.front() == '/'))
                     rel.remove_prefix(1);
                 files.emplace_back(rel);
                 return 0;
             },
             nullptr);
    int rc = kOk;
    uint64_t done = 0;
    for (size_t i = 0; i < files.size() && rc == kOk; i++) {
        if (cancelled("im_move")) {
            if (err) err_set(err, kErrIo, "已取消");
            rc = kErrIo;
            break;
        }
        const std::string &rel = files[i];
        std::string sp = path_join_win(src, rel);
        std::string dp = path_join_win(dst, rel);
        HandleChain sfc, tfc;
        OwnedHandle sf, tf;
        if (open_read_file_checked(sp, source_root, sfc, sf, &e) != kOk) {
            if (err)
                err_set(err, kErrIo, "%s 无法安全读取：%s", rel.c_str(), e.message);
            rc = kErrIo;
        } else if (open_read_file_checked(dp, target_root, tfc, tf, &e) != kOk) {
            if (err)
                err_set(err, kErrIo, "%s 未完整拷贝：%s", rel.c_str(), e.message);
            rc = kErrIo;
        } else {
            uint64_t ss = 0, ts = 0;
            if (handle_file_size(sf.get(), ss, &e) != kOk ||
                handle_file_size(tf.get(), ts, &e) != kOk || ss != ts) {
                if (err) err_set(err, kErrIo, "%s 大小不一致", rel.c_str());
                rc = kErrIo;
            } else {
                uint8_t left[65536], right[65536];
                for (;;) {
                    size_t lg = 0, rg = 0;
                    int lr = handle_read(sf.get(), left, sizeof(left), lg, &e);
                    int rr = handle_read(tf.get(), right, sizeof(right), rg, &e);
                    if (lr != kOk || rr != kOk) {
                        if (err) err_set(err, kErrIo, "%s 读取失败", rel.c_str());
                        rc = kErrIo;
                        break;
                    }
                    if (lg != rg || (lg > 0 && memcmp(left, right, lg) != 0)) {
                        if (err) err_set(err, kErrIo, "%s 内容不一致", rel.c_str());
                        rc = kErrIo;
                        break;
                    }
                    if (lg == 0) break;
                }
            }
        }
        if (rc == kOk) {
            done++;
            progress("im_move", done, 0);
        }
    }
    return rc;
}

}  // namespace

int execute_im_migration(std::string_view source_str, std::string_view target_dir_str,
                         std::string_view kind, std::string *out_msg, Error *err) {
    if (err) err_init(err);
    if (out_msg) out_msg->clear();
    if (source_str.empty() || target_dir_str.empty() || kind.empty()) {
        if (err) err_set(err, kErrInvalidArg, "NULL param");
        return kErrInvalidArg;
    }
    if (!safe_for_quoted_cmd_path(source_str) || !safe_for_quoted_cmd_path(target_dir_str)) {
        if (err) err_set(err, kErrInvalidArg, "迁移路径包含不受支持的命令行字符");
        return kErrInvalidArg;
    }
    if (!path_exists_utf8(source_str)) {
        if (err) err_set(err, kErrNotFound, "源数据目录不存在");
        return kErrNotFound;
    }
    if (!is_dir_utf8(source_str)) {
        if (err) err_set(err, kErrNotFound, "源数据目录不存在");
        return kErrNotFound;
    }
    if (kind != "wechat" && kind != "qq") {
        if (err) err_set(err, kErrInvalidArg, "无效的搬家类型");
        return kErrInvalidArg;
    }
    std::vector<std::string> roots;
    if (im_roots(kind, &roots, nullptr) != kOk) {
        if (err) err_set(err, kErrIo, "无法探测 %s 数据目录", std::string(kind).c_str());
        return kErrIo;
    }
    std::vector<const char *> root_ptrs;
    root_ptrs.reserve(roots.size());
    for (auto &r : roots) root_ptrs.push_back(r.c_str());
    std::string source_norm = normalize(source_str);
    bool is_root = false;
    for (const std::string &r : roots) {
        if (ascii_iequal(normalize(r), source_norm)) {
            is_root = true;
            break;
        }
    }
    if (!is_root) {
        if (err)
            err_set(err, kErrInvalidArg,
                    "只能迁移微信/QQ 自己探测到的数据根目录，已拒绝本次操作");
        return kErrInvalidArg;
    }
    if (explicit_delete_forbidden(source_str)) {
        if (err) err_set(err, kErrInvalidArg, "该目录受保护，不能迁移");
        return kErrInvalidArg;
    }
    bool is_reparse = false;
    Error e2;
    if (reparse_point_status(source_str, is_reparse, &e2) != kOk) {
        if (err) err_set(err, kErrIo, "无法安全检查源目录：%s", e2.message);
        return kErrIo;
    }
    if (is_reparse) {
        if (err)
            err_set(err, kErrInvalidArg,
                    "源目录已经是软链接/联接点，之前已经完成过搬家");
        return kErrInvalidArg;
    }
    {
        std::string source_s(source_str);
        std::vector<const char *> within{source_s.c_str()};
        if (is_within(target_dir_str, within)) {
            if (err) err_set(err, kErrInvalidArg, "目标目录不能位于源目录内部");
            return kErrInvalidArg;
        }
    }
    if (path_exists_utf8(target_dir_str)) {
        if (err)
            err_set(err, kErrInvalidArg,
                    "目标目录已存在，请换一个位置或先手动清理");
        return kErrInvalidArg;
    }
    std::vector<std::string> procs = {"wechat.exe", "qq.exe", "xwechat.exe"};
    std::vector<std::string> running;
    check_running(procs, &running, nullptr);
    if (!running.empty()) {
        std::string msg = "检测到 ";
        for (size_t i = 0; i < running.size(); i++) {
            if (i > 0) msg += "、";
            msg += running[i];
        }
        msg += " 正在运行，请先彻底退出该软件后再执行搬家！";
        if (err) err_set(err, kErrIo, "%s", msg.c_str());
        return kErrIo;
    }

    stage("im_move", "统计源数据");
    SizeCount src_sc = dir_size_count(source_str);
    uint64_t src_size = src_sc.size, file_cnt = src_sc.count;
    if (src_size == 0 || file_cnt == 0) {
        if (err) err_set(err, kErrInvalidArg, "源数据目录是空的，无需搬家");
        return kErrInvalidArg;
    }
    std::string target_norm = normalize(target_dir_str);
    std::string target_drive;
    if (target_norm.size() >= 3) target_drive = target_norm.substr(0, 3);
    std::vector<std::string> drives;
    bool drive_ok = false;
    if (fixed_drives(&drives, nullptr) == kOk) {
        for (const std::string &d : drives) {
            if (ascii_iequal(d, target_drive)) {
                drive_ok = true;
                break;
            }
        }
    }
    if (!drive_ok) {
        if (err) err_set(err, kErrInvalidArg, "目标位置不是本机固定磁盘");
        return kErrInvalidArg;
    }
    uint64_t free_b = 0, total_b = 0;
    if (drive_free_space(target_drive, &free_b, &total_b, nullptr) == kOk) {
        if (free_b < src_size + (1024ULL * 1024ULL * 1024ULL)) {
            if (err)
                err_set(err, kErrInvalidArg,
                        "目标磁盘剩余空间不足，请选择剩余空间更充裕的磁盘");
            return kErrInvalidArg;
        }
    }

    stage("im_move", "复制文件");
    unit("im_move", "字节");
    g_im_move_bytes_done = 0;
    progress("im_move", 0, src_size);
    HandleChain source_chain, target_chain;
    Error e3;
    if (copy_dir_recursive(source_str, target_dir_str, &source_chain, &target_chain,
                           &e3) != kOk) {
        if (err)
            err_set(err, kErrIo, "拷贝文件至新目录失败：%s", e3.message);
        return kErrIo;
    }
    stage("im_move", "校验文件");
    unit("im_move", "个文件");
    progress("im_move", 0, file_cnt);
    SizeCount dst_sc = dir_size_count(target_dir_str);
    if (dst_sc.size != src_size || dst_sc.count != file_cnt) {
        cleanup_target(std::string(target_dir_str), std::move(target_chain));
        if (err)
            err_set(err, kErrIo,
                    "拷贝校验失败（源 %llu 个文件 / %llu 字节，目标 %llu 个文件 / %llu 字节），已放弃搬家并清理目标目录",
                    (unsigned long long)file_cnt, (unsigned long long)src_size,
                    (unsigned long long)dst_sc.count, (unsigned long long)dst_sc.size);
        return kErrIo;
    }
    if (verify_copy(source_str, target_dir_str, &e3) != kOk) {
        cleanup_target(std::string(target_dir_str), std::move(target_chain));
        if (err)
            err_set(err, kErrIo, "逐文件校验失败（%s），已放弃搬家并清理目标目录",
                    e3.message);
        return kErrIo;
    }

    /* 原目录改名备份（先写恢复日志，再改名）。 */
    stage("im_move", "备份原目录");
    std::string backup_name =
        basename(source_str) + "_backup_" + stamp();
    auto spar = parent_of(source_str);
    std::string backup_path = spar ? path_join_win(*spar, backup_name) : std::string();
    if (backup_path.empty()) {
        cleanup_target(std::string(target_dir_str), std::move(target_chain));
        if (err) err_set(err, kErrNoMem, "out of memory");
        return kErrNoMem;
    }
    Error je;
    if (write_journal(source_str, backup_path, kind, &je) != kOk) {
        cleanup_target(std::string(target_dir_str), std::move(target_chain));
        if (err) {
            if (je.code == kErrExists)
                err_set(err, kErrIo,
                        "发现尚未解决的搬家恢复日志，原目录未改名；请先处理上次搬家留下的备份状态。");
            else
                err_set(err, kErrIo, "搬家恢复日志未能可靠写入，原目录未改名：%s",
                        je.message);
        }
        return kErrIo;
    }
    Error re;
    if (rename_directory_sibling_checked(source_str, backup_path, root_ptrs, &re) != kOk) {
        clear_journal();
        cleanup_target(std::string(target_dir_str), std::move(target_chain));
        if (err)
            err_set(err, kErrIo, "备份原目录失败（可能被占用）：%s", re.message);
        return kErrIo;
    }

    /* 在原位置创建 NTFS Junction (mklink /J)。 */
    stage("im_move", "创建目录联接");
    std::string cmd_exe = system32_path("cmd.exe");
    std::string s = std::string(source_str);
    std::string t = std::string(target_dir_str);
    for (char &c : s)
        if (c == '/') c = '\\';
    for (char &c : t)
        if (c == '/') c = '\\';
    while (!s.empty() && s.back() == '\\') s.pop_back();
    while (!t.empty() && t.back() == '\\') t.pop_back();
    std::string margs = "/D /C mklink /J \"" + s + "\" \"" + t + "\"";
    long mcode = -1;
    Error me;
    int mrc = run_command(cmd_exe, margs, true, nullptr, nullptr, &mcode, &me);
    if (mrc == kOk && mcode == 0) {
        stage("im_move", "清理备份");
        std::vector<const char *> broots{backup_path.c_str()};
        uint64_t removed = 0;
        Error rme;
        bool removed_ok = remove_tree_checked(backup_path, broots, removed, &rme) == kOk;
        if (removed_ok) {
            clear_journal();
        } else {
            std::string line = "搬家已完成，但备份清理未完成；恢复日志保留：" + backup_path;
            std::string lines[1] = {line};
            write_log(lines, nullptr, nullptr);
        }
        if (out_msg) {
            *out_msg = format(
                "数据搬家成功！已将 %llu 个文件（%.2f GB）迁移至 %s，原 C 盘空间已释放%s。",
                (unsigned long long)file_cnt,
                (double)src_size / (1024.0 * 1024.0 * 1024.0),
                std::string(target_dir_str).c_str(),
                removed_ok ? "" : "（备份目录删除失败，请手动清理残留）");
        }
        return kOk;
    }
    /* Junction 失败：把备份改回原名（成功才清日志），再清目标。 */
    Error rre;
    if (rename_directory_sibling_checked(backup_path, source_str, root_ptrs, &rre) != kOk) {
        if (err)
            err_set(err, kErrIo,
                    "创建目录联接失败，自动恢复暂未成功；原数据保留在 %s，恢复日志已保留供下次重试：%s",
                    backup_path.c_str(), rre.message);
        return kErrIo;
    }
    bool target_removed =
        cleanup_target(std::string(target_dir_str), std::move(target_chain));
    clear_journal();
    if (err) {
        if (target_removed)
            err_set(err, kErrIo, "创建 NTFS 目录联接失败，原数据已自动恢复。");
        else
            err_set(err, kErrIo,
                    "创建 NTFS 目录联接失败，原数据已恢复；目标目录有残留，请手动清理。");
    }
    return kErrIo;
}

}  // namespace cdc
