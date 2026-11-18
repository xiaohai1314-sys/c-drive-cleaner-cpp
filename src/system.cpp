/* system.c — 移植自 Rust 版 desktop/src/system.rs + main.rs 的
 * system32 / fixed_drives / get_drive_free_space。
 * 句柄删除/固定全部走 safety.cpp 的句柄 API；Linux 下 Windows 专属
 * 功能返回 CDC_ERR_IO。 */
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
#include "im.h"
#include "logs.h"
#include "cdc_process.h"
#include "safety.h"
#include "timeutil.h"
#include "raii.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static cdc_hooks_t g_hooks = {};

void cdc_system_set_hooks(const cdc_hooks_t *hooks) {
    if (hooks) {
        g_hooks = *hooks;
    } else {
        g_hooks = {};
    }
}

static int cancelled(const char *task) {
    return g_hooks.cancel && g_hooks.cancel(g_hooks.ctx, task);
}

static void progress(const char *task, uint64_t done, uint64_t total) {
    if (g_hooks.progress) g_hooks.progress(g_hooks.ctx, task, done, total);
}

static void stage(const char *task, const char *s) {
    if (g_hooks.stage) g_hooks.stage(g_hooks.ctx, task, s);
}

static void unit(const char *task, const char *u) {
    if (g_hooks.unit) g_hooks.unit(g_hooks.ctx, task, u);
}

/* ---------------- 路径辅助（main.rs） ---------------- */

static char *dup_cstr(std::string_view value) {
    char *out = static_cast<char *>(std::malloc(value.size() + 1));
    if (!out) return nullptr;
    if (!value.empty()) std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

static std::string env_or_string(const char *name, std::string_view fallback) {
    if (std::optional<std::string> value = cdc::getenv_utf8(name);
        value && !value->empty())
        return std::move(*value);
    return std::string(fallback);
}

static char *env_or(const char *name, std::string_view fallback) {
    return dup_cstr(env_or_string(name, fallback));
}

char *cdc_system32_path(const char *name) {
    std::string root = env_or_string("SystemRoot", "C:\\Windows");
    while (!root.empty() && root.back() == '\\') root.pop_back();
    root += "\\System32\\";
    root += name ? name : "";
    return dup_cstr(root);
}

static char *windows_dir(void) {
    return env_or("SystemRoot", "C:\\Windows");
}

static char *programdata_dir(void) {
    return env_or("ProgramData", "C:\\ProgramData");
}

static char *hiber_path(void) {
    std::string drive = env_or_string("SystemDrive", "C:");
    while (!drive.empty() && drive.back() == '\\') drive.pop_back();
    drive += "\\hiberfil.sys";
    return dup_cstr(drive);
}

static char *path_join_win(const char *a, const char *b) {
    std::string out = a ? a : "";
    std::string_view suffix = b ? std::string_view(b) : std::string_view();
    while (!suffix.empty() && (suffix.front() == '\\' || suffix.front() == '/'))
        suffix.remove_prefix(1);
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out.push_back('\\');
    out.append(suffix);
    return dup_cstr(out);
}

int cdc_fixed_drives(char ***out, size_t *out_n, cdc_err_t *err) {
    cdc::err_init(err);
    if (!out || !out_n) {
        cdc::err_set(err, cdc::kErrInvalidArg, "NULL out param");
        return cdc::kErrInvalidArg;
    }
    *out = nullptr;
    *out_n = 0;
    std::vector<std::string> drives;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (uint32_t i = 0; i < 26; i++) {
        if ((mask & (1u << i)) == 0) continue;
        char root[4] = {static_cast<char>('A' + i), ':', '\\', '\0'};
        std::optional<std::wstring> wide = cdc::utf8_to_wide(root);
        if (!wide) continue;
        if (GetDriveTypeW(wide->c_str()) == DRIVE_FIXED) drives.emplace_back(root);
    }
#endif
    if (drives.empty()) drives.emplace_back("C:\\");
    char **result = static_cast<char **>(std::calloc(drives.size(), sizeof(char *)));
    if (!result) {
        cdc::err_set(err, cdc::kErrNoMem, "out of memory");
        return cdc::kErrNoMem;
    }
    size_t made = 0;
    for (; made < drives.size(); made++) {
        result[made] = dup_cstr(drives[made]);
        if (!result[made]) break;
    }
    if (made != drives.size()) {
        for (size_t i = 0; i < made; i++) std::free(result[i]);
        std::free(result);
        cdc::err_set(err, cdc::kErrNoMem, "out of memory");
        return cdc::kErrNoMem;
    }
    *out = result;
    *out_n = drives.size();
    return cdc::kOk;
}

int cdc_drive_free_space(const char *drive, uint64_t *out_free,
                         uint64_t *out_total, cdc_err_t *err) {
    cdc::err_init(err);
    if (!drive || !out_free || !out_total) {
        cdc::err_set(err, cdc::kErrInvalidArg, "NULL param");
        return cdc::kErrInvalidArg;
    }
#ifdef _WIN32
    std::optional<std::wstring> wide = cdc::utf8_to_wide(drive);
    if (!wide) {
        cdc::err_set(err, cdc::kErrUtf8, "invalid UTF-8 drive");
        return cdc::kErrUtf8;
    }
    ULARGE_INTEGER free_avail{}, total{}, total_free{};
    if (!GetDiskFreeSpaceExW(wide->c_str(), &free_avail, &total, &total_free)) {
        cdc::err_set(err, cdc::kErrIo, "获取磁盘空间失败");
        return cdc::kErrIo;
    }
    *out_free = free_avail.QuadPart;
    *out_total = total.QuadPart;
    return cdc::kOk;
#else
    (void)drive;
    cdc::err_set(err, cdc::kErrIo, "仅支持 Windows");
    return cdc::kErrIo;
#endif
}

/* ---------------- 资源管理器联动 ---------------- */

#ifdef _WIN32
static char *explorer_exe(void) {
    char *root = env_or("SystemRoot", "C:\\Windows");
    size_t n = strlen(root);
    while (n > 0 && root[n - 1] == '\\') root[--n] = '\0';
    cdc_buf_t b;
    cdc_buf_init(&b);
    cdc_buf_append_str(&b, root);
    free(root);
    cdc_buf_append_str(&b, "\\explorer.exe");
    return cdc_buf_take_cstr(&b);
}
#endif /* _WIN32 */

#ifdef _WIN32
/* spawn 不等待：CreateProcessW 后立即关闭句柄（与 Rust
 * Command::spawn 一致，子进程独立运行）。 */
static int spawn_no_wait(const char *exe, const char *args, cdc_err_t *err) {
    cdc_buf_t cmd;
    cdc_buf_init(&cmd);
    cdc_buf_append_str(&cmd, "\"");
    cdc_buf_append_str(&cmd, exe);
    cdc_buf_append_str(&cmd, "\"");
    if (args && args[0]) {
        cdc_buf_append_str(&cmd, " ");
        cdc_buf_append_str(&cmd, args);
    }
    char *cmdline = cdc_buf_take_cstr(&cmd);
    if (!cmdline) {
        cdc_buf_free(&cmd);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    wchar_t *wcmd = cdc_utf8_to_wide(cmdline);
    free(cmdline);
    if (!wcmd) {
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 command");
        return CDC_ERR_UTF8;
    }
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(wcmd);
    if (!ok) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "启动失败（Win32 %lu）",
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return CDC_OK;
}
#endif

static int path_exists_utf8(const char *path) {
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return path && stat(path, &st) == 0;
#endif
}

int cdc_reveal_in_explorer(const char *path, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!path || !path_exists_utf8(path)) {
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "文件不存在：%s",
                             path ? path : "");
        return CDC_ERR_NOT_FOUND;
    }
#ifdef _WIN32
    char *exe = explorer_exe();
    /* /select,"C:\..."：'/' 换 '\'（与 Rust .replace('/', "\\") 一致）。 */
    cdc_buf_t arg;
    cdc_buf_init(&arg);
    cdc_buf_append_str(&arg, "/select,\"");
    for (const char *p = path; *p; p++)
        cdc_buf_append(&arg, (*p == '/') ? "\\" : (const void *)p, 1);
    cdc_buf_append_str(&arg, "\"");
    char *args = cdc_buf_take_cstr(&arg);
    int rc = spawn_no_wait(exe, args, err);
    if (rc != CDC_OK && err && err->code == CDC_ERR_IO)
        cdc_err_set(err, CDC_ERR_IO, "启动资源管理器失败：%s",
                    path);
    free(exe);
    free(args);
    return rc;
#else
    if (err) cdc_err_set(err, CDC_ERR_IO, "仅支持 Windows");
    return CDC_ERR_IO;
#endif
}

int cdc_open_path(const char *path, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!path || !path_exists_utf8(path)) {
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "路径不存在：%s",
                             path ? path : "");
        return CDC_ERR_NOT_FOUND;
    }
#ifdef _WIN32
    char *exe = explorer_exe();
    cdc_buf_t arg;
    cdc_buf_init(&arg);
    cdc_buf_append_str(&arg, "\"");
    for (const char *p = path; *p; p++)
        cdc_buf_append(&arg, (*p == '/') ? "\\" : (const void *)p, 1);
    cdc_buf_append_str(&arg, "\"");
    char *args = cdc_buf_take_cstr(&arg);
    int rc = spawn_no_wait(exe, args, err);
    if (rc != CDC_OK && err && err->code == CDC_ERR_IO)
        cdc_err_set(err, CDC_ERR_IO, "打开路径失败：%s", path);
    free(exe);
    free(args);
    return rc;
