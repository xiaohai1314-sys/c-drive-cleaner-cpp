// gui.cpp — GUI 入口与 45 个后端命令的适配层。
//
// 对照 Rust desktop/src/main.rs 的 invoke_handler（45 个命令，
// 命令表见 docs/bridge-commands.md；前端发什么见 web/src/api/bridge.ts）。
// 每个命令一个适配函数：cJSON 解析 args_json → 调 src/ 下的后端函数 →
// 拼 reply JSON（成功填 *out_json，失败填 *out_error，均为 malloc 分配，
// 与 host/bridge.h 的 handler 签名一致）。
//
// 与 Rust 版有意的差别（其余语义逐行对齐，见各函数注释）：
//  - 任务坞（task.rs）与扫描状态（scan_begin/end）由本文件实现；
//    互斥规则与 30 秒保留期与 Rust 一致。
//  - 安全删除的 Pending（需重启删除）与 Rust 对齐：DeleteOutcome 有
//    Pending 一档，clean_categories 按 Rust 的办法计 pending + skipped；
//    但句柄相对的删除实现不做基于路径的重启删除，目前没有调用方返回
//    Pending（与 Rust 的 dead_code 状态一致）。
//  - scan_dups/scan_big 的索引优先 + 5 分钟 JSON 缓存在这里实现
//    （scan.cpp 只做实时扫描）；索引增量追赶复用 main.cpp 的编排。
//  - 前端发来的参数名有两个与 Rust 签名不一致（前端 bug，
//    Rust 版会静默忽略/报错）：scan_dups 的 minSize（Rust 要 min_size）、
//    im_move_execute 的 targetDir（Rust 要 target_dir）。这里两个名字都认，
//    以前端实际发的为准。
//  - imthumb 缩略图协议与 pickPaths 原生对话框是 host 侧的缺口
//    （见 docs/frontend-bridge.md），与本文件无关。
//
// Windows-only 的部分全部包在 #ifdef _WIN32 里；Linux 下本文件只编
// 纯函数（frag 解析、缓存键、任务坞等，见 tests/test_gui.cpp）。
#include "gui.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cJSON.h"
#include "blake3.h"
#include "logs.h"
#include "safety.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h> /* IsUserAnAdmin */

#include "bigfile.h"
#include "bridge.h"
#include "cdc_process.h"
#include "fastio.h" /* is_ssd */
#include "fileindex.h"
#include "hashcache.h"
#include "im.h"
#include "indextask.h"
#include "mft.h"
#include "privacy.h"
#include "recycle.h"
#include "rules.h"
#include "scan.h"
#include "shred.h"
#include "system.h"
#include "timeutil.h"
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace cdc {
namespace {

/* ---------------- 小工具 ---------------- */

#ifdef _WIN32
/* bridge 的 out_json/out_error 是 malloc 分配、由 bridge 以 free 释放。 */
char *gui_strdup(const std::string &s) {
    char *p = (char *)malloc(s.size() + 1);
    if (p) memcpy(p, s.c_str(), s.size() + 1);
    return p;
}
#endif

std::string copy_truncated(std::string_view value, size_t cap) {
    if (cap == 0) return {};
    std::string result(value);
    if (result.size() >= cap) result.resize(cap - 1);
    return result;
}

/* ASCII 小写（就地；UTF-8 多字节字节原样保留，与 Rust 的 to_lowercase
 * 在 ASCII 标签上的效果一致）。 */
#ifdef _WIN32
void ascii_lower(std::string &s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
}

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
#endif

/* epoch 毫秒（Rust task.rs 的 now_ms）。 */
uint64_t gui_now_ms() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

#ifdef _WIN32
SRWLOCK g_gui_mu = SRWLOCK_INIT;
#define GUI_LOCK() AcquireSRWLockExclusive(&g_gui_mu)
#define GUI_UNLOCK() ReleaseSRWLockExclusive(&g_gui_mu)
#else
pthread_mutex_t g_gui_mu = PTHREAD_MUTEX_INITIALIZER;
#define GUI_LOCK() pthread_mutex_lock(&g_gui_mu)
#define GUI_UNLOCK() pthread_mutex_unlock(&g_gui_mu)
#endif

/* ---------------- defrag 输出解析（纯函数，移植自 frag.rs） ---------------- */

/* UTF-8 解一个码点，返回字节数（非法字节按 1 字节计）。 */
size_t gui_utf8_next(std::string_view s, uint32_t *cp) {
    if (s.empty()) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && s.size() >= 2) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)((unsigned char)s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && s.size() >= 3) {
        *cp = ((uint32_t)(c & 0x0F) << 12) |
              ((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
              (uint32_t)((unsigned char)s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && s.size() >= 4) {
        *cp = ((uint32_t)(c & 0x07) << 18) |
              ((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
              ((uint32_t)((unsigned char)s[2] & 0x3F) << 6) |
              (uint32_t)((unsigned char)s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

/* 前 count 个字符的子串（不足则全文）；返回字节长度。 */
size_t gui_window_chars_len(std::string_view s, size_t count) {
    size_t off = 0;
    uint32_t cp = 0;
    while (off < s.size() && count > 0) {
        size_t adv = gui_utf8_next(s.substr(off), &cp);
        if (adv == 0) break;
        off += adv;
        count--;
    }
    return off;
}

bool ascii_ci_equal(unsigned char a, unsigned char b) {
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
    return a == b;
}

/* ASCII 大小写不敏感的子串查找；返回标签之后的位置，找不到返回空。 */
std::string_view gui_find_after_ci(std::string_view text, std::string_view label) {
    if (label.empty() || label.size() > text.size()) return {};
    for (size_t i = 0; i + label.size() <= text.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < label.size(); j++) {
            if (!ascii_ci_equal((unsigned char)text[i + j], (unsigned char)label[j])) {
                match = false;
                break;
            }
        }
        if (match) return text.substr(i + label.size());
    }
    return {};
}

/* 在 text 里找第一个数（数字开头，可含 . 与 ,）；返回 true 并填值与结束字节偏移。 */
bool gui_next_number(std::string_view text, double *out_value, size_t *out_end) {
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] >= '0' && text[i] <= '9') {
            size_t start = i;
            size_t end = i + 1;
            while (end < text.size() &&
                   ((text[end] >= '0' && text[end] <= '9') || text[end] == '.' ||
                    text[end] == ',')) {
                end++;
            }
            /* 去掉千分位逗号再转 double。 */
            std::string raw(text.substr(start, end - start));
            raw.erase(std::remove(raw.begin(), raw.end(), ','), raw.end());
            char *ep = nullptr;
            double v = strtod(raw.c_str(), &ep);
            if (ep && ep != raw.c_str()) {
                *out_value = v;
                *out_end = end;
                return true;
            }
            i = end;
            continue;
        }
        i++;
    }
    return false;
}

/* 在 labels 里任一标签之后 60 个字符内找数字，且数字后紧跟 unit（大小写不敏感）。 */
bool gui_number_with_unit(std::string_view text, std::span<const std::string_view> labels,
                          std::string_view unit, double *out) {
    for (std::string_view label : labels) {
        std::string_view after = gui_find_after_ci(text, label);
        if (after.empty()) continue;
        size_t win = gui_window_chars_len(after, 60);
        std::string window(after.substr(0, win));
        double value = 0;
        size_t end = 0;
        if (!gui_next_number(window, &value, &end)) return false;
        std::string_view rest = std::string_view(window).substr(end);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
            rest.remove_prefix(1);
        if (rest.size() < unit.size()) return false;
        bool match = true;
        for (size_t k = 0; k < unit.size(); k++) {
            if (!ascii_ci_equal((unsigned char)rest[k], (unsigned char)unit[k])) {
                match = false;
                break;
            }
        }
        if (match) {
            *out = value;
            return true;
        }
        return false;
    }
    return false;
}

bool gui_frag_search_percent(std::string_view text, double *out) {
    static constexpr std::array<std::string_view, 4> labels = {
        "碎片率", "碎片总计", "文件碎片百分比", "Percent file fragmentation"};
    if (gui_number_with_unit(text, labels, "%", out)) return true;
    static constexpr std::array<std::string_view, 2> labels2 = {"fragmented", "碎片"};
    if (gui_number_with_unit(text, labels2, "%", out)) return true;
    /* 旧版输出：第一个「xxx = 12%」通常就是碎片率，跳过可用空间那一行。 */
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos
                                                       ? std::string_view::npos
                                                       : nl - pos);
        if (line.find('=') != std::string_view::npos &&
            line.find('%') != std::string_view::npos) {
            std::string lower(line);
            for (char &c : lower) {
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            }
            bool skip = (line.find("可用") != std::string_view::npos ||
                         lower.find("free") != std::string::npos);
            if (!skip) {
                double v = 0;
                size_t end = 0;
                if (gui_next_number(line, &v, &end)) {
                    *out = v;
                    return true;
                }
            }
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return false;
}

bool gui_frag_search_count(std::string_view text, uint64_t *out) {
    static constexpr std::array<std::string_view, 6> labels = {
        "碎片文件数", "总碎片数", "碎片数", "Fragmented files", "Total fragments",
        "碎片文件"};
    std::string_view after;
    for (std::string_view label : labels) {
        after = gui_find_after_ci(text, label);
        if (!after.empty()) break;
    }
    if (after.empty()) return false;
    size_t win = gui_window_chars_len(after, 60);
    std::string window(after.substr(0, win));
    double v = 0;
    size_t end = 0;
    if (!gui_next_number(window, &v, &end)) return false;
    *out = (uint64_t)v;
    return true;
}

}  // namespace

FragParsed frag_parse_defrag(std::string_view text) {
    FragParsed r;
    /* U+3000（E3 80 80）折叠为空格，与 Rust 的 replace('\u{3000}', " ") 一致。 */
    std::string folded;
    folded.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (i + 2 < text.size() && (unsigned char)text[i] == 0xE3 &&
            (unsigned char)text[i + 1] == 0x80 &&
            (unsigned char)text[i + 2] == 0x80) {
            folded.push_back(' ');
            i += 3;
        } else {
            folded.push_back(text[i++]);
        }
    }
    double v = 0;
    if (gui_frag_search_percent(folded, &v)) {
        r.has_percent = true;
        r.percent = v;
    }
    {
        static constexpr std::array<std::string_view, 4> labels = {
            "碎片空间", "文件碎片", "Total fragmented space", "碎片文件"};
        if (gui_number_with_unit(folded, labels, "GB", &v)) {
            r.has_space_gb = true;
            r.space_gb = v;
        }
    }
    {
        uint64_t c = 0;
        if (gui_frag_search_count(folded, &c)) {
            r.has_count = true;
            r.count = c;
        }
    }
    {
        static constexpr std::array<std::string_view, 2> labels = {"可用空间",
                                                                   "Free space"};
        if (gui_number_with_unit(folded, labels, "GB", &v)) {
            r.has_free_gb = true;
            r.free_gb = v;
        }
    }
    return r;
}

namespace {

/* ---------------- dup 缓存键 / 索引作用域（纯函数） ---------------- */

/* Rust Debug 格式转义（只处理 " 与 \；控制字符在盘符/路径键里不出现）。 */
void debug_escape_into(std::string &out, std::string_view s) {
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
}

}  // namespace

std::string gui_dup_cache_key(std::span<const std::string> roots, uint64_t min_size,
                              bool fast) {
    std::string s = "[";
    for (size_t i = 0; i < roots.size(); i++) {
        if (i > 0) s += ", ";
        s += "\"";
        debug_escape_into(s, roots[i]);
        s += "\"";
    }
    s += "]";
    s += format(":%llu:%s", (unsigned long long)min_size, fast ? "true" : "false");
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, s.data(), s.size());
    uint8_t digest[32];
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    static const char hexd[] = "0123456789abcdef";
    std::string hex(64, '0');
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = hexd[digest[i] >> 4];
        hex[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    return hex;
}

bool gui_parse_index_scope(std::string_view root, std::string *drive_out,
                           std::optional<std::string> *prefix_out) {
    if (prefix_out) *prefix_out = std::nullopt;
    if (root.empty() || !drive_out) return false;
    size_t len = root.size();
    while (len > 0 && root[len - 1] == '\\') len--;
    if (len < 2 || root[1] != ':') return false;
    char letter = root[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
        return false;
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    *drive_out = std::string({letter, ':', '\\'});
    if (prefix_out && len > 2) {
        *prefix_out = std::string(root.substr(0, len));
    }
    return true;
}

bool gui_path_under_dir(std::string_view path, std::string_view dir_prefix) {
    if (path.size() <= dir_prefix.size()) return false;
    for (size_t i = 0; i < dir_prefix.size(); i++) {
        if (!ascii_ci_equal((unsigned char)path[i], (unsigned char)dir_prefix[i]))
            return false;
    }
    return path[dir_prefix.size()] == '\\';
}

namespace {

/* ---------------- 任务坞（纯函数，移植自 Rust task.rs） ---------------- */

constexpr size_t kGuiMaxTasks = 16;
constexpr uint64_t kGuiTaskRetainMs = 30000ULL;

struct GuiTask {
    bool used = false;
    std::string id;
    std::string title;
    std::string stage;
    std::string unit;
    uint64_t done = 0;
    uint64_t total = 0;
    bool active = false;
    bool cancellable = false;
    bool cancelling = false;
    bool mutating = false;
    std::string outcome;
    std::string summary;
    uint64_t started_ms = 0;
    uint64_t finished_ms = 0;
};

std::array<GuiTask, kGuiMaxTasks> g_tasks;

GuiTask *gui_task_find(std::string_view id) {
    for (auto &t : g_tasks) {
        if (t.used && t.id == id) return &t;
    }
    return nullptr;
}

}  // namespace

int task_begin(std::string_view id, std::string_view title, bool cancellable,
               bool mutating, std::string *out_error) {
    if (out_error) out_error->clear();
    if (id.empty()) {
        if (out_error) *out_error = "任务 id 为空";
        return -1;
    }
    GUI_LOCK();
    if (mutating) {
        for (auto &t : g_tasks) {
            if (t.used && t.active && t.mutating) {
                GUI_UNLOCK();
                if (out_error) *out_error = "已有清理任务正在进行，请等它结束后再试";
                return -1;
            }
        }
    }
    GuiTask *slot = gui_task_find(id);
    if (!slot) {
        for (auto &t : g_tasks) {
            if (!t.used) {
                slot = &t;
                break;
            }
        }
    }
    if (!slot) {
        GUI_UNLOCK();
        if (out_error) *out_error = "任务过多";
        return -1;
    }
    *slot = GuiTask{};
    slot->used = true;
    slot->id = copy_truncated(id, 64);
    slot->title = copy_truncated(title, 128);
    slot->active = true;
    slot->cancellable = cancellable;
    slot->mutating = mutating;
    slot->outcome = "running";
    slot->started_ms = gui_now_ms();
    GUI_UNLOCK();
    return 0;
}

void task_finish(std::string_view id, std::string_view outcome, std::string_view summary) {
    GUI_LOCK();
    GuiTask *t = gui_task_find(id);
    if (t && t->active) {
        t->active = false;
        t->outcome = copy_truncated(outcome.empty() ? "ok" : outcome, 16);
        t->summary = copy_truncated(summary, 512);
        t->finished_ms = gui_now_ms();
    }
    GUI_UNLOCK();
}

namespace {

void gui_task_set_str(std::string_view id, int which, std::string_view value) {
    GUI_LOCK();
    GuiTask *t = gui_task_find(id);
    if (t) {
        if (which == 0) t->stage = copy_truncated(value, 128);
        if (which == 1) t->unit = copy_truncated(value, 32);
    }
    GUI_UNLOCK();
}

}  // namespace

void task_set_stage(std::string_view id, std::string_view stage) {
    gui_task_set_str(id, 0, stage);
}

void task_set_unit(std::string_view id, std::string_view unit) {
    gui_task_set_str(id, 1, unit);
}

void task_set_total(std::string_view id, uint64_t total) {
    GUI_LOCK();
    GuiTask *t = gui_task_find(id);
    if (t) t->total = total;
    GUI_UNLOCK();
}

void task_set_progress(std::string_view id, uint64_t done, uint64_t total) {
    GUI_LOCK();
    GuiTask *t = gui_task_find(id);
    if (t) {
        t->done = done;
        t->total = total;
    }
    GUI_UNLOCK();
}

void task_add_done(std::string_view id, uint64_t amount) {
    GUI_LOCK();
    GuiTask *t = gui_task_find(id);
    if (t) t->done += amount;
    GUI_UNLOCK();
}

bool task_cancelled(std::string_view id) {
    GUI_LOCK();
    GuiTask *t = gui_task_find(id);
    bool c = (t && t->cancelling);
    GUI_UNLOCK();
    return c;
}

std::string task_status_json() {
    uint64_t now = gui_now_ms();
    GUI_LOCK();
    /* 剪掉已结束超 30 秒的任务（与 Rust status() 的 retain 一致）。 */
    for (auto &t : g_tasks) {
        if (t.used && !t.active && t.finished_ms != 0 &&
            now - t.finished_ms >= kGuiTaskRetainMs) {
            t = GuiTask{};
        }
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        GUI_UNLOCK();
        return {};
    }
    for (auto &t : g_tasks) {
        if (!t.used) continue;
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "id", t.id.c_str());
        cJSON_AddStringToObject(o, "title", t.title.c_str());
        cJSON_AddStringToObject(o, "stage", t.stage.c_str());
        cJSON_AddNumberToObject(o, "done", (double)t.done);
        cJSON_AddNumberToObject(o, "total", (double)t.total);
        cJSON_AddStringToObject(o, "unit", t.unit.c_str());
        cJSON_AddBoolToObject(o, "active", t.active);
        cJSON_AddBoolToObject(o, "cancellable", t.cancellable);
        cJSON_AddBoolToObject(o, "cancelling", t.cancelling);
        cJSON_AddStringToObject(o, "outcome", t.outcome.c_str());
        cJSON_AddStringToObject(o, "summary", t.summary.c_str());
        cJSON_AddNumberToObject(o, "started_ms", (double)t.started_ms);
        cJSON_AddNumberToObject(o, "finished_ms", (double)t.finished_ms);
        cJSON_AddBoolToObject(o, "mutating", t.mutating);
        cJSON_AddItemToObject(root, t.id.c_str(), o);
    }
    GUI_UNLOCK();
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return {};
    /* cJSON_PrintUnformatted 用 cJSON 自身的钩子分配（glibc malloc），
     * 这里转拷一份 std::string 再交出。 */
    std::string copy(s);
    cJSON_free(s);
    return copy;
}

bool task_cancel(std::optional<std::string_view> id) {
    bool any = false;
    GUI_LOCK();
    if (id) {
        GuiTask *t = gui_task_find(*id);
        if (t && t->active && t->cancellable) {
            t->cancelling = true;
            any = true;
        }
    } else {
        for (auto &t : g_tasks) {
            if (t.used && t.active && t.cancellable) {
                t.cancelling = true;
                any = true;
            }
        }
    }
    GUI_UNLOCK();
    return any;
}

namespace {

/* ---------------- 扫描状态（纯函数） ---------------- */

struct ScanCell {
    bool active = false;
    std::string stage;
    bool cancelled = false;
    uint64_t done = 0;
    uint64_t total = 0;
    uint64_t found = 0;
};

ScanCell g_scan_dup;
ScanCell g_scan_big;
volatile int g_dup_cancel = 0;
volatile int g_big_cancel = 0;

ScanCell *gui_scan_cell(std::string_view kind) {
    if (kind == "big") return &g_scan_big;
    return &g_scan_dup;
}

volatile int *gui_scan_cancel_flag(std::string_view kind) {
    if (kind == "big") return &g_big_cancel;
    return &g_dup_cancel;
}

cJSON *gui_scan_cell_json(ScanCell *c) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return nullptr;
    cJSON_AddBoolToObject(o, "active", c->active);
    cJSON_AddStringToObject(o, "stage", c->stage.c_str());
    cJSON_AddBoolToObject(o, "cancelled", c->cancelled);
    cJSON_AddNumberToObject(o, "done", (double)c->done);
    cJSON_AddNumberToObject(o, "total", (double)c->total);
    cJSON_AddNumberToObject(o, "found", (double)c->found);
    return o;
}

}  // namespace

void scan_begin(std::string_view kind, std::string_view stage) {
    GUI_LOCK();
    ScanCell *c = gui_scan_cell(kind);
    *c = ScanCell{};
    c->active = true;
    c->stage = copy_truncated(stage, 128);
    GUI_UNLOCK();
    *gui_scan_cancel_flag(kind) = 0;
}

void scan_stage(std::string_view kind, std::string_view stage) {
    GUI_LOCK();
    gui_scan_cell(kind)->stage = copy_truncated(stage, 128);
    GUI_UNLOCK();
}

void scan_end(std::string_view kind, std::string_view stage) {
    GUI_LOCK();
    ScanCell *c = gui_scan_cell(kind);
    c->active = false;
    c->stage = copy_truncated(stage, 128);
    GUI_UNLOCK();
}

void scan_set_found(std::string_view kind, uint64_t found) {
    GUI_LOCK();
    gui_scan_cell(kind)->found = found;
    GUI_UNLOCK();
}

void scan_set_total(std::string_view kind, uint64_t total) {
    GUI_LOCK();
    gui_scan_cell(kind)->total = total;
    GUI_UNLOCK();
}

void scan_add_done(std::string_view kind, uint64_t amount) {
    GUI_LOCK();
    gui_scan_cell(kind)->done += amount;
    GUI_UNLOCK();
}

bool scan_cancelled(std::string_view kind) {
    GUI_LOCK();
    bool c = gui_scan_cell(kind)->cancelled;
    GUI_UNLOCK();
    return c;
}

std::string scan_status_json() {
    GUI_LOCK();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        GUI_UNLOCK();
        return {};
    }
    cJSON_AddItemToObject(root, "dup", gui_scan_cell_json(&g_scan_dup));
    cJSON_AddItemToObject(root, "big", gui_scan_cell_json(&g_scan_big));
    GUI_UNLOCK();
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return {};
    std::string copy(s);
    cJSON_free(s);
    return copy;
}