#else
    if (err) cdc_err_set(err, CDC_ERR_IO, "仅支持 Windows");
    return CDC_ERR_IO;
#endif
}

/* ---------------- 休眠 ---------------- */

void cdc_hiber_info_free(cdc_hiber_info_t *info) {
    if (!info) return;
    free(info->path);
    free(info->description);
    info->path = NULL;
    info->description = NULL;
}

int cdc_query_hiber(cdc_hiber_info_t *out, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "out is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char *path = hiber_path();
    uint64_t size = 0;
    int exists = 0;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (w) {
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(w, GetFileExInfoStandard, &attr)) {
            size = ((uint64_t)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
            exists = 1;
        }
        free(w);
    }
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        size = (uint64_t)st.st_size;
        exists = 1;
    }
#endif
    out->enabled = exists;
    out->size = size;
    out->path = path;
    if (exists) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "休眠文件正在占用 %.2f GB 空间。若无需使用系统休眠（如只用睡眠），可一键关闭释放空间。",
                 (double)size / (1024.0 * 1024.0 * 1024.0));
        out->description = strdup(buf);
    } else {
        out->description = strdup("系统休眠未开启，未占用磁盘空间。");
    }
    if (!out->description) {
        free(out->path);
        out->path = NULL;
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

int cdc_set_hiber(const char *mode, char **out_msg, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (out_msg) *out_msg = NULL;
    const char *args = NULL;
    const char *ok_msg = NULL;
    if (strcmp(mode, "off") == 0) {
        args = "/h off";
        ok_msg = "休眠功能已彻底关闭，hiberfil.sys 已移除并释放全部空间。";
    } else if (strcmp(mode, "reduced") == 0) {
        args = "/h /type reduced";
        ok_msg = "休眠文件已精简为 Reduced 模式（体积缩减约 50%）。";
    } else if (strcmp(mode, "full") == 0 || strcmp(mode, "on") == 0) {
        args = "/h on";
        ok_msg = "休眠功能已开启。";
    } else {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "无效的休眠模式参数");
        return CDC_ERR_INVALID_ARG;
    }
    char *powercfg = cdc_system32_path("powercfg.exe");
    char *so = NULL, *se = NULL;
    long code = -1;
    cdc_err_t e2;
    int rc = cdc_run_command(powercfg, args, 1, &so, &se, &code, &e2);
    free(powercfg);
    if (rc != CDC_OK || code != 0) {
        /* Rust 取 stderr 的 lossy 文本 trim 后拼进错误。 */
        const char *detail = se ? se : "";
        while (*detail == ' ' || *detail == '\t' || *detail == '\n' ||
               *detail == '\r')
            detail++;
        char *tmp = strdup(detail);
        if (tmp) {
            size_t n = strlen(tmp);
            while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t' ||
                             tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
                tmp[--n] = '\0';
            if (err) cdc_err_set(err, CDC_ERR_IO, "设置休眠失败：%s", tmp);
            free(tmp);
        } else if (err) {
            cdc_err_set(err, CDC_ERR_IO, "设置休眠失败");
        }
        free(so);
        free(se);
        return CDC_ERR_IO;
    }
    free(so);
    free(se);
    if (out_msg) *out_msg = strdup(ok_msg);
    return CDC_OK;
}

/* ---------------- 回收站 ---------------- */

int cdc_empty_all_recycle_bins(cdc_err_t *err) {
    if (err) cdc_err_init(err);
#ifdef _WIN32
    HRESULT code = SHEmptyRecycleBinW(NULL, NULL,
                                      SHERB_NOCONFIRMATION |
                                          SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    /* S_OK = 0, S_FALSE = 1。 */
    if (code == 0 || code == 1) return CDC_OK;
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "清空回收站返回错误码：0x%lX",
                    (unsigned long)code);
    return CDC_ERR_IO;
#else
    if (err) cdc_err_set(err, CDC_ERR_IO, "仅支持 Windows");
    return CDC_ERR_IO;
#endif
}

/* ---------------- 系统清理项 ---------------- */

/* clean_dir_contents 的 walk 回调：只收普通文件（跳过重解析点与目录）。 */
static int clean_collect_cb(void *ctx, const char *path, int is_dir,
                            int is_symlink, uint64_t size, uint64_t mtime,
                            uint32_t attrs);
/* verify_copy 的 walk 回调：收普通文件相对路径，跳过重解析点。 */
static int verify_collect_cb(void *ctx, const char *path, int is_dir,
                             int is_symlink, uint64_t size, uint64_t mtime,
                             uint32_t attrs);

/* system 任务的已完成计数（每次 cdc_clean_system_item 入口清零；
 * Rust 的 task::add_done 是按任务累计的，这里用同一语义）。 */
static uint64_t g_system_done = 0;

static void system_add_done(void) {
    g_system_done++;
    if (g_hooks.progress)
        g_hooks.progress(g_hooks.ctx, "system", g_system_done, 0);
}

/* im_move 复制阶段的已复制字节数（每次搬家入口清零）。 */
static uint64_t g_im_move_bytes_done = 0;

/* ASCII 不区分大小写比较（只折叠 A-Z；中文路径不受影响）。 */
static bool ascii_iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return true;
}

typedef struct {
    uint64_t size;
    uint64_t count;
} size_count_t;

static size_count_t dir_size_count(const char *dir) {
    size_count_t result{};
    if (!dir) return result;
    cdc::Error ignored;
    cdc::walk_dir(dir, 1,
                  [&](const cdc::WalkEntry &entry) -> int {
                      if (!entry.is_dir && !entry.is_symlink) {
                          result.size += entry.size;
                          result.count++;
                      }
                      return 0;
                  },
                  &ignored);
    return result;
}