/* ---------------- dup 缓存失效（Rust invalidate_dup_cache） ---------------- */

void gui_invalidate_dup_cache() {
    auto dir = cache_dir();
    if (!dir) return;
    std::vector<std::string> paths;
    Error err;
    /* 先收集再删（遍历中不删文件）。 */
    walk_dir(dir->c_str(), 1,
             [&](const WalkEntry &entry) {
                 if (entry.is_dir) return 0;
                 std::string base = basename(entry.path);
                 if (base.size() >= 10 && base.substr(0, 5) == "dups-" &&
                     base.substr(base.size() - 5) == ".json") {
                     paths.emplace_back(entry.path);
                 }
                 return 0;
             },
             &err);
    for (const std::string &p : paths) remove(p.c_str());
}

/* ================= Windows 适配层（45 个命令） ================= */
#ifdef _WIN32

namespace {

/* ---------------- 参数解析 / 回复构造 ---------------- */

int gui_fail(char **out_error, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    *out_error = gui_strdup(buf);
    return -1;
}

int gui_ok_json(char **out_json, cJSON *root) {
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return -1;
    /* 同上：转拷一份，bridge 以 free 释放。 */
    char *copy = gui_strdup(s);
    cJSON_free(s);
    if (!copy) return -1;
    *out_json = copy;
    return 0;
}

int gui_ok_string(char **out_json, const std::string &s) {
    return gui_ok_json(out_json, cJSON_CreateString(s.c_str()));
}

int gui_ok_null(char **out_json) {
    return gui_ok_json(out_json, cJSON_CreateNull());
}

int gui_ok_bool(char **out_json, bool value) {
    return gui_ok_json(out_json, cJSON_CreateBool(value));
}

int gui_ok_u64(char **out_json, uint64_t value) {
    return gui_ok_json(out_json, cJSON_CreateNumber((double)value));
}

/* 解析 args_json（桥传来的 args 对象文本）；NULL/空串视作 {}。 */
cJSON *gui_parse_args(const char *args_json) {
    if (!args_json || !args_json[0]) return cJSON_CreateObject();
    cJSON *root = cJSON_Parse(args_json);
    if (!root) return cJSON_CreateObject();
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return cJSON_CreateObject();
    }
    return root;
}

const cJSON *gui_arg(const cJSON *args, const char *key) {
    if (!args) return nullptr;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!it || cJSON_IsNull(it)) return nullptr;
    return it;
}

const char *gui_arg_str(const cJSON *args, const char *key) {
    const cJSON *it = gui_arg(args, key);
    return (it && cJSON_IsString(it)) ? it->valuestring : nullptr;
}

/* 两个名字都认（前端与 Rust 签名不一致的两个参数：minSize/min_size、
 * targetDir/target_dir；以前者——文档里的名字——优先）。 */
const char *gui_arg_str2(const cJSON *args, const char *k1, const char *k2) {
    const char *s = gui_arg_str(args, k1);
    return s ? s : gui_arg_str(args, k2);
}

bool gui_arg_bool(const cJSON *args, const char *key, bool dflt) {
    const cJSON *it = gui_arg(args, key);
    if (!it) return dflt;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it);
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return dflt;
}

uint64_t gui_arg_u64(const cJSON *args, const char *key, uint64_t dflt) {
    const cJSON *it = gui_arg(args, key);
    if (!it || !cJSON_IsNumber(it)) return dflt;
    double v = it->valuedouble;
    if (v <= 0.0) return 0;
    return (uint64_t)v;
}

std::vector<std::string> gui_arg_str_array(const cJSON *args, const char *key) {
    std::vector<std::string> out;
    const cJSON *it = gui_arg(args, key);
    if (!it || !cJSON_IsArray(it)) return out;
    int n = cJSON_GetArraySize(it);
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(it, i);
        if (item && cJSON_IsString(item) && item->valuestring)
            out.emplace_back(item->valuestring);
    }
    return out;
}

/* ---------------- 路径小工具 ---------------- */