static char *parent_of(const char *path) {
    /* 去尾分隔符后取最后一个分隔符之前；"C:\" 无父目录。 */
    if (!path) return nullptr;
    std::string_view text(path);
    size_t n = text.size();
    while (n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/')) n--;
    if (n == 0) return NULL;
    size_t cut = n;
    while (cut > 0 && path[cut - 1] != '\\' && path[cut - 1] != '/') cut--;
    if (cut == 0) return NULL;
    size_t plen = cut;
    while (plen > 1 && (path[plen - 1] == '\\' || path[plen - 1] == '/'))
        plen--;
    if (plen == 0) return NULL;
    return dup_cstr(text.substr(0, plen));
}

/* 祖先链上任一组件是重解析点，或某一级元数据读失败（不存在除外），
 * 都视为不安全（Rust path_has_reparse_ancestor 的语义）。 */
static int path_has_reparse_ancestor(const char *path) {
    if (!path) return 1;
    std::string cur(path);
    for (;;) {
        bool is_reparse = false;
        cdc::Error e;
        int rc = cdc::reparse_point_status(cur, is_reparse, &e);
        if (rc == cdc::kOk) {
            if (is_reparse) {
                return 1;
            }
        } else if (rc == cdc::kErrNotFound) {
            /* 不存在：继续向上检查祖先。 */
        } else {
            return 1;
        }
        char *par = parent_of(cur.c_str());
        if (!par) return 0;
        cur.assign(par);
        std::free(par);
    }
}

/* 清空目录内容（保留目录本身）：不跟随任何重解析点，逐个文件走
 * cdc_delete_file_checked（untrusted=0——目录本身是已知系统目录，
 * 不在远程 allowlist 内，Rust 版同样传 false），再自底向上删空目录。
 * 先收集文件列表再删除，避免在枚举过程中删除扰乱遍历。 */
static void clean_dir_contents(const char *dir, uint64_t *out_freed,
                               uint64_t *out_count) {
    *out_freed = 0;
    *out_count = 0;
    if (!path_exists_utf8(dir)) return;
    if (cdc_is_reparse_point(dir)) return;
    int is_dir = 0;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(dir);
    if (w) {
        DWORD a = GetFileAttributesW(w);
        is_dir = a != INVALID_FILE_ATTRIBUTES &&
                 (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        free(w);
    }
#else
    struct stat st;
    is_dir = stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
#endif
    if (!is_dir) return;
    const char *roots[1] = { dir };
    cdc_vec_t files; /* char* */
    if (cdc_vec_init(&files, sizeof(char *)) != CDC_OK) return;
    cdc_walk_dir(dir, 1, clean_collect_cb, &files, NULL);
    for (size_t i = 0; i < files.len; i++) {
        if (cancelled("system")) break;
        const char *p = ((char **)files.data)[i];
        /* 删除前先 stat 取大小（Rust 取 entry.metadata() 的 len）。 */
        uint64_t len = 0;
#ifdef _WIN32
        wchar_t *wp = cdc_utf8_to_wide(p);
        if (wp) {
            WIN32_FILE_ATTRIBUTE_DATA attr;
            if (GetFileAttributesExW(wp, GetFileExInfoStandard, &attr))
                len = ((uint64_t)attr.nFileSizeHigh << 32) |
                      attr.nFileSizeLow;
            free(wp);
        }
#else
        struct stat st;
        if (stat(p, &st) == 0) len = (uint64_t)st.st_size;
#endif
        cdc_err_t e;
        if (cdc_delete_file_checked(p, roots, 1, 0, &e) ==
            CDC_DELETE_DELETED) {
            *out_freed += len;
            *out_count += 1;
            system_add_done();
        }
    }
    for (size_t i = 0; i < files.len; i++) free(((char **)files.data)[i]);
    cdc_vec_free(&files);
    cdc_remove_empty_dirs(dir);
}

static int clean_collect_cb(void *ctx, const char *path, int is_dir,
                            int is_symlink, uint64_t size, uint64_t mtime,
                            uint32_t attrs) {
    (void)size;
    (void)mtime;
    (void)attrs;
    if (is_dir || is_symlink) return 0;
    cdc_vec_t *files = (cdc_vec_t *)ctx;
    char *dup = strdup(path);
    if (!dup) return CDC_ERR_NOMEM;
    if (cdc_vec_push(files, &dup) != CDC_OK) {
        free(dup);
        return CDC_ERR_NOMEM;
    }
    return 0;
}

void cdc_system_items_free(cdc_system_item_t *items, size_t n) {
    if (!items) return;
    for (size_t i = 0; i < n; i++) {
        free(items[i].key);
        free(items[i].name);
        free(items[i].describe);
    }
    free(items);
}

static int push_system_item(cdc_vec_t *items, const char *key,
                            const char *name, const char *describe,
                            uint64_t size, uint64_t count, int safe,
                            int is_command) {
    cdc_system_item_t it;
    memset(&it, 0, sizeof(it));
    it.key = strdup(key);
    it.name = strdup(name);
    it.describe = strdup(describe);
    it.size = size;
    it.count = count;
    it.safe_to_clean = safe;
    it.is_command = is_command;
    if (!it.key || !it.name || !it.describe) {
        free(it.key);
        free(it.name);
        free(it.describe);
        return CDC_ERR_NOMEM;
    }
    if (cdc_vec_push(items, &it) != CDC_OK) {
        free(it.key);
        free(it.name);
        free(it.describe);
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

int cdc_scan_system_deep(cdc_system_item_t **out, size_t *out_n,
                         cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_n = 0;
    cdc_vec_t items;
    if (cdc_vec_init(&items, sizeof(cdc_system_item_t)) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    char *win = windows_dir();
    char *upd = path_join_win(win, "SoftwareDistribution\\Download");
    if (path_exists_utf8(upd)) {
        /* 需是目录才计入（Rust 用 is_dir() 判定）。 */
        int is_dir = 0;
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(upd);
        if (w) {
            DWORD a = GetFileAttributesW(w);
            is_dir = a != INVALID_FILE_ATTRIBUTES &&
                     (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
            free(w);
        }
#else
        struct stat st;
        is_dir = stat(upd, &st) == 0 && S_ISDIR(st.st_mode);
#endif
        if (is_dir) {
            size_count_t s = dir_size_count(upd);
            push_system_item(&items, "sys_update_dl", "Windows 更新下载安装包",
                             "系统更新完成后的旧安装包和补丁残留，清理后不影响已安装的更新",
                             s.size, s.count, 1, 0);
        }
    }
    free(upd);
    char *dopt = path_join_win(win, "SoftwareDistribution\\DeliveryOptimization");
    if (path_exists_utf8(dopt)) {
        int is_dir = 0;
#ifdef _WIN32
        wchar_t *w = cdc_utf8_to_wide(dopt);
        if (w) {
            DWORD a = GetFileAttributesW(w);
            is_dir = a != INVALID_FILE_ATTRIBUTES &&
                     (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
            free(w);
        }
#else
        struct stat st;
        is_dir = stat(dopt, &st) == 0 && S_ISDIR(st.st_mode);
#endif
        if (is_dir) {
            size_count_t s = dir_size_count(dopt);
            push_system_item(&items, "sys_delivery_opt",
                             "Windows 传递优化网络缓存",
                             "微软更新与其他电脑局域网共享的下载分片缓存",
                             s.size, s.count, 1, 0);
        }
    }
    free(dopt);
    /* 崩溃转储：%SystemRoot%\Minidump + %ProgramData%\Microsoft\Windows\WER
     * + %LOCALAPPDATA%\CrashDumps。 */
    uint64_t dump_size = 0, dump_count = 0;
    char *md = path_join_win(win, "Minidump");
    free(win);
    char *pd = programdata_dir();
    char *wer = path_join_win(pd, "Microsoft\\Windows\\WER");
    free(pd);
    char *local = cdc_getenv_utf8("LOCALAPPDATA");
    char *cd = local ? path_join_win(local, "CrashDumps") : NULL;
    free(local);
    const char *dump_dirs[3] = { md, wer, cd };
    for (int i = 0; i < 3; i++) {
        if (!dump_dirs[i]) continue;
        if (path_exists_utf8(dump_dirs[i])) {
            size_count_t s = dir_size_count(dump_dirs[i]);
            dump_size += s.size;
            dump_count += s.count;
        }
    }
    free(md);
    free(wer);
    free(cd);
    push_system_item(&items, "sys_crash_dumps", "系统崩溃转储与错误报告",
                     "软件或系统蓝屏崩溃时记录的 .dmp 文件与诊断日志",
                     dump_size, dump_count, 1, 0);
    push_system_item(&items, "sys_dism_winsxs", "WinSxS 系统组件库精简 (DISM)",
                     "执行 DISM 原生命令精简 WinSxS 目录下的历史备份更新，通常可释放 3~15GB（耗时约 1~3 分钟）",
                     0, 0, 1, 1);
    *out = (cdc_system_item_t *)items.data;
    *out_n = items.len;
    items.data = NULL;
    items.len = items.cap = 0;
    cdc_vec_free(&items);
    return CDC_OK;
}

static int service_running(const char *name) {
    char *sc = cdc_system32_path("sc.exe");
    char args[256];
    snprintf(args, sizeof(args), "query %s", name);
    char *so = NULL, *se = NULL;
    long code = -1;
    cdc_err_t e;
    int rc = cdc_run_command(sc, args, 1, &so, &se, &code, &e);
    free(sc);
    int running = 0;
    if (rc == CDC_OK && so && strstr(so, "RUNNING")) running = 1;
    free(so);
    free(se);
    return running;
}

static void start_services(const char **names, size_t n) {
    /* 倒序启动（与 Rust start_services 的 .rev() 一致）。 */
    for (size_t ri = 0; ri < n; ri++) {
        const char *name = names[n - 1 - ri];
        char *net = cdc_system32_path("net.exe");
        char args[256];
        snprintf(args, sizeof(args), "start %s", name);
        cdc_run_command(net, args, 1, NULL, NULL, NULL, NULL);
        free(net);
    }
}

/* 停止仍在运行的服务，返回本次停掉的服务名；任何一个停不下来就把
 * 已停的恢复并报错。 */
static int stop_services(const char **names, size_t n, char ***out_stopped,
                         size_t *out_n, cdc_err_t *err) {
    *out_stopped = NULL;
    *out_n = 0;
    cdc_vec_t stopped;
    if (cdc_vec_init(&stopped, sizeof(char *)) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        const char *name = names[i];
        if (!service_running(name)) continue;
        char *net = cdc_system32_path("net.exe");
        char args[256];
        snprintf(args, sizeof(args), "stop %s /y", name);
        cdc_run_command(net, args, 1, NULL, NULL, NULL, NULL);
        free(net);
        if (service_running(name)) {
            start_services((const char **)stopped.data, stopped.len);
            for (size_t k = 0; k < stopped.len; k++)
                free(((char **)stopped.data)[k]);
            cdc_vec_free(&stopped);
            if (err)
                cdc_err_set(err, CDC_ERR_IO,
                            "%s 服务正在工作且无法暂停，请等待 Windows 更新完成后再清理",
                            name);
            return CDC_ERR_IO;
        }
        char *dup = strdup(name);
        if (dup && cdc_vec_push(&stopped, &dup) != CDC_OK) free(dup);
    }
    *out_stopped = (char **)stopped.data;
    *out_n = stopped.len;
    stopped.data = NULL;
    stopped.len = stopped.cap = 0;
    cdc_vec_free(&stopped);
    return CDC_OK;
}

static char *mb_string(double mb) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", mb);
    return strdup(buf);
}

int cdc_clean_system_item(const char *key, uint64_t *out_freed,
                          uint64_t *out_count, char **out_msg,
                          cdc_err_t *err) {
    if (err) cdc_err_init(err);
    g_system_done = 0;
    if (out_freed) *out_freed = 0;
    if (out_count) *out_count = 0;
    if (out_msg) *out_msg = NULL;
    if (!key) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "key is NULL");
        return CDC_ERR_INVALID_ARG;
    }
    if (strcmp(key, "sys_update_dl") == 0) {
        stage("system", "停止更新服务");
        const char *svc[2] = { "wuauserv", "bits" };
        char **stopped = NULL;
        size_t nstopped = 0;
        cdc_err_t e;
        if (stop_services(svc, 2, &stopped, &nstopped, &e) != CDC_OK) {
            if (err) *err = e;
            return CDC_ERR_IO;
        }
        stage("system", "清理更新安装包");
        char *win = windows_dir();
        char *dir = path_join_win(win, "SoftwareDistribution\\Download");
        free(win);
        uint64_t freed = 0, count = 0;
        clean_dir_contents(dir, &freed, &count);
        free(dir);
        stage("system", "恢复更新服务");
        start_services((const char **)stopped, nstopped);
        cdc_strvec_free(stopped, nstopped);
        if (out_freed) *out_freed = freed;
        if (out_count) *out_count = count;
        if (out_msg) {
            char *mb = mb_string((double)freed / 1048576.0);
            cdc_buf_t b;
            cdc_buf_init(&b);
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "已清理更新安装包 %llu 个，释放 %s MB",
                     (unsigned long long)count, mb ? mb : "0.00");
            free(mb);
            cdc_buf_append_str(&b, tmp);
            *out_msg = cdc_buf_take_cstr(&b);
        }
        return CDC_OK;
    }
    if (strcmp(key, "sys_delivery_opt") == 0) {
        stage("system", "调用系统清理传递优化缓存");
        char *win = windows_dir();
        char *dir = path_join_win(win, "SoftwareDistribution\\DeliveryOptimization");
        free(win);
        size_count_t before = dir_size_count(dir);
        char *ps = cdc_system32_path("WindowsPowerShell\\v1.0\\powershell.exe");
        const char *args =
            "-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
            "\"Delete-DeliveryOptimizationCache -Force -ErrorAction Stop\"";
        char *so = NULL, *se = NULL;
        long code = -1;
        cdc_err_t e2;
        int rc = cdc_run_command(ps, args, 1, &so, &se, &code, &e2);
        free(ps);
        if (rc != CDC_OK || code != 0) {
            const char *detail = se ? se : "";
            while (*detail == ' ' || *detail == '\t' || *detail == '\n' ||
                   *detail == '\r')
                detail++;
            char *tmp = strdup(detail);
            if (tmp) {
                size_t n = strlen(tmp);
                while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t' ||
                                 tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
                    tmp[--n] = '\0';
                if (err) cdc_err_set(err, CDC_ERR_IO, "传递优化缓存清理失败：%s", tmp);
                free(tmp);
            } else if (err) {
                cdc_err_set(err, CDC_ERR_IO, "传递优化缓存清理失败");
            }
            free(so);
            free(se);
            free(dir);
            return CDC_ERR_IO;
        }
        free(so);
        free(se);
        size_count_t after = dir_size_count(dir);
        free(dir);
        uint64_t freed = before.size >= after.size ? before.size - after.size : 0;
        uint64_t count = before.count >= after.count ? before.count - after.count : 0;
        if (out_freed) *out_freed = freed;
        if (out_count) *out_count = count;
        if (out_msg) {
            char *mb = mb_string((double)freed / 1048576.0);
            cdc_buf_t b;
            cdc_buf_init(&b);
            char tmp[96];
            snprintf(tmp, sizeof(tmp), "已清理传递优化分片 %llu 个，释放 %s MB",
                     (unsigned long long)count, mb ? mb : "0.00");
            free(mb);
            cdc_buf_append_str(&b, tmp);
            *out_msg = cdc_buf_take_cstr(&b);
        }
        return CDC_OK;
    }
    if (strcmp(key, "sys_crash_dumps") == 0) {
        stage("system", "清理崩溃转储与错误报告");
        char *win = windows_dir();
        char *md = path_join_win(win, "Minidump");
        free(win);
        char *pd = programdata_dir();
        char *wer = path_join_win(pd, "Microsoft\\Windows\\WER");
        free(pd);
        char *local = cdc_getenv_utf8("LOCALAPPDATA");
        char *cd = local ? path_join_win(local, "CrashDumps") : NULL;
        free(local);
        const char *dirs[3] = { md, wer, cd };
        uint64_t freed = 0, count = 0;
        for (int i = 0; i < 3; i++) {
            if (!dirs[i]) continue;
            uint64_t f = 0, c = 0;
            clean_dir_contents(dirs[i], &f, &c);
            freed += f;
            count += c;
        }
        free(md);
        free(wer);
        free(cd);
        if (out_freed) *out_freed = freed;
        if (out_count) *out_count = count;
        if (out_msg) {
            char *mb = mb_string((double)freed / 1048576.0);
            cdc_buf_t b;
            cdc_buf_init(&b);
            char tmp[96];
            snprintf(tmp, sizeof(tmp), "已清理转储文件 %llu 个，释放 %s MB",
                     (unsigned long long)count, mb ? mb : "0.00");
            free(mb);
            cdc_buf_append_str(&b, tmp);
            *out_msg = cdc_buf_take_cstr(&b);
        }
        return CDC_OK;
    }
    if (strcmp(key, "sys_dism_winsxs") == 0) {
        stage("system", "DISM 组件库精简（约 1~3 分钟）");
        char *dism = cdc_system32_path("Dism.exe");
        char *so = NULL, *se = NULL;
        long code = -1;
        cdc_err_t e2;
        int rc = cdc_run_command(dism, "/Online /Cleanup-Image /StartComponentCleanup",
                                 1, &so, &se, &code, &e2);
        free(dism);
        if (rc == CDC_OK && code == 0) {
            free(so);
            free(se);
            if (out_msg)
                *out_msg = strdup("DISM WinSxS 组件库精简优化已成功完成！");
            return CDC_OK;
        }
        const char *detail = se ? se : "";
        while (*detail == ' ' || *detail == '\t' || *detail == '\n' ||
               *detail == '\r')
            detail++;
        char *tmp = strdup(detail);
        if (tmp) {
            size_t n = strlen(tmp);
            while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t' ||
                             tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
                tmp[--n] = '\0';
            if (err) cdc_err_set(err, CDC_ERR_IO, "DISM 组件精简失败：%s", tmp);
            free(tmp);
        } else if (err) {
            cdc_err_set(err, CDC_ERR_IO, "DISM 组件精简失败");
        }
        free(so);
        free(se);
        return CDC_ERR_IO;
    }
    if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "未知系统清理项");
    return CDC_ERR_INVALID_ARG;
}

/* ---------------- IM 迁移 ---------------- */

void cdc_move_analysis_free(cdc_move_analysis_t *a) {
    if (!a) return;
    free(a->source);
    for (size_t i = 0; i < a->ntargets; i++) free(a->targets[i].letter);
    free(a->targets);
    a->source = NULL;
    a->targets = NULL;
    a->ntargets = 0;
}

int cdc_analyze_im_migration(const char *kind, cdc_move_analysis_t *out,
                             cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out || !kind) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL param");
        return CDC_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char **roots = NULL;
    size_t nroots = 0;
    if (cdc_im_roots(kind, &roots, &nroots, NULL) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "无法探测 %s 数据目录",
                             kind);
        return CDC_ERR_IO;
    }
    char *best = NULL;
    uint64_t max_size = 0, max_count = 0;
    for (size_t i = 0; i < nroots; i++) {
        char *norm = cdc_normalize(roots[i]);
        if (!norm) continue;
        if (cdc_im_is_program_root(norm)) {
            free(norm);
            continue;
        }
        /* 以 C: 开头（Rust 用 to_uppercase().starts_with("C:")）。 */
        int is_c = (norm[0] == 'C' || norm[0] == 'c') && norm[1] == ':';
        if (is_c) {
            size_count_t s = dir_size_count(norm);
            if (s.size > max_size) {
                max_size = s.size;
                max_count = s.count;
                free(best);
                best = norm;
                norm = NULL;
            }
        }
        free(norm);
    }
    cdc_strvec_free(roots, nroots);
    out->source = best ? best : strdup("");
    out->exists = best != NULL;
    out->size = max_size;
    out->file_count = max_count;
    out->is_junction = best ? cdc_is_reparse_point(best) : 0;
    char **drives = NULL;
    size_t ndrives = 0;
    if (cdc_fixed_drives(&drives, &ndrives, NULL) == CDC_OK) {
        cdc_vec_t targets;
        cdc_vec_init(&targets, sizeof(cdc_drive_target_t));
        for (size_t i = 0; i < ndrives; i++) {
            int is_c = (drives[i][0] == 'C' || drives[i][0] == 'c') &&
                       drives[i][1] == ':';
            if (is_c) continue;
            uint64_t free_b = 0, total_b = 0;
            if (cdc_drive_free_space(drives[i], &free_b, &total_b, NULL) ==
                CDC_OK) {
                cdc_drive_target_t t;
                t.letter = strdup(drives[i]);
                t.free_bytes = free_b;
                t.total_bytes = total_b;
                if (t.letter &&
                    cdc_vec_push(&targets, &t) != CDC_OK)
                    free(t.letter);
            }
        }
        out->targets = (cdc_drive_target_t *)targets.data;
        out->ntargets = targets.len;
        targets.data = NULL;
        targets.len = targets.cap = 0;
        cdc_vec_free(&targets);
    }
    cdc_strvec_free(drives, ndrives);
    if (!out->source) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

/* ---------------- 搬家恢复日志 ---------------- */

static char *journal_path(void) {
    char *cache = cdc_cache_dir();
    if (!cache) return NULL;
    char *p = path_join_win(cache, "im_move_pending.json");
    free(cache);
    return p;
}

typedef struct {
    char *source;
    char *backup;
    char *kind; /* 可能为空串（旧版日志无 kind 字段） */
} move_journal_t;

static void journal_free(move_journal_t *j) {
    if (!j) return;
    free(j->source);
    free(j->backup);
    free(j->kind);
}

static int write_journal(const char *source, const char *backup,
                         const char *kind, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    char *cache = cdc_cache_dir();
    if (!cache) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "无法创建缓存目录");
        return CDC_ERR_IO;
    }
    const char *roots[1] = { cache };
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "source", source);
    cJSON_AddStringToObject(obj, "backup", backup);
    cJSON_AddStringToObject(obj, "kind", kind);
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!text) {
        free(cache);
        if (err) cdc_err_set(err, CDC_ERR_IO, "序列化搬家日志失败");
        return CDC_ERR_IO;
    }
    char *path = journal_path();
    cdc_handle_chain_t *chain = NULL;
    cdc_handle_t file = NULL;
    cdc_err_t e;
    int rc = cdc_create_new_file_checked(path, roots, 1, &chain, &file, &e);
    if (rc != CDC_OK) {
        /* create_new 失败：Rust 映射 AlreadyExists（文件已存在时
         * create_new 返回 AlreadyExists）。 */
        /* text 是 cJSON_PrintUnformatted 的返回值，用 cJSON_free 释放
         *（cJSON 自身的 free 钩子，不依赖本文件是否走 mimalloc 全局替换）。 */
        cJSON_free(text);
        free(path);
        free(cache);
        if (err) *err = e;
        return rc;
    }
    int wrc = cdc_handle_write_all(file, (const uint8_t *)text,
                                   strlen(text), &e);
    if (wrc == CDC_OK) wrc = cdc_handle_sync(file, &e);