bool gui_path_exists(std::string_view path) {
    auto w = utf8_to_wide(std::string(path));
    if (!w) return false;
    DWORD attr = GetFileAttributesW(w->c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool gui_path_is_dir(std::string_view path) {
    auto w = utf8_to_wide(std::string(path));
    if (!w) return false;
    DWORD attr = GetFileAttributesW(w->c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool gui_path_is_reparse(std::string_view path) {
    auto w = utf8_to_wide(std::string(path));
    if (!w) return false;
    DWORD attr = GetFileAttributesW(w->c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

/* 文件大小与 mtime（unix 秒）；成功返回 true，取不到返回 false（size/mtime 置 0）。 */
bool gui_file_stat(std::string_view path, uint64_t *out_size, uint64_t *out_mtime) {
    if (out_size) *out_size = 0;
    if (out_mtime) *out_mtime = 0;
    auto w = utf8_to_wide(std::string(path));
    if (!w) return false;
    WIN32_FILE_ATTRIBUTE_DATA data;
    bool ok = GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &data) != 0;
    if (ok) {
        if (out_size) {
            *out_size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        }
        if (out_mtime) {
            ULARGE_INTEGER ft;
            ft.LowPart = data.ftLastWriteTime.dwLowDateTime;
            ft.HighPart = data.ftLastWriteTime.dwHighDateTime;
            if (ft.QuadPart >= 116444736000000000ULL) {
                *out_mtime = (ft.QuadPart - 116444736000000000ULL) / 10000000ULL;
            }
        }
    }
    return ok;
}

bool gui_is_admin() { return IsUserAnAdmin() != 0; }

/* ---------------- glob 展开（Rust expand_paths / glob::glob） ---------------- */

/* 把 pattern 按 '\\' 切段；含通配符的段逐级枚举目录匹配。 */
void gui_glob_expand_one(std::string_view pattern, std::vector<std::string> *out);

bool gui_glob_has_wild(std::string_view s) {
    return s.find('*') != std::string_view::npos ||
           s.find('?') != std::string_view::npos;
}

/* 枚举 dir 下与 seg（小写 glob）匹配的条目，把完整路径追加到 out。 */
void gui_glob_expand_segment(std::string_view dir, std::string_view seg_lower,
                             std::vector<std::string> *out) {
    auto wdir = utf8_to_wide(std::string(dir));
    if (!wdir) return;
    std::wstring wpat = *wdir;
    if (!wpat.empty() && wpat.back() != L'\\') wpat += L'\\';
    wpat += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        auto name = wide_to_utf8(fd.cFileName);
        if (!name) continue;
        std::string lower = *name;
        ascii_lower(lower);
        if (glob_match(seg_lower, lower)) {
            /* 拼完整路径：dir + '\\' + name。 */
            std::string full(dir);
            if (!full.empty() && full.back() != '\\') full += '\\';
            full += *name;
            out->push_back(std::move(full));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 递归展开一段 pattern：segs 为剩余段，cur 为当前前缀。 */
void gui_glob_expand_segs(const std::vector<std::string> &segs, size_t idx,
                          std::string_view cur, std::vector<std::string> *out) {
    if (idx >= segs.size()) {
        if (!cur.empty() && gui_path_exists(cur)) out->emplace_back(cur);
        return;
    }
    const std::string &seg = segs[idx];
    if (gui_glob_has_wild(seg)) {
        std::string lower = seg;
        ascii_lower(lower);
        /* 通配段只能枚举一个已存在的目录。 */
        std::string dir = cur.empty() ? "." : std::string(cur);
        std::vector<std::string> matches;
        gui_glob_expand_segment(dir, lower, &matches);
        for (const std::string &m : matches)
            gui_glob_expand_segs(segs, idx + 1, m, out);
        return;
    }
    /* 字面段：拼到当前前缀上。 */
    std::string next(cur);
    if (!next.empty()) {
        if (next.back() != '\\' && next.back() != ':') next += '\\';
    }
    next += seg;
    /* 中间段必须是已存在的目录（glob 只返回存在的路径）。 */
    if (idx + 1 < segs.size() && !gui_path_is_dir(next)) return;
    gui_glob_expand_segs(segs, idx + 1, next, out);
}

void gui_glob_expand_one(std::string_view pattern, std::vector<std::string> *out) {
    /* 切段：先处理盘符 "C:"。 */
    std::string prefix;
    std::string_view cur = pattern;
    if (pattern.size() >= 2 && pattern[1] == ':' &&
        ((pattern[0] >= 'A' && pattern[0] <= 'Z') ||
         (pattern[0] >= 'a' && pattern[0] <= 'z'))) {
        prefix = std::string({pattern[0], ':', '\\'});
        cur = pattern.substr(2);
        if (!cur.empty() && cur.front() == '\\') cur.remove_prefix(1);
    }
    /* 按 '\\' 切段（连续斜杠合并）。 */
    std::vector<std::string> segs;
    size_t p = 0;
    while (p < cur.size() && segs.size() < 64) {
        while (p < cur.size() && cur[p] == '\\') p++;
        if (p >= cur.size()) break;
        size_t start = p;
        while (p < cur.size() && cur[p] != '\\') p++;
        segs.emplace_back(cur.substr(start, p - start));
    }
    if (segs.empty()) return;
    gui_glob_expand_segs(segs, 0, prefix, out);
}

/* Rust expand_paths：%ENV% 展开 + glob 展开 + 存在性过滤 + 按小写去重。 */
std::vector<std::string> gui_expand_paths(std::span<const std::string> raw) {
    std::vector<std::string> out;
    for (const std::string &r : raw) {
        std::string expanded = expand_env(r);
        if (gui_glob_has_wild(expanded)) {
            gui_glob_expand_one(expanded, &out);
        } else if (gui_path_exists(expanded)) {
            out.push_back(expanded);
        }
    }
    /* 按小写去重（保留首次出现顺序，与 Rust HashSet 去重一致）。 */
    std::vector<std::string> dedup;
    for (std::string &s : out) {
        std::string lower = s;
        ascii_lower(lower);
        bool dup = false;
        for (const std::string &kept : dedup) {
            std::string kl = kept;
            ascii_lower(kl);
            if (lower == kl) {
                dup = true;
                break;
            }
        }
        if (!dup) dedup.push_back(std::move(s));
    }
    return dedup;
}

/* ---------------- 匹配器（Rust build_matcher / include_matches） ---------------- */

struct Matcher {
    std::vector<std::string> patterns;
};

void matcher_init(Matcher *m, std::span<const std::string> patterns) {
    m->patterns.clear();
    if (patterns.empty()) {
        m->patterns.emplace_back("*");
        return;
    }
    for (const std::string &p : patterns) {
        std::string lower = p;
        ascii_lower(lower);
        m->patterns.push_back(std::move(lower));
    }
}

/* 规则里的 ".pdf" 这类裸扩展名按文件名后缀处理（Rust normalize_exclude）。 */
std::string normalize_exclude(std::string_view pattern) {
    if (pattern.size() >= 2 && pattern[0] == '.') {
        bool all_alnum = true;
        for (size_t i = 1; i < pattern.size(); i++) {
            char c = pattern[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z'))) {
                all_alnum = false;
                break;
            }
        }
        if (all_alnum) return "*" + std::string(pattern);
    }
    return std::string(pattern);
}

bool include_matches(std::string_view name_lower, const Matcher &inc, const Matcher &exc) {
    bool ok = false;
    for (const std::string &p : inc.patterns) {
        if (glob_match(p, name_lower)) {
            ok = true;
            break;
        }
    }
    if (!ok) return false;
    for (const std::string &p : exc.patterns) {
        if (glob_match(p, name_lower)) return false;
    }
    return true;
}

/* 年龄检查（Rust age_ok）：min_age 分钟为 0 永真；mtime 为 0 永真；
 * 未来时间永真；否则 now - mtime >= min_age * 60。 */
bool age_ok(uint64_t mtime, uint64_t min_age_minutes, uint64_t now) {
    if (min_age_minutes == 0) return true;
    if (mtime == 0) return true;
    if (mtime > now) return true;
    return (now - mtime) >= min_age_minutes * 60ULL;
}

/* ---------------- 回复 JSON 构造器 ---------------- */

cJSON *gui_str_array_json(std::span<const std::string> items) {
    cJSON *a = cJSON_CreateArray();
    if (!a) return nullptr;
    for (const std::string &it : items)
        cJSON_AddItemToArray(a, cJSON_CreateString(it.c_str()));
    return a;
}

cJSON *gui_delete_report_json(uint64_t freed, uint64_t deleted,
                             std::span<const std::string> failed,
                             std::span<const std::string> reasons) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return nullptr;
    cJSON_AddNumberToObject(o, "freed", (double)freed);
    cJSON_AddNumberToObject(o, "deleted", (double)deleted);
    cJSON_AddItemToObject(o, "failed", gui_str_array_json(failed));
    cJSON_AddItemToObject(o, "reasons", gui_str_array_json(reasons));
    return o;
}

void gui_delete_log(std::string_view title, uint64_t freed, uint64_t deleted,
                    size_t nfailed) {
    std::string line2 = format("清理：%s", std::string(title).c_str());
    std::string line3 = format("完成：删除 %llu 个，释放 %llu 字节，失败 %llu 个",
                               (unsigned long long)deleted,
                               (unsigned long long)freed,
                               (unsigned long long)nfailed);
    std::string lines[3] = {"开始清理", line2, line3};
    write_log(lines, nullptr, nullptr);
}

/* ---------------- scan_categories / clean_categories ---------------- */

struct ScanCtx {
    const Matcher *inc = nullptr;
    const Matcher *exc = nullptr;
    uint64_t min_age_minutes = 0;
    uint64_t now = 0;
    uint64_t size = 0;
    uint64_t count = 0;
};

/* scan_rule 的移植（Rust main.rs）：返回 true 并填 category，或 false 表示跳过该项。 */
bool gui_scan_rule(const Rule &rule, cJSON **out_category) {
    std::string_view kind = !rule.kind.empty() ? std::string_view(rule.kind) : "files";
    if (rule.key.empty()) return false;
    std::string_view name = !rule.name.empty() ? std::string_view(rule.name) : rule.key;
    std::string_view tier = !rule.tier.empty() ? std::string_view(rule.tier) : "standard";
    cJSON *o = cJSON_CreateObject();
    if (!o) return false;
    cJSON_AddStringToObject(o, "key", rule.key.c_str());
    cJSON_AddStringToObject(o, "name", std::string(name).c_str());
    cJSON_AddStringToObject(o, "tier", std::string(tier).c_str());
    cJSON_AddStringToObject(o, "describe", rule.describe.c_str());
    cJSON_AddStringToObject(o, "warning", rule.warning.c_str());
    cJSON_AddStringToObject(o, "note", rule.note.c_str());
    if (kind != "files") {
        cJSON_AddNumberToObject(o, "size", 0);
        cJSON_AddNumberToObject(o, "count", 0);
        cJSON_AddBoolToObject(o, "requires_confirm", true);
        cJSON_AddBoolToObject(o, "implemented", false);
        *out_category = o;
        return true;
    }
    std::vector<std::string> roots = gui_expand_paths(rule.paths);
    std::vector<std::string> glob_roots = gui_expand_paths(rule.glob_dirs);
    for (const std::string &g : glob_roots) {
        if (gui_path_is_dir(g)) roots.push_back(g);
    }
    if (roots.size() > 1) std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    if (roots.empty()) {
        cJSON_Delete(o);
        return false;
    }
    Matcher inc, exc;
    matcher_init(&inc, rule.patterns);
    std::vector<std::string> norm_exc;
    for (const std::string &e : rule.exclude) norm_exc.push_back(normalize_exclude(e));
    matcher_init(&exc, norm_exc);
    ScanCtx ctx;
    ctx.inc = &inc;
    ctx.exc = &exc;
    ctx.min_age_minutes = rule.min_age_minutes;
    ctx.now = now_seconds();
    Error err;
    for (const std::string &root : roots) {
        if (gui_path_is_reparse(root)) continue;
        if (!gui_path_is_dir(root)) {
            /* 根是文件：单独校验。 */
            uint64_t size = 0, mtime = 0;
            gui_file_stat(root, &size, &mtime);
            std::string lower = basename(root);
            ascii_lower(lower);
            if (include_matches(lower, *ctx.inc, *ctx.exc) &&
                age_ok(mtime, ctx.min_age_minutes, ctx.now)) {
                ctx.size += size;
                ctx.count += 1;
            }
            continue;
        }
        walk_dir(root.c_str(), 1,
                 [&](const WalkEntry &entry) {
                     if (entry.is_dir || entry.is_symlink) return 0;
                     std::string lower = basename(entry.path);
                     ascii_lower(lower);
                     if (include_matches(lower, *ctx.inc, *ctx.exc) &&
                         age_ok(entry.mtime, ctx.min_age_minutes, ctx.now)) {
                         ctx.size += entry.size;
                         ctx.count += 1;
                     }
                     return 0;
                 },
                 &err);
    }
    cJSON_AddNumberToObject(o, "size", (double)(int64_t)ctx.size);
    cJSON_AddNumberToObject(o, "count", (double)ctx.count);
    cJSON_AddBoolToObject(o, "requires_confirm",
                          rule.requires_confirm || rule.tier == "deep");
    cJSON_AddBoolToObject(o, "implemented", true);
    *out_category = o;
    return true;
}

int h_scan_categories(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    std::string begin_err;
    task_begin("scan_categories", "统计清理项", false, false, &begin_err);
    task_set_unit("scan_categories", "项");
    task_set_stage("scan_categories", "统计清理项");
    Error err;
    std::vector<Rule> rules;
    if (load_all_rules(&rules, &err) != kOk) {
        task_finish("scan_categories", "failed", "加载规则失败");
        return gui_fail(out_error, "加载规则失败");
    }
    task_set_total("scan_categories", (uint64_t)rules.size());
    std::vector<cJSON *> cats;
    for (size_t i = 0; i < rules.size(); i++) {
        cJSON *cat = nullptr;
        if (!gui_scan_rule(rules[i], &cat)) continue;
        cats.push_back(cat);
        task_set_progress("scan_categories", (uint64_t)(i + 1), (uint64_t)rules.size());
    }
    if (cats.size() > 1) {
        std::sort(cats.begin(), cats.end(), [](const cJSON *a, const cJSON *b) {
            const cJSON *sa = cJSON_GetObjectItemCaseSensitive(a, "size");
            const cJSON *sb = cJSON_GetObjectItemCaseSensitive(b, "size");
            double va = sa ? sa->valuedouble : 0;
            double vb = sb ? sb->valuedouble : 0;
            return va > vb;
        });
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        for (cJSON *c : cats) cJSON_Delete(c);
        task_finish("scan_categories", "failed", "内存不足");
        return gui_fail(out_error, "内存不足");
    }
    for (cJSON *c : cats) cJSON_AddItemToArray(arr, c);
    std::string summary = format("统计 %llu 项", (unsigned long long)cats.size());
    task_finish("scan_categories", "ok", summary);
    return gui_ok_json(out_json, arr);
}

/* ---------------- clean_categories ---------------- */

struct CleanReport {
    uint64_t freed = 0;
    uint64_t deleted = 0;
    uint64_t skipped = 0;
    uint64_t pending = 0;
    std::vector<std::string> errors;
    std::vector<std::string> notes;
    bool cancelled = false;
};

struct CleanCtx {
    CleanReport *report = nullptr;
    std::vector<const char *> roots;
    const Matcher *inc = nullptr;
    const Matcher *exc = nullptr;
    uint64_t min_age_minutes = 0;
    uint64_t now = 0;
    std::string_view task_id;
};

void gui_clean_one(std::string_view path, CleanCtx *c) {
    CleanReport *report = c->report;
    if (task_cancelled(c->task_id)) return;
    std::string lower = basename(path);
    ascii_lower(lower);
    if (!include_matches(lower, *c->inc, *c->exc)) return;
    uint64_t size = 0, mtime = 0;
    if (!gui_file_stat(path, &size, &mtime)) {
        report->skipped += 1;
        return;
    }
    if (!age_ok(mtime, c->min_age_minutes, c->now)) return;
    task_add_done(c->task_id, 1);
    Error err;
    DeleteOutcome rc = delete_file_checked(path, c->roots, true, &err);
    if (rc == DeleteOutcome::Deleted) {
        report->deleted += 1;
        report->freed += size;
    } else if (rc == DeleteOutcome::Skipped) {
        report->skipped += 1;
    } else if (rc == DeleteOutcome::Pending) {
        /* 与 Rust clean_one 一致：待重启的也算跳过。 */
        report->pending += 1;
        report->skipped += 1;
    } else {
        report->skipped += 1;
        if (report->errors.size() < 20) {
            report->errors.push_back(err.message[0] ? err.message : "删除失败");
        }
    }
}

int h_clean_categories(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    std::vector<std::string> keys = gui_arg_str_array(args, "keys");
    cJSON_Delete(args);
    if (keys.empty()) {
        return gui_fail(out_error, "缺少参数：keys");
    }
    CleanReport report;
    std::string begin_err;
    if (task_begin("clean", "C盘瘦身清理", true, true, &begin_err) != 0) {
        report.errors.push_back(begin_err.empty() ? "任务启动失败" : begin_err);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "freed", 0);
        cJSON_AddNumberToObject(o, "deleted", 0);
        cJSON_AddNumberToObject(o, "skipped", 0);
        cJSON_AddNumberToObject(o, "pending", 0);
        cJSON_AddItemToObject(o, "errors", gui_str_array_json(report.errors));
        cJSON_AddItemToObject(o, "notes", gui_str_array_json(report.notes));
        return gui_ok_json(out_json, o);
    }
    task_set_unit("clean", "个文件");
    task_set_stage("clean", "准备清理项");
    Error err;
    std::vector<Rule> rules;
    if (load_all_rules(&rules, &err) != kOk) {
        task_finish("clean", "failed", "加载规则失败");
        return gui_fail(out_error, "加载规则失败");
    }
    bool cancelled = false;
    for (const std::string &key : keys) {
        if (cancelled) break;
        const Rule *rule = nullptr;
        for (const Rule &r : rules) {
            if (r.key == key) {
                rule = &r;
                break;
            }
        }
        if (!rule) {
            report.errors.push_back("找不到清理项：" + key);
            continue;
        }
        if (task_cancelled("clean")) {
            cancelled = true;
            break;
        }
        task_set_stage("clean", "清理" + (!rule->name.empty() ? rule->name : key));
        std::string_view kind = !rule->kind.empty() ? std::string_view(rule->kind) : "files";
        if (kind != "files") {
            report.notes.push_back("「" + (!rule->name.empty() ? rule->name : key) +
                                   "」需要系统命令，已跳过（请到系统页清理）");
            continue;
        }
        std::vector<std::string> roots = gui_expand_paths(rule->paths);
        std::vector<std::string> glob_roots = gui_expand_paths(rule->glob_dirs);
        for (const std::string &g : glob_roots) {
            if (gui_path_is_dir(g)) roots.push_back(g);
        }
        if (roots.size() > 1) std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        if (roots.empty()) continue;
        Matcher inc, exc;
        matcher_init(&inc, rule->patterns);
        std::vector<std::string> norm_exc;
        for (const std::string &e : rule->exclude) norm_exc.push_back(normalize_exclude(e));
        matcher_init(&exc, norm_exc);
        CleanCtx ctx;
        ctx.report = &report;
        ctx.roots.reserve(roots.size());
        for (const std::string &r : roots) ctx.roots.push_back(r.c_str());
        ctx.inc = &inc;
        ctx.exc = &exc;
        ctx.min_age_minutes = rule->min_age_minutes;
        ctx.now = now_seconds();
        ctx.task_id = "clean";
        for (const std::string &root : roots) {
            if (cancelled) break;
            if (task_cancelled("clean")) {
                cancelled = true;
                break;
            }
            if (gui_path_is_reparse(root)) {
                report.skipped += 1;
                continue;
            }
            if (!gui_path_is_dir(root)) {
                gui_clean_one(root, &ctx);
                continue;
            }
            walk_dir(root.c_str(), 1,
                     [&](const WalkEntry &entry) {
                         if (entry.is_dir || entry.is_symlink) return 0;
                         if (task_cancelled("clean")) return 1;
                         gui_clean_one(entry.path, &ctx);
                         return 0;
                     },
                     &err);
            if (task_cancelled("clean")) {
                cancelled = true;
                break;
            }
            if (rule->prune_dirs && !is_never_clean(root)) {
                int removed = remove_empty_dirs(root.c_str());
                if (removed > 0) {
                    report.notes.push_back(
                        format("移除空目录 %llu 个", (unsigned long long)removed));
                }
            }
        }
        if (cancelled) break;
    }
    if (report.errors.size() > 10) report.errors.resize(10);
    if (cancelled) {
        report.errors.push_back("已取消");
    }
    {
        std::string summary = format("删除 %llu 个文件，释放 %.1f MB",
                                     (unsigned long long)report.deleted,
                                     (double)report.freed / 1048576.0);
        task_finish("clean", cancelled ? "cancelled" : "ok", summary);
    }
    {
        std::string line3 = format(
            "完成：删除 %llu 个文件，释放 %llu 字节，跳过 %llu 个，待重启 %llu 个",
            (unsigned long long)report.deleted, (unsigned long long)report.freed,
            (unsigned long long)report.skipped, (unsigned long long)report.pending);
        std::string lines[3] = {"开始清理", "清理：C盘瘦身", line3};
        write_log(lines, nullptr, nullptr);
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddNumberToObject(o, "freed", (double)report.freed);
    cJSON_AddNumberToObject(o, "deleted", (double)report.deleted);
    cJSON_AddNumberToObject(o, "skipped", (double)report.skipped);
    cJSON_AddNumberToObject(o, "pending", (double)report.pending);
    cJSON_AddItemToObject(o, "errors", gui_str_array_json(report.errors));
    cJSON_AddItemToObject(o, "notes", gui_str_array_json(report.notes));
    return gui_ok_json(out_json, o);
}

}  // namespace
#endif /* _WIN32 */

#ifdef _WIN32
namespace {

/* ---------------- frag ---------------- */

cJSON *gui_frag_report_json(std::string_view drive, const FragParsed *p, int ssd) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return nullptr;
    cJSON_AddStringToObject(o, "drive", std::string(drive).c_str());
    if (p->has_percent) {
        cJSON_AddNumberToObject(o, "percent", p->percent);
    } else {
        cJSON_AddNullToObject(o, "percent");
    }
    if (p->has_space_gb) {
        cJSON_AddNumberToObject(o, "space_gb", p->space_gb);
    } else {
        cJSON_AddNullToObject(o, "space_gb");
    }
    if (p->has_count) {
        cJSON_AddNumberToObject(o, "count", (double)p->count);
    } else {
        cJSON_AddNullToObject(o, "count");
    }
    if (p->has_free_gb) {
        cJSON_AddNumberToObject(o, "free_gb", p->free_gb);
    } else {
        cJSON_AddNullToObject(o, "free_gb");
    }
    if (ssd >= 0) {
        cJSON_AddBoolToObject(o, "ssd", ssd);
    } else {
        cJSON_AddNullToObject(o, "ssd");
    }
    return o;
}

bool gui_valid_fixed_drive(std::string_view drive) {
    /* Rust frag::valid_fixed_drive：必须是固定盘符 "X:\\" 形且在 fixed 列表里。 */
    if (drive.size() != 3 || drive[1] != ':' || drive[2] != '\\')
        return false;
    char letter = drive[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
        return false;
    Error err;
    std::vector<std::string> drives;
    if (fixed_drives(&drives, &err) != kOk) return false;
    for (const std::string &d : drives) {
        if (ascii_iequal(d, drive)) return true;
    }
    return false;
}

int h_frag_analyze(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *drive = gui_arg_str(args, "drive");
    std::string drive_buf = drive ? drive : "";
    cJSON_Delete(args);
    if (drive_buf.empty()) return gui_fail(out_error, "缺少参数：drive");
    std::string begin_err;
    task_begin("frag_analyze", "磁盘碎片分析", false, false, &begin_err);
    task_set_stage("frag_analyze", "分析磁盘碎片");
    cJSON *report = nullptr;
    if (!gui_valid_fixed_drive(drive_buf)) {
        FragParsed empty_parsed;
        report = gui_frag_report_json(drive_buf, &empty_parsed, -1);
        task_finish("frag_analyze", "ok", "碎片率 0.0%");
        return gui_ok_json(out_json, report);
    }
    int ssd = is_ssd(drive_buf.c_str());
    if (ssd < 0) ssd = 1; /* 未知按 SSD 处理（Rust unwrap_or(true)）。 */
    std::string defrag = system32_path("defrag.exe");
    std::string argbuf = drive_buf + " /A /V";
    Error err;
    std::string out_text, err_text;
    long exit_code = -1;
    int rc = run_command(defrag, argbuf, true, &out_text, &err_text, &exit_code, &err);
    std::string text;
    if (rc == kOk && !out_text.empty()) {
        text = out_text;
    } else if (!err_text.empty()) {
        text = err_text;
    }
    FragParsed parsed = frag_parse_defrag(text);
    report = gui_frag_report_json(drive_buf, &parsed, ssd);
    std::string summary = format("碎片率 %.1f%%", parsed.has_percent ? parsed.percent : 0.0);
    task_finish("frag_analyze", "ok", summary);
    if (!report) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, report);
}

int h_frag_optimize(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *drive = gui_arg_str(args, "drive");
    std::string drive_buf = drive ? drive : "";
    cJSON_Delete(args);
    if (drive_buf.empty()) return gui_fail(out_error, "缺少参数：drive");
    std::string begin_err;
    if (task_begin("frag", "磁盘碎片优化", false, true, &begin_err) != 0) {
        std::string msg = begin_err.empty() ? "任务启动失败" : begin_err;
        return gui_fail(out_error, "%s", msg.c_str());
    }
    task_set_stage("frag", "执行磁盘优化（可能需要几分钟）");
    std::string err_msg;
    if (!gui_valid_fixed_drive(drive_buf)) {
        err_msg = "只允许优化系统识别出的固定磁盘";
    } else {
        std::string defrag = system32_path("defrag.exe");
        std::string argbuf = drive_buf + " /O";
        Error err;
        long exit_code = -1;
        int rc = run_command(defrag, argbuf, false, nullptr, nullptr, &exit_code, &err);
        if (!(rc == kOk && exit_code == 0)) {
            err_msg = "磁盘优化失败，请检查权限或系统工具状态";
        }
    }
    if (err_msg.empty()) {
        task_finish("frag", "ok", "磁盘优化完成");
        return gui_ok_null(out_json);
    }
    task_finish("frag", "failed", err_msg);
    return gui_fail(out_error, "%s", err_msg.c_str());
}

/* ---------------- scan_dups / scan_big（索引优先 + JSON 缓存 + 实时） ---------------- */

/* 固定盘列表（system.h 版；与 Rust fixed_drives 一致：空时回落 ["C:\\"]）。 */
std::vector<std::string> gui_fixed_drives_list() {
    Error err;
    std::vector<std::string> drives;
    if (fixed_drives(&drives, &err) != kOk) drives.clear();
    if (drives.empty()) drives.emplace_back("C:\\");
    return drives;
}

/* 解析 scan_dups 的 roots：drives 非空 → 过滤固定盘（大小写不敏感，
 * 保留原串）；否则 root 单个；否则全部固定盘。 */
std::vector<std::string> gui_resolve_dup_roots(const cJSON *args) {
    std::vector<std::string> drives = gui_arg_str_array(args, "drives");
    const char *root = gui_arg_str(args, "root");
    std::vector<std::string> fixed = gui_fixed_drives_list();
    std::vector<std::string> roots;
    if (!drives.empty()) {
        for (const std::string &d : drives) {
            for (const std::string &f : fixed) {
                if (ascii_iequal(d, f)) {
                    roots.push_back(d);
                    break;
                }
            }
        }
    } else if (root && root[0]) {
        roots.emplace_back(root);
    } else {
        roots = fixed;
    }
    return roots;
}

cJSON *gui_dup_group_json(std::string_view hash_hex, uint64_t size,
                          std::span<const std::string> files,
                          std::span<const uint64_t> mtimes) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return nullptr;
    cJSON_AddStringToObject(o, "hash", std::string(hash_hex).c_str());
    cJSON_AddNumberToObject(o, "size", (double)size);
    cJSON_AddItemToObject(o, "files", gui_str_array_json(files));
    cJSON *m = cJSON_CreateArray();
    if (m) {
        for (uint64_t mt : mtimes) {
            cJSON_AddItemToArray(m, cJSON_CreateNumber((double)mt));
        }
    }
    cJSON_AddItemToObject(o, "mtimes", m);
    return o;
}

/* 索引行展平后排序用：(size, 摘要 hex, path)。 */
struct FlatDupRow {
    uint64_t size = 0;
    std::string hex;
    bool has_digest = false;
    std::string path;
    uint64_t mtime = 0;
};

/* 索引查重复（Rust query_dups_from_index）：返回 cJSON 数组或 nullptr（回落）。 */
cJSON *gui_query_dups_from_index(std::span<const std::string> roots, uint64_t min_size,
                                 bool fast) {
    if (min_size < kIndexMinSize) return nullptr;
    Error err;
    std::unique_ptr<FileIndex> idx_owner = FileIndex::open_default(&err);
    FileIndex *idx = idx_owner.get();
    if (!idx) return nullptr;
    std::vector<uint64_t> serials;
    std::vector<std::optional<std::string>> prefixes;
    for (const std::string &root : roots) {
        std::string drive;
        std::optional<std::string> prefix;
        if (!gui_parse_index_scope(root, &drive, &prefix)) return nullptr;
        uint64_t serial = 0;
        if (!index_scope_fresh(idx, drive, true, &serial) || serial == 0)
            return nullptr;
        serials.push_back(serial);
        prefixes.push_back(std::move(prefix));
    }
    if (serials.empty()) return nullptr;
    std::vector<IndexDupRow> rows = idx->query_dup_rows(serials, min_size);
    if (rows.empty()) return nullptr;
    /* 过滤系统文件 + 作用域前缀。 */
    std::vector<size_t> keep;
    for (size_t i = 0; i < rows.size(); i++) {
        if (is_system_file_name(rows[i].path)) continue;
        bool in_scope = false;
        for (size_t s = 0; s < serials.size(); s++) {
            if (serials[s] != rows[i].volume_serial) continue;
            if (!prefixes[s] || gui_path_under_dir(rows[i].path, *prefixes[s])) {
                in_scope = true;
                break;
            }
        }
        if (in_scope) keep.push_back(i);
    }
    /* 展平 + 按 (size, hex, path) 排序：完整性检查与建组都走线性扫描。 */
    std::vector<FlatDupRow> flat;
    for (size_t i : keep) {
        const IndexDupRow &r = rows[i];
        FlatDupRow row;
        row.size = r.size;
        row.has_digest = fast ? r.digests.has_strong : r.digests.has_full;
        char hex[65];
        digest_to_hex(fast ? r.digests.strong : r.digests.full, hex);
        row.hex = hex;
        row.path = r.path;
        row.mtime = r.mtime;
        flat.push_back(std::move(row));
    }
    if (flat.size() > 1) {
        std::sort(flat.begin(), flat.end(), [](const FlatDupRow &a, const FlatDupRow &b) {
            if (a.size != b.size) return a.size < b.size;
            if (a.hex != b.hex) return a.hex < b.hex;
            return a.path < b.path;
        });
    }
    if (flat.empty()) return nullptr; /* 全部被过滤：回落实时扫描（与旧版一致）。 */
    /* 完整性：任一 size 组（≥2 个成员）有成员缺所需摘要 → 回落实时扫描。 */
    bool complete = true;
    for (size_t i = 0; i < flat.size() && complete;) {
        size_t j = i + 1;
        while (j < flat.size() && flat[j].size == flat[i].size) j++;
        if (j - i >= 2) {
            for (size_t k = i; k < j; k++) {
                if (!flat[k].has_digest) {
                    complete = false;
                    break;
                }
            }
        }
        i = j;
    }
    if (!complete) return nullptr;
    /* 按 (size, hex) 分段建组（段内已按 path 排好序）。flat 为空时
     * 直接返回空数组（索引新鲜且完整，只是本卷无重复）。 */
    std::vector<cJSON *> groups;
    for (size_t i = 0; i < flat.size();) {
        size_t j = i + 1;
        while (j < flat.size() && flat[j].size == flat[i].size &&
               flat[j].hex == flat[i].hex) {
            j++;
        }
        if (j - i >= 2) {
            cJSON *g = gui_dup_group_json(flat[i].hex, flat[i].size, {}, {});
            if (!g) break;
            cJSON *files = cJSON_GetObjectItemCaseSensitive(g, "files");
            cJSON *mtimes = cJSON_GetObjectItemCaseSensitive(g, "mtimes");
            for (size_t k = i; k < j; k++) {
                if (files)
                    cJSON_AddItemToArray(files, cJSON_CreateString(flat[k].path.c_str()));
                if (mtimes)
                    cJSON_AddItemToArray(mtimes,
                                         cJSON_CreateNumber((double)flat[k].mtime));
            }
            groups.push_back(g);
        }
        i = j;
    }
    if (groups.size() > 1) {
        std::sort(groups.begin(), groups.end(), [](const cJSON *a, const cJSON *b) {
            const cJSON *sa = cJSON_GetObjectItemCaseSensitive(a, "size");
            const cJSON *sb = cJSON_GetObjectItemCaseSensitive(b, "size");
            const cJSON *fa = cJSON_GetObjectItemCaseSensitive(a, "files");
            const cJSON *fb = cJSON_GetObjectItemCaseSensitive(b, "files");
            uint64_t va = (uint64_t)(sa ? sa->valuedouble : 0) *
                          (uint64_t)(fa ? cJSON_GetArraySize(fa) : 0);
            uint64_t vb = (uint64_t)(sb ? sb->valuedouble : 0) *
                          (uint64_t)(fb ? cJSON_GetArraySize(fb) : 0);
            return va > vb;
        });
    }
    cJSON *result = cJSON_CreateArray();
    if (!result) {
        for (cJSON *g : groups) cJSON_Delete(g);
        return nullptr;
    }
    for (cJSON *g : groups) cJSON_AddItemToArray(result, g);
    return result;
}

/* 索引查大文件（Rust query_big_from_index）：返回 cJSON 数组或 nullptr（回落）。 */
cJSON *gui_query_big_from_index(std::span<const std::string> drives) {
    Error err;
    std::unique_ptr<FileIndex> idx_owner = FileIndex::open_default(&err);
    FileIndex *idx = idx_owner.get();
    if (!idx) return nullptr;
    std::vector<uint64_t> serials;
    for (const std::string &d : drives) {
        uint64_t serial = 0;
        if (!index_scope_fresh(idx, d, false, &serial) || serial == 0)
            return nullptr;
        serials.push_back(serial);
    }
    if (serials.empty()) return nullptr;
    std::vector<IndexBigRow> rows = idx->query_big_rows(serials);
    if (rows.empty()) return nullptr;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return nullptr;
    for (const IndexBigRow &r : rows) {
        if (is_system_file_name(r.path)) continue;
        if (r.size < big_file_threshold_bytes(r.path)) continue;
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "path", r.path.c_str());
        cJSON_AddNumberToObject(o, "size", (double)r.size);
        cJSON_AddNumberToObject(o, "mtime", (double)r.mtime);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* dup 缓存路径（cache_dir()/dups-{key}.json）。 */
std::string gui_dup_cache_path(std::string_view key) {
    auto dir = cache_dir();
    if (!dir) return {};
    return *dir + "/dups-" + std::string(key) + ".json";
}

void gui_dup_tick() { scan_add_done("dup", 1); }
void gui_dup_stage(const char *stage) { scan_stage("dup", stage); }
void gui_dup_total(uint64_t total) { scan_set_total("dup", total); }

int h_scan_dups(const char *args_json, char **out_json, char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    bool fast = gui_arg_bool(args, "fast", false);
    uint64_t min = gui_arg_u64(args, "min_size", 0);
    /* 前端发 minSize，Rust 签名是 min_size：两个名字都认。 */
    const cJSON *min_it = gui_arg(args, "minSize");
    if (min_it && cJSON_IsNumber(min_it) && min_it->valuedouble > 0)
        min = (uint64_t)min_it->valuedouble;
    if (min == 0) min = (uint64_t)1 << 20;
    std::vector<std::string> roots = gui_resolve_dup_roots(args);
    cJSON_Delete(args);
    scan_begin("dup", "枚举文件");
    if (roots.empty()) {
        scan_end("dup", "完成");
        return gui_ok_json(out_json, cJSON_CreateArray());
    }
    scan_stage("dup", "索引同步");
    cJSON *groups = gui_query_dups_from_index(roots, min, fast);
    if (groups) {
        scan_set_found("dup", (uint64_t)cJSON_GetArraySize(groups));
        scan_end("dup", "完成");
        return gui_ok_json(out_json, groups);
    }
    /* 5 分钟 JSON 缓存。 */
    std::string key = gui_dup_cache_key(roots, min, fast);
    std::string cache_path = gui_dup_cache_path(key);
    if (!cache_path.empty()) {
        uint64_t mtime = 0;
        uint64_t fsize = 0;
        gui_file_stat(cache_path, &fsize, &mtime);
        if (mtime != 0) {
            uint64_t now = now_seconds();
            if (now >= mtime && now - mtime < 300) {
                FILE *f = fopen(cache_path.c_str(), "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    if (sz > 0 && sz < 64 * 1024 * 1024) {
                        std::string text((size_t)sz, '\0');
                        if (fread(text.data(), 1, (size_t)sz, f) == (size_t)sz) {
                            cJSON *cached = cJSON_Parse(text.c_str());
                            if (cached && cJSON_IsArray(cached)) {
                                /* 校验形状：每组有 hash/size/files/mtimes。 */
                                bool valid = true;
                                int ng = cJSON_GetArraySize(cached);
                                for (int i = 0; i < ng && valid; i++) {
                                    const cJSON *g = cJSON_GetArrayItem(cached, i);
                                    const cJSON *h =
                                        cJSON_GetObjectItemCaseSensitive(g, "hash");
                                    const cJSON *s =
                                        cJSON_GetObjectItemCaseSensitive(g, "size");
                                    const cJSON *fl =
                                        cJSON_GetObjectItemCaseSensitive(g, "files");
                                    const cJSON *mt =
                                        cJSON_GetObjectItemCaseSensitive(g, "mtimes");
                                    if (!h || !cJSON_IsString(h) || !s ||
                                        !cJSON_IsNumber(s) || !fl ||
                                        !cJSON_IsArray(fl) || !mt ||
                                        !cJSON_IsArray(mt) ||
                                        cJSON_GetArraySize(fl) !=
                                            cJSON_GetArraySize(mt)) {
                                        valid = false;
                                    }
                                }
                                if (valid) {
                                    scan_set_found("dup", (uint64_t)ng);
                                    scan_end("dup", "完成");
                                    fclose(f);
                                    return gui_ok_json(out_json, cached);
                                }
                            }
                            if (cached) cJSON_Delete(cached);
                        }
                    }
                    fclose(f);
                }
            }
        }
    }
    /* 实时扫描。 */
    scan_stage("dup", "枚举文件");
    ScanOpts opts;
    opts.cancel = &g_dup_cancel;
    opts.on_tick = gui_dup_tick;
    opts.on_stage = gui_dup_stage;
    opts.on_total = gui_dup_total;
    opts.trace = getenv("CDC_TRACE") != nullptr;
    opts.fast = fast;
    std::vector<const char *> root_ptrs;
    root_ptrs.reserve(roots.size());
    for (const std::string &r : roots) root_ptrs.push_back(r.c_str());
    DupGroupList out;
    int rc = scan_dups(root_ptrs, min, opts, out);
    bool cancelled = scan_cancelled("dup") || *opts.cancel;
    cJSON *arr = cJSON_CreateArray();
    if (rc == 0 && arr) {
        for (DupGroup &g : out) {
            cJSON *gj = gui_dup_group_json(g.hash_hex, g.size, g.files, g.mtimes);
            if (gj) cJSON_AddItemToArray(arr, gj);
        }
    }
    scan_set_found("dup", (uint64_t)(arr ? cJSON_GetArraySize(arr) : 0));
    scan_end("dup", "完成");
    /* 未取消且非空时写回 JSON 缓存（供下次 5 分钟内复用）。 */
    if (!cancelled && !cache_path.empty() && arr && cJSON_GetArraySize(arr) > 0) {
        char *text = cJSON_PrintUnformatted(arr);
        if (text) {
            FILE *f = fopen(cache_path.c_str(), "wb");
            if (f) {
                fwrite(text, 1, strlen(text), f);
                fclose(f);
            }
            cJSON_free(text);
        }
    }
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

int h_scan_big(const char *args_json, char **out_json, char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    std::vector<std::string> drives = gui_arg_str_array(args, "drives");
    cJSON_Delete(args);
    if (drives.empty()) {
        drives = gui_fixed_drives_list();
    }
    scan_begin("big", "大文件扫描");
    scan_stage("big", "索引同步");
    cJSON *rows = nullptr;
    if (!drives.empty()) {
        rows = gui_query_big_from_index(drives);
    }
    if (rows) {
        scan_end("big", "完成");
        return gui_ok_json(out_json, rows);
    }
    ScanOpts opts;
    opts.cancel = &g_big_cancel;
    opts.trace = getenv("CDC_TRACE") != nullptr;
    std::vector<const char *> drive_ptrs;
    drive_ptrs.reserve(drives.size());
    for (const std::string &d : drives) drive_ptrs.push_back(d.c_str());
    BigFileList out;
    int rc = scan_big(drive_ptrs, opts, out);
    cJSON *arr = cJSON_CreateArray();
    if (rc == 0 && arr) {
        for (const BigFile &bf : out) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "path", bf.path.c_str());
            cJSON_AddNumberToObject(o, "size", (double)bf.size);
            cJSON_AddNumberToObject(o, "mtime", (double)bf.mtime);
            cJSON_AddItemToArray(arr, o);
        }
    }
    scan_end("big", "完成");
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

}  // namespace
#endif /* _WIN32 */

#ifdef _WIN32
namespace {

/* ---------------- delete_recycle / delete_dups ---------------- */

int h_delete_recycle(const char *args_json, char **out_json, char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    std::vector<std::string> paths = gui_arg_str_array(args, "paths");
    cJSON_Delete(args);
    if (paths.empty()) {
        return gui_fail(out_error, "缺少参数：paths");
    }
    const std::string title = "大文件";
    std::string begin_err;
    std::vector<std::string> failed;
    std::vector<std::string> reasons;
    if (task_begin("big_delete", "大文件清理", true, true, &begin_err) != 0) {
        reasons.push_back(begin_err.empty() ? "任务启动失败" : begin_err);
        gui_delete_log(title, 0, 0, 0);
        cJSON *o = gui_delete_report_json(0, 0, failed, reasons);
        return gui_ok_json(out_json, o);
    }
    task_set_unit("big_delete", "个文件");
    task_set_total("big_delete", (uint64_t)paths.size());
    task_set_stage("big_delete", "校验文件");
    std::vector<uint64_t> sizes(paths.size(), 0);
    std::vector<size_t> eligible; /* paths 里的下标 */
    bool cancelled = false;
    for (size_t i = 0; i < paths.size(); i++) {
        if (task_cancelled("big_delete")) {
            cancelled = true;
            break;
        }
        task_add_done("big_delete", 1);
        if (!gui_path_exists(paths[i])) {
            failed.push_back(paths[i]);
            reasons.push_back(paths[i] + "：文件已不存在");
            continue;
        }
        if (explicit_delete_forbidden(paths[i])) {
            failed.push_back(paths[i]);
            reasons.push_back(paths[i] + "：受保护路径或密码库，已跳过");
            continue;
        }
        uint64_t size = 0, mtime = 0;
        gui_file_stat(paths[i], &size, &mtime);
        sizes[i] = size;
        eligible.push_back(i);
    }
    uint64_t deleted = 0, freed = 0;
    if (cancelled) {
        reasons.push_back("已取消");
        task_finish("big_delete", "cancelled", "已取消");
        gui_delete_log(title, 0, 0, failed.size());
        cJSON *o = gui_delete_report_json(0, 0, failed, reasons);
        return gui_ok_json(out_json, o);
    }
    task_set_stage("big_delete", "送入回收站");
    {
        std::vector<const char *> eligible_ptrs;
        eligible_ptrs.reserve(eligible.size());
        for (size_t i : eligible) eligible_ptrs.push_back(paths[i].c_str());
        send_to_recycle(eligible_ptrs);
    }
    for (size_t i : eligible) {
        if (gui_path_exists(paths[i])) {
            failed.push_back(paths[i]);
            reasons.push_back(paths[i] + "：送回收站失败（可能被占用）");
        } else {
            deleted += 1;
            freed += sizes[i];
        }
    }
    gui_invalidate_dup_cache();
    {
        std::string summary = format("删除 %llu 个，释放 %.1f MB",
                                     (unsigned long long)deleted,
                                     (double)freed / 1048576.0);
        task_finish("big_delete", "ok", summary);
    }
    gui_delete_log(title, freed, deleted, failed.size());
    cJSON *o = gui_delete_report_json(freed, deleted, failed, reasons);
    return gui_ok_json(out_json, o);
}

struct VerifyItem {
    std::string path;
    std::string hash;
    uint64_t size = 0;
    uint64_t mtime = 0;
    std::string keeper;
};

int h_delete_dups(const char *args_json, char **out_json, char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    const cJSON *items_json = gui_arg(args, "items");
    std::vector<VerifyItem> items;
    if (items_json && cJSON_IsArray(items_json)) {
        int n = cJSON_GetArraySize(items_json);
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(items_json, i);
            if (!it) continue;
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "path");
            const cJSON *h = cJSON_GetObjectItemCaseSensitive(it, "hash");
            const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "size");
            const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "mtime");
            const cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "keeper");
            VerifyItem item;
            item.path = (p && cJSON_IsString(p)) ? p->valuestring : "";
            item.hash = (h && cJSON_IsString(h)) ? h->valuestring : "";
            item.size = (s && cJSON_IsNumber(s)) ? (uint64_t)s->valuedouble : 0;
            item.mtime = (m && cJSON_IsNumber(m)) ? (uint64_t)m->valuedouble : 0;
            item.keeper = (k && cJSON_IsString(k)) ? k->valuestring : "";
            items.push_back(std::move(item));
        }
    }
    cJSON_Delete(args);
    if (items.empty()) {
        return gui_fail(out_error, "缺少参数：items");
    }
    const std::string title = "重复文件";
    std::vector<std::string> failed;
    std::vector<std::string> reasons;
    std::string begin_err;
    if (task_begin("dup_delete", "重复文件清理", true, true, &begin_err) != 0) {
        reasons.push_back(begin_err.empty() ? "任务启动失败" : begin_err);
        gui_delete_log(title, 0, 0, 0);
        cJSON *o = gui_delete_report_json(0, 0, failed, reasons);
        return gui_ok_json(out_json, o);
    }
    task_set_unit("dup_delete", "个文件");
    task_set_total("dup_delete", (uint64_t)items.size());
    task_set_stage("dup_delete", "校验文件");
    /* 待复核的项 + 保留文件集合（去重）。 */
    std::vector<VerifyItem> pending;
    std::vector<std::string> keepers;
    bool cancelled = false;
    for (VerifyItem &item : items) {
        if (task_cancelled("dup_delete")) {
            cancelled = true;
            break;
        }
        task_add_done("dup_delete", 1);
        const std::string &path = item.path;
        if (!gui_path_exists(path)) {
            failed.push_back(path);
            reasons.push_back(path + "：文件已不存在");
            continue;
        }
        if (explicit_delete_forbidden(path)) {
            failed.push_back(path);
            reasons.push_back(path + "：受保护路径或密码库，已跳过");
            continue;
        }
        uint64_t size = 0, mtime = 0;
        if (!gui_file_stat(path, &size, &mtime)) {
            failed.push_back(path);
            reasons.push_back(path + "：读不到文件属性");
            continue;
        }
        if (size != item.size) {
            failed.push_back(path);
            reasons.push_back(path + "：扫描后大小变了，已跳过");
            continue;
        }
        if (mtime != item.mtime) {
            failed.push_back(path);
            reasons.push_back(path + "：扫描后修改时间变了，已跳过");
            continue;
        }
        if (!item.keeper.empty()) {
            bool seen = false;
            for (const std::string &k : keepers) {
                if (k == item.keeper) {
                    seen = true;
                    break;
                }
            }
            if (!seen) keepers.push_back(item.keeper);
        }
        pending.push_back(std::move(item));
    }
    uint64_t deleted = 0, freed = 0;
    if (cancelled) {
        reasons.push_back("已取消");
        task_finish("dup_delete", "cancelled", "已取消");
        gui_delete_log(title, 0, 0, failed.size());
        cJSON *o = gui_delete_report_json(0, 0, failed, reasons);
        return gui_ok_json(out_json, o);
    }
    /* 并行按盘计算保留项与待删副本的完整哈希（C 版顺序执行，结果一致）。 */
    size_t npaths_to_hash = keepers.size() + pending.size();
    task_set_stage("dup_delete", "核对内容");
    task_set_progress("dup_delete", 0, (uint64_t)npaths_to_hash);
    /* hashed[i]：keepers[0..nkeepers) + pending[].path 的 hex，失败为空串。 */
    std::vector<std::string> hashed(npaths_to_hash);
    for (size_t i = 0; i < npaths_to_hash; i++) {
        const std::string &p = i < keepers.size() ? keepers[i] : pending[i - keepers.size()].path;
        Digest digest;
        if (full_hash_file(p.c_str(), 0, digest.bytes) == 0) {
            char hex[65];
            digest_to_hex(digest, hex);
            hashed[i] = hex;
        }
        task_add_done("dup_delete", 1);
    }
    task_set_stage("dup_delete", "复核内容");
    task_set_progress("dup_delete", 0, (uint64_t)pending.size());
    std::vector<size_t> eligible; /* pending 里的下标 */
    for (size_t i = 0; i < pending.size(); i++) {
        if (task_cancelled("dup_delete")) {
            cancelled = true;
            break;
        }
        task_add_done("dup_delete", 1);
        const std::string &path = pending[i].path;
        std::string expected;
        bool have_expected = false;
        if (!pending[i].keeper.empty()) {
            for (size_t k = 0; k < keepers.size(); k++) {
                if (keepers[k] == pending[i].keeper) {
                    expected = hashed[k];
                    have_expected = !expected.empty();
                    break;
                }
            }
            if (!have_expected) {
                failed.push_back(path);
                reasons.push_back(path + "：保留的文件读不到，已跳过");
                continue;
            }
        } else {
            expected = pending[i].hash;
            have_expected = true;
        }
        const std::string &actual = hashed[keepers.size() + i];
        if (!actual.empty() && have_expected && actual == expected) {
            eligible.push_back(i);
        } else {
            failed.push_back(path);
            reasons.push_back(path + "：内容与保留文件不一致，已跳过");
        }
    }
    if (cancelled) {
        reasons.push_back("已取消");
        task_finish("dup_delete", "cancelled", "已取消");
        gui_delete_log(title, 0, 0, failed.size());
        cJSON *o = gui_delete_report_json(0, 0, failed, reasons);
        return gui_ok_json(out_json, o);
    }
    task_set_stage("dup_delete", "送入回收站");
    {
        std::vector<const char *> eligible_ptrs;
        eligible_ptrs.reserve(eligible.size());
        for (size_t i : eligible) eligible_ptrs.push_back(pending[i].path.c_str());
        send_to_recycle(eligible_ptrs);
    }
    for (size_t i : eligible) {
        if (gui_path_exists(pending[i].path)) {
            failed.push_back(pending[i].path);
            reasons.push_back(pending[i].path + "：送回收站失败（可能被占用）");
        } else {
            deleted += 1;
            freed += pending[i].size;
        }
    }
    gui_invalidate_dup_cache();
    {
        std::string summary = format("删除 %llu 个，释放 %.1f MB",
                                     (unsigned long long)deleted,
                                     (double)freed / 1048576.0);
        task_finish("dup_delete", "ok", summary);
    }
    gui_delete_log(title, freed, deleted, failed.size());
    cJSON *o = gui_delete_report_json(freed, deleted, failed, reasons);
    return gui_ok_json(out_json, o);
}