#ifdef _WIN32
    if (file) CloseHandle((HANDLE)file);
#endif
    cdc_handle_chain_free(chain);
    free(chain);
    cJSON_free(text);
    if (wrc != CDC_OK) {
        cdc_delete_file_checked(path, roots, 1, 0, NULL);
        free(path);
        free(cache);
        if (err) *err = e;
        return wrc;
    }
    free(path);
    free(cache);
    return CDC_OK;
}

static void clear_journal(void) {
    char *path = journal_path();
    char *cache = cdc_cache_dir();
    if (!path || !cache) {
        free(path);
        free(cache);
        return;
    }
    const char *roots[1] = { cache };
    cdc_delete_file_checked(path, roots, 1, 0, NULL);
    free(path);
    free(cache);
}

static int read_journal_text(char **out_text, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    char *path = journal_path();
    char *cache = cdc_cache_dir();
    if (!path || !cache) {
        free(path);
        free(cache);
        if (err) cdc_err_set(err, CDC_ERR_IO, "无法定位搬家日志");
        return CDC_ERR_IO;
    }
    const char *roots[1] = { cache };
    cdc_handle_chain_t *chain = NULL;
    cdc_handle_t file = NULL;
    cdc_err_t e;
    int rc = cdc_open_read_file_checked(path, roots, 1, &chain, &file, &e);
    free(path);
    free(cache);
    if (rc != CDC_OK) {
        if (err) *err = e;
        return rc;
    }
    cdc_buf_t buf;
    cdc_buf_init(&buf);
    uint8_t chunk[8192];
    for (;;) {
        size_t got = 0;
        if (cdc_handle_read(file, chunk, sizeof(chunk), &got, &e) != CDC_OK ||
            got == 0)
            break;
        cdc_buf_append(&buf, chunk, got);
    }
#ifdef _WIN32
    if (file) CloseHandle((HANDLE)file);
#endif
    cdc_handle_chain_free(chain);
    free(chain);
    *out_text = cdc_buf_take_cstr(&buf);
    if (!*out_text) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

static int parse_journal(const char *text, move_journal_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *obj = cJSON_Parse(text);
    if (!obj || !cJSON_IsObject(obj)) {
        if (obj) cJSON_Delete(obj);
        return CDC_ERR_INVALID_ARG;
    }
    cJSON *s = cJSON_GetObjectItemCaseSensitive(obj, "source");
    cJSON *b = cJSON_GetObjectItemCaseSensitive(obj, "backup");
    cJSON *k = cJSON_GetObjectItemCaseSensitive(obj, "kind");
    if (!cJSON_IsString(s) || !cJSON_IsString(b)) {
        cJSON_Delete(obj);
        return CDC_ERR_INVALID_ARG;
    }
    out->source = strdup(s->valuestring ? s->valuestring : "");
    out->backup = strdup(b->valuestring ? b->valuestring : "");
    out->kind = strdup(cJSON_IsString(k) && k->valuestring ? k->valuestring : "");
    cJSON_Delete(obj);
    if (!out->source || !out->backup || !out->kind) {
        journal_free(out);
        return CDC_ERR_NOMEM;
    }
    return CDC_OK;
}

int cdc_safe_for_quoted_cmd_path(const char *path) {
    if (!path || !path[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p == '"' || *p == '%' || *p < 0x20 || *p == 0x7F) return 0;
    }
    return 1;
}

int cdc_valid_recovery_paths(const char *source, const char *backup,
                             const char **allowed_roots, size_t nroots) {
    if (!source || !backup) return 0;
    if (cdc_path_syntax_error(source) || cdc_path_syntax_error(backup) ||
        cdc_is_protected_path(source) || cdc_is_never_clean(source) ||
        cdc_is_drive_root(source) || cdc_is_critical_file(source) ||
        cdc_is_password_store(source))
        return 0;
    /* 必须都是绝对路径：盘符 X:\ 或 UNC \\ 开头。 */
    int abs_s = (strlen(source) >= 3 && source[1] == ':' &&
                 (source[2] == '\\' || source[2] == '/')) ||
                (source[0] == '\\' && source[1] == '\\');
    int abs_b = (strlen(backup) >= 3 && backup[1] == ':' &&
                 (backup[2] == '\\' || backup[2] == '/')) ||
                (backup[0] == '\\' && backup[1] == '\\');
    if (!abs_s || !abs_b) return 0;
    char *source_norm = cdc_normalize(source);
    if (!source_norm) return 0;
    int allowed = 0;
    for (size_t i = 0; i < nroots; i++) {
        char *rn = cdc_normalize(allowed_roots[i]);
        if (rn && ascii_iequal(rn, source_norm)) allowed = 1;
        free(rn);
        if (allowed) break;
    }
    free(source_norm);
    if (!allowed) return 0;
    char *sp = parent_of(source);
    char *bp = parent_of(backup);
    char *sn = cdc_basename(source);
    char *bn = cdc_basename(backup);
    if (!sp || !bp || !sn || !bn) {
        free(sp);
        free(bp);
        free(sn);
        free(bn);
        return 0;
    }
    char *spn = cdc_normalize(sp);
    char *bpn = cdc_normalize(bp);
    int parents_equal = spn && bpn && ascii_iequal(spn, bpn);
    free(spn);
    free(bpn);
    free(sp);
    free(bp);
    if (!parents_equal) {
        free(sn);
        free(bn);
        return 0;
    }
    /* backup 名 = source 名小写 + "_backup_" + 15 字符戳
     *（YYYYMMDD_HHMMSS：8 位数字 + '_' + 6 位数字）。 */
    size_t sl = strlen(sn), bl = strlen(bn);
    int ok = 0;
    if (bl > sl + 8) {
        /* 前缀比对（ASCII 小写化）。 */
        int prefix_ok = 1;
        for (size_t i = 0; i < sl; i++) {
            char a = sn[i], b = bn[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) {
                prefix_ok = 0;
                break;
            }
        }
        const char *mid = "_backup_";
        if (prefix_ok && strncmp(bn + sl, mid, 8) == 0) {
            const char *stamp = bn + sl + 8;
            if (strlen(stamp) == 15 && stamp[8] == '_') {
                ok = 1;
                for (int i = 0; i < 15; i++) {
                    if (i == 8) continue;
                    if (stamp[i] < '0' || stamp[i] > '9') {
                        ok = 0;
                        break;
                    }
                }
            }
        }
    }
    free(sn);
    free(bn);
    return ok;
}

static int path_has_reparse_parent(const char *path) {
    char *par = parent_of(path);
    if (!par) return 1; /* 无父目录视为不安全（Rust 的 unwrap_or(true)） */
    int r = path_has_reparse_ancestor(par);
    free(par);
    return r;
}

void cdc_recover_pending_move(void) {
    char *text = NULL;
    cdc_err_t e;
    int rc = read_journal_text(&text, &e);
    if (rc != CDC_OK) return; /* 不存在或读失败：静默返回 */
    move_journal_t j;
    if (parse_journal(text, &j) != CDC_OK) {
        /* 格式错误的日志保留，供诊断；不删除。 */
        free(text);
        return;
    }
    free(text);
    char **allowed = NULL;
    size_t nallowed = 0;
    if (strcmp(j.kind, "wechat") == 0) {
        cdc_im_migration_roots("wechat", &allowed, &nallowed, NULL);
    } else if (strcmp(j.kind, "qq") == 0) {
        cdc_im_migration_roots("qq", &allowed, &nallowed, NULL);
    } else if (j.kind[0] == '\0') {
        char **w = NULL, **q = NULL;
        size_t nw = 0, nq = 0;
        cdc_im_migration_roots("wechat", &w, &nw, NULL);
        cdc_im_migration_roots("qq", &q, &nq, NULL);
        cdc_vec_t all;
        cdc_vec_init(&all, sizeof(char *));
        for (size_t i = 0; i < nw; i++) {
            if (cdc_vec_push(&all, &w[i]) != CDC_OK) free(w[i]);
        }
        free(w);
        for (size_t i = 0; i < nq; i++) {
            if (cdc_vec_push(&all, &q) != CDC_OK) free(q[i]);
        }
        free(q);
        allowed = (char **)all.data;
        nallowed = all.len;
        all.data = NULL;
        all.len = all.cap = 0;
        cdc_vec_free(&all);
    } else {
        journal_free(&j);
        return;
    }
    if (!cdc_valid_recovery_paths(j.source, j.backup,
                                  (const char **)allowed, nallowed) ||
        path_has_reparse_parent(j.source) ||
        path_has_reparse_parent(j.backup)) {
        cdc_strvec_free(allowed, nallowed);
        journal_free(&j);
        return;
    }
    /* 源路径已存在：绝不覆盖；备份不存在时清日志，否则保留日志并记一笔。 */
    int source_exists = path_exists_utf8(j.source);
    if (source_exists) {
        if (!path_exists_utf8(j.backup)) {
            clear_journal();
        } else {
            cdc_buf_t b;
            cdc_buf_init(&b);
            cdc_buf_append_str(&b, "搬家恢复日志保留：源路径已存在，但备份目录仍在或状态不确定：");
            cdc_buf_append_str(&b, j.backup);
            char *line = cdc_buf_take_cstr(&b);
            const char *lines[1] = { line ? line : "" };
            cdc_write_log(lines, 1, NULL, NULL);
            free(line);
        }
        cdc_strvec_free(allowed, nallowed);
        journal_free(&j);
        return;
    }
    /* 备份必须是普通目录（非重解析点）。 */
    int backup_ok = 0;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(j.backup);
    if (w) {
        DWORD a = GetFileAttributesW(w);
        backup_ok = a != INVALID_FILE_ATTRIBUTES &&
                    (a & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                    (a & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        free(w);
    }
#else
    struct stat st;
    backup_ok = stat(j.backup, &st) == 0 && S_ISDIR(st.st_mode);
#endif
    if (!backup_ok) {
        cdc_strvec_free(allowed, nallowed);
        journal_free(&j);
        return;
    }
    cdc_err_t e2;
    int rrc = cdc_rename_directory_sibling_checked(j.backup, j.source,
                                                   (const char **)allowed,
                                                   nallowed, &e2);
    if (rrc == CDC_OK) {
        cdc_buf_t b;
        cdc_buf_init(&b);
        cdc_buf_append_str(&b, "微信/QQ 搬家中断恢复：");
        cdc_buf_append_str(&b, j.backup);
        cdc_buf_append_str(&b, " 已还原为 ");
        cdc_buf_append_str(&b, j.source);
        char *line = cdc_buf_take_cstr(&b);
        const char *lines[1] = { line ? line : "" };
        cdc_write_log(lines, 1, NULL, NULL);
        free(line);
        clear_journal();
    }
    cdc_strvec_free(allowed, nallowed);
    journal_free(&j);
}

/* ---------------- 搬家执行 ---------------- */

/* 列出目录的直接子项（名 + 是否目录，不跟随链接）。 */
typedef struct {
    char *name;
    int is_dir;
} dir_entry_t;

static int list_entries(const char *dir, dir_entry_t **out, size_t *out_n) {
    *out = NULL;
    *out_n = 0;
    cdc_vec_t entries;
    if (cdc_vec_init(&entries, sizeof(dir_entry_t)) != CDC_OK)
        return CDC_ERR_NOMEM;
#ifdef _WIN32
    char *pat = path_join_win(dir, "*");
    wchar_t *wpat = cdc_utf8_to_wide(pat);
    free(pat);
    if (!wpat) {
        cdc_vec_free(&entries);
        return CDC_ERR_UTF8;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) {
        cdc_vec_free(&entries);
        return CDC_OK;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;
        char *u = cdc_wide_to_utf8(fd.cFileName);
        if (!u) continue;
        dir_entry_t e;
        e.name = u;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (cdc_vec_push(&entries, &e) != CDC_OK) free(u);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) {
        cdc_vec_free(&entries);
        return CDC_OK;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char *full = path_join_win(dir, e->d_name);
        struct stat st;
        int is_dir = full && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        free(full);
        dir_entry_t de;
        de.name = strdup(e->d_name);
        de.is_dir = is_dir;
        if (!de.name) continue;
        if (cdc_vec_push(&entries, &de) != CDC_OK) free(de.name);
    }
    closedir(d);
#endif
    *out = (dir_entry_t *)entries.data;
    *out_n = entries.len;
    entries.data = NULL;
    entries.len = entries.cap = 0;
    cdc_vec_free(&entries);
    return CDC_OK;
}

static void entries_free(dir_entry_t *entries, size_t n) {
    for (size_t i = 0; i < n; i++) free(entries[i].name);
    free(entries);
}

static void close_handle(cdc_handle_t h) {
#ifdef _WIN32
    if (h) CloseHandle((HANDLE)h);
#else
    (void)h;
#endif
}

/* 递归复制目录内容（句柄相对打开；重解析点跳过；取消时返回
 * CDC_ERR_IO 且 err 文案为“已取消”——调用方按失败处理）。 */
static int copy_dir_contents(const char *src, const char *dst,
                             const char **source_root,
                             const char **target_root, cdc_err_t *err) {
    cdc_handle_chain_t *chain = NULL;
    cdc_err_t e;
    if (cdc_pin_directory_checked(src, source_root, 1, &chain, &e) != CDC_OK) {
        if (err) *err = e;
        return CDC_ERR_IO;
    }
    dir_entry_t *entries = NULL;
    size_t nentries = 0;
    list_entries(src, &entries, &nentries);
    int rc = CDC_OK;
    for (size_t i = 0; i < nentries && rc == CDC_OK; i++) {
        if (cancelled("im_move")) {
            if (err) cdc_err_set(err, CDC_ERR_IO, "已取消");
            rc = CDC_ERR_IO;
            break;
        }
        char *from = path_join_win(src, entries[i].name);
        char *to = path_join_win(dst, entries[i].name);
        int is_reparse = 0;
        cdc_err_t e2;
        if (cdc_reparse_point_status(from, &is_reparse, &e2) != CDC_OK) {
            if (err) *err = e2;
            rc = CDC_ERR_IO;
            free(from);
            free(to);
            break;
        }
        if (is_reparse) {
            free(from);
            free(to);
            continue;
        }
        if (entries[i].is_dir) {
            cdc_handle_chain_t *dc = NULL;
            cdc_handle_t dirh = NULL;
            if (cdc_create_directory_checked(to, target_root, 1, &dc, &dirh,
                                             &e2) != CDC_OK) {
                if (err) *err = e2;
                rc = CDC_ERR_IO;
                free(from);
                free(to);
                break;
            }
            close_handle(dirh);
            cdc_handle_chain_free(dc);
            free(dc);
            rc = copy_dir_contents(from, to, source_root, target_root, err);
            free(from);
            free(to);
            continue;
        }
        /* 普通文件：句柄相对打开源、创建目标，流式复制后 sync。 */
        cdc_handle_chain_t *sc = NULL, *tc = NULL;
        cdc_handle_t sf = NULL, tf = NULL;
        if (cdc_open_read_file_checked(from, source_root, 1, &sc, &sf, &e2) !=
            CDC_OK) {
            if (err) *err = e2;
            rc = CDC_ERR_IO;
            free(from);
            free(to);
            break;
        }
        if (cdc_create_new_file_checked(to, target_root, 1, &tc, &tf, &e2) !=
            CDC_OK) {
            close_handle(sf);
            cdc_handle_chain_free(sc);
            free(sc);
            if (err) *err = e2;
            rc = CDC_ERR_IO;
            free(from);
            free(to);
            break;
        }
        uint8_t buf[65536];
        uint64_t copied = 0;
        int io_err = 0;
        for (;;) {
            size_t got = 0;
            if (cdc_handle_read(sf, buf, sizeof(buf), &got, &e2) != CDC_OK) {
                if (err) *err = e2;
                io_err = 1;
                break;
            }
            if (got == 0) break;
            if (cdc_handle_write_all(tf, buf, got, &e2) != CDC_OK) {
                if (err) *err = e2;
                io_err = 1;
                break;
            }
            copied += got;
        }
        if (!io_err && cdc_handle_sync(tf, &e2) != CDC_OK) {
            if (err) *err = e2;
            io_err = 1;
        }
        close_handle(sf);
        close_handle(tf);
        cdc_handle_chain_free(sc);
        free(sc);
        cdc_handle_chain_free(tc);
        free(tc);
        if (io_err) {
            rc = CDC_ERR_IO;
            free(from);
            free(to);
            break;
        }
        /* Rust 按字节累计进度（set_unit "字节"）。 */
        g_im_move_bytes_done += copied;
        if (g_hooks.progress)
            g_hooks.progress(g_hooks.ctx, "im_move", g_im_move_bytes_done, 0);
        free(from);
        free(to);
    }
    entries_free(entries, nentries);
    cdc_handle_chain_free(chain);
    free(chain);
    return rc;
}

static int cleanup_target(const char *target, cdc_handle_chain_t *chain) {
    const char *roots[1] = { target };
    uint64_t removed = 0;
    cdc_err_t e;
    int first = cdc_remove_tree_checked(target, roots, 1, &removed, &e) == CDC_OK;
    cdc_handle_chain_free(chain);
    free(chain);
    if (first) return 1;
    return cdc_remove_tree_checked(target, roots, 1, &removed, &e) == CDC_OK;
}

static int copy_dir_recursive(const char *src, const char *dst,
                              cdc_handle_chain_t **out_source,
                              cdc_handle_chain_t **out_target,
                              cdc_err_t *err) {
    const char *source_root[1] = { src };
    const char *target_root[1] = { dst };
    cdc_handle_chain_t *source_chain = NULL;
    cdc_err_t e;
    if (cdc_pin_directory_checked(src, source_root, 1, &source_chain, &e) !=
        CDC_OK) {
        if (err) *err = e;
        return CDC_ERR_IO;
    }
    cdc_handle_chain_t *target_chain = NULL;
    if (cdc_create_directory_tree_checked(dst, &target_chain, &e) != CDC_OK) {
        cdc_handle_chain_free(source_chain);
        free(source_chain);
        if (err) *err = e;
        return CDC_ERR_IO;
    }
    if (copy_dir_contents(src, dst, source_root, target_root, err) != CDC_OK) {
        uint64_t removed = 0;
        cdc_remove_tree_checked(dst, target_root, 1, &removed, NULL);
        cdc_handle_chain_free(source_chain);
        free(source_chain);
        cleanup_target(dst, target_chain);
        return CDC_ERR_IO;
    }
    *out_source = source_chain;
    *out_target = target_chain;
    return CDC_OK;
}

/* 逐文件校验：大小 + 64KB 分块内容比对（句柄相对打开）。 */
static int verify_copy(const char *src, const char *dst, cdc_err_t *err) {
    const char *source_root[1] = { src };
    const char *target_root[1] = { dst };
    cdc_handle_chain_t *sc = NULL, *tc = NULL;
    cdc_err_t e;
    if (cdc_pin_directory_checked(src, source_root, 1, &sc, &e) != CDC_OK) {
        if (err) *err = e;
        return CDC_ERR_IO;
    }
    if (cdc_pin_directory_checked(dst, target_root, 1, &tc, &e) != CDC_OK) {
        cdc_handle_chain_free(sc);
        free(sc);
        if (err) *err = e;
        return CDC_ERR_IO;
    }
    cdc_vec_t files; /* char*（相对路径） */
    if (cdc_vec_init(&files, sizeof(char *)) != CDC_OK) {
        cdc_handle_chain_free(sc);
        free(sc);
        cdc_handle_chain_free(tc);
        free(tc);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    /* 收集源目录下的普通文件（相对路径）。 */
    typedef struct {
        cdc_vec_t *files;
        const char *src;
        size_t src_len;
    } verify_ctx_t;
    verify_ctx_t vctx;
    vctx.files = &files;
    vctx.src = src;
    vctx.src_len = strlen(src);
    cdc_walk_dir(src, 1, verify_collect_cb, &vctx, NULL);
    int rc = CDC_OK;
    uint64_t done = 0;
    for (size_t i = 0; i < files.len && rc == CDC_OK; i++) {
        if (cancelled("im_move")) {
            if (err) cdc_err_set(err, CDC_ERR_IO, "已取消");
            rc = CDC_ERR_IO;
            break;
        }
        const char *rel = ((char **)files.data)[i];
        char *sp = path_join_win(src, rel);
        char *dp = path_join_win(dst, rel);
        cdc_handle_chain_t *sfc = NULL, *tfc = NULL;
        cdc_handle_t sf = NULL, tf = NULL;
        if (cdc_open_read_file_checked(sp, source_root, 1, &sfc, &sf, &e) !=
            CDC_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_IO, "%s 无法安全读取：%s", rel,
                            e.message);
            rc = CDC_ERR_IO;
        } else if (cdc_open_read_file_checked(dp, target_root, 1, &tfc, &tf,
                                              &e) != CDC_OK) {
            if (err)
                cdc_err_set(err, CDC_ERR_IO, "%s 未完整拷贝：%s", rel,
                            e.message);
            rc = CDC_ERR_IO;
        } else {
            uint64_t ss = 0, ts = 0;
            if (cdc_handle_file_size(sf, &ss, &e) != CDC_OK ||
                cdc_handle_file_size(tf, &ts, &e) != CDC_OK || ss != ts) {
                if (err) cdc_err_set(err, CDC_ERR_IO, "%s 大小不一致", rel);
                rc = CDC_ERR_IO;
            } else {
                uint8_t left[65536], right[65536];
                for (;;) {
                    size_t lg = 0, rg = 0;
                    int lr = cdc_handle_read(sf, left, sizeof(left), &lg, &e);
                    int rr = cdc_handle_read(tf, right, sizeof(right), &rg, &e);
                    if (lr != CDC_OK || rr != CDC_OK) {
                        if (err)
                            cdc_err_set(err, CDC_ERR_IO, "%s 读取失败", rel);
                        rc = CDC_ERR_IO;
                        break;
                    }
                    if (lg != rg || (lg > 0 && memcmp(left, right, lg) != 0)) {
                        if (err)
                            cdc_err_set(err, CDC_ERR_IO, "%s 内容不一致", rel);
                        rc = CDC_ERR_IO;
                        break;
                    }
                    if (lg == 0) break;
                }
            }
        }
        close_handle(sf);
        close_handle(tf);
        cdc_handle_chain_free(sfc);
        free(sfc);
        cdc_handle_chain_free(tfc);
        free(tfc);
        free(sp);
        free(dp);
        if (rc == CDC_OK) {
            done++;
            progress("im_move", done, 0);
        }
    }
    for (size_t i = 0; i < files.len; i++) free(((char **)files.data)[i]);
    cdc_vec_free(&files);
    cdc_handle_chain_free(sc);
    free(sc);
    cdc_handle_chain_free(tc);
    free(tc);
    return rc;
}

/* verify_copy 的 walk 回调：收普通文件相对路径，跳过重解析点。
 * 上下文与 verify_copy 内的 verify_ctx_t 布局一致（files/src/src_len）。 */
static int verify_collect_cb(void *ctx, const char *path, int is_dir,
                             int is_symlink, uint64_t size, uint64_t mtime,
                             uint32_t attrs) {
    (void)size;
    (void)mtime;
    (void)attrs;
    if (is_dir || is_symlink) return 0;
    /* 相对路径 = path 去掉 src 前缀 + 分隔符。 */
    typedef struct {
        cdc_vec_t *files;
        const char *src;
        size_t src_len;
    } verify_ctx_t;
    verify_ctx_t *c = (verify_ctx_t *)ctx;
    const char *rel = path + c->src_len;
    while (*rel == '\\' || *rel == '/') rel++;
    char *dup = strdup(rel);
    if (!dup) return CDC_ERR_NOMEM;
    if (cdc_vec_push(c->files, &dup) != CDC_OK) {
        free(dup);
        return CDC_ERR_NOMEM;
    }
    return 0;
}

int cdc_execute_im_migration(const char *source_str, const char *target_dir_str,
                             const char *kind, char **out_msg,
                             cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (out_msg) *out_msg = NULL;
    if (!source_str || !target_dir_str || !kind) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL param");
        return CDC_ERR_INVALID_ARG;
    }
    if (!cdc_safe_for_quoted_cmd_path(source_str) ||
        !cdc_safe_for_quoted_cmd_path(target_dir_str)) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "迁移路径包含不受支持的命令行字符");
        return CDC_ERR_INVALID_ARG;
    }
    if (!path_exists_utf8(source_str)) {
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "源数据目录不存在");
        return CDC_ERR_NOT_FOUND;
    }
    int source_is_dir = 0;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(source_str);
    if (w) {
        DWORD a = GetFileAttributesW(w);
        source_is_dir = a != INVALID_FILE_ATTRIBUTES &&
                        (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
        free(w);
    }
#else
    struct stat st;
    source_is_dir = stat(source_str, &st) == 0 && S_ISDIR(st.st_mode);
#endif
    if (!source_is_dir) {
        if (err) cdc_err_set(err, CDC_ERR_NOT_FOUND, "源数据目录不存在");
        return CDC_ERR_NOT_FOUND;
    }
    if (strcmp(kind, "wechat") != 0 && strcmp(kind, "qq") != 0) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "无效的搬家类型");
        return CDC_ERR_INVALID_ARG;
    }
    char **roots = NULL;
    size_t nroots = 0;
    if (cdc_im_roots(kind, &roots, &nroots, NULL) != CDC_OK) {
        if (err) cdc_err_set(err, CDC_ERR_IO, "无法探测 %s 数据目录", kind);
        return CDC_ERR_IO;
    }
    char *source_norm = cdc_normalize(source_str);
    int is_root = 0;
    if (source_norm) {
        for (size_t i = 0; i < nroots; i++) {
            char *rn = cdc_normalize(roots[i]);
            if (rn && ascii_iequal(rn, source_norm)) is_root = 1;
            free(rn);
            if (is_root) break;
        }
    }
    free(source_norm);
    if (!is_root) {
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "只能迁移微信/QQ 自己探测到的数据根目录，已拒绝本次操作");
        return CDC_ERR_INVALID_ARG;
    }
    if (cdc_explicit_delete_forbidden(source_str)) {
        cdc_strvec_free(roots, nroots);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "该目录受保护，不能迁移");
        return CDC_ERR_INVALID_ARG;
    }
    int is_reparse = 0;
    cdc_err_t e2;
    int rrc = cdc_reparse_point_status(source_str, &is_reparse, &e2);
    if (rrc != CDC_OK) {
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "无法安全检查源目录：%s", e2.message);
        return CDC_ERR_IO;
    }
    if (is_reparse) {
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "源目录已经是软链接/联接点，之前已经完成过搬家");
        return CDC_ERR_INVALID_ARG;
    }
    const char *within_roots[1] = { source_str };
    if (cdc_is_within(target_dir_str, within_roots, 1)) {
        cdc_strvec_free(roots, nroots);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "目标目录不能位于源目录内部");
        return CDC_ERR_INVALID_ARG;
    }
    if (path_exists_utf8(target_dir_str)) {
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_INVALID_ARG,
                        "目标目录已存在，请换一个位置或先手动清理");
        return CDC_ERR_INVALID_ARG;
    }
    const char *procs[3] = { "wechat.exe", "qq.exe", "xwechat.exe" };
    char **running = NULL;
    size_t nrunning = 0;
    cdc_check_running(procs, 3, &running, &nrunning, NULL);
    if (nrunning > 0) {
        cdc_buf_t b;
        cdc_buf_init(&b);
        cdc_buf_append_str(&b, "检测到 ");
        for (size_t i = 0; i < nrunning; i++) {
            if (i > 0) cdc_buf_append_str(&b, "、");
            cdc_buf_append_str(&b, running[i]);
        }
        cdc_buf_append_str(&b, " 正在运行，请先彻底退出该软件后再执行搬家！");
        char *msg = cdc_buf_take_cstr(&b);
        cdc_strvec_free(running, nrunning);
        cdc_strvec_free(roots, nroots);
        if (err) cdc_err_set(err, CDC_ERR_IO, "%s", msg ? msg : "");
        free(msg);
        return CDC_ERR_IO;
    }
    cdc_strvec_free(running, nrunning);

    stage("im_move", "统计源数据");
    size_count_t src_sc = dir_size_count(source_str);
    uint64_t src_size = src_sc.size, file_cnt = src_sc.count;
    if (src_size == 0 || file_cnt == 0) {
        cdc_strvec_free(roots, nroots);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "源数据目录是空的，无需搬家");
        return CDC_ERR_INVALID_ARG;
    }
    char *target_norm = cdc_normalize(target_dir_str);
    char target_drive[4] = { 0, 0, 0, 0 };
    if (target_norm && strlen(target_norm) >= 3) {
        target_drive[0] = target_norm[0];
        target_drive[1] = target_norm[1];
        target_drive[2] = target_norm[2];
    }
    free(target_norm);
    char **drives = NULL;
    size_t ndrives = 0;
    int drive_ok = 0;
    if (cdc_fixed_drives(&drives, &ndrives, NULL) == CDC_OK) {
        for (size_t i = 0; i < ndrives; i++) {
            if (ascii_iequal(drives[i], target_drive)) {
                drive_ok = 1;
                break;
            }
        }
    }
    cdc_strvec_free(drives, ndrives);
    if (!drive_ok) {
        cdc_strvec_free(roots, nroots);
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "目标位置不是本机固定磁盘");
        return CDC_ERR_INVALID_ARG;
    }
    uint64_t free_b = 0, total_b = 0;
    if (cdc_drive_free_space(target_drive, &free_b, &total_b, NULL) == CDC_OK) {
        if (free_b < src_size + (1024ULL * 1024ULL * 1024ULL)) {
            cdc_strvec_free(roots, nroots);
            if (err)
                cdc_err_set(err, CDC_ERR_INVALID_ARG,
                            "目标磁盘剩余空间不足，请选择剩余空间更充裕的磁盘");
            return CDC_ERR_INVALID_ARG;
        }
    }

    stage("im_move", "复制文件");
    unit("im_move", "字节");
    g_im_move_bytes_done = 0;
    progress("im_move", 0, src_size);
    cdc_handle_chain_t *source_chain = NULL, *target_chain = NULL;
    cdc_err_t e3;
    if (copy_dir_recursive(source_str, target_dir_str, &source_chain,
                           &target_chain, &e3) != CDC_OK) {
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "拷贝文件至新目录失败：%s", e3.message);
        return CDC_ERR_IO;
    }
    stage("im_move", "校验文件");
    unit("im_move", "个文件");
    progress("im_move", 0, file_cnt);
    size_count_t dst_sc = dir_size_count(target_dir_str);
    if (dst_sc.size != src_size || dst_sc.count != file_cnt) {
        cleanup_target(target_dir_str, target_chain);
        cdc_handle_chain_free(source_chain);
        free(source_chain);
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_IO,
                        "拷贝校验失败（源 %llu 个文件 / %llu 字节，目标 %llu 个文件 / %llu 字节），已放弃搬家并清理目标目录",
                        (unsigned long long)file_cnt,
                        (unsigned long long)src_size,
                        (unsigned long long)dst_sc.count,
                        (unsigned long long)dst_sc.size);
        return CDC_ERR_IO;
    }
    if (verify_copy(source_str, target_dir_str, &e3) != CDC_OK) {
        cleanup_target(target_dir_str, target_chain);
        cdc_handle_chain_free(source_chain);
        free(source_chain);
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "逐文件校验失败（%s），已放弃搬家并清理目标目录",
                        e3.message);
        return CDC_ERR_IO;
    }
    cdc_handle_chain_free(source_chain);
    free(source_chain);

    /* 原目录改名备份（先写恢复日志，再改名）。 */
    stage("im_move", "备份原目录");
    char *stamp = cdc_stamp();
    char *base = cdc_basename(source_str);
    cdc_buf_t bb;
    cdc_buf_init(&bb);
    cdc_buf_append_str(&bb, base ? base : "");
    cdc_buf_append_str(&bb, "_backup_");
    cdc_buf_append_str(&bb, stamp ? stamp : "");
    free(stamp);
    free(base);
    char *backup_name = cdc_buf_take_cstr(&bb);
    char *spar = parent_of(source_str);
    char *backup_path = spar ? path_join_win(spar, backup_name ? backup_name : "")
                             : NULL;
    free(spar);
    free(backup_name);
    if (!backup_path) {
        cleanup_target(target_dir_str, target_chain);
        cdc_strvec_free(roots, nroots);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    cdc_err_t je;
    int jrc = write_journal(source_str, backup_path, kind, &je);
    if (jrc != CDC_OK) {
        cleanup_target(target_dir_str, target_chain);
        free(backup_path);
        cdc_strvec_free(roots, nroots);
        if (err) {
            if (je.code == CDC_ERR_EXISTS)
                cdc_err_set(err, CDC_ERR_IO,
                            "发现尚未解决的搬家恢复日志，原目录未改名；请先处理上次搬家留下的备份状态。");
            else
                cdc_err_set(err, CDC_ERR_IO,
                            "搬家恢复日志未能可靠写入，原目录未改名：%s",
                            je.message);
        }
        return CDC_ERR_IO;
    }
    cdc_err_t re;
    if (cdc_rename_directory_sibling_checked(source_str, backup_path,
                                             (const char **)roots, nroots,
                                             &re) != CDC_OK) {
        clear_journal();
        cleanup_target(target_dir_str, target_chain);
        free(backup_path);
        cdc_strvec_free(roots, nroots);
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "备份原目录失败（可能被占用）：%s",
                        re.message);
        return CDC_ERR_IO;
    }

    /* 在原位置创建 NTFS Junction (mklink /J)。 */
    stage("im_move", "创建目录联接");
    char *cmd_exe = cdc_system32_path("cmd.exe");
    cdc_buf_t mb;
    cdc_buf_init(&mb);
    cdc_buf_append_str(&mb, "/D /C mklink /J \"");
    {
        /* 路径：'/' 换 '\'，去尾 '\'。 */
        char *s = strdup(source_str);
        char *t = strdup(target_dir_str);
        for (char *p = s; s && *p; p++)
            if (*p == '/') *p = '\\';
        for (char *p = t; t && *p; p++)
            if (*p == '/') *p = '\\';
        size_t ns = s ? strlen(s) : 0;
        while (ns > 0 && s[ns - 1] == '\\') s[--ns] = '\0';
        size_t nt = t ? strlen(t) : 0;
        while (nt > 0 && t[nt - 1] == '\\') t[--nt] = '\0';
        cdc_buf_append_str(&mb, s ? s : "");
        cdc_buf_append_str(&mb, "\" \"");
        cdc_buf_append_str(&mb, t ? t : "");
        cdc_buf_append_str(&mb, "\"");
        free(s);
        free(t);
    }
    char *margs = cdc_buf_take_cstr(&mb);
    long mcode = -1;
    cdc_err_t me;
    int mrc = cdc_run_command(cmd_exe, margs, 1, NULL, NULL, &mcode, &me);
    free(cmd_exe);
    free(margs);
    if (mrc == CDC_OK && mcode == 0) {
        stage("im_move", "清理备份");
        const char *broots[1] = { backup_path };
        uint64_t removed = 0;
        cdc_err_t rme;
        int removed_ok =
            cdc_remove_tree_checked(backup_path, broots, 1, &removed, &rme) ==
            CDC_OK;
        if (removed_ok) {
            clear_journal();
        } else {
            cdc_buf_t lb;
            cdc_buf_init(&lb);
            cdc_buf_append_str(&lb, "搬家已完成，但备份清理未完成；恢复日志保留：");
            cdc_buf_append_str(&lb, backup_path);
            char *line = cdc_buf_take_cstr(&lb);
            const char *lines[1] = { line ? line : "" };
            cdc_write_log(lines, 1, NULL, NULL);
            free(line);
        }
        if (out_msg) {
            cdc_buf_t ob;
            cdc_buf_init(&ob);
            char tmp[512];
            snprintf(tmp, sizeof(tmp),
                     "数据搬家成功！已将 %llu 个文件（%.2f GB）迁移至 %s，原 C 盘空间已释放%s。",
                     (unsigned long long)file_cnt,
                     (double)src_size / (1024.0 * 1024.0 * 1024.0),
                     target_dir_str,
                     removed_ok ? "" : "（备份目录删除失败，请手动清理残留）");
            cdc_buf_append_str(&ob, tmp);
            *out_msg = cdc_buf_take_cstr(&ob);
        }
        cdc_handle_chain_free(target_chain);
        free(target_chain);
        free(backup_path);
        cdc_strvec_free(roots, nroots);
        return CDC_OK;
    }
    /* Junction 失败：把备份改回原名（成功才清日志），再清目标。 */
    cdc_err_t rre;
    if (cdc_rename_directory_sibling_checked(backup_path, source_str,
                                             (const char **)roots, nroots,
                                             &rre) != CDC_OK) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO,
                        "创建目录联接失败，自动恢复暂未成功；原数据保留在 %s，恢复日志已保留供下次重试：%s",
                        backup_path, rre.message);
        free(backup_path);
        cdc_strvec_free(roots, nroots);
        return CDC_ERR_IO;
    }
    int target_removed = cleanup_target(target_dir_str, target_chain);
    clear_journal();
    free(backup_path);
    cdc_strvec_free(roots, nroots);
    if (err) {
        if (target_removed)
            cdc_err_set(err, CDC_ERR_IO, "创建 NTFS 目录联接失败，原数据已自动恢复。");
        else
            cdc_err_set(err, CDC_ERR_IO,
                        "创建 NTFS 目录联接失败，原数据已恢复；目标目录有残留，请手动清理。");
    }
    return CDC_ERR_IO;
}