/* ---------------- 其余简单命令 ---------------- */

void gui_scan_mark_cancelled(std::string_view kind) {
    GUI_LOCK();
    ScanCell *c = gui_scan_cell(kind);
    c->cancelled = true;
    GUI_UNLOCK();
    *gui_scan_cancel_flag(kind) = 1;
}

int h_disk_info(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    Error err;
    uint64_t free_bytes = 0, total = 0;
    if (drive_free_space("C:\\", &free_bytes, &total, &err) != kOk) {
        return gui_fail(out_error, "读取磁盘信息失败");
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return gui_fail(out_error, "内存不足");
    cJSON_AddNumberToObject(o, "total", (double)total);
    cJSON_AddNumberToObject(o, "used", (double)(total - free_bytes));
    cJSON_AddNumberToObject(o, "free", (double)free_bytes);
    return gui_ok_json(out_json, o);
}

int h_scan_status(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    std::string s = scan_status_json();
    if (s.empty()) return gui_fail(out_error, "内存不足");
    *out_json = gui_strdup(s);
    return 0;
}

int h_cancel_scan(const char *args_json, char **out_json, char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    cJSON_Delete(args);
    if (!kind || kind[0] == '\0') {
        gui_scan_mark_cancelled("dup");
        gui_scan_mark_cancelled("big");
    } else if (strcmp(kind, "dup") == 0) {
        gui_scan_mark_cancelled("dup");
    } else if (strcmp(kind, "big") == 0) {
        gui_scan_mark_cancelled("big");
    }
    return gui_ok_null(out_json);
}

int h_task_status(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    std::string s = task_status_json();
    if (s.empty()) return gui_fail(out_error, "内存不足");
    *out_json = gui_strdup(s);
    return 0;
}

int h_cancel_task(const char *args_json, char **out_json, char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    const char *id = gui_arg_str(args, "id");
    cJSON_Delete(args);
    std::optional<std::string_view> oid;
    if (id) oid = id;
    return gui_ok_bool(out_json, task_cancel(oid));
}

int h_list_drives(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    std::vector<std::string> drives = gui_fixed_drives_list();
    cJSON *arr = gui_str_array_json(drives);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

int h_user_profile(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        auto utf8 = wide_to_utf8(buf);
        if (utf8) {
            return gui_ok_string(out_json, *utf8);
        }
    }
    return gui_ok_string(out_json, "C:\\");
}

/* ---------------- 索引命令 ---------------- */

int h_index_status(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    std::vector<std::string> drives = gui_fixed_drives_list();
    Error err;
    std::unique_ptr<FileIndex> idx_owner = FileIndex::open_default(&err);
    FileIndex *idx = idx_owner.get();
    cJSON *volumes = cJSON_CreateArray();
    if (!volumes) {
        return gui_fail(out_error, "内存不足");
    }
    for (const std::string &drive : drives) {
        cJSON *v = cJSON_CreateObject();
        if (!v) continue;
        cJSON_AddStringToObject(v, "drive", drive.c_str());
        uint64_t files = 0, with_full = 0, updated_at = 0;
        std::string state = "missing";
        if (idx) {
            uint64_t serial = 0;
            index_freshness(idx, drive, &serial, &state);
            if (serial != 0) {
                IndexCounts c = idx->volume_counts(serial);
                files = c.total;
                with_full = c.with_full;
                updated_at = idx->usn_state_updated_at(serial).value_or(0);
            }
        }
        cJSON_AddStringToObject(v, "state", state.c_str());
        cJSON_AddNumberToObject(v, "files", (double)files);
        cJSON_AddNumberToObject(v, "with_full", (double)with_full);
        cJSON_AddNumberToObject(v, "updated_at", (double)updated_at);
        cJSON_AddItemToArray(volumes, v);
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cJSON_Delete(volumes);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddItemToObject(o, "volumes", volumes);
    return gui_ok_json(out_json, o);
}

int h_index_sync_now(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    if (!gui_is_admin()) {
        return gui_ok_string(out_json, "--index 需要管理员权限。");
    }
    std::string summary;
    run_index_summary(true, false, &summary);
    return gui_ok_string(out_json, summary);
}

int h_index_rebuild(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    if (!gui_is_admin()) {
        return gui_ok_string(out_json, "--index 需要管理员权限。");
    }
    std::string summary;
    run_index_summary(true, true, &summary);
    return gui_ok_string(out_json, summary);
}

int h_index_task_status(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    (void)out_error;
    TaskStatus st = task_status();
    cJSON *o = cJSON_CreateObject();
    if (!o) return gui_fail(out_error, "内存不足");
    cJSON_AddBoolToObject(o, "exists", st.exists);
    cJSON_AddBoolToObject(o, "enabled", st.enabled);
    return gui_ok_json(out_json, o);
}

/* 计划任务命令统一返回 msg 字符串（成功与失败都是字符串，与 Rust 一致）。 */
int gui_indextask_msg(char **out_json, char **out_error, int rc, std::string msg,
                      const Error &err) {
    (void)out_error;
    if (rc != 0 && msg.empty() && err.message[0]) {
        msg = err.message;
    }
    return gui_ok_string(out_json, msg);
}

int h_index_task_run(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    std::string msg;
    Error err;
    int rc = run_task(&msg, &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, err);
}

int h_index_task_set_enabled(const char *args_json, char **out_json,
                             char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    bool enabled = gui_arg_bool(args, "enabled", false);
    cJSON_Delete(args);
    std::string msg;
    Error err;
    int rc = set_task_enabled(enabled, &msg, &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, err);
}

int h_index_task_register(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    std::string msg;
    Error err;
    int rc = register_task(&msg, &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, err);
}

int h_index_task_delete(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    std::string msg;
    Error err;
    int rc = delete_task(&msg, &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, err);
}

}  // namespace
#endif /* _WIN32 */

#ifdef _WIN32
namespace {

/* ---------------- 系统 / 碎片 / 电源 ---------------- */

int h_system_scan(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    std::string begin_err;
    task_begin("system_scan", "系统清理扫描", false, false, &begin_err);
    Error err;
    std::vector<SystemItem> items;
    if (scan_system_deep(&items, &err) != kOk) {
        task_finish("system_scan", "failed",
                    err.message[0] ? err.message : "系统扫描失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "系统扫描失败");
    }
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (const SystemItem &it : items) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "key", it.key.c_str());
            cJSON_AddStringToObject(o, "name", it.name.c_str());
            cJSON_AddStringToObject(o, "describe", it.describe.c_str());
            cJSON_AddNumberToObject(o, "size", (double)it.size);
            cJSON_AddNumberToObject(o, "count", (double)it.count);
            cJSON_AddBoolToObject(o, "safe_to_clean", it.safe_to_clean);
            cJSON_AddBoolToObject(o, "is_command", it.is_command);
            cJSON_AddItemToArray(arr, o);
        }
    }
    task_finish("system_scan", "ok", "系统扫描完成");
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

int h_system_clean(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *key = gui_arg_str(args, "key");
    std::string key_buf = key ? key : "";
    cJSON_Delete(args);
    if (key_buf.empty()) return gui_fail(out_error, "缺少参数：key");
    bool cancellable = key_buf == "sys_update_dl" || key_buf == "sys_crash_dumps";
    std::string begin_err;
    if (task_begin("system", "系统清理", cancellable, true, &begin_err) != 0) {
        std::string msg = begin_err.empty() ? "任务启动失败" : begin_err;
        return gui_fail(out_error, "%s", msg.c_str());
    }
    Error err;
    uint64_t freed = 0, count = 0;
    std::string msg;
    if (clean_system_item(key_buf, &freed, &count, &msg, &err) != kOk) {
        task_finish("system", "failed", err.message[0] ? err.message : "系统清理失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "系统清理失败");
    }
    task_finish("system", "ok", msg.empty() ? "系统清理完成" : msg);
    return gui_ok_string(out_json, msg);
}

int h_hiber_query(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    Error err;
    HiberInfo info;
    if (query_hiber(&info, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "读取休眠状态失败");
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddBoolToObject(o, "enabled", info.enabled);
    cJSON_AddNumberToObject(o, "size", (double)info.size);
    cJSON_AddStringToObject(o, "path", info.path.c_str());
    cJSON_AddStringToObject(o, "description", info.description.c_str());
    return gui_ok_json(out_json, o);
}

int h_hiber_set(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *mode = gui_arg_str(args, "mode");
    std::string mode_buf = mode ? mode : "";
    cJSON_Delete(args);
    if (mode_buf.empty()) return gui_fail(out_error, "缺少参数：mode");
    Error err;
    std::string msg;
    if (set_hiber(mode_buf, &msg, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "设置休眠失败");
    }
    return gui_ok_string(out_json, msg);
}

int h_recycle_empty(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    std::string begin_err;
    if (task_begin("recycle", "清空回收站", false, true, &begin_err) != 0) {
        std::string msg = begin_err.empty() ? "任务启动失败" : begin_err;
        return gui_fail(out_error, "%s", msg.c_str());
    }
    Error err;
    if (empty_all_recycle_bins(&err) != kOk) {
        task_finish("recycle", "failed", err.message[0] ? err.message : "清空回收站失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "清空回收站失败");
    }
    task_finish("recycle", "ok", "回收站已清空");
    return gui_ok_null(out_json);
}

/* ---------------- 微信 / QQ ---------------- */

int h_im_roots(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    std::string kind_buf = kind ? kind : "";
    cJSON_Delete(args);
    if (kind_buf.empty()) return gui_fail(out_error, "缺少参数：kind");
    Error err;
    std::vector<std::string> roots;
    if (im_roots(kind_buf, &roots, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "读取微信/QQ 目录失败");
    }
    cJSON *arr = gui_str_array_json(roots);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

cJSON *gui_im_record_json(const ImRecord &r) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return nullptr;
    cJSON_AddStringToObject(o, "path", r.path.c_str());
    cJSON_AddNumberToObject(o, "size", (double)r.size);
    cJSON_AddNumberToObject(o, "mtime", (double)r.mtime);
    cJSON_AddStringToObject(o, "kind", r.kind.c_str());
    cJSON_AddStringToObject(o, "sub", r.sub.c_str());
    cJSON_AddStringToObject(o, "month", r.month.c_str());
    cJSON_AddStringToObject(o, "name", r.name.c_str());
    return o;
}

int h_im_scan(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    std::string kind_buf = kind ? kind : "";
    cJSON_Delete(args);
    if (kind_buf.empty()) return gui_fail(out_error, "缺少参数：kind");
    std::string begin_err;
    task_begin("im_scan", "扫描微信/QQ 数据", false, false, &begin_err);
    Error err;
    std::vector<std::string> roots;
    if (im_roots(kind_buf, &roots, &err) != kOk) {
        task_finish("im_scan", "failed", err.message[0] ? err.message : "读取目录失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "读取目录失败");
    }
    std::vector<ImRecord> records;
    if (im_list_media(roots, &records, &err) != kOk) {
        task_finish("im_scan", "failed", err.message[0] ? err.message : "扫描失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "扫描失败");
    }
    im_remember_scanned(records);
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (const ImRecord &r : records) {
            cJSON *o = gui_im_record_json(r);
            if (o) cJSON_AddItemToArray(arr, o);
        }
    }
    task_finish("im_scan", "ok", "扫描完成");
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

int h_im_preview(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *path = gui_arg_str(args, "path");
    std::string path_copy = path ? path : "";
    cJSON_Delete(args);
    if (path_copy.empty()) return gui_fail(out_error, "缺少参数：path");
    if (!im_is_scanned(path_copy)) {
        return gui_fail(out_error, "文件不在扫描结果里");
    }
    Error err;
    ImPreview pv;
    if (im_preview(path_copy, &pv, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "预览失败");
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddStringToObject(o, "text", pv.text.c_str());
    cJSON_AddBoolToObject(o, "binary", pv.binary);
    cJSON_AddBoolToObject(o, "truncated", pv.truncated);
    cJSON_AddNumberToObject(o, "size", (double)pv.size);
    return gui_ok_json(out_json, o);
}

int h_im_clean(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    std::string kind_buf = kind ? kind : "";
    std::vector<std::string> kinds = gui_arg_str_array(args, "kinds");
    std::vector<std::string> paths = gui_arg_str_array(args, "paths");
    bool has_paths = gui_arg(args, "paths") != nullptr;
    cJSON_Delete(args);
    if (kind_buf.empty()) {
        return gui_fail(out_error, "缺少参数：kind");
    }
    std::string begin_err;
    if (task_begin("im_clean", "微信/QQ 清理", true, true, &begin_err) != 0) {
        std::string msg = begin_err.empty() ? "任务启动失败" : begin_err;
        return gui_fail(out_error, "%s", msg.c_str());
    }
    Error err;
    DeleteReport report;
    if (im_clean(kind_buf, kinds, paths, has_paths, &report, &err) != kOk) {
        task_finish("im_clean", "failed", err.message[0] ? err.message : "微信/QQ 清理失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "微信/QQ 清理失败");
    }
    {
        std::string summary = format("删除 %llu 个，释放 %.1f MB",
                                     (unsigned long long)report.deleted,
                                     (double)report.freed / 1048576.0);
        task_finish("im_clean", "ok", summary);
    }
    cJSON *o = gui_delete_report_json(report.freed, report.deleted, report.failed,
                                      report.reasons);
    if (!o) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, o);
}

int h_im_move_analyze(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    std::string kind_buf = kind ? kind : "";
    cJSON_Delete(args);
    if (kind_buf.empty()) return gui_fail(out_error, "缺少参数：kind");
    std::string begin_err;
    task_begin("im_move_analyze", "分析搬家数据", false, false, &begin_err);
    Error err;
    MoveAnalysis a;
    if (analyze_im_migration(kind_buf, &a, &err) != kOk) {
        task_finish("im_move_analyze", "failed", err.message[0] ? err.message : "分析搬家失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "分析搬家失败");
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddStringToObject(o, "source", a.source.c_str());
    cJSON_AddBoolToObject(o, "exists", a.exists);
    cJSON_AddNumberToObject(o, "size", (double)a.size);
    cJSON_AddNumberToObject(o, "file_count", (double)a.file_count);
    cJSON_AddBoolToObject(o, "is_junction", a.is_junction);
    cJSON *targets = cJSON_CreateArray();
    if (targets) {
        for (const DriveTarget &t : a.targets) {
            cJSON *tt = cJSON_CreateObject();
            if (!tt) continue;
            cJSON_AddStringToObject(tt, "letter", t.letter.c_str());
            cJSON_AddNumberToObject(tt, "free_bytes", (double)t.free_bytes);
            cJSON_AddNumberToObject(tt, "total_bytes", (double)t.total_bytes);
            cJSON_AddItemToArray(targets, tt);
        }
    }
    cJSON_AddItemToObject(o, "target_drives", targets);
    task_finish("im_move_analyze", "ok", "分析完成");
    return gui_ok_json(out_json, o);
}

int h_im_move_execute(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *source = gui_arg_str(args, "source");
    /* 前端发 targetDir，Rust 签名是 target_dir：两个名字都认。 */
    const char *target_dir = gui_arg_str2(args, "target_dir", "targetDir");
    const char *kind = gui_arg_str(args, "kind");
    std::string source_buf = source ? source : "";
    std::string target_buf = target_dir ? target_dir : "";
    std::string kind_buf = kind ? kind : "";
    cJSON_Delete(args);
    if (source_buf.empty()) return gui_fail(out_error, "缺少参数：source");
    if (target_buf.empty()) return gui_fail(out_error, "缺少参数：target_dir");
    if (kind_buf.empty()) return gui_fail(out_error, "缺少参数：kind");
    std::string begin_err;
    if (task_begin("im_move", "微信/QQ 数据搬家", true, true, &begin_err) != 0) {
        std::string msg = begin_err.empty() ? "任务启动失败" : begin_err;
        return gui_fail(out_error, "%s", msg.c_str());
    }
    Error err;
    std::string msg;
    if (execute_im_migration(source_buf, target_buf, kind_buf, &msg, &err) != kOk) {
        task_finish("im_move", "failed", err.message[0] ? err.message : "搬家失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "搬家失败");
    }
    task_finish("im_move", "ok", msg.empty() ? "搬家完成" : msg);
    return gui_ok_string(out_json, msg);
}

/* ---------------- 隐私 / 粉碎 / 历史 / 规则 / 路径 / 进程 ---------------- */

int h_privacy_scan(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    std::string begin_err;
    task_begin("privacy_scan", "隐私清理扫描", false, false, &begin_err);
    std::vector<PrivacyItem> items = privacy_scan();
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (const PrivacyItem &it : items) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "id", it.id.c_str());
            cJSON_AddStringToObject(o, "section", it.section.c_str());
            cJSON_AddStringToObject(o, "name", it.name.c_str());
            cJSON_AddNumberToObject(o, "count", (double)it.count);
            cJSON_AddBoolToObject(o, "count_available", it.count_available);
            cJSON_AddItemToObject(o, "files", gui_str_array_json(it.files));
            cJSON_AddStringToObject(o, "action", it.action.c_str());
            cJSON_AddStringToObject(o, "note", it.note.c_str());
            cJSON_AddItemToArray(arr, o);
        }
    }
    task_finish("privacy_scan", "ok", "隐私扫描完成");
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

int h_privacy_clean(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    std::vector<std::string> ids = gui_arg_str_array(args, "ids");
    cJSON_Delete(args);
    if (ids.empty()) {
        return gui_fail(out_error, "缺少参数：ids");
    }
    std::string begin_err;
    if (task_begin("privacy", "隐私清理", true, true, &begin_err) != 0) {
        std::string msg = begin_err.empty() ? "任务启动失败" : begin_err;
        return gui_fail(out_error, "%s", msg.c_str());
    }
    std::vector<const char *> id_ptrs;
    id_ptrs.reserve(ids.size());
    for (const std::string &id : ids) id_ptrs.push_back(id.c_str());
    PrivacyReport report = privacy_clean(id_ptrs);
    {
        std::string summary = format("清理 %lld 个文件", (long long)report.files);
        task_finish("privacy", "ok", summary);
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return gui_fail(out_error, "内存不足");
    cJSON_AddNumberToObject(o, "files", (double)report.files);
    cJSON_AddNumberToObject(o, "sql", (double)report.sql);
    cJSON_AddNumberToObject(o, "registry", (double)report.registry);
    cJSON_AddItemToObject(o, "failed", gui_str_array_json(report.failed));
    return gui_ok_json(out_json, o);
}

int h_shred(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    std::vector<std::string> paths = gui_arg_str_array(args, "paths");
    uint64_t passes = gui_arg_u64(args, "passes", 1);
    cJSON_Delete(args);
    if (paths.empty()) {
        return gui_fail(out_error, "缺少参数：paths");
    }
    if (passes < 1) passes = 1;
    if (passes > 3) passes = 3;
    std::string begin_err;
    if (task_begin("shred", "文件粉碎", true, true, &begin_err) != 0) {
        /* Rust 的 shred：begin 失败时返回带 errors 的空报告，不是 Err。 */
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            return gui_fail(out_error, "内存不足");
        }
        cJSON_AddNumberToObject(o, "freed", 0);
        cJSON_AddNumberToObject(o, "files_ok", 0);
        cJSON_AddNumberToObject(o, "files_failed", 0);
        cJSON_AddNumberToObject(o, "finished", 0);
        cJSON *errs = cJSON_CreateArray();
        if (errs) {
            cJSON_AddItemToArray(
                errs, cJSON_CreateString(begin_err.empty() ? "任务启动失败"
                                                          : begin_err.c_str()));
        }
        cJSON_AddItemToObject(o, "errors", errs);
        return gui_ok_json(out_json, o);
    }
    task_set_unit("shred", "个文件");
    task_set_stage("shred", "粉碎文件");
    Error err;
    ShredReport report;
    if (shred_paths(paths, (uint32_t)passes, &report, &err) != kOk) {
        task_finish("shred", "failed", err.message[0] ? err.message : "粉碎失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "粉碎失败");
    }
    {
        std::string summary = format("成功 %llu 个，失败 %llu 个，释放 %.1f MB",
                                     (unsigned long long)report.files_ok,
                                     (unsigned long long)report.files_failed,
                                     (double)report.freed / 1048576.0);
        task_finish("shred", task_cancelled("shred") ? "cancelled" : "ok", summary);
    }
    {
        std::string stamp = human_stamp(now_seconds());
        std::string line = format("%s\tok\t成功 %llu 个，失败 %llu 个",
                                  stamp.c_str(),
                                  (unsigned long long)report.files_ok,
                                  (unsigned long long)report.files_failed);
        append_line("shred_history.log", line);
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddNumberToObject(o, "freed", (double)report.freed);
    cJSON_AddNumberToObject(o, "files_ok", (double)report.files_ok);
    cJSON_AddNumberToObject(o, "files_failed", (double)report.files_failed);
    cJSON_AddNumberToObject(o, "finished", (double)report.finished);
    cJSON_AddItemToObject(o, "errors", gui_str_array_json(report.errors));
    return gui_ok_json(out_json, o);
}

int h_history_list(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    Error err;
    std::vector<LogEntry> entries;
    if (logs_list(&entries, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "读取日志列表失败");
    }
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (const LogEntry &e : entries) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "name", e.name.c_str());
            cJSON_AddNumberToObject(o, "size", (double)e.size);
            cJSON_AddNumberToObject(o, "mtime", (double)e.mtime);
            cJSON_AddItemToArray(arr, o);
        }
    }
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

int h_history_read(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *name = gui_arg_str(args, "name");
    std::string name_buf = name ? name : "";
    cJSON_Delete(args);
    if (name_buf.empty()) return gui_fail(out_error, "缺少参数：name");
    Error err;
    std::string text;
    if (log_read(name_buf, &text, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "读取日志失败");
    }
    return gui_ok_string(out_json, text);
}

int h_rules_update(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    std::string begin_err;
    if (task_begin("rules", "更新清理规则", false, false, &begin_err) != 0) {
        std::string msg = begin_err.empty() ? "任务启动失败" : begin_err;
        return gui_fail(out_error, "%s", msg.c_str());
    }
    Error err;
    RulesUpdateResult result;
    if (update_rules(&result, &err) != kOk) {
        task_finish("rules", "failed", err.message[0] ? err.message : "更新规则失败");
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "更新规则失败");
    }
    std::string buf = format("新增 %llu 条，共 %llu 条。%s\n下次扫描生效。",
                             (unsigned long long)result.accepted,
                             (unsigned long long)result.total,
                             result.message.c_str());
    task_finish("rules", "ok", buf);
    return gui_ok_string(out_json, buf);
}

int h_rules_rollback(const char *args_json, char **out_json, char **out_error) {
    (void)args_json;
    Error err;
    std::string msg;
    if (rollback_rules(&msg, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "回滚规则失败");
    }
    return gui_ok_string(out_json, msg);
}

int h_reveal_path(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *path = gui_arg_str(args, "path");
    std::string path_copy = path ? path : "";
    cJSON_Delete(args);
    if (path_copy.empty()) return gui_fail(out_error, "缺少参数：path");
    Error err;
    if (reveal_in_explorer(path_copy, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "定位失败");
    }
    return gui_ok_null(out_json);
}

int h_open_path(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *path = gui_arg_str(args, "path");
    std::string path_copy = path ? path : "";
    cJSON_Delete(args);
    if (path_copy.empty()) return gui_fail(out_error, "缺少参数：path");
    Error err;
    if (open_path(path_copy, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "打开失败");
    }
    return gui_ok_null(out_json);
}

int h_check_processes(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    std::vector<std::string> targets = gui_arg_str_array(args, "targets");
    cJSON_Delete(args);
    if (targets.empty()) {
        return gui_fail(out_error, "缺少参数：targets");
    }
    Error err;
    std::vector<std::string> running;
    if (check_running(targets, &running, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "检测进程失败");
    }
    cJSON *arr = gui_str_array_json(running);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

int h_kill_process(const char *args_json, char **out_json, char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *name = gui_arg_str(args, "name");
    std::string name_buf = name ? name : "";
    cJSON_Delete(args);
    if (name_buf.empty()) return gui_fail(out_error, "缺少参数：name");
    Error err;
    size_t killed = 0;
    if (kill_process_by_name(name_buf, &killed, &err) != kOk) {
        return gui_fail(out_error, "%s", err.message[0] ? err.message : "结束进程失败");
    }
    return gui_ok_u64(out_json, (uint64_t)killed);
}

/* ---------------- 进度回调（注册给各后端模块） ---------------- */

void gui_hook_progress(void *ctx, const char *task, uint64_t done, uint64_t total) {
    (void)ctx;
    task_set_progress(task, done, total);
}

void gui_hook_stage(void *ctx, const char *task, const char *stage) {
    (void)ctx;
    task_set_stage(task, stage);
}

void gui_hook_unit(void *ctx, const char *task, const char *unit) {
    (void)ctx;
    task_set_unit(task, unit);
}

int gui_hook_cancel(void *ctx, const char *task) {
    (void)ctx;
    return task_cancelled(task) ? 1 : 0;
}

void gui_hook_invalidate(void *ctx) {
    (void)ctx;
    gui_invalidate_dup_cache();
}

/* ---------------- 命令表 ---------------- */

struct GuiCommand {
    const char *name;
    cdc_command_handler handler;
};

const GuiCommand kGuiCommands[] = {
    {"disk_info", h_disk_info},
    {"scan_status", h_scan_status},
    {"cancel_scan", h_cancel_scan},
    {"task_status", h_task_status},
    {"cancel_task", h_cancel_task},
    {"scan_categories", h_scan_categories},
    {"scan_big", h_scan_big},
    {"scan_dups", h_scan_dups},
    {"index_status", h_index_status},
    {"index_sync_now", h_index_sync_now},
    {"index_rebuild", h_index_rebuild},
    {"index_task_status", h_index_task_status},
    {"index_task_run", h_index_task_run},
    {"index_task_set_enabled", h_index_task_set_enabled},
    {"index_task_register", h_index_task_register},
    {"index_task_delete", h_index_task_delete},
    {"user_profile", h_user_profile},
    {"delete_recycle", h_delete_recycle},
    {"delete_dups", h_delete_dups},
    {"clean_categories", h_clean_categories},
    {"list_drives", h_list_drives},
    {"frag_analyze", h_frag_analyze},
    {"frag_optimize", h_frag_optimize},
    {"im_roots", h_im_roots},
    {"im_scan", h_im_scan},
    {"im_preview", h_im_preview},
    {"im_clean", h_im_clean},
    {"shred", h_shred},
    {"history_list", h_history_list},
    {"history_read", h_history_read},
    {"rules_update", h_rules_update},
    {"rules_rollback", h_rules_rollback},
    {"privacy_scan", h_privacy_scan},
    {"privacy_clean", h_privacy_clean},
    {"reveal_path", h_reveal_path},
    {"open_path", h_open_path},
    {"check_processes", h_check_processes},
    {"kill_process", h_kill_process},
    {"system_scan", h_system_scan},
    {"system_clean", h_system_clean},
    {"hiber_query", h_hiber_query},
    {"hiber_set", h_hiber_set},
    {"recycle_empty", h_recycle_empty},
    {"im_move_analyze", h_im_move_analyze},
    {"im_move_execute", h_im_move_execute},
};

}  // namespace

int gui_register_commands() {
    /* 进度/取消回调：各模块注册时做值拷贝，栈上结构体即可。 */
    cdc_hooks_t hooks{};
    hooks.progress = gui_hook_progress;
    hooks.stage = gui_hook_stage;
    hooks.unit = gui_hook_unit;
    hooks.cancel = gui_hook_cancel;
    hooks.invalidate_dup_cache = gui_hook_invalidate;
    hooks.ctx = nullptr;
    shred_set_hooks(&hooks);
    privacy_set_hooks(&hooks);
    system_set_hooks(&hooks);
    im_set_hooks(&hooks);
    size_t n = sizeof(kGuiCommands) / sizeof(kGuiCommands[0]);
    for (size_t i = 0; i < n; i++) {
        if (cdc_bridge_register(kGuiCommands[i].name, kGuiCommands[i].handler) != 0) {
            return -1;
        }
    }
    return 0;
}

#endif /* _WIN32 */

/* ---------------- Linux 下的空操作 ---------------- */

#ifndef _WIN32
int gui_register_commands() {
    return 0;
}
#endif

}  // namespace cdc

/* C 入口：main.cpp 的 GUI 分支用。 */
int cdc_gui_register_commands(void) { return cdc::gui_register_commands(); }
