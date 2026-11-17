/* gui.c — GUI 入口与 45 个后端命令的适配层。
 *
 * 对照 Rust desktop/src/main.rs 的 invoke_handler（45 个命令，
 * 命令表见 docs/bridge-commands.md；前端发什么见 web/src/api/bridge.ts）。
 * 每个命令一个适配函数：cJSON 解析 args_json → 调 src/ 下的后端函数 →
 * 拼 reply JSON（成功填 *out_json，失败填 *out_error，均为 malloc 分配，
 * 与 host/bridge.h 的 handler 签名一致）。
 *
 * 与 Rust 版有意的差别（其余语义逐行对齐，见各函数注释）：
 *  - 任务坞（task.rs）与扫描状态（scan_begin/end）由本文件实现；
 *    互斥规则与 30 秒保留期与 Rust 一致。
 *  - 安全删除没有 Pending（需重启删除）结果：C 的 safety.c 只有
 *    Deleted / Skipped / Failed 三态，clean_categories 的 pending 恒为 0。
 *  - scan_dups/scan_big 的索引优先 + 5 分钟 JSON 缓存在这里实现
 *    （scan.c 只做实时扫描）；索引增量追赶复用 main.c 的编排。
 *  - 前端发来的参数名有两个与 Rust 签名不一致（前端 bug，
 *    Rust 版会静默忽略/报错）：scan_dups 的 minSize（Rust 要 min_size）、
 *    im_move_execute 的 targetDir（Rust 要 target_dir）。这里两个名字都认，
 *    以前端实际发的为准。
 *  - imthumb 缩略图协议与 pickPaths 原生对话框是 host 侧的缺口
 *    （见 docs/frontend-bridge.md），与本文件无关。
 *
 * Windows-only 的部分全部包在 #ifdef _WIN32 里；Linux 下本文件只编
 * 纯函数（frag 解析、缓存键、任务坞等，见 tests/test_gui.c）。
 */
#include "gui.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "blake3.h"
#include "logs.h"   /* cdc_write_log / cdc_append_line / cdc_cache_dir */
#include "safety.h" /* cdc_walk_dir（缓存失效用）/ cdc_glob_match */
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h> /* IsUserAnAdmin */

#include "bigfile.h"
#include "bridge.h"
#include "cdc_process.h"
#include "fastio.h" /* cdc_is_ssd */
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

/* ---------------- 小工具 ---------------- */

static char *gui_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *gui_strndup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ASCII 小写（就地；UTF-8 多字节字节原样保留，与 Rust 的 to_lowercase
 * 在 ASCII 标签上的效果一致）。 */
static void gui_ascii_lower(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

/* epoch 毫秒（Rust task.rs 的 now_ms）。 */
static uint64_t gui_now_ms(void) {
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
static SRWLOCK g_gui_mu = SRWLOCK_INIT;
#define GUI_LOCK() AcquireSRWLockExclusive(&g_gui_mu)
#define GUI_UNLOCK() ReleaseSRWLockExclusive(&g_gui_mu)
#else
static pthread_mutex_t g_gui_mu = PTHREAD_MUTEX_INITIALIZER;
#define GUI_LOCK() pthread_mutex_lock(&g_gui_mu)
#define GUI_UNLOCK() pthread_mutex_unlock(&g_gui_mu)
#endif

/* ---------------- defrag 输出解析（纯函数，移植自 frag.rs） ---------------- */

/* UTF-8 解一个码点，返回字节数（非法字节按 1 字节计）。 */
static size_t gui_utf8_next(const char *s, size_t len, uint32_t *cp) {
    if (len == 0) return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && len >= 2) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((uint32_t)(c & 0x0F) << 12) |
              ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((uint32_t)(c & 0x07) << 18) |
              ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

/* 前 count 个字符的子串（不足则全文）；返回字节长度。 */
static size_t gui_window_chars_len(const char *s, size_t count) {
    size_t len = strlen(s);
    size_t off = 0;
    uint32_t cp = 0;
    while (off < len && count > 0) {
        size_t adv = gui_utf8_next(s + off, len - off, &cp);
        if (adv == 0) break;
        off += adv;
        count--;
    }
    return off;
}

/* ASCII 大小写不敏感的子串查找；返回标签之后的位置，找不到返回 NULL。 */
static const char *gui_find_after_ci(const char *text, const char *label) {
    size_t n = strlen(text);
    size_t m = strlen(label);
    if (m == 0 || m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        for (; j < m; j++) {
            unsigned char a = (unsigned char)text[i + j];
            unsigned char b = (unsigned char)label[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == m) return text + i + m;
    }
    return NULL;
}

/* 在 text 里找第一个数（数字开头，可含 . 与 ,）；返回 1 并填值与结束字节偏移。 */
static int gui_next_number(const char *text, double *out_value, size_t *out_end) {
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        if (text[i] >= '0' && text[i] <= '9') {
            size_t start = i;
            size_t end = i + 1;
            while (end < len &&
                   ((text[end] >= '0' && text[end] <= '9') || text[end] == '.' ||
                    text[end] == ',')) {
                end++;
            }
            /* 去掉千分位逗号再转 double（就地过滤，dst 恒不超前 src）。 */
            char *raw = gui_strndup(text + start, end - start);
            if (!raw) return 0;
            {
                char *dst = raw;
                for (const char *src = raw; *src; src++) {
                    if (*src != ',') *dst++ = *src;
                }
                *dst = '\0';
            }
            char *ep = NULL;
            double v = strtod(raw, &ep);
            free(raw);
            if (ep && ep != raw) {
                *out_value = v;
                *out_end = end;
                return 1;
            }
            i = end;
            continue;
        }
        i++;
    }
    return 0;
}

/* 在 labels 里任一标签之后 60 个字符内找数字，且数字后紧跟 unit（大小写不敏感）。 */
static int gui_number_with_unit(const char *text, const char *const *labels,
                                size_t n_labels, const char *unit, double *out) {
    for (size_t li = 0; li < n_labels; li++) {
        const char *after = gui_find_after_ci(text, labels[li]);
        if (!after) continue;
        size_t win = gui_window_chars_len(after, 60);
        char *window = gui_strndup(after, win);
        if (!window) return 0;
        double value = 0;
        size_t end = 0;
        int ok = gui_next_number(window, &value, &end);
        if (ok) {
            const char *rest = window + end;
            while (*rest == ' ' || *rest == '\t') rest++;
            size_t ulen = strlen(unit);
            int match = 1;
            for (size_t k = 0; k < ulen; k++) {
                unsigned char a = (unsigned char)rest[k];
                unsigned char b = (unsigned char)unit[k];
                if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
                if (a != b) {
                    match = 0;
                    break;
                }
            }
            free(window);
            if (match) {
                *out = value;
                return 1;
            }
            return 0;
        }
        free(window);
        return 0;
    }
    return 0;
}

static int gui_frag_search_percent(const char *text, double *out) {
    static const char *const labels[] = {"碎片率", "碎片总计", "文件碎片百分比",
                                         "Percent file fragmentation"};
    if (gui_number_with_unit(text, labels, 4, "%", out)) return 1;
    static const char *const labels2[] = {"fragmented", "碎片"};
    if (gui_number_with_unit(text, labels2, 2, "%", out)) return 1;
    /* 旧版输出：第一个「xxx = 12%」通常就是碎片率，跳过可用空间那一行。 */
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        char *line = gui_strndup(p, linelen);
        if (!line) return 0;
        if (strchr(line, '=') && strchr(line, '%')) {
            char *lower = gui_strdup(line);
            int skip = 0;
            if (lower) {
                gui_ascii_lower(lower);
                if (strstr(line, "可用") || strstr(lower, "free")) skip = 1;
                free(lower);
            }
            if (!skip) {
                double v = 0;
                size_t end = 0;
                if (gui_next_number(line, &v, &end)) {
                    free(line);
                    *out = v;
                    return 1;
                }
            }
        }
        free(line);
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static int gui_frag_search_count(const char *text, uint64_t *out) {
    static const char *const labels[] = {"碎片文件数", "总碎片数", "碎片数",
                                         "Fragmented files", "Total fragments",
                                         "碎片文件"};
    const char *after = NULL;
    for (size_t i = 0; i < 6; i++) {
        after = gui_find_after_ci(text, labels[i]);
        if (after) break;
    }
    if (!after) return 0;
    size_t win = gui_window_chars_len(after, 60);
    char *window = gui_strndup(after, win);
    if (!window) return 0;
    double v = 0;
    size_t end = 0;
    int ok = gui_next_number(window, &v, &end);
    free(window);
    if (!ok) return 0;
    *out = (uint64_t)v;
    return 1;
}

cdc_frag_parsed_t cdc_frag_parse_defrag(const char *text) {
    cdc_frag_parsed_t r;
    memset(&r, 0, sizeof(r));
    if (!text) return r;
    /* U+3000（E3 80 80）折叠为空格，与 Rust 的 replace('\u{3000}', " ") 一致。 */
    size_t len = strlen(text);
    char *folded = (char *)malloc(len + 1);
    if (!folded) return r;
    size_t w = 0;
    for (size_t i = 0; i < len;) {
        if (i + 2 < len && (unsigned char)text[i] == 0xE3 &&
            (unsigned char)text[i + 1] == 0x80 &&
            (unsigned char)text[i + 2] == 0x80) {
            folded[w++] = ' ';
            i += 3;
        } else {
            folded[w++] = text[i++];
        }
    }
    folded[w] = '\0';
    double v = 0;
    if (gui_frag_search_percent(folded, &v)) {
        r.has_percent = 1;
        r.percent = v;
    }
    {
        static const char *const labels[] = {"碎片空间", "文件碎片",
                                             "Total fragmented space", "碎片文件"};
        if (gui_number_with_unit(folded, labels, 4, "GB", &v)) {
            r.has_space_gb = 1;
            r.space_gb = v;
        }
    }
    {
        uint64_t c = 0;
        if (gui_frag_search_count(folded, &c)) {
            r.has_count = 1;
            r.count = c;
        }
    }
    {
        static const char *const labels[] = {"可用空间", "Free space"};
        if (gui_number_with_unit(folded, labels, 2, "GB", &v)) {
            r.has_free_gb = 1;
            r.free_gb = v;
        }
    }
    free(folded);
    return r;
}

/* ---------------- dup 缓存键 / 索引作用域（纯函数） ---------------- */

/* Rust Debug 格式转义（只处理 " 与 \；控制字符在盘符/路径键里不出现）。 */
static void gui_debug_escape_into(cdc_buf_t *b, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p == '\\') {
            cdc_buf_append_str(b, "\\\\");
        } else if (*p == '"') {
            cdc_buf_append_str(b, "\\\"");
        } else {
            cdc_buf_push(b, (uint8_t)*p);
        }
    }
}

char *cdc_gui_dup_cache_key(const char *const *roots, size_t n_roots,
                            uint64_t min_size, int fast) {
    cdc_buf_t b;
    cdc_buf_init(&b);
    cdc_buf_append_str(&b, "[");
    for (size_t i = 0; i < n_roots; i++) {
        if (i > 0) cdc_buf_append_str(&b, ", ");
        cdc_buf_append_str(&b, "\"");
        gui_debug_escape_into(&b, roots[i]);
        cdc_buf_append_str(&b, "\"");
    }
    cdc_buf_append_str(&b, "]");
    char tail[64];
    snprintf(tail, sizeof(tail), ":%llu:%s", (unsigned long long)min_size,
             fast ? "true" : "false");
    cdc_buf_append_str(&b, tail);
    char *s = cdc_buf_take_cstr(&b);
    if (!s) {
        cdc_buf_free(&b);
        return NULL;
    }
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, s, strlen(s));
    uint8_t digest[32];
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    free(s);
    char *hex = (char *)malloc(65);
    if (!hex) return NULL;
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = hexd[digest[i] >> 4];
        hex[i * 2 + 1] = hexd[digest[i] & 0xF];
    }
    hex[64] = '\0';
    return hex;
}

int cdc_gui_parse_index_scope(const char *root, char *drive_out,
                              char **prefix_out) {
    if (prefix_out) *prefix_out = NULL;
    if (!root || !drive_out) return 0;
    size_t len = strlen(root);
    while (len > 0 && root[len - 1] == '\\') len--;
    if (len < 2 || root[1] != ':') return 0;
    char letter = root[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
        return 0;
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    drive_out[0] = letter;
    drive_out[1] = ':';
    drive_out[2] = '\\';
    drive_out[3] = '\0';
    if (prefix_out && len > 2) {
        *prefix_out = gui_strndup(root, len);
        if (!*prefix_out) return 0;
    }
    return 1;
}

int cdc_gui_path_under_dir(const char *path, const char *dir_prefix) {
    if (!path || !dir_prefix) return 0;
    size_t n = strlen(dir_prefix);
    size_t plen = strlen(path);
    if (plen <= n) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)path[i];
        unsigned char b = (unsigned char)dir_prefix[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return path[n] == '\\';
}

/* ---------------- 任务坞（纯函数，移植自 Rust task.rs） ---------------- */

#define CDC_GUI_MAX_TASKS 16
#define CDC_GUI_TASK_RETAIN_MS 30000ULL

typedef struct {
    int used;
    char id[64];
    char title[128];
    char stage[128];
    char unit[32];
    uint64_t done;
    uint64_t total;
    int active;
    int cancellable;
    int cancelling;
    int mutating;
    char outcome[16];
    char summary[512];
    uint64_t started_ms;
    uint64_t finished_ms;
} cdc_gui_task_t;

static cdc_gui_task_t g_tasks[CDC_GUI_MAX_TASKS];

static cdc_gui_task_t *gui_task_find(const char *id) {
    for (int i = 0; i < CDC_GUI_MAX_TASKS; i++) {
        if (g_tasks[i].used && strcmp(g_tasks[i].id, id) == 0) return &g_tasks[i];
    }
    return NULL;
}

int cdc_task_begin(const char *id, const char *title, int cancellable,
                   int mutating, char **out_error) {
    if (out_error) *out_error = NULL;
    if (!id || !id[0]) {
        if (out_error) *out_error = gui_strdup("任务 id 为空");
        return -1;
    }
    GUI_LOCK();
    if (mutating) {
        for (int i = 0; i < CDC_GUI_MAX_TASKS; i++) {
            if (g_tasks[i].used && g_tasks[i].active && g_tasks[i].mutating) {
                GUI_UNLOCK();
                if (out_error)
                    *out_error = gui_strdup("已有清理任务正在进行，请等它结束后再试");
                return -1;
            }
        }
    }
    cdc_gui_task_t *slot = gui_task_find(id);
    if (!slot) {
        for (int i = 0; i < CDC_GUI_MAX_TASKS; i++) {
            if (!g_tasks[i].used) {
                slot = &g_tasks[i];
                break;
            }
        }
    }
    if (!slot) {
        GUI_UNLOCK();
        if (out_error) *out_error = gui_strdup("任务过多");
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    snprintf(slot->id, sizeof(slot->id), "%s", id);
    snprintf(slot->title, sizeof(slot->title), "%s", title ? title : "");
    slot->active = 1;
    slot->cancellable = cancellable;
    slot->mutating = mutating;
    snprintf(slot->outcome, sizeof(slot->outcome), "running");
    slot->started_ms = gui_now_ms();
    GUI_UNLOCK();
    return 0;
}

void cdc_task_finish(const char *id, const char *outcome, const char *summary) {
    if (!id) return;
    GUI_LOCK();
    cdc_gui_task_t *t = gui_task_find(id);
    if (t && t->active) {
        t->active = 0;
        snprintf(t->outcome, sizeof(t->outcome), "%s", outcome ? outcome : "ok");
        snprintf(t->summary, sizeof(t->summary), "%s", summary ? summary : "");
        t->finished_ms = gui_now_ms();
    }
    GUI_UNLOCK();
}

static void gui_task_set_str(const char *id, int which, const char *value) {
    if (!id) return;
    GUI_LOCK();
    cdc_gui_task_t *t = gui_task_find(id);
    if (t) {
        if (which == 0) snprintf(t->stage, sizeof(t->stage), "%s", value ? value : "");
        if (which == 1) snprintf(t->unit, sizeof(t->unit), "%s", value ? value : "");
    }
    GUI_UNLOCK();
}

void cdc_task_set_stage(const char *id, const char *stage) {
    gui_task_set_str(id, 0, stage);
}

void cdc_task_set_unit(const char *id, const char *unit) {
    gui_task_set_str(id, 1, unit);
}

void cdc_task_set_total(const char *id, uint64_t total) {
    if (!id) return;
    GUI_LOCK();
    cdc_gui_task_t *t = gui_task_find(id);
    if (t) t->total = total;
    GUI_UNLOCK();
}

void cdc_task_set_progress(const char *id, uint64_t done, uint64_t total) {
    if (!id) return;
    GUI_LOCK();
    cdc_gui_task_t *t = gui_task_find(id);
    if (t) {
        t->done = done;
        t->total = total;
    }
    GUI_UNLOCK();
}

void cdc_task_add_done(const char *id, uint64_t amount) {
    if (!id) return;
    GUI_LOCK();
    cdc_gui_task_t *t = gui_task_find(id);
    if (t) t->done += amount;
    GUI_UNLOCK();
}

int cdc_task_cancelled(const char *id) {
    if (!id) return 0;
    GUI_LOCK();
    cdc_gui_task_t *t = gui_task_find(id);
    int c = (t && t->cancelling) ? 1 : 0;
    GUI_UNLOCK();
    return c;
}

char *cdc_task_status_json(void) {
    uint64_t now = gui_now_ms();
    GUI_LOCK();
    /* 剪掉已结束超 30 秒的任务（与 Rust status() 的 retain 一致）。 */
    for (int i = 0; i < CDC_GUI_MAX_TASKS; i++) {
        if (g_tasks[i].used && !g_tasks[i].active && g_tasks[i].finished_ms != 0 &&
            now - g_tasks[i].finished_ms >= CDC_GUI_TASK_RETAIN_MS) {
            memset(&g_tasks[i], 0, sizeof(g_tasks[i]));
        }
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        GUI_UNLOCK();
        return NULL;
    }
    for (int i = 0; i < CDC_GUI_MAX_TASKS; i++) {
        cdc_gui_task_t *t = &g_tasks[i];
        if (!t->used) continue;
        cJSON *o = cJSON_CreateObject();
        if (!o) continue;
        cJSON_AddStringToObject(o, "id", t->id);
        cJSON_AddStringToObject(o, "title", t->title);
        cJSON_AddStringToObject(o, "stage", t->stage);
        cJSON_AddNumberToObject(o, "done", (double)t->done);
        cJSON_AddNumberToObject(o, "total", (double)t->total);
        cJSON_AddStringToObject(o, "unit", t->unit);
        cJSON_AddBoolToObject(o, "active", t->active);
        cJSON_AddBoolToObject(o, "cancellable", t->cancellable);
        cJSON_AddBoolToObject(o, "cancelling", t->cancelling);
        cJSON_AddStringToObject(o, "outcome", t->outcome);
        cJSON_AddStringToObject(o, "summary", t->summary);
        cJSON_AddNumberToObject(o, "started_ms", (double)t->started_ms);
        cJSON_AddNumberToObject(o, "finished_ms", (double)t->finished_ms);
        cJSON_AddBoolToObject(o, "mutating", t->mutating);
        cJSON_AddItemToObject(root, t->id, o);
    }
    GUI_UNLOCK();
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    /* cJSON_PrintUnformatted 用 cJSON 自身的钩子分配（glibc malloc：
     * cJSON.c 的 #define internal_malloc malloc 是函数指针形式，
     * mimalloc 的函数式宏 #define malloc(n) 不展开），而调用方以
     * free/mi_free 释放；这里转拷一份 mimalloc 内存再交出。 */
    char *copy = gui_strdup(s);
    cJSON_free(s);
    return copy;
}

int cdc_task_cancel(const char *id) {
    int any = 0;
    GUI_LOCK();
    if (id) {
        cdc_gui_task_t *t = gui_task_find(id);
        if (t && t->active && t->cancellable) {
            t->cancelling = 1;
            any = 1;
        }
    } else {
        for (int i = 0; i < CDC_GUI_MAX_TASKS; i++) {
            if (g_tasks[i].used && g_tasks[i].active && g_tasks[i].cancellable) {
                g_tasks[i].cancelling = 1;
                any = 1;
            }
        }
    }
    GUI_UNLOCK();
    return any;
}

/* ---------------- 扫描状态（纯函数） ---------------- */

typedef struct {
    int active;
    char stage[128];
    int cancelled;
    uint64_t done;
    uint64_t total;
    uint64_t found;
} cdc_gui_scan_cell_t;

static cdc_gui_scan_cell_t g_scan_dup;
static cdc_gui_scan_cell_t g_scan_big;
static volatile int g_dup_cancel = 0;
static volatile int g_big_cancel = 0;

static cdc_gui_scan_cell_t *gui_scan_cell(const char *kind) {
    if (kind && strcmp(kind, "big") == 0) return &g_scan_big;
    return &g_scan_dup;
}

static volatile int *gui_scan_cancel_flag(const char *kind) {
    if (kind && strcmp(kind, "big") == 0) return &g_big_cancel;
    return &g_dup_cancel;
}

void cdc_scan_begin(const char *kind, const char *stage) {
    GUI_LOCK();
    cdc_gui_scan_cell_t *c = gui_scan_cell(kind);
    memset(c, 0, sizeof(*c));
    c->active = 1;
    snprintf(c->stage, sizeof(c->stage), "%s", stage ? stage : "");
    GUI_UNLOCK();
    *gui_scan_cancel_flag(kind) = 0;
}

void cdc_scan_stage(const char *kind, const char *stage) {
    GUI_LOCK();
    snprintf(gui_scan_cell(kind)->stage, sizeof(gui_scan_cell(kind)->stage), "%s",
             stage ? stage : "");
    GUI_UNLOCK();
}

void cdc_scan_end(const char *kind, const char *stage) {
    GUI_LOCK();
    cdc_gui_scan_cell_t *c = gui_scan_cell(kind);
    c->active = 0;
    snprintf(c->stage, sizeof(c->stage), "%s", stage ? stage : "");
    GUI_UNLOCK();
}

void cdc_scan_set_found(const char *kind, uint64_t found) {
    GUI_LOCK();
    gui_scan_cell(kind)->found = found;
    GUI_UNLOCK();
}

void cdc_scan_add_done(const char *kind, uint64_t amount) {
    GUI_LOCK();
    gui_scan_cell(kind)->done += amount;
    GUI_UNLOCK();
}

int cdc_scan_cancelled(const char *kind) {
    GUI_LOCK();
    int c = gui_scan_cell(kind)->cancelled;
    GUI_UNLOCK();
    return c;
}

static cJSON *gui_scan_cell_json(cdc_gui_scan_cell_t *c) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "active", c->active);
    cJSON_AddStringToObject(o, "stage", c->stage);
    cJSON_AddBoolToObject(o, "cancelled", c->cancelled);
    cJSON_AddNumberToObject(o, "done", (double)c->done);
    cJSON_AddNumberToObject(o, "total", (double)c->total);
    cJSON_AddNumberToObject(o, "found", (double)c->found);
    return o;
}

char *cdc_scan_status_json(void) {
    GUI_LOCK();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        GUI_UNLOCK();
        return NULL;
    }
    cJSON_AddItemToObject(root, "dup", gui_scan_cell_json(&g_scan_dup));
    cJSON_AddItemToObject(root, "big", gui_scan_cell_json(&g_scan_big));
    GUI_UNLOCK();
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    /* cJSON_PrintUnformatted 用 cJSON 自身的钩子分配（glibc malloc：
     * cJSON.c 的 #define internal_malloc malloc 是函数指针形式，
     * mimalloc 的函数式宏 #define malloc(n) 不展开），而调用方以
     * free/mi_free 释放；这里转拷一份 mimalloc 内存再交出。 */
    char *copy = gui_strdup(s);
    cJSON_free(s);
    return copy;
}

/* ---------------- dup 缓存失效（Rust invalidate_dup_cache） ---------------- */

typedef struct {
    char **paths;
    size_t n;
    size_t cap;
} gui_invalidate_ctx_t;

static int gui_invalidate_cb(void *ctx, const char *path, int is_dir,
                             int is_symlink, uint64_t size, uint64_t mtime,
                             uint32_t attrs) {
    (void)is_symlink;
    (void)size;
    (void)mtime;
    (void)attrs;
    gui_invalidate_ctx_t *c = (gui_invalidate_ctx_t *)ctx;
    if (is_dir) return 0;
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    if (strncmp(base, "dups-", 5) != 0) return 0;
    size_t blen = strlen(base);
    if (blen < 5 || strcmp(base + blen - 5, ".json") != 0) return 0;
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 8;
        char **np = (char **)realloc(c->paths, ncap * sizeof(char *));
        if (!np) return 1;
        c->paths = np;
        c->cap = ncap;
    }
    c->paths[c->n] = gui_strdup(path);
    if (!c->paths[c->n]) return 1;
    c->n++;
    return 0;
}

void cdc_gui_invalidate_dup_cache(void) {
    char *dir = cdc_cache_dir();
    if (!dir) return;
    gui_invalidate_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    cdc_err_t err;
    /* 先收集再删（遍历中不删文件）。 */
    cdc_walk_dir(dir, 1, gui_invalidate_cb, &ctx, &err);
    for (size_t i = 0; i < ctx.n; i++) {
        remove(ctx.paths[i]);
        free(ctx.paths[i]);
    }
    free(ctx.paths);
    free(dir);
}

/* ================= Windows 适配层（45 个命令） ================= */
#ifdef _WIN32

/* ---------------- 参数解析 / 回复构造 ---------------- */

static int gui_fail(char **out_error, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    *out_error = gui_strdup(buf);
    return -1;
}

static int gui_ok_json(char **out_json, cJSON *root) {
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return -1;
    /* 同上：转拷为 mimalloc 内存，bridge 以 mi_free 释放。 */
    char *copy = gui_strdup(s);
    cJSON_free(s);
    if (!copy) return -1;
    *out_json = copy;
    return 0;
}

static int gui_ok_string(char **out_json, const char *s) {
    return gui_ok_json(out_json, cJSON_CreateString(s ? s : ""));
}

static int gui_ok_null(char **out_json) {
    return gui_ok_json(out_json, cJSON_CreateNull());
}

static int gui_ok_bool(char **out_json, int value) {
    return gui_ok_json(out_json, cJSON_CreateBool(value));
}

static int gui_ok_u64(char **out_json, uint64_t value) {
    return gui_ok_json(out_json, cJSON_CreateNumber((double)value));
}

/* 解析 args_json（桥传来的 args 对象文本）；NULL/空串视作 {}。 */
static cJSON *gui_parse_args(const char *args_json) {
    if (!args_json || !args_json[0]) return cJSON_CreateObject();
    cJSON *root = cJSON_Parse(args_json);
    if (!root) return cJSON_CreateObject();
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return cJSON_CreateObject();
    }
    return root;
}

static const cJSON *gui_arg(const cJSON *args, const char *key) {
    if (!args) return NULL;
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(args, key);
    if (!it || cJSON_IsNull(it)) return NULL;
    return it;
}

static const char *gui_arg_str(const cJSON *args, const char *key) {
    const cJSON *it = gui_arg(args, key);
    return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* 两个名字都认（前端与 Rust 签名不一致的两个参数：minSize/min_size、
 * targetDir/target_dir；以前者——文档里的名字——优先）。 */
static const char *gui_arg_str2(const cJSON *args, const char *k1,
                                const char *k2) {
    const char *s = gui_arg_str(args, k1);
    return s ? s : gui_arg_str(args, k2);
}

static int gui_arg_bool(const cJSON *args, const char *key, int dflt) {
    const cJSON *it = gui_arg(args, key);
    if (!it) return dflt;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it) ? 1 : 0;
    if (cJSON_IsNumber(it)) return it->valuedouble != 0.0;
    return dflt;
}

static uint64_t gui_arg_u64(const cJSON *args, const char *key, uint64_t dflt) {
    const cJSON *it = gui_arg(args, key);
    if (!it || !cJSON_IsNumber(it)) return dflt;
    double v = it->valuedouble;
    if (v <= 0.0) return 0;
    return (uint64_t)v;
}

static char **gui_arg_str_array(const cJSON *args, const char *key,
                                size_t *out_n) {
    *out_n = 0;
    const cJSON *it = gui_arg(args, key);
    if (!it || !cJSON_IsArray(it)) return NULL;
    int n = cJSON_GetArraySize(it);
    if (n <= 0) return NULL;
    char **arr = (char **)malloc((size_t)n * sizeof(char *));
    if (!arr) return NULL;
    size_t got = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(it, i);
        if (item && cJSON_IsString(item) && item->valuestring) {
            arr[got] = gui_strdup(item->valuestring);
            if (!arr[got]) break;
            got++;
        }
    }
    *out_n = got;
    return arr;
}

static void gui_free_str_array(char **arr, size_t n) {
    if (!arr) return;
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

/* ---------------- 路径小工具 ---------------- */

static int gui_path_exists(const char *path) {
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    DWORD attr = GetFileAttributesW(w);
    free(w);
    return attr != INVALID_FILE_ATTRIBUTES;
}

static int gui_path_is_dir(const char *path) {
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    DWORD attr = GetFileAttributesW(w);
    free(w);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int gui_path_is_reparse(const char *path) {
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    DWORD attr = GetFileAttributesW(w);
    free(w);
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

/* 文件大小与 mtime（unix 秒）；成功返回 1，取不到返回 0（size/mtime 置 0）。 */
static int gui_file_stat(const char *path, uint64_t *out_size,
                         uint64_t *out_mtime) {
    if (out_size) *out_size = 0;
    if (out_mtime) *out_mtime = 0;
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    WIN32_FILE_ATTRIBUTE_DATA data;
    int ok = GetFileAttributesExW(w, GetFileExInfoStandard, &data) ? 1 : 0;
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
    free(w);
    return ok;
}

static int gui_is_admin(void) {
    return IsUserAnAdmin() ? 1 : 0;
}

/* ---------------- glob 展开（Rust expand_paths / glob::glob） ---------------- */

/* 把 pattern 按 '\\' 切段；含通配符的段逐级枚举目录匹配。 */
static int gui_glob_expand_one(const char *pattern, char ***out, size_t *out_n,
                               size_t *out_cap);

static int gui_glob_push(char ***out, size_t *out_n, size_t *out_cap,
                         const char *path) {
    if (*out_n == *out_cap) {
        size_t ncap = *out_cap ? *out_cap * 2 : 16;
        char **np = (char **)realloc(*out, ncap * sizeof(char *));
        if (!np) return -1;
        *out = np;
        *out_cap = ncap;
    }
    (*out)[*out_n] = gui_strdup(path);
    if (!(*out)[*out_n]) return -1;
    (*out_n)++;
    return 0;
}

static int gui_glob_has_wild(const char *s) {
    return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

/* 枚举 dir 下与 seg（小写 glob）匹配的条目，把完整路径追加到 out。 */
static void gui_glob_expand_segment(const char *dir, const char *seg_lower,
                                    char ***out, size_t *out_n, size_t *out_cap) {
    wchar_t *wdir = cdc_utf8_to_wide(dir);
    if (!wdir) return;
    size_t wlen = wcslen(wdir);
    wchar_t *wpat = (wchar_t *)malloc((wlen + 3) * sizeof(wchar_t));
    if (!wpat) {
        free(wdir);
        return;
    }
    wcscpy(wpat, wdir);
    if (wlen > 0 && wpat[wlen - 1] != L'\\') wcscat(wpat, L"\\");
    wcscat(wpat, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            char *name = cdc_wide_to_utf8(fd.cFileName);
            if (!name) continue;
            char *lower = gui_strdup(name);
            if (lower) {
                gui_ascii_lower(lower);
                if (cdc_glob_match(seg_lower, lower)) {
                    /* 拼完整路径：dir + '\\' + name。 */
                    size_t dl = strlen(dir);
                    size_t nl = strlen(name);
                    char *full = (char *)malloc(dl + 1 + nl + 1);
                    if (full) {
                        memcpy(full, dir, dl);
                        full[dl] = '\\';
                        memcpy(full + dl + 1, name, nl + 1);
                        if (gui_glob_push(out, out_n, out_cap, full) != 0) {
                            free(full);
                            free(lower);
                            free(name);
                            break;
                        }
                        free(full);
                    }
                }
                free(lower);
            }
            free(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(wpat);
    free(wdir);
}

/* 递归展开一段 pattern：segs 为剩余段（每段为 UTF-8），cur 为当前前缀。 */
static void gui_glob_expand_segs(char **segs, size_t nsegs, const char *cur,
                                 char ***out, size_t *out_n, size_t *out_cap) {
    if (nsegs == 0) {
        if (cur[0] && gui_path_exists(cur)) gui_glob_push(out, out_n, out_cap, cur);
        return;
    }
    const char *seg = segs[0];
    if (gui_glob_has_wild(seg)) {
        char *lower = gui_strdup(seg);
        if (!lower) return;
        gui_ascii_lower(lower);
        /* 通配段只能枚举一个已存在的目录。 */
        const char *dir = cur[0] ? cur : ".";
        char **matches = NULL;
        size_t nm = 0, cm = 0;
        gui_glob_expand_segment(dir, lower, &matches, &nm, &cm);
        free(lower);
        for (size_t i = 0; i < nm; i++) {
            gui_glob_expand_segs(segs + 1, nsegs - 1, matches[i], out, out_n,
                                 out_cap);
            free(matches[i]);
        }
        free(matches);
        return;
    }
    /* 字面段：拼到当前前缀上。 */
    size_t cl = strlen(cur);
    size_t sl = strlen(seg);
    char *next = (char *)malloc(cl + 1 + sl + 1);
    if (!next) return;
    size_t w = 0;
    if (cl > 0) {
        memcpy(next, cur, cl);
        w = cl;
        if (next[w - 1] != '\\' && next[w - 1] != ':') next[w++] = '\\';
    }
    memcpy(next + w, seg, sl + 1);
    /* 中间段必须是已存在的目录（glob 只返回存在的路径）。 */
    if (nsegs > 1 && !gui_path_is_dir(next)) {
        free(next);
        return;
    }
    gui_glob_expand_segs(segs + 1, nsegs - 1, next, out, out_n, out_cap);
    free(next);
}

static int gui_glob_expand_one(const char *pattern, char ***out, size_t *out_n,
                               size_t *out_cap) {
    /* 切段：先处理盘符 "C:"。 */
    char *copy = gui_strdup(pattern);
    if (!copy) return -1;
    char *segs[64];
    size_t nsegs = 0;
    char *cur = copy;
    char prefix[8] = "";
    if (strlen(copy) >= 2 && copy[1] == ':' &&
        ((copy[0] >= 'A' && copy[0] <= 'Z') ||
         (copy[0] >= 'a' && copy[0] <= 'z'))) {
        prefix[0] = copy[0];
        prefix[1] = ':';
        prefix[2] = '\\';
        prefix[3] = '\0';
        cur = copy + 2;
        if (cur[0] == '\\') cur++;
    }
    /* 按 '\\' 切段（连续斜杠合并）。 */
    char *save = NULL;
    (void)save;
    char *p = cur;
    while (*p && nsegs < 64) {
        while (*p == '\\') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != '\\') p++;
        if (*p == '\\') {
            *p = '\0';
            p++;
        }
        segs[nsegs++] = start;
    }
    if (nsegs == 0) {
        free(copy);
        return 0;
    }
    gui_glob_expand_segs(segs, nsegs, prefix, out, out_n, out_cap);
    free(copy);
    return 0;
}

/* Rust expand_paths：%ENV% 展开 + glob 展开 + 存在性过滤 + 按小写去重。 */
static char **gui_expand_paths(char **raw, size_t nraw, size_t *out_n) {
    *out_n = 0;
    char **out = NULL;
    size_t cap = 0;
    for (size_t i = 0; i < nraw; i++) {
        char *expanded = cdc_expand_env(raw[i]);
        if (!expanded) continue;
        if (gui_glob_has_wild(expanded)) {
            size_t before = *out_n;
            gui_glob_expand_one(expanded, &out, out_n, &cap);
            (void)before;
        } else if (gui_path_exists(expanded)) {
            gui_glob_push(&out, out_n, &cap, expanded);
        }
        free(expanded);
    }
    /* 按小写去重（保留首次出现顺序，与 Rust HashSet 去重一致）。 */
    size_t w = 0;
    for (size_t i = 0; i < *out_n; i++) {
        char *lower = gui_strdup(out[i]);
        if (!lower) continue;
        gui_ascii_lower(lower);
        int dup = 0;
        for (size_t j = 0; j < w; j++) {
            char *l2 = gui_strdup(out[j]);
            if (!l2) continue;
            gui_ascii_lower(l2);
            if (strcmp(lower, l2) == 0) dup = 1;
            free(l2);
            if (dup) break;
        }
        free(lower);
        if (dup) {
            free(out[i]);
        } else {
            out[w++] = out[i];
        }
    }
    *out_n = w;
    return out;
}

static int gui_cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---------------- 匹配器（Rust build_matcher / include_matches） ---------------- */

typedef struct {
    char **patterns;
    size_t n;
} gui_matcher_t;

static void gui_matcher_init(gui_matcher_t *m, char **patterns, size_t n) {
    m->patterns = NULL;
    m->n = 0;
    if (n == 0) {
        m->patterns = (char **)malloc(sizeof(char *));
        if (m->patterns) {
            m->patterns[0] = gui_strdup("*");
            m->n = m->patterns[0] ? 1 : 0;
        }
        return;
    }
    m->patterns = (char **)malloc(n * sizeof(char *));
    if (!m->patterns) return;
    for (size_t i = 0; i < n; i++) {
        char *lower = gui_strdup(patterns[i]);
        if (!lower) continue;
        gui_ascii_lower(lower);
        m->patterns[m->n++] = lower;
    }
}

static void gui_matcher_free(gui_matcher_t *m) {
    for (size_t i = 0; i < m->n; i++) free(m->patterns[i]);
    free(m->patterns);
    m->patterns = NULL;
    m->n = 0;
}

/* 规则里的 ".pdf" 这类裸扩展名按文件名后缀处理（Rust normalize_exclude）。 */
static char *gui_normalize_exclude(const char *pattern) {
    if (pattern[0] == '.' && pattern[1] != '\0') {
        int all_alnum = 1;
        for (const char *p = pattern + 1; *p; p++) {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z'))) {
                all_alnum = 0;
                break;
            }
        }
        if (all_alnum) {
            size_t n = strlen(pattern);
            char *r = (char *)malloc(n + 2);
            if (r) {
                r[0] = '*';
                memcpy(r + 1, pattern, n + 1);
            }
            return r;
        }
    }
    return gui_strdup(pattern);
}

static int gui_include_matches(const char *name_lower, gui_matcher_t *inc,
                               gui_matcher_t *exc) {
    int ok = 0;
    for (size_t i = 0; i < inc->n; i++) {
        if (cdc_glob_match(inc->patterns[i], name_lower)) {
            ok = 1;
            break;
        }
    }
    if (!ok) return 0;
    for (size_t i = 0; i < exc->n; i++) {
        if (cdc_glob_match(exc->patterns[i], name_lower)) return 0;
    }
    return 1;
}

/* 年龄检查（Rust age_ok）：min_age 分钟为 0 永真；mtime 为 0 永真；
 * 未来时间永真；否则 now - mtime >= min_age * 60。 */
static int gui_age_ok(uint64_t mtime, uint64_t min_age_minutes, uint64_t now) {
    if (min_age_minutes == 0) return 1;
    if (mtime == 0) return 1;
    if (mtime > now) return 1;
    return (now - mtime) >= min_age_minutes * 60ULL;
}

/* ---------------- 回复 JSON 构造器 ---------------- */

static cJSON *gui_str_array_json(char **items, size_t n) {
    cJSON *a = cJSON_CreateArray();
    if (!a) return NULL;
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(a, cJSON_CreateString(items[i] ? items[i] : ""));
    }
    return a;
}

static cJSON *gui_delete_report_json(uint64_t freed, uint64_t deleted,
                                     char **failed, size_t nfailed,
                                     char **reasons, size_t nreasons) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddNumberToObject(o, "freed", (double)freed);
    cJSON_AddNumberToObject(o, "deleted", (double)deleted);
    cJSON_AddItemToObject(o, "failed", gui_str_array_json(failed, nfailed));
    cJSON_AddItemToObject(o, "reasons", gui_str_array_json(reasons, nreasons));
    return o;
}

static void gui_delete_log(const char *title, uint64_t freed, uint64_t deleted,
                           size_t nfailed) {
    char line2[256];
    char line3[512];
    snprintf(line2, sizeof(line2), "清理：%s", title);
    snprintf(line3, sizeof(line3), "完成：删除 %llu 个，释放 %llu 字节，失败 %llu 个",
             (unsigned long long)deleted, (unsigned long long)freed,
             (unsigned long long)nfailed);
    const char *lines[3] = {"开始清理", line2, line3};
    cdc_write_log(lines, 3, NULL, NULL);
}

/* ---------------- scan_categories / clean_categories ---------------- */

typedef struct {
    gui_matcher_t inc;
    gui_matcher_t exc;
    uint64_t min_age_minutes;
    uint64_t now;
    uint64_t size;
    uint64_t count;
} gui_scan_ctx_t;

static int gui_scan_walk_cb(void *ctx, const char *path, int is_dir,
                            int is_symlink, uint64_t size, uint64_t mtime,
                            uint32_t attrs) {
    (void)attrs;
    gui_scan_ctx_t *c = (gui_scan_ctx_t *)ctx;
    if (is_dir || is_symlink) return 0;
    char *base = cdc_basename(path);
    if (!base) return 0;
    char *lower = gui_strdup(base);
    free(base);
    if (!lower) return 0;
    gui_ascii_lower(lower);
    int ok = gui_include_matches(lower, &c->inc, &c->exc) &&
             gui_age_ok(mtime, c->min_age_minutes, c->now);
    free(lower);
    if (ok) {
        c->size += size;
        c->count += 1;
    }
    return 0;
}

/* scan_rule 的移植（Rust main.rs）：返回 1 并填 category，或 0 表示跳过该项。 */
static int gui_scan_rule(cdc_rule_t *rule, cJSON **out_category) {
    const char *kind = (rule->kind && rule->kind[0]) ? rule->kind : "files";
    if (!rule->key || !rule->key[0]) return 0;
    const char *name = (rule->name && rule->name[0]) ? rule->name : rule->key;
    const char *tier = (rule->tier && rule->tier[0]) ? rule->tier : "standard";
    cJSON *o = cJSON_CreateObject();
    if (!o) return 0;
    cJSON_AddStringToObject(o, "key", rule->key);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "tier", tier);
    cJSON_AddStringToObject(o, "describe", rule->describe ? rule->describe : "");
    cJSON_AddStringToObject(o, "warning", rule->warning ? rule->warning : "");
    cJSON_AddStringToObject(o, "note", rule->note ? rule->note : "");
    if (strcmp(kind, "files") != 0) {
        cJSON_AddNumberToObject(o, "size", 0);
        cJSON_AddNumberToObject(o, "count", 0);
        cJSON_AddBoolToObject(o, "requires_confirm", 1);
        cJSON_AddBoolToObject(o, "implemented", 0);
        *out_category = o;
        return 1;
    }
    size_t nroots = 0;
    char **roots = gui_expand_paths(rule->paths, rule->npaths, &nroots);
    size_t nglob = 0;
    char **glob_roots = gui_expand_paths(rule->glob_dirs, rule->nglob_dirs, &nglob);
    for (size_t i = 0; i < nglob; i++) {
        if (gui_path_is_dir(glob_roots[i])) {
            char **np = (char **)realloc(roots, (nroots + 1) * sizeof(char *));
            if (np) {
                roots = np;
                roots[nroots++] = glob_roots[i];
                glob_roots[i] = NULL;
            }
        }
    }
    gui_free_str_array(glob_roots, nglob);
    if (nroots > 1) qsort(roots, nroots, sizeof(char *), gui_cmp_str);
    /* 去重（排序后相邻去重）。 */
    size_t w = 0;
    for (size_t i = 0; i < nroots; i++) {
        if (w > 0 && strcmp(roots[w - 1], roots[i]) == 0) {
            free(roots[i]);
        } else {
            roots[w++] = roots[i];
        }
    }
    nroots = w;
    if (nroots == 0) {
        gui_free_str_array(roots, 0);
        cJSON_Delete(o);
        return 0;
    }
    gui_matcher_t inc;
    gui_matcher_t exc;
    gui_matcher_init(&inc, rule->patterns, rule->npatterns);
    char **norm_exc = NULL;
    if (rule->nexclude > 0) {
        norm_exc = (char **)malloc(rule->nexclude * sizeof(char *));
        if (norm_exc) {
            for (size_t i = 0; i < rule->nexclude; i++) {
                norm_exc[i] = gui_normalize_exclude(rule->exclude[i]);
            }
        }
    }
    gui_matcher_init(&exc, norm_exc, rule->nexclude);
    gui_free_str_array(norm_exc, rule->nexclude);
    gui_scan_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.inc = inc;
    ctx.exc = exc;
    ctx.min_age_minutes = rule->min_age_minutes;
    ctx.now = cdc::now_seconds();
    cdc_err_t err;
    for (size_t i = 0; i < nroots; i++) {
        if (gui_path_is_reparse(roots[i])) continue;
        if (!gui_path_is_dir(roots[i])) {
            /* 根是文件：单独校验。 */
            uint64_t size = 0, mtime = 0;
            gui_file_stat(roots[i], &size, &mtime);
            char *base = cdc_basename(roots[i]);
            if (base) {
                char *lower = gui_strdup(base);
                free(base);
                if (lower) {
                    gui_ascii_lower(lower);
                    if (gui_include_matches(lower, &ctx.inc, &ctx.exc) &&
                        gui_age_ok(mtime, ctx.min_age_minutes, ctx.now)) {
                        ctx.size += size;
                        ctx.count += 1;
                    }
                    free(lower);
                }
            }
            continue;
        }
        cdc_walk_dir(roots[i], 1, gui_scan_walk_cb, &ctx, &err);
    }
    gui_matcher_free(&inc);
    gui_matcher_free(&exc);
    gui_free_str_array(roots, nroots);
    cJSON_AddNumberToObject(o, "size", (double)(int64_t)ctx.size);
    cJSON_AddNumberToObject(o, "count", (double)ctx.count);
    cJSON_AddBoolToObject(o, "requires_confirm",
                          rule->requires_confirm || strcmp(rule->tier, "deep") == 0);
    cJSON_AddBoolToObject(o, "implemented", 1);
    *out_category = o;
    return 1;
}

static int gui_cmp_category_size_desc(const void *a, const void *b) {
    const cJSON * const *ca = (const cJSON * const *)a;
    const cJSON * const *cb = (const cJSON * const *)b;
    const cJSON *sa = cJSON_GetObjectItemCaseSensitive(*ca, "size");
    const cJSON *sb = cJSON_GetObjectItemCaseSensitive(*cb, "size");
    double va = sa ? sa->valuedouble : 0;
    double vb = sb ? sb->valuedouble : 0;
    if (vb > va) return 1;
    if (vb < va) return -1;
    return 0;
}

static int h_scan_categories(const char *args_json, char **out_json,
                             char **out_error) {
    (void)args_json;
    (void)out_error;
    char *begin_err = NULL;
    cdc_task_begin("scan_categories", "统计清理项", 0, 0, &begin_err);
    free(begin_err);
    cdc_task_set_unit("scan_categories", "项");
    cdc_task_set_stage("scan_categories", "统计清理项");
    cdc_err_t err;
    cdc_rule_t *rules = NULL;
    size_t nrules = 0;
    if (cdc_rules_load_all(&rules, &nrules, &err) != CDC_OK) {
        cdc_task_finish("scan_categories", "failed", "加载规则失败");
        return gui_fail(out_error, "加载规则失败");
    }
    cdc_task_set_total("scan_categories", (uint64_t)nrules);
    cJSON **cats = NULL;
    size_t ncats = 0, capcats = 0;
    for (size_t i = 0; i < nrules; i++) {
        cJSON *cat = NULL;
        if (!gui_scan_rule(&rules[i], &cat)) continue;
        if (ncats == capcats) {
            size_t ncap = capcats ? capcats * 2 : 16;
            cJSON **np = (cJSON **)realloc(cats, ncap * sizeof(cJSON *));
            if (!np) break;
            cats = np;
            capcats = ncap;
        }
        cats[ncats++] = cat;
        cdc_task_set_progress("scan_categories", (uint64_t)(i + 1),
                              (uint64_t)nrules);
    }
    cdc_rules_free(rules, nrules);
    if (ncats > 1) qsort(cats, ncats, sizeof(cJSON *), gui_cmp_category_size_desc);
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        for (size_t i = 0; i < ncats; i++) cJSON_Delete(cats[i]);
        free(cats);
        cdc_task_finish("scan_categories", "failed", "内存不足");
        return gui_fail(out_error, "内存不足");
    }
    for (size_t i = 0; i < ncats; i++) cJSON_AddItemToArray(arr, cats[i]);
    free(cats);
    char summary[64];
    snprintf(summary, sizeof(summary), "统计 %llu 项", (unsigned long long)ncats);
    cdc_task_finish("scan_categories", "ok", summary);
    return gui_ok_json(out_json, arr);
}

/* ---------------- clean_categories ---------------- */

typedef struct {
    uint64_t freed;
    uint64_t deleted;
    uint64_t skipped;
    uint64_t pending;
    char **errors;
    size_t nerrors;
    char **notes;
    size_t nnotes;
    int cancelled;
} gui_clean_report_t;

static void gui_clean_report_free(gui_clean_report_t *r) {
    gui_free_str_array(r->errors, r->nerrors);
    gui_free_str_array(r->notes, r->nnotes);
}

static void gui_clean_push_str(char ***arr, size_t *n, const char *s) {
    char **np = (char **)realloc(*arr, (*n + 1) * sizeof(char *));
    if (!np) return;
    *arr = np;
    (*arr)[*n] = gui_strdup(s);
    if ((*arr)[*n]) (*n)++;
}

typedef struct {
    gui_clean_report_t *report;
    char **roots;
    size_t nroots;
    gui_matcher_t *inc;
    gui_matcher_t *exc;
    uint64_t min_age_minutes;
    uint64_t now;
    const char *task_id;
} gui_clean_ctx_t;

static void gui_clean_one(const char *path, gui_clean_ctx_t *c) {
    gui_clean_report_t *report = c->report;
    if (cdc_task_cancelled(c->task_id)) return;
    char *base = cdc_basename(path);
    if (!base) return;
    char *lower = gui_strdup(base);
    free(base);
    if (!lower) return;
    gui_ascii_lower(lower);
    int ok = gui_include_matches(lower, c->inc, c->exc);
    free(lower);
    if (!ok) return;
    uint64_t size = 0, mtime = 0;
    if (!gui_file_stat(path, &size, &mtime)) {
        report->skipped += 1;
        return;
    }
    if (!gui_age_ok(mtime, c->min_age_minutes, c->now)) return;
    cdc_task_add_done(c->task_id, 1);
    cdc_err_t err;
    cdc_err_init(&err);
    int rc = cdc_delete_file_checked(path, (const char **)c->roots, c->nroots, 1,
                                     &err);
    if (rc == CDC_DELETE_DELETED) {
        report->deleted += 1;
        report->freed += size;
    } else if (rc == CDC_DELETE_SKIPPED) {
        report->skipped += 1;
    } else {
        report->skipped += 1;
        if (report->nerrors < 20) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", err.message[0] ? err.message : "删除失败");
            gui_clean_push_str(&report->errors, &report->nerrors, buf);
        }
    }
}

static int gui_clean_walk_cb(void *ctx, const char *path, int is_dir,
                             int is_symlink, uint64_t size, uint64_t mtime,
                             uint32_t attrs) {
    (void)size;
    (void)mtime;
    (void)attrs;
    gui_clean_ctx_t *c = (gui_clean_ctx_t *)ctx;
    if (is_dir || is_symlink) return 0;
    if (cdc_task_cancelled(c->task_id)) return 1;
    gui_clean_one(path, c);
    return 0;
}

static int h_clean_categories(const char *args_json, char **out_json,
                              char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    size_t nkeys = 0;
    char **keys = gui_arg_str_array(args, "keys", &nkeys);
    cJSON_Delete(args);
    if (!keys || nkeys == 0) {
        gui_free_str_array(keys, nkeys);
        return gui_fail(out_error, "缺少参数：keys");
    }
    gui_clean_report_t report;
    memset(&report, 0, sizeof(report));
    char *begin_err = NULL;
    if (cdc_task_begin("clean", "C盘瘦身清理", 1, 1, &begin_err) != 0) {
        gui_clean_push_str(&report.errors, &report.nerrors,
                           begin_err ? begin_err : "任务启动失败");
        free(begin_err);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "freed", 0);
        cJSON_AddNumberToObject(o, "deleted", 0);
        cJSON_AddNumberToObject(o, "skipped", 0);
        cJSON_AddNumberToObject(o, "pending", 0);
        cJSON_AddItemToObject(o, "errors", gui_str_array_json(report.errors, report.nerrors));
        cJSON_AddItemToObject(o, "notes", gui_str_array_json(report.notes, report.nnotes));
        gui_clean_report_free(&report);
        gui_free_str_array(keys, nkeys);
        return gui_ok_json(out_json, o);
    }
    cdc_task_set_unit("clean", "个文件");
    cdc_task_set_stage("clean", "准备清理项");
    cdc_err_t err;
    cdc_rule_t *rules = NULL;
    size_t nrules = 0;
    if (cdc_rules_load_all(&rules, &nrules, &err) != CDC_OK) {
        cdc_task_finish("clean", "failed", "加载规则失败");
        gui_free_str_array(keys, nkeys);
        gui_clean_report_free(&report);
        return gui_fail(out_error, "加载规则失败");
    }
    int cancelled = 0;
    for (size_t ki = 0; ki < nkeys && !cancelled; ki++) {
        const char *key = keys[ki];
        cdc_rule_t *rule = NULL;
        for (size_t i = 0; i < nrules; i++) {
            if (rules[i].key && strcmp(rules[i].key, key) == 0) {
                rule = &rules[i];
                break;
            }
        }
        if (!rule) {
            char buf[256];
            snprintf(buf, sizeof(buf), "找不到清理项：%s", key);
            gui_clean_push_str(&report.errors, &report.nerrors, buf);
            continue;
        }
        if (cdc_task_cancelled("clean")) {
            cancelled = 1;
            break;
        }
        {
            char stage[192];
            snprintf(stage, sizeof(stage), "清理%s",
                     (rule->name && rule->name[0]) ? rule->name : key);
            cdc_task_set_stage("clean", stage);
        }
        const char *kind = (rule->kind && rule->kind[0]) ? rule->kind : "files";
        if (strcmp(kind, "files") != 0) {
            char note[256];
            snprintf(note, sizeof(note), "「%s」需要系统命令，已跳过（请到系统页清理）",
                     (rule->name && rule->name[0]) ? rule->name : key);
            gui_clean_push_str(&report.notes, &report.nnotes, note);
            continue;
        }
        size_t nroots = 0;
        char **roots = gui_expand_paths(rule->paths, rule->npaths, &nroots);
        size_t nglob = 0;
        char **glob_roots = gui_expand_paths(rule->glob_dirs, rule->nglob_dirs, &nglob);
        for (size_t i = 0; i < nglob; i++) {
            if (gui_path_is_dir(glob_roots[i])) {
                char **np = (char **)realloc(roots, (nroots + 1) * sizeof(char *));
                if (np) {
                    roots = np;
                    roots[nroots++] = glob_roots[i];
                    glob_roots[i] = NULL;
                }
            }
        }
        gui_free_str_array(glob_roots, nglob);
        if (nroots > 1) qsort(roots, nroots, sizeof(char *), gui_cmp_str);
        size_t w = 0;
        for (size_t i = 0; i < nroots; i++) {
            if (w > 0 && strcmp(roots[w - 1], roots[i]) == 0) {
                free(roots[i]);
            } else {
                roots[w++] = roots[i];
            }
        }
        nroots = w;
        if (nroots == 0) {
            gui_free_str_array(roots, 0);
            continue;
        }
        gui_matcher_t inc;
        gui_matcher_t exc;
        gui_matcher_init(&inc, rule->patterns, rule->npatterns);
        char **norm_exc = NULL;
        if (rule->nexclude > 0) {
            norm_exc = (char **)malloc(rule->nexclude * sizeof(char *));
            if (norm_exc) {
                for (size_t i = 0; i < rule->nexclude; i++) {
                    norm_exc[i] = gui_normalize_exclude(rule->exclude[i]);
                }
            }
        }
        gui_matcher_init(&exc, norm_exc, rule->nexclude);
        gui_free_str_array(norm_exc, rule->nexclude);
        gui_clean_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.report = &report;
        ctx.roots = roots;
        ctx.nroots = nroots;
        ctx.inc = &inc;
        ctx.exc = &exc;
        ctx.min_age_minutes = rule->min_age_minutes;
        ctx.now = cdc::now_seconds();
        ctx.task_id = "clean";
        for (size_t ri = 0; ri < nroots && !cancelled; ri++) {
            if (cdc_task_cancelled("clean")) {
                cancelled = 1;
                break;
            }
            if (gui_path_is_reparse(roots[ri])) {
                report.skipped += 1;
                continue;
            }
            if (!gui_path_is_dir(roots[ri])) {
                gui_clean_one(roots[ri], &ctx);
                continue;
            }
            cdc_walk_dir(roots[ri], 1, gui_clean_walk_cb, &ctx, &err);
            if (cdc_task_cancelled("clean")) {
                cancelled = 1;
                break;
            }
            if (rule->prune_dirs && !cdc_is_never_clean(roots[ri])) {
                uint64_t removed = cdc_remove_empty_dirs(roots[ri]);
                if (removed > 0) {
                    char note[128];
                    snprintf(note, sizeof(note), "移除空目录 %llu 个",
                             (unsigned long long)removed);
                    gui_clean_push_str(&report.notes, &report.nnotes, note);
                }
            }
        }
        gui_matcher_free(&inc);
        gui_matcher_free(&exc);
        gui_free_str_array(roots, nroots);
        if (cancelled) break;
    }
    cdc_rules_free(rules, nrules);
    if (report.nerrors > 10) {
        for (size_t i = 10; i < report.nerrors; i++) free(report.errors[i]);
        report.nerrors = 10;
    }
    if (cancelled) {
        gui_clean_push_str(&report.errors, &report.nerrors, "已取消");
    }
    {
        char summary[256];
        snprintf(summary, sizeof(summary), "删除 %llu 个文件，释放 %.1f MB",
                 (unsigned long long)report.deleted,
                 (double)report.freed / 1048576.0);
        cdc_task_finish("clean", cancelled ? "cancelled" : "ok", summary);
    }
    {
        char line3[512];
        snprintf(line3, sizeof(line3),
                 "完成：删除 %llu 个文件，释放 %llu 字节，跳过 %llu 个，待重启 %llu 个",
                 (unsigned long long)report.deleted,
                 (unsigned long long)report.freed,
                 (unsigned long long)report.skipped,
                 (unsigned long long)report.pending);
        const char *lines[3] = {"开始清理", "清理：C盘瘦身", line3};
        cdc_write_log(lines, 3, NULL, NULL);
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        gui_clean_report_free(&report);
        gui_free_str_array(keys, nkeys);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddNumberToObject(o, "freed", (double)report.freed);
    cJSON_AddNumberToObject(o, "deleted", (double)report.deleted);
    cJSON_AddNumberToObject(o, "skipped", (double)report.skipped);
    cJSON_AddNumberToObject(o, "pending", (double)report.pending);
    cJSON_AddItemToObject(o, "errors", gui_str_array_json(report.errors, report.nerrors));
    cJSON_AddItemToObject(o, "notes", gui_str_array_json(report.notes, report.nnotes));
    gui_clean_report_free(&report);
    gui_free_str_array(keys, nkeys);
    return gui_ok_json(out_json, o);
}

/* ---------------- frag ---------------- */

static cJSON *gui_frag_report_json(const char *drive, cdc_frag_parsed_t *p,
                                   int ssd) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "drive", drive);
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

static int gui_valid_fixed_drive(const char *drive) {
    /* Rust frag::valid_fixed_drive：必须是固定盘符 "X:\\" 形且在 fixed 列表里。 */
    if (!drive || strlen(drive) != 3 || drive[1] != ':' || drive[2] != '\\')
        return 0;
    char letter = drive[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z')))
        return 0;
    cdc_err_t err;
    char **drives = NULL;
    size_t n = 0;
    if (cdc_fixed_drives(&drives, &n, &err) != CDC_OK) return 0;
    int ok = 0;
    for (size_t i = 0; i < n; i++) {
        if (drives[i] && _stricmp(drives[i], drive) == 0) {
            ok = 1;
            break;
        }
    }
    gui_free_str_array(drives, n);
    return ok;
}

static int h_frag_analyze(const char *args_json, char **out_json,
                          char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *drive = gui_arg_str(args, "drive");
    char drive_buf[8] = "";
    if (drive) snprintf(drive_buf, sizeof(drive_buf), "%s", drive);
    cJSON_Delete(args);
    if (!drive_buf[0]) return gui_fail(out_error, "缺少参数：drive");
    char *begin_err = NULL;
    cdc_task_begin("frag_analyze", "磁盘碎片分析", 0, 0, &begin_err);
    free(begin_err);
    cdc_task_set_stage("frag_analyze", "分析磁盘碎片");
    cJSON *report = NULL;
    if (!gui_valid_fixed_drive(drive_buf)) {
        cdc_frag_parsed_t empty_parsed = {};
        report = gui_frag_report_json(drive_buf, &empty_parsed, -1);
        cdc_task_finish("frag_analyze", "ok", "碎片率 0.0%");
        return gui_ok_json(out_json, report);
    }
    int ssd = cdc_is_ssd(drive_buf);
    if (ssd < 0) ssd = 1; /* 未知按 SSD 处理（Rust unwrap_or(true)）。 */
    char *defrag = cdc_system32_path("defrag.exe");
    char argbuf[32];
    snprintf(argbuf, sizeof(argbuf), "%s /A /V", drive_buf);
    cdc_err_t err;
    char *out_text = NULL;
    char *err_text = NULL;
    long exit_code = -1;
    int rc = cdc_run_command(defrag ? defrag : "defrag.exe", argbuf, 1,
                             &out_text, &err_text, &exit_code, &err);
    free(defrag);
    const char *text = "";
    if (rc == CDC_OK && out_text && out_text[0]) {
        text = out_text;
    } else if (err_text && err_text[0]) {
        text = err_text;
    }
    cdc_frag_parsed_t parsed = cdc_frag_parse_defrag(text);
    report = gui_frag_report_json(drive_buf, &parsed, ssd);
    char summary[64];
    snprintf(summary, sizeof(summary), "碎片率 %.1f%%",
             parsed.has_percent ? parsed.percent : 0.0);
    cdc_task_finish("frag_analyze", "ok", summary);
    free(out_text);
    free(err_text);
    if (!report) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, report);
}

static int h_frag_optimize(const char *args_json, char **out_json,
                           char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *drive = gui_arg_str(args, "drive");
    char drive_buf[8] = "";
    if (drive) snprintf(drive_buf, sizeof(drive_buf), "%s", drive);
    cJSON_Delete(args);
    if (!drive_buf[0]) return gui_fail(out_error, "缺少参数：drive");
    char *begin_err = NULL;
    if (cdc_task_begin("frag", "磁盘碎片优化", 0, 1, &begin_err) != 0) {
        char *msg = begin_err ? begin_err : gui_strdup("任务启动失败");
        free(begin_err);
        int r = gui_fail(out_error, "%s", msg);
        free(msg);
        return r;
    }
    cdc_task_set_stage("frag", "执行磁盘优化（可能需要几分钟）");
    char *err_msg = NULL;
    if (!gui_valid_fixed_drive(drive_buf)) {
        err_msg = gui_strdup("只允许优化系统识别出的固定磁盘");
    } else {
        char *defrag = cdc_system32_path("defrag.exe");
        char argbuf[32];
        snprintf(argbuf, sizeof(argbuf), "%s /O", drive_buf);
        cdc_err_t err;
        long exit_code = -1;
        int rc = cdc_run_command(defrag ? defrag : "defrag.exe", argbuf, 0,
                                 NULL, NULL, &exit_code, &err);
        free(defrag);
        if (!(rc == CDC_OK && exit_code == 0)) {
            err_msg = gui_strdup("磁盘优化失败，请检查权限或系统工具状态");
        }
    }
    if (!err_msg) {
        cdc_task_finish("frag", "ok", "磁盘优化完成");
        return gui_ok_null(out_json);
    }
    cdc_task_finish("frag", "failed", err_msg);
    int r = gui_fail(out_error, "%s", err_msg);
    free(err_msg);
    return r;
}

/* ---------------- scan_dups / scan_big（索引优先 + JSON 缓存 + 实时） ---------------- */

/* 固定盘列表（system.h 版，malloc 数组；调用方释放）。 */
static char **gui_fixed_drives_list(size_t *out_n) {
    *out_n = 0;
    cdc_err_t err;
    char **drives = NULL;
    if (cdc_fixed_drives(&drives, out_n, &err) != CDC_OK) {
        gui_free_str_array(drives, *out_n);
        *out_n = 0;
        return NULL;
    }
    if (*out_n == 0) {
        /* 与 Rust fixed_drives 一致：空时回落 ["C:\\"]。 */
        drives = (char **)malloc(sizeof(char *));
        if (drives) {
            drives[0] = gui_strdup("C:\\");
            *out_n = drives[0] ? 1 : 0;
        }
    }
    return drives;
}

/* 解析 scan_dups 的 roots：drives 非空 → 过滤固定盘（大小写不敏感，
 * 保留原串）；否则 root 单个；否则全部固定盘。 */
static char **gui_resolve_dup_roots(const cJSON *args, size_t *out_n) {
    *out_n = 0;
    size_t ndrives = 0;
    char **drives = gui_arg_str_array(args, "drives", &ndrives);
    const char *root = gui_arg_str(args, "root");
    char **roots = NULL;
    size_t nroots = 0;
    size_t nfixed = 0;
    char **fixed = gui_fixed_drives_list(&nfixed);
    if (drives && ndrives > 0) {
        for (size_t i = 0; i < ndrives; i++) {
            for (size_t j = 0; j < nfixed; j++) {
                if (_stricmp(drives[i], fixed[j]) == 0) {
                    char **np = (char **)realloc(roots, (nroots + 1) * sizeof(char *));
                    if (np) {
                        roots = np;
                        roots[nroots++] = gui_strdup(drives[i]);
                    }
                    break;
                }
            }
        }
    } else if (root && root[0]) {
        roots = (char **)malloc(sizeof(char *));
        if (roots) {
            roots[0] = gui_strdup(root);
            nroots = roots[0] ? 1 : 0;
        }
    } else {
        roots = fixed;
        nroots = nfixed;
        fixed = NULL;
        nfixed = 0;
    }
    gui_free_str_array(drives, ndrives);
    gui_free_str_array(fixed, nfixed);
    *out_n = nroots;
    return roots;
}

static cJSON *gui_dup_group_json(const char *hash_hex, uint64_t size,
                                 char **files, uint64_t *mtimes, size_t n) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "hash", hash_hex);
    cJSON_AddNumberToObject(o, "size", (double)size);
    cJSON_AddItemToObject(o, "files", gui_str_array_json(files, n));
    cJSON *m = cJSON_CreateArray();
    if (m) {
        for (size_t i = 0; i < n; i++) {
            cJSON_AddItemToArray(m, cJSON_CreateNumber((double)mtimes[i]));
        }
    }
    cJSON_AddItemToObject(o, "mtimes", m);
    return o;
}

static int gui_cmp_dup_group_desc(const void *a, const void *b) {
    const cJSON * const *ga = (const cJSON * const *)a;
    const cJSON * const *gb = (const cJSON * const *)b;
    const cJSON *sa = cJSON_GetObjectItemCaseSensitive(*ga, "size");
    const cJSON *sb = cJSON_GetObjectItemCaseSensitive(*gb, "size");
    const cJSON *fa = cJSON_GetObjectItemCaseSensitive(*ga, "files");
    const cJSON *fb = cJSON_GetObjectItemCaseSensitive(*gb, "files");
    uint64_t va = (uint64_t)(sa ? sa->valuedouble : 0) *
                  (uint64_t)(fa ? cJSON_GetArraySize(fa) : 0);
    uint64_t vb = (uint64_t)(sb ? sb->valuedouble : 0) *
                  (uint64_t)(fb ? cJSON_GetArraySize(fb) : 0);
    if (vb > va) return 1;
    if (vb < va) return -1;
    return 0;
}

/* 索引行展平后排序用：(size, 摘要 hex, path)。 */
typedef struct {
    uint64_t size;
    char hex[65];
    int has_digest;
    const char *path; /* 借用 rows 里的指针 */
    uint64_t mtime;
} gui_flat_dup_row_t;

static int gui_cmp_flat_dup_row(const void *a, const void *b) {
    const gui_flat_dup_row_t *ra = (const gui_flat_dup_row_t *)a;
    const gui_flat_dup_row_t *rb = (const gui_flat_dup_row_t *)b;
    if (ra->size < rb->size) return -1;
    if (ra->size > rb->size) return 1;
    int c = strcmp(ra->hex, rb->hex);
    if (c != 0) return c;
    return strcmp(ra->path, rb->path);
}

/* 索引查重复（Rust query_dups_from_index）：返回 cJSON 数组或 NULL（回落）。 */
static cJSON *gui_query_dups_from_index(char **roots, size_t nroots,
                                        uint64_t min_size, int fast) {
    if (min_size < CDC_INDEX_MIN_SIZE) return NULL;
    cdc_err_t err;
    cdc_fileindex_t *idx = cdc_fileindex_open_default(&err);
    if (!idx) return NULL;
    uint64_t *serials = (uint64_t *)malloc(nroots * sizeof(uint64_t));
    char **prefixes = (char **)calloc(nroots, sizeof(char *));
    if (!serials || !prefixes) {
        free(serials);
        free(prefixes);
        cdc_fileindex_close(idx);
        return NULL;
    }
    size_t nscopes = 0;
    for (size_t i = 0; i < nroots; i++) {
        char drive[4];
        char *prefix = NULL;
        if (!cdc_gui_parse_index_scope(roots[i], drive, &prefix)) {
            for (size_t j = 0; j < nscopes; j++) free(prefixes[j]);
            free(prefixes);
            free(serials);
            cdc_fileindex_close(idx);
            return NULL;
        }
        uint64_t serial = 0;
        if (!cdc_index_scope_fresh(idx, drive, 1, &serial) || serial == 0) {
            free(prefix);
            for (size_t j = 0; j < nscopes; j++) free(prefixes[j]);
            free(prefixes);
            free(serials);
            cdc_fileindex_close(idx);
            return NULL;
        }
        serials[nscopes] = serial;
        prefixes[nscopes] = prefix;
        nscopes++;
    }
    if (nscopes == 0) {
        free(prefixes);
        free(serials);
        cdc_fileindex_close(idx);
        return NULL;
    }
    size_t nrows = 0;
    cdc_index_dup_row_t *rows =
        cdc_fileindex_query_dup_rows(idx, serials, nscopes, min_size, &nrows);
    cJSON *result = NULL;
    if (rows && nrows > 0) {
        /* 过滤系统文件 + 作用域前缀。 */
        size_t *keep = (size_t *)malloc(nrows * sizeof(size_t));
        size_t nkept = 0;
        if (keep) {
            for (size_t i = 0; i < nrows; i++) {
                if (cdc::is_system_file_name(rows[i].path)) continue;
                int in_scope = 0;
                for (size_t s = 0; s < nscopes; s++) {
                    if (serials[s] != rows[i].volume_serial) continue;
                    if (!prefixes[s] ||
                        cdc_gui_path_under_dir(rows[i].path, prefixes[s])) {
                        in_scope = 1;
                        break;
                    }
                }
                if (in_scope) keep[nkept++] = i;
            }
        }
        /* 展平 + 按 (size, hex, path) 排序：完整性检查与建组都走线性扫描。 */
        gui_flat_dup_row_t *flat = NULL;
        size_t nflat = 0;
        if (keep && nkept > 0) {
            flat = (gui_flat_dup_row_t *)malloc(nkept * sizeof(gui_flat_dup_row_t));
            if (flat) {
                for (size_t i = 0; i < nkept; i++) {
                    cdc_index_dup_row_t *r = &rows[keep[i]];
                    flat[nflat].size = r->size;
                    flat[nflat].has_digest =
                        fast ? r->digests.has_strong : r->digests.has_full;
                    cdc_digest_to_hex(
                        fast ? &r->digests.strong : &r->digests.full,
                        flat[nflat].hex);
                    flat[nflat].path = r->path;
                    flat[nflat].mtime = r->mtime;
                    nflat++;
                }
                if (nflat > 1)
                    qsort(flat, nflat, sizeof(gui_flat_dup_row_t),
                          gui_cmp_flat_dup_row);
            }
        }
        /* 完整性：任一 size 组（≥2 个成员）有成员缺所需摘要 → 回落实时扫描。 */
        int complete = 1;
        for (size_t i = 0; i < nflat && complete;) {
            size_t j = i + 1;
            while (j < nflat && flat[j].size == flat[i].size) j++;
            if (j - i >= 2) {
                for (size_t k = i; k < j; k++) {
                    if (!flat[k].has_digest) {
                        complete = 0;
                        break;
                    }
                }
            }
            i = j;
        }
        if (flat && complete) {
            /* 按 (size, hex) 分段建组（段内已按 path 排好序）。nflat==0 时
             * 直接返回空数组（索引新鲜且完整，只是本卷无重复）。 */
            cJSON **groups = NULL;
            size_t ngroups = 0, capgroups = 0;
            for (size_t i = 0; i < nflat;) {
                size_t j = i + 1;
                while (j < nflat && flat[j].size == flat[i].size &&
                       strcmp(flat[j].hex, flat[i].hex) == 0) {
                    j++;
                }
                if (j - i >= 2) {
                    if (ngroups == capgroups) {
                        size_t ncap = capgroups ? capgroups * 2 : 8;
                        cJSON **np =
                            (cJSON **)realloc(groups, ncap * sizeof(cJSON *));
                        if (!np) break;
                        groups = np;
                        capgroups = ncap;
                    }
                    cJSON *g = gui_dup_group_json(
                        flat[i].hex, flat[i].size, NULL, NULL, 0);
                    if (!g) break;
                    cJSON *files = cJSON_GetObjectItemCaseSensitive(g, "files");
                    cJSON *mtimes =
                        cJSON_GetObjectItemCaseSensitive(g, "mtimes");
                    for (size_t k = i; k < j; k++) {
                        if (files)
                            cJSON_AddItemToArray(
                                files, cJSON_CreateString(flat[k].path));
                        if (mtimes)
                            cJSON_AddItemToArray(
                                mtimes,
                                cJSON_CreateNumber((double)flat[k].mtime));
                    }
                    groups[ngroups++] = g;
                }
                i = j;
            }
            if (ngroups > 0) {
                if (ngroups > 1)
                    qsort(groups, ngroups, sizeof(cJSON *),
                          gui_cmp_dup_group_desc);
                result = cJSON_CreateArray();
                if (result) {
                    for (size_t gi = 0; gi < ngroups; gi++) {
                        cJSON_AddItemToArray(result, groups[gi]);
                    }
                } else {
                    for (size_t gi = 0; gi < ngroups; gi++)
                        cJSON_Delete(groups[gi]);
                }
            } else {
                result = cJSON_CreateArray();
            }
            free(groups);
        }
        free(flat);
        free(keep);
    }
    if (rows) cdc_index_dup_rows_free(rows, nrows);
    for (size_t j = 0; j < nscopes; j++) free(prefixes[j]);
    free(prefixes);
    free(serials);
    cdc_fileindex_close(idx);
    return result;
}

/* 索引查大文件（Rust query_big_from_index）：返回 cJSON 数组或 NULL（回落）。 */
static cJSON *gui_query_big_from_index(char **drives, size_t ndrives) {
    cdc_err_t err;
    cdc_fileindex_t *idx = cdc_fileindex_open_default(&err);
    if (!idx) return NULL;
    uint64_t *serials = (uint64_t *)malloc((ndrives ? ndrives : 1) * sizeof(uint64_t));
    if (!serials) {
        cdc_fileindex_close(idx);
        return NULL;
    }
    size_t nserials = 0;
    for (size_t i = 0; i < ndrives; i++) {
        uint64_t serial = 0;
        if (!cdc_index_scope_fresh(idx, drives[i], 0, &serial) || serial == 0) {
            free(serials);
            cdc_fileindex_close(idx);
            return NULL;
        }
        serials[nserials++] = serial;
    }
    if (nserials == 0) {
        free(serials);
        cdc_fileindex_close(idx);
        return NULL;
    }
    size_t nrows = 0;
    cdc_index_big_row_t *rows =
        cdc_fileindex_query_big_rows(idx, serials, nserials, &nrows);
    free(serials);
    cJSON *arr = NULL;
    if (rows) {
        arr = cJSON_CreateArray();
        if (arr) {
            for (size_t i = 0; i < nrows; i++) {
                if (cdc::is_system_file_name(rows[i].path)) continue;
                if (rows[i].size < cdc::big_file_threshold_bytes(rows[i].path))
                    continue;
                cJSON *o = cJSON_CreateObject();
                if (!o) continue;
                cJSON_AddStringToObject(o, "path", rows[i].path);
                cJSON_AddNumberToObject(o, "size", (double)rows[i].size);
                cJSON_AddNumberToObject(o, "mtime", (double)rows[i].mtime);
                cJSON_AddItemToArray(arr, o);
            }
        }
        cdc_index_big_rows_free(rows, nrows);
    }
    cdc_fileindex_close(idx);
    return arr;
}

/* dup 缓存路径（cache_dir()/dups-{key}.json）。 */
static char *gui_dup_cache_path(const char *key) {
    char *dir = cdc_cache_dir();
    if (!dir) return NULL;
    size_t n = strlen(dir) + 1 + 5 + 64 + 5 + 1;
    char *p = (char *)malloc(n);
    if (p) snprintf(p, n, "%s/dups-%s.json", dir, key);
    free(dir);
    return p;
}

static void gui_dup_tick(void *ctx) {
    (void)ctx;
    cdc_scan_add_done("dup", 1);
}

static int h_scan_dups(const char *args_json, char **out_json,
                       char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    int fast = gui_arg_bool(args, "fast", 0);
    uint64_t min = gui_arg_u64(args, "min_size", 0);
    /* 前端发 minSize，Rust 签名是 min_size：两个名字都认。 */
    const cJSON *min_it = gui_arg(args, "minSize");
    if (min_it && cJSON_IsNumber(min_it) && min_it->valuedouble > 0)
        min = (uint64_t)min_it->valuedouble;
    if (min == 0) min = (uint64_t)1 << 20;
    size_t nroots = 0;
    char **roots = gui_resolve_dup_roots(args, &nroots);
    cJSON_Delete(args);
    cdc_scan_begin("dup", "枚举文件");
    if (nroots == 0) {
        cdc_scan_end("dup", "完成");
        gui_free_str_array(roots, nroots);
        return gui_ok_json(out_json, cJSON_CreateArray());
    }
    cdc_scan_stage("dup", "索引同步");
    cJSON *groups = gui_query_dups_from_index(roots, nroots, min, fast);
    if (groups) {
        cdc_scan_set_found("dup", (uint64_t)cJSON_GetArraySize(groups));
        cdc_scan_end("dup", "完成");
        gui_free_str_array(roots, nroots);
        return gui_ok_json(out_json, groups);
    }
    /* 5 分钟 JSON 缓存。 */
    char *key = cdc_gui_dup_cache_key((const char **)roots, nroots, min, fast);
    char *cache_path = key ? gui_dup_cache_path(key) : NULL;
    if (cache_path) {
        uint64_t mtime = 0;
        uint64_t fsize = 0;
        gui_file_stat(cache_path, &fsize, &mtime);
        if (mtime != 0) {
            uint64_t now = cdc::now_seconds();
            if (now >= mtime && now - mtime < 300) {
                FILE *f = fopen(cache_path, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    if (sz > 0 && sz < 64 * 1024 * 1024) {
                        char *text = (char *)malloc((size_t)sz + 1);
                        if (text &&
                            fread(text, 1, (size_t)sz, f) == (size_t)sz) {
                            text[sz] = '\0';
                            cJSON *cached = cJSON_Parse(text);
                            if (cached && cJSON_IsArray(cached)) {
                                /* 校验形状：每组有 hash/size/files/mtimes。 */
                                int valid = 1;
                                int ng = cJSON_GetArraySize(cached);
                                for (int i = 0; i < ng && valid; i++) {
                                    const cJSON *g = cJSON_GetArrayItem(cached, i);
                                    const cJSON *h = cJSON_GetObjectItemCaseSensitive(g, "hash");
                                    const cJSON *s = cJSON_GetObjectItemCaseSensitive(g, "size");
                                    const cJSON *fl = cJSON_GetObjectItemCaseSensitive(g, "files");
                                    const cJSON *mt = cJSON_GetObjectItemCaseSensitive(g, "mtimes");
                                    if (!h || !cJSON_IsString(h) || !s ||
                                        !cJSON_IsNumber(s) || !fl ||
                                        !cJSON_IsArray(fl) || !mt ||
                                        !cJSON_IsArray(mt) ||
                                        cJSON_GetArraySize(fl) !=
                                            cJSON_GetArraySize(mt)) {
                                        valid = 0;
                                    }
                                }
                                if (valid) {
                                    cdc_scan_set_found("dup", (uint64_t)ng);
                                    cdc_scan_end("dup", "完成");
                                    fclose(f);
                                    free(text);
                                    free(cache_path);
                                    free(key);
                                    gui_free_str_array(roots, nroots);
                                    return gui_ok_json(out_json, cached);
                                }
                            }
                            if (cached) cJSON_Delete(cached);
                        }
                        free(text);
                    }
                    fclose(f);
                }
            }
        }
    }
    /* 实时扫描。 */
    cdc_scan_stage("dup", "枚举文件");
    const char **r = (const char **)roots;
    CdcScanOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.cancel = &g_dup_cancel;
    opts.on_tick = gui_dup_tick;
    opts.tick_ctx = NULL;
    opts.trace = getenv("CDC_TRACE") != NULL;
    opts.fast = fast;
    CdcDupGroupList out;
    memset(&out, 0, sizeof(out));
    int rc = cdc_scan_dups(r, nroots, min, &opts, &out);
    int cancelled = cdc_scan_cancelled("dup") || *opts.cancel;
    cJSON *arr = cJSON_CreateArray();
    if (rc == 0 && arr) {
        for (size_t i = 0; i < out.n; i++) {
            CdcDupGroup *g = &out.items[i];
            cJSON *gj = gui_dup_group_json(g->hash_hex, g->size, g->files,
                                           g->mtimes, g->n_files);
            if (gj) cJSON_AddItemToArray(arr, gj);
        }
    }
    cdc_dup_group_list_free(&out);
    cdc_scan_set_found("dup", (uint64_t)(arr ? cJSON_GetArraySize(arr) : 0));
    cdc_scan_end("dup", "完成");
    /* 未取消且非空时写回 JSON 缓存（供下次 5 分钟内复用）。 */
    if (!cancelled && cache_path && arr && cJSON_GetArraySize(arr) > 0) {
        char *text = cJSON_PrintUnformatted(arr);
        if (text) {
            FILE *f = fopen(cache_path, "wb");
            if (f) {
                fwrite(text, 1, strlen(text), f);
                fclose(f);
            }
            cJSON_free(text);
        }
    }
    free(cache_path);
    free(key);
    gui_free_str_array(roots, nroots);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static int h_scan_big(const char *args_json, char **out_json,
                      char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    size_t ndrives = 0;
    char **drives = gui_arg_str_array(args, "drives", &ndrives);
    cJSON_Delete(args);
    size_t nfixed = 0;
    if (!drives || ndrives == 0) {
        gui_free_str_array(drives, ndrives);
        drives = gui_fixed_drives_list(&nfixed);
        ndrives = nfixed;
    }
    cdc_scan_begin("big", "大文件扫描");
    cdc_scan_stage("big", "索引同步");
    cJSON *rows = NULL;
    if (drives && ndrives > 0) {
        rows = gui_query_big_from_index(drives, ndrives);
    }
    if (rows) {
        cdc_scan_end("big", "完成");
        gui_free_str_array(drives, ndrives);
        return gui_ok_json(out_json, rows);
    }
    const char **d = (const char **)drives;
    CdcScanOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.cancel = &g_big_cancel;
    opts.trace = getenv("CDC_TRACE") != NULL;
    CdcBigFileList out;
    memset(&out, 0, sizeof(out));
    int rc = cdc_scan_big(d, ndrives, &opts, &out);
    cJSON *arr = cJSON_CreateArray();
    if (rc == 0 && arr) {
        for (size_t i = 0; i < out.n; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "path", out.items[i].path);
            cJSON_AddNumberToObject(o, "size", (double)out.items[i].size);
            cJSON_AddNumberToObject(o, "mtime", (double)out.items[i].mtime);
            cJSON_AddItemToArray(arr, o);
        }
    }
    cdc_big_file_list_free(&out);
    cdc_scan_end("big", "完成");
    gui_free_str_array(drives, ndrives);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

/* ---------------- delete_recycle / delete_dups ---------------- */

static int h_delete_recycle(const char *args_json, char **out_json,
                            char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    size_t npaths = 0;
    char **paths = gui_arg_str_array(args, "paths", &npaths);
    cJSON_Delete(args);
    if (!paths || npaths == 0) {
        gui_free_str_array(paths, npaths);
        return gui_fail(out_error, "缺少参数：paths");
    }
    const char *title = "大文件";
    char *begin_err = NULL;
    char **failed = NULL;
    size_t nfailed = 0;
    char **reasons = NULL;
    size_t nreasons = 0;
    if (cdc_task_begin("big_delete", "大文件清理", 1, 1, &begin_err) != 0) {
        gui_clean_push_str(&reasons, &nreasons,
                           begin_err ? begin_err : "任务启动失败");
        free(begin_err);
        gui_delete_log(title, 0, 0, 0);
        cJSON *o = gui_delete_report_json(0, 0, failed, nfailed, reasons, nreasons);
        gui_free_str_array(failed, nfailed);
        gui_free_str_array(reasons, nreasons);
        gui_free_str_array(paths, npaths);
        return gui_ok_json(out_json, o);
    }
    cdc_task_set_unit("big_delete", "个文件");
    cdc_task_set_total("big_delete", (uint64_t)npaths);
    cdc_task_set_stage("big_delete", "校验文件");
    uint64_t *sizes = (uint64_t *)calloc(npaths, sizeof(uint64_t));
    char **eligible = NULL;
    size_t neligible = 0;
    int cancelled = 0;
    for (size_t i = 0; i < npaths; i++) {
        if (cdc_task_cancelled("big_delete")) {
            cancelled = 1;
            break;
        }
        cdc_task_add_done("big_delete", 1);
        if (!gui_path_exists(paths[i])) {
            gui_clean_push_str(&failed, &nfailed, paths[i]);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：文件已不存在", paths[i]);
            gui_clean_push_str(&reasons, &nreasons, buf);
            continue;
        }
        if (cdc_explicit_delete_forbidden(paths[i])) {
            gui_clean_push_str(&failed, &nfailed, paths[i]);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：受保护路径或密码库，已跳过", paths[i]);
            gui_clean_push_str(&reasons, &nreasons, buf);
            continue;
        }
        uint64_t size = 0, mtime = 0;
        gui_file_stat(paths[i], &size, &mtime);
        if (sizes) sizes[i] = size;
        char **np = (char **)realloc(eligible, (neligible + 1) * sizeof(char *));
        if (np) {
            eligible = np;
            eligible[neligible++] = paths[i];
        }
    }
    uint64_t deleted = 0, freed = 0;
    if (cancelled) {
        gui_clean_push_str(&reasons, &nreasons, "已取消");
        cdc_task_finish("big_delete", "cancelled", "已取消");
        gui_delete_log(title, 0, 0, nfailed);
        cJSON *o = gui_delete_report_json(0, 0, failed, nfailed, reasons, nreasons);
        free(sizes);
        free(eligible);
        gui_free_str_array(failed, nfailed);
        gui_free_str_array(reasons, nreasons);
        gui_free_str_array(paths, npaths);
        return gui_ok_json(out_json, o);
    }
    cdc_task_set_stage("big_delete", "送入回收站");
    {
        cdc_err_t err;
        char **send_failed = NULL;
        size_t nsend_failed = 0;
        cdc_send_to_recycle((const char **)eligible, neligible, &send_failed,
                            &nsend_failed, &err);
        gui_free_str_array(send_failed, nsend_failed);
    }
    for (size_t i = 0; i < neligible; i++) {
        if (gui_path_exists(eligible[i])) {
            gui_clean_push_str(&failed, &nfailed, eligible[i]);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：送回收站失败（可能被占用）", eligible[i]);
            gui_clean_push_str(&reasons, &nreasons, buf);
        } else {
            deleted += 1;
            /* sizes 按原 paths 下标存；eligible 存的是 paths 里的指针，
             * 用指针相等找回下标。 */
            for (size_t j = 0; j < npaths; j++) {
                if (paths[j] == eligible[i] && sizes) {
                    freed += sizes[j];
                    break;
                }
            }
        }
    }
    cdc_gui_invalidate_dup_cache();
    {
        char summary[128];
        snprintf(summary, sizeof(summary), "删除 %llu 个，释放 %.1f MB",
                 (unsigned long long)deleted, (double)freed / 1048576.0);
        cdc_task_finish("big_delete", "ok", summary);
    }
    gui_delete_log(title, freed, deleted, nfailed);
    cJSON *o = gui_delete_report_json(freed, deleted, failed, nfailed, reasons,
                                      nreasons);
    free(sizes);
    free(eligible);
    gui_free_str_array(failed, nfailed);
    gui_free_str_array(reasons, nreasons);
    gui_free_str_array(paths, npaths);
    return gui_ok_json(out_json, o);
}

typedef struct {
    char *path;
    char *hash;
    uint64_t size;
    uint64_t mtime;
    char *keeper;
} gui_verify_item_t;

static void gui_verify_items_free(gui_verify_item_t *items, size_t n) {
    if (!items) return;
    for (size_t i = 0; i < n; i++) {
        free(items[i].path);
        free(items[i].hash);
        free(items[i].keeper);
    }
    free(items);
}

static int h_delete_dups(const char *args_json, char **out_json,
                         char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    const cJSON *items_json = gui_arg(args, "items");
    gui_verify_item_t *items = NULL;
    size_t nitems = 0;
    if (items_json && cJSON_IsArray(items_json)) {
        int n = cJSON_GetArraySize(items_json);
        items = (gui_verify_item_t *)calloc((size_t)(n > 0 ? n : 1),
                                            sizeof(gui_verify_item_t));
        if (items) {
            for (int i = 0; i < n; i++) {
                const cJSON *it = cJSON_GetArrayItem(items_json, i);
                if (!it) continue;
                const cJSON *p = cJSON_GetObjectItemCaseSensitive(it, "path");
                const cJSON *h = cJSON_GetObjectItemCaseSensitive(it, "hash");
                const cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "size");
                const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "mtime");
                const cJSON *k = cJSON_GetObjectItemCaseSensitive(it, "keeper");
                items[nitems].path =
                    (p && cJSON_IsString(p)) ? gui_strdup(p->valuestring) : gui_strdup("");
                items[nitems].hash =
                    (h && cJSON_IsString(h)) ? gui_strdup(h->valuestring) : gui_strdup("");
                items[nitems].size = (s && cJSON_IsNumber(s)) ? (uint64_t)s->valuedouble : 0;
                items[nitems].mtime = (m && cJSON_IsNumber(m)) ? (uint64_t)m->valuedouble : 0;
                items[nitems].keeper =
                    (k && cJSON_IsString(k)) ? gui_strdup(k->valuestring) : gui_strdup("");
                nitems++;
            }
        }
    }
    cJSON_Delete(args);
    if (!items || nitems == 0) {
        gui_verify_items_free(items, nitems);
        return gui_fail(out_error, "缺少参数：items");
    }
    const char *title = "重复文件";
    char **failed = NULL;
    size_t nfailed = 0;
    char **reasons = NULL;
    size_t nreasons = 0;
    char *begin_err = NULL;
    if (cdc_task_begin("dup_delete", "重复文件清理", 1, 1, &begin_err) != 0) {
        gui_clean_push_str(&reasons, &nreasons,
                           begin_err ? begin_err : "任务启动失败");
        free(begin_err);
        gui_delete_log(title, 0, 0, 0);
        cJSON *o = gui_delete_report_json(0, 0, failed, nfailed, reasons, nreasons);
        gui_free_str_array(failed, nfailed);
        gui_free_str_array(reasons, nreasons);
        gui_verify_items_free(items, nitems);
        return gui_ok_json(out_json, o);
    }
    cdc_task_set_unit("dup_delete", "个文件");
    cdc_task_set_total("dup_delete", (uint64_t)nitems);
    cdc_task_set_stage("dup_delete", "校验文件");
    /* 待复核的项 + 保留文件集合（去重）。 */
    gui_verify_item_t *pending = NULL;
    size_t npending = 0;
    char **keepers = NULL;
    size_t nkeepers = 0;
    int cancelled = 0;
    for (size_t i = 0; i < nitems; i++) {
        if (cdc_task_cancelled("dup_delete")) {
            cancelled = 1;
            break;
        }
        cdc_task_add_done("dup_delete", 1);
        const char *path = items[i].path;
        if (!gui_path_exists(path)) {
            gui_clean_push_str(&failed, &nfailed, path);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：文件已不存在", path);
            gui_clean_push_str(&reasons, &nreasons, buf);
            continue;
        }
        if (cdc_explicit_delete_forbidden(path)) {
            gui_clean_push_str(&failed, &nfailed, path);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：受保护路径或密码库，已跳过", path);
            gui_clean_push_str(&reasons, &nreasons, buf);
            continue;
        }
        uint64_t size = 0, mtime = 0;
        if (!gui_file_stat(path, &size, &mtime)) {
            gui_clean_push_str(&failed, &nfailed, path);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：读不到文件属性", path);
            gui_clean_push_str(&reasons, &nreasons, buf);
            continue;
        }
        if (size != items[i].size) {
            gui_clean_push_str(&failed, &nfailed, path);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：扫描后大小变了，已跳过", path);
            gui_clean_push_str(&reasons, &nreasons, buf);
            continue;
        }
        if (mtime != items[i].mtime) {
            gui_clean_push_str(&failed, &nfailed, path);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：扫描后修改时间变了，已跳过", path);
            gui_clean_push_str(&reasons, &nreasons, buf);
            continue;
        }
        if (items[i].keeper[0]) {
            int seen = 0;
            for (size_t k = 0; k < nkeepers; k++) {
                if (strcmp(keepers[k], items[i].keeper) == 0) {
                    seen = 1;
                    break;
                }
            }
            if (!seen) {
                char **np =
                    (char **)realloc(keepers, (nkeepers + 1) * sizeof(char *));
                if (np) {
                    keepers = np;
                    keepers[nkeepers++] = gui_strdup(items[i].keeper);
                }
            }
        }
        gui_verify_item_t *np = (gui_verify_item_t *)realloc(
            pending, (npending + 1) * sizeof(gui_verify_item_t));
        if (np) {
            pending = np;
            pending[npending] = items[i];
            /* 转移所有权：items 里的指针不再由 items 释放。 */
            items[i].path = NULL;
            items[i].hash = NULL;
            items[i].keeper = NULL;
            npending++;
        }
    }
    uint64_t deleted = 0, freed = 0;
    if (cancelled) {
        gui_clean_push_str(&reasons, &nreasons, "已取消");
        cdc_task_finish("dup_delete", "cancelled", "已取消");
        gui_delete_log(title, 0, 0, nfailed);
        cJSON *o = gui_delete_report_json(0, 0, failed, nfailed, reasons, nreasons);
        gui_free_str_array(keepers, nkeepers);
        gui_verify_items_free(pending, npending);
        gui_verify_items_free(items, nitems);
        gui_free_str_array(failed, nfailed);
        gui_free_str_array(reasons, nreasons);
        return gui_ok_json(out_json, o);
    }
    /* 并行按盘计算保留项与待删副本的完整哈希（C 版顺序执行，结果一致）。 */
    size_t npaths_to_hash = nkeepers + npending;
    cdc_task_set_stage("dup_delete", "核对内容");
    cdc_task_set_progress("dup_delete", 0, (uint64_t)npaths_to_hash);
    /* hashed[i]：keepers[0..nkeepers) + pending[].path 的 hex，失败为 NULL。 */
    char **hashed = (char **)calloc(npaths_to_hash ? npaths_to_hash : 1,
                                   sizeof(char *));
    for (size_t i = 0; i < npaths_to_hash; i++) {
        const char *p = i < nkeepers ? keepers[i] : pending[i - nkeepers].path;
        uint8_t digest[32];
        if (cdc_full_hash_file(p, 0, digest) == 0) {
            char hex[65];
            cdc_digest_to_hex((const cdc_digest_t *)digest, hex);
            hashed[i] = gui_strdup(hex);
        }
        cdc_task_add_done("dup_delete", 1);
    }
    cdc_task_set_stage("dup_delete", "复核内容");
    cdc_task_set_progress("dup_delete", 0, (uint64_t)npending);
    char **eligible = NULL;
    size_t neligible = 0;
    uint64_t *esizes = NULL;
    for (size_t i = 0; i < npending; i++) {
        if (cdc_task_cancelled("dup_delete")) {
            cancelled = 1;
            break;
        }
        cdc_task_add_done("dup_delete", 1);
        const char *path = pending[i].path;
        const char *expected = NULL;
        if (pending[i].keeper[0]) {
            for (size_t k = 0; k < nkeepers; k++) {
                if (strcmp(keepers[k], pending[i].keeper) == 0) {
                    expected = hashed[k];
                    break;
                }
            }
            if (!expected) {
                gui_clean_push_str(&failed, &nfailed, path);
                char buf[1024];
                snprintf(buf, sizeof(buf), "%s：保留的文件读不到，已跳过", path);
                gui_clean_push_str(&reasons, &nreasons, buf);
                continue;
            }
        } else {
            expected = pending[i].hash;
        }
        const char *actual = hashed[nkeepers + i];
        if (actual && expected && strcmp(actual, expected) == 0) {
            char **np =
                (char **)realloc(eligible, (neligible + 1) * sizeof(char *));
            uint64_t *ns = (uint64_t *)realloc(
                esizes, (neligible + 1) * sizeof(uint64_t));
            if (np && ns) {
                eligible = np;
                esizes = ns;
                eligible[neligible] = (char *)path;
                esizes[neligible] = pending[i].size;
                neligible++;
            }
        } else {
            gui_clean_push_str(&failed, &nfailed, path);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：内容与保留文件不一致，已跳过", path);
            gui_clean_push_str(&reasons, &nreasons, buf);
        }
    }
    if (cancelled) {
        gui_clean_push_str(&reasons, &nreasons, "已取消");
        cdc_task_finish("dup_delete", "cancelled", "已取消");
        gui_delete_log(title, 0, 0, nfailed);
        cJSON *o = gui_delete_report_json(0, 0, failed, nfailed, reasons, nreasons);
        gui_free_str_array(hashed, npaths_to_hash);
        gui_free_str_array(keepers, nkeepers);
        free(eligible);
        free(esizes);
        gui_verify_items_free(pending, npending);
        gui_verify_items_free(items, nitems);
        gui_free_str_array(failed, nfailed);
        gui_free_str_array(reasons, nreasons);
        return gui_ok_json(out_json, o);
    }
    cdc_task_set_stage("dup_delete", "送入回收站");
    {
        cdc_err_t err;
        char **send_failed = NULL;
        size_t nsend_failed = 0;
        cdc_send_to_recycle((const char **)eligible, neligible, &send_failed,
                            &nsend_failed, &err);
        gui_free_str_array(send_failed, nsend_failed);
    }
    for (size_t i = 0; i < neligible; i++) {
        if (gui_path_exists(eligible[i])) {
            gui_clean_push_str(&failed, &nfailed, eligible[i]);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s：送回收站失败（可能被占用）", eligible[i]);
            gui_clean_push_str(&reasons, &nreasons, buf);
        } else {
            deleted += 1;
            freed += esizes[i];
        }
    }
    cdc_gui_invalidate_dup_cache();
    {
        char summary[128];
        snprintf(summary, sizeof(summary), "删除 %llu 个，释放 %.1f MB",
                 (unsigned long long)deleted, (double)freed / 1048576.0);
        cdc_task_finish("dup_delete", "ok", summary);
    }
    gui_delete_log(title, freed, deleted, nfailed);
    cJSON *o = gui_delete_report_json(freed, deleted, failed, nfailed, reasons,
                                      nreasons);
    gui_free_str_array(hashed, npaths_to_hash);
    gui_free_str_array(keepers, nkeepers);
    free(eligible);
    free(esizes);
    gui_verify_items_free(pending, npending);
    gui_verify_items_free(items, nitems);
    gui_free_str_array(failed, nfailed);
    gui_free_str_array(reasons, nreasons);
    return gui_ok_json(out_json, o);
}

/* ---------------- 其余简单命令 ---------------- */

static void gui_scan_mark_cancelled(const char *kind) {
    GUI_LOCK();
    cdc_gui_scan_cell_t *c = gui_scan_cell(kind);
    c->cancelled = 1;
    GUI_UNLOCK();
    *gui_scan_cancel_flag(kind) = 1;
}

static int h_disk_info(const char *args_json, char **out_json,
                       char **out_error) {
    (void)args_json;
    cdc_err_t err;
    uint64_t free_bytes = 0, total = 0;
    if (cdc_drive_free_space("C:\\", &free_bytes, &total, &err) != CDC_OK) {
        return gui_fail(out_error, "读取磁盘信息失败");
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) return gui_fail(out_error, "内存不足");
    cJSON_AddNumberToObject(o, "total", (double)total);
    cJSON_AddNumberToObject(o, "used", (double)(total - free_bytes));
    cJSON_AddNumberToObject(o, "free", (double)free_bytes);
    return gui_ok_json(out_json, o);
}

static int h_scan_status(const char *args_json, char **out_json,
                         char **out_error) {
    (void)args_json;
    (void)out_error;
    char *s = cdc_scan_status_json();
    if (!s) return gui_fail(out_error, "内存不足");
    *out_json = s;
    return 0;
}

static int h_cancel_scan(const char *args_json, char **out_json,
                         char **out_error) {
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

static int h_task_status(const char *args_json, char **out_json,
                         char **out_error) {
    (void)args_json;
    (void)out_error;
    char *s = cdc_task_status_json();
    if (!s) return gui_fail(out_error, "内存不足");
    *out_json = s;
    return 0;
}

static int h_cancel_task(const char *args_json, char **out_json,
                         char **out_error) {
    (void)out_error;
    cJSON *args = gui_parse_args(args_json);
    const char *id = gui_arg_str(args, "id");
    cJSON_Delete(args);
    return gui_ok_bool(out_json, cdc_task_cancel(id));
}

static int h_list_drives(const char *args_json, char **out_json,
                         char **out_error) {
    (void)args_json;
    (void)out_error;
    size_t n = 0;
    char **drives = gui_fixed_drives_list(&n);
    cJSON *arr = gui_str_array_json(drives, n);
    gui_free_str_array(drives, n);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static int h_user_profile(const char *args_json, char **out_json,
                          char **out_error) {
    (void)args_json;
    (void)out_error;
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char *utf8 = cdc_wide_to_utf8(buf);
        if (utf8) {
            int r = gui_ok_string(out_json, utf8);
            free(utf8);
            return r;
        }
    }
    return gui_ok_string(out_json, "C:\\");
}

/* ---------------- 索引命令 ---------------- */

static int h_index_status(const char *args_json, char **out_json,
                          char **out_error) {
    (void)args_json;
    (void)out_error;
    size_t ndrives = 0;
    char **drives = gui_fixed_drives_list(&ndrives);
    cdc_err_t err;
    cdc_fileindex_t *idx = cdc_fileindex_open_default(&err);
    cJSON *volumes = cJSON_CreateArray();
    if (!volumes) {
        gui_free_str_array(drives, ndrives);
        if (idx) cdc_fileindex_close(idx);
        return gui_fail(out_error, "内存不足");
    }
    for (size_t i = 0; i < ndrives; i++) {
        cJSON *v = cJSON_CreateObject();
        if (!v) continue;
        cJSON_AddStringToObject(v, "drive", drives[i]);
        uint64_t files = 0, with_full = 0, updated_at = 0;
        char state[16] = "missing";
        if (idx) {
            uint64_t serial = 0;
            cdc_index_freshness(idx, drives[i], &serial, state, sizeof(state));
            if (serial != 0) {
                cdc_fileindex_volume_counts(idx, serial, &files, &with_full);
                uint64_t at = 0;
                if (cdc_fileindex_usn_state_updated_at(idx, serial, &at) == 1) {
                    updated_at = at;
                }
            }
        }
        cJSON_AddStringToObject(v, "state", state);
        cJSON_AddNumberToObject(v, "files", (double)files);
        cJSON_AddNumberToObject(v, "with_full", (double)with_full);
        cJSON_AddNumberToObject(v, "updated_at", (double)updated_at);
        cJSON_AddItemToArray(volumes, v);
    }
    if (idx) cdc_fileindex_close(idx);
    gui_free_str_array(drives, ndrives);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cJSON_Delete(volumes);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddItemToObject(o, "volumes", volumes);
    return gui_ok_json(out_json, o);
}

static int h_index_sync_now(const char *args_json, char **out_json,
                            char **out_error) {
    (void)args_json;
    (void)out_error;
    if (!gui_is_admin()) {
        return gui_ok_string(out_json, "--index 需要管理员权限。");
    }
    char summary[2048];
    cdc_run_index_summary(1, 0, summary, sizeof(summary));
    return gui_ok_string(out_json, summary);
}

static int h_index_rebuild(const char *args_json, char **out_json,
                           char **out_error) {
    (void)args_json;
    (void)out_error;
    if (!gui_is_admin()) {
        return gui_ok_string(out_json, "--index 需要管理员权限。");
    }
    char summary[2048];
    cdc_run_index_summary(1, 1, summary, sizeof(summary));
    return gui_ok_string(out_json, summary);
}

static int h_index_task_status(const char *args_json, char **out_json,
                               char **out_error) {
    (void)args_json;
    (void)out_error;
    int exists = 0, enabled = 0;
    cdc_indextask_task_status(&exists, &enabled);
    cJSON *o = cJSON_CreateObject();
    if (!o) return gui_fail(out_error, "内存不足");
    cJSON_AddBoolToObject(o, "exists", exists);
    cJSON_AddBoolToObject(o, "enabled", enabled);
    return gui_ok_json(out_json, o);
}

/* 计划任务命令统一返回 msg 字符串（成功与失败都是字符串，与 Rust 一致）。 */
static int gui_indextask_msg(char **out_json, char **out_error, int rc,
                             char *msg, size_t cap, cdc_err_t *err) {
    (void)cap;
    if (rc != 0 && (!msg || !msg[0]) && err && err->message[0]) {
        snprintf(msg, cap, "%s", err->message);
    }
    if (!msg) return gui_fail(out_error, "内存不足");
    return gui_ok_string(out_json, msg);
}

static int h_index_task_run(const char *args_json, char **out_json,
                            char **out_error) {
    (void)args_json;
    char msg[512] = "";
    cdc_err_t err;
    cdc_err_init(&err);
    int rc = cdc_indextask_run_task(msg, sizeof(msg), &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, sizeof(msg), &err);
}

static int h_index_task_set_enabled(const char *args_json, char **out_json,
                                    char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    int enabled = gui_arg_bool(args, "enabled", 0);
    cJSON_Delete(args);
    char msg[512] = "";
    cdc_err_t err;
    cdc_err_init(&err);
    int rc = cdc_indextask_set_task_enabled(enabled, msg, sizeof(msg), &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, sizeof(msg), &err);
}

static int h_index_task_register(const char *args_json, char **out_json,
                                 char **out_error) {
    (void)args_json;
    char msg[512] = "";
    cdc_err_t err;
    cdc_err_init(&err);
    int rc = cdc_indextask_register(msg, sizeof(msg), &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, sizeof(msg), &err);
}

static int h_index_task_delete(const char *args_json, char **out_json,
                               char **out_error) {
    (void)args_json;
    char msg[512] = "";
    cdc_err_t err;
    cdc_err_init(&err);
    int rc = cdc_indextask_delete_task(msg, sizeof(msg), &err);
    return gui_indextask_msg(out_json, out_error, rc, msg, sizeof(msg), &err);
}

/* ---------------- 系统 / 碎片 / 电源 ---------------- */

static int h_system_scan(const char *args_json, char **out_json,
                         char **out_error) {
    (void)args_json;
    char *begin_err = NULL;
    cdc_task_begin("system_scan", "系统清理扫描", 0, 0, &begin_err);
    free(begin_err);
    cdc_err_t err;
    cdc_system_item_t *items = NULL;
    size_t n = 0;
    if (cdc_scan_system_deep(&items, &n, &err) != CDC_OK) {
        cdc_task_finish("system_scan", "failed",
                        err.message[0] ? err.message : "系统扫描失败");
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "系统扫描失败");
    }
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "key", items[i].key);
            cJSON_AddStringToObject(o, "name", items[i].name);
            cJSON_AddStringToObject(o, "describe", items[i].describe);
            cJSON_AddNumberToObject(o, "size", (double)items[i].size);
            cJSON_AddNumberToObject(o, "count", (double)items[i].count);
            cJSON_AddBoolToObject(o, "safe_to_clean", items[i].safe_to_clean);
            cJSON_AddBoolToObject(o, "is_command", items[i].is_command);
            cJSON_AddItemToArray(arr, o);
        }
    }
    cdc_system_items_free(items, n);
    cdc_task_finish("system_scan", "ok", "系统扫描完成");
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static int h_system_clean(const char *args_json, char **out_json,
                          char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *key = gui_arg_str(args, "key");
    char key_buf[64] = "";
    if (key) snprintf(key_buf, sizeof(key_buf), "%s", key);
    cJSON_Delete(args);
    if (!key_buf[0]) return gui_fail(out_error, "缺少参数：key");
    int cancellable = strcmp(key_buf, "sys_update_dl") == 0 ||
                      strcmp(key_buf, "sys_crash_dumps") == 0;
    char *begin_err = NULL;
    if (cdc_task_begin("system", "系统清理", cancellable, 1, &begin_err) != 0) {
        char *msg = begin_err ? begin_err : gui_strdup("任务启动失败");
        free(begin_err);
        int r = gui_fail(out_error, "%s", msg);
        free(msg);
        return r;
    }
    cdc_err_t err;
    uint64_t freed = 0, count = 0;
    char *msg = NULL;
    if (cdc_clean_system_item(key_buf, &freed, &count, &msg, &err) != CDC_OK) {
        cdc_task_finish("system", "failed",
                        err.message[0] ? err.message : "系统清理失败");
        free(msg);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "系统清理失败");
    }
    cdc_task_finish("system", "ok", msg ? msg : "系统清理完成");
    int r = gui_ok_string(out_json, msg ? msg : "");
    free(msg);
    return r;
}

static int h_hiber_query(const char *args_json, char **out_json,
                         char **out_error) {
    (void)args_json;
    cdc_err_t err;
    cdc_hiber_info_t info;
    memset(&info, 0, sizeof(info));
    if (cdc_query_hiber(&info, &err) != CDC_OK) {
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "读取休眠状态失败");
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cdc_hiber_info_free(&info);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddBoolToObject(o, "enabled", info.enabled);
    cJSON_AddNumberToObject(o, "size", (double)info.size);
    cJSON_AddStringToObject(o, "path", info.path ? info.path : "");
    cJSON_AddStringToObject(o, "description",
                            info.description ? info.description : "");
    cdc_hiber_info_free(&info);
    return gui_ok_json(out_json, o);
}

static int h_hiber_set(const char *args_json, char **out_json,
                       char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *mode = gui_arg_str(args, "mode");
    char mode_buf[16] = "";
    if (mode) snprintf(mode_buf, sizeof(mode_buf), "%s", mode);
    cJSON_Delete(args);
    if (!mode_buf[0]) return gui_fail(out_error, "缺少参数：mode");
    cdc_err_t err;
    char *msg = NULL;
    if (cdc_set_hiber(mode_buf, &msg, &err) != CDC_OK) {
        free(msg);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "设置休眠失败");
    }
    int r = gui_ok_string(out_json, msg ? msg : "");
    free(msg);
    return r;
}

static int h_recycle_empty(const char *args_json, char **out_json,
                           char **out_error) {
    (void)args_json;
    char *begin_err = NULL;
    if (cdc_task_begin("recycle", "清空回收站", 0, 1, &begin_err) != 0) {
        char *msg = begin_err ? begin_err : gui_strdup("任务启动失败");
        free(begin_err);
        int r = gui_fail(out_error, "%s", msg);
        free(msg);
        return r;
    }
    cdc_err_t err;
    if (cdc_empty_all_recycle_bins(&err) != CDC_OK) {
        cdc_task_finish("recycle", "failed",
                        err.message[0] ? err.message : "清空回收站失败");
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "清空回收站失败");
    }
    cdc_task_finish("recycle", "ok", "回收站已清空");
    return gui_ok_null(out_json);
}

/* ---------------- 微信 / QQ ---------------- */

static int h_im_roots(const char *args_json, char **out_json,
                      char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    char kind_buf[16] = "";
    if (kind) snprintf(kind_buf, sizeof(kind_buf), "%s", kind);
    cJSON_Delete(args);
    if (!kind_buf[0]) return gui_fail(out_error, "缺少参数：kind");
    cdc_err_t err;
    char **roots = NULL;
    size_t n = 0;
    if (cdc_im_roots(kind_buf, &roots, &n, &err) != CDC_OK) {
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "读取微信/QQ 目录失败");
    }
    cJSON *arr = gui_str_array_json(roots, n);
    gui_free_str_array(roots, n);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static cJSON *gui_im_record_json(cdc_im_record_t *r) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "path", r->path ? r->path : "");
    cJSON_AddNumberToObject(o, "size", (double)r->size);
    cJSON_AddNumberToObject(o, "mtime", (double)r->mtime);
    cJSON_AddStringToObject(o, "kind", r->kind ? r->kind : "");
    cJSON_AddStringToObject(o, "sub", r->sub ? r->sub : "");
    cJSON_AddStringToObject(o, "month", r->month ? r->month : "");
    cJSON_AddStringToObject(o, "name", r->name ? r->name : "");
    return o;
}

static int h_im_scan(const char *args_json, char **out_json,
                     char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    char kind_buf[16] = "";
    if (kind) snprintf(kind_buf, sizeof(kind_buf), "%s", kind);
    cJSON_Delete(args);
    if (!kind_buf[0]) return gui_fail(out_error, "缺少参数：kind");
    char *begin_err = NULL;
    cdc_task_begin("im_scan", "扫描微信/QQ 数据", 0, 0, &begin_err);
    free(begin_err);
    cdc_err_t err;
    char **roots = NULL;
    size_t nroots = 0;
    if (cdc_im_roots(kind_buf, &roots, &nroots, &err) != CDC_OK) {
        cdc_task_finish("im_scan", "failed",
                        err.message[0] ? err.message : "读取目录失败");
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "读取目录失败");
    }
    cdc_im_record_t *records = NULL;
    size_t nrecords = 0;
    if (cdc_im_list_media((const char **)roots, nroots, &records, &nrecords,
                          &err) != CDC_OK) {
        gui_free_str_array(roots, nroots);
        cdc_task_finish("im_scan", "failed",
                        err.message[0] ? err.message : "扫描失败");
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "扫描失败");
    }
    gui_free_str_array(roots, nroots);
    cdc_im_remember_scanned(records, nrecords);
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < nrecords; i++) {
            cJSON *o = gui_im_record_json(&records[i]);
            if (o) cJSON_AddItemToArray(arr, o);
        }
    }
    cdc_im_records_free(records, nrecords);
    cdc_task_finish("im_scan", "ok", "扫描完成");
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static int h_im_preview(const char *args_json, char **out_json,
                        char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *path = gui_arg_str(args, "path");
    char *path_copy = path ? gui_strdup(path) : NULL;
    cJSON_Delete(args);
    if (!path_copy) return gui_fail(out_error, "缺少参数：path");
    if (!cdc_im_is_scanned(path_copy)) {
        free(path_copy);
        return gui_fail(out_error, "文件不在扫描结果里");
    }
    cdc_err_t err;
    cdc_im_preview_t pv;
    memset(&pv, 0, sizeof(pv));
    if (cdc_im_preview(path_copy, &pv, &err) != CDC_OK) {
        free(path_copy);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "预览失败");
    }
    free(path_copy);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cdc_im_preview_free(&pv);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddStringToObject(o, "text", pv.text ? pv.text : "");
    cJSON_AddBoolToObject(o, "binary", pv.binary);
    cJSON_AddBoolToObject(o, "truncated", pv.truncated);
    cJSON_AddNumberToObject(o, "size", (double)pv.size);
    cdc_im_preview_free(&pv);
    return gui_ok_json(out_json, o);
}

static int h_im_clean(const char *args_json, char **out_json,
                      char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    char kind_buf[16] = "";
    if (kind) snprintf(kind_buf, sizeof(kind_buf), "%s", kind);
    size_t nkinds = 0;
    char **kinds = gui_arg_str_array(args, "kinds", &nkinds);
    size_t npaths = 0;
    char **paths = gui_arg_str_array(args, "paths", &npaths);
    int has_paths = gui_arg(args, "paths") != NULL;
    cJSON_Delete(args);
    if (!kind_buf[0]) {
        gui_free_str_array(kinds, nkinds);
        gui_free_str_array(paths, npaths);
        return gui_fail(out_error, "缺少参数：kind");
    }
    char *begin_err = NULL;
    if (cdc_task_begin("im_clean", "微信/QQ 清理", 1, 1, &begin_err) != 0) {
        char *msg = begin_err ? begin_err : gui_strdup("任务启动失败");
        free(begin_err);
        int r = gui_fail(out_error, "%s", msg);
        free(msg);
        gui_free_str_array(kinds, nkinds);
        gui_free_str_array(paths, npaths);
        return r;
    }
    cdc_err_t err;
    cdc_delete_report_t report;
    memset(&report, 0, sizeof(report));
    if (cdc_im_clean(kind_buf, (const char **)kinds, nkinds,
                     (const char **)paths, npaths, has_paths, &report,
                     &err) != CDC_OK) {
        cdc_task_finish("im_clean", "failed",
                        err.message[0] ? err.message : "微信/QQ 清理失败");
        gui_free_str_array(kinds, nkinds);
        gui_free_str_array(paths, npaths);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "微信/QQ 清理失败");
    }
    {
        char summary[128];
        snprintf(summary, sizeof(summary), "删除 %llu 个，释放 %.1f MB",
                 (unsigned long long)report.deleted,
                 (double)report.freed / 1048576.0);
        cdc_task_finish("im_clean", "ok", summary);
    }
    cJSON *o = gui_delete_report_json(report.freed, report.deleted,
                                      report.failed, report.nfailed,
                                      report.reasons, report.nreasons);
    cdc_delete_report_free(&report);
    gui_free_str_array(kinds, nkinds);
    gui_free_str_array(paths, npaths);
    if (!o) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, o);
}

static int h_im_move_analyze(const char *args_json, char **out_json,
                             char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *kind = gui_arg_str(args, "kind");
    char kind_buf[16] = "";
    if (kind) snprintf(kind_buf, sizeof(kind_buf), "%s", kind);
    cJSON_Delete(args);
    if (!kind_buf[0]) return gui_fail(out_error, "缺少参数：kind");
    char *begin_err = NULL;
    cdc_task_begin("im_move_analyze", "分析搬家数据", 0, 0, &begin_err);
    free(begin_err);
    cdc_err_t err;
    cdc_move_analysis_t a;
    memset(&a, 0, sizeof(a));
    if (cdc_analyze_im_migration(kind_buf, &a, &err) != CDC_OK) {
        cdc_task_finish("im_move_analyze", "failed",
                        err.message[0] ? err.message : "分析搬家失败");
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "分析搬家失败");
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cdc_move_analysis_free(&a);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddStringToObject(o, "source", a.source ? a.source : "");
    cJSON_AddBoolToObject(o, "exists", a.exists);
    cJSON_AddNumberToObject(o, "size", (double)a.size);
    cJSON_AddNumberToObject(o, "file_count", (double)a.file_count);
    cJSON_AddBoolToObject(o, "is_junction", a.is_junction);
    cJSON *targets = cJSON_CreateArray();
    if (targets) {
        for (size_t i = 0; i < a.ntargets; i++) {
            cJSON *t = cJSON_CreateObject();
            if (!t) continue;
            cJSON_AddStringToObject(t, "letter",
                                    a.targets[i].letter ? a.targets[i].letter : "");
            cJSON_AddNumberToObject(t, "free_bytes",
                                    (double)a.targets[i].free_bytes);
            cJSON_AddNumberToObject(t, "total_bytes",
                                    (double)a.targets[i].total_bytes);
            cJSON_AddItemToArray(targets, t);
        }
    }
    cJSON_AddItemToObject(o, "target_drives", targets);
    cdc_move_analysis_free(&a);
    cdc_task_finish("im_move_analyze", "ok", "分析完成");
    return gui_ok_json(out_json, o);
}

static int h_im_move_execute(const char *args_json, char **out_json,
                             char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *source = gui_arg_str(args, "source");
    /* 前端发 targetDir，Rust 签名是 target_dir：两个名字都认。 */
    const char *target_dir = gui_arg_str2(args, "target_dir", "targetDir");
    const char *kind = gui_arg_str(args, "kind");
    char source_buf[1024] = "";
    char target_buf[1024] = "";
    char kind_buf[16] = "";
    if (source) snprintf(source_buf, sizeof(source_buf), "%s", source);
    if (target_dir) snprintf(target_buf, sizeof(target_buf), "%s", target_dir);
    if (kind) snprintf(kind_buf, sizeof(kind_buf), "%s", kind);
    cJSON_Delete(args);
    if (!source_buf[0]) return gui_fail(out_error, "缺少参数：source");
    if (!target_buf[0]) return gui_fail(out_error, "缺少参数：target_dir");
    if (!kind_buf[0]) return gui_fail(out_error, "缺少参数：kind");
    char *begin_err = NULL;
    if (cdc_task_begin("im_move", "微信/QQ 数据搬家", 1, 1, &begin_err) != 0) {
        char *msg = begin_err ? begin_err : gui_strdup("任务启动失败");
        free(begin_err);
        int r = gui_fail(out_error, "%s", msg);
        free(msg);
        return r;
    }
    cdc_err_t err;
    char *msg = NULL;
    if (cdc_execute_im_migration(source_buf, target_buf, kind_buf, &msg,
                                 &err) != CDC_OK) {
        cdc_task_finish("im_move", "failed",
                        err.message[0] ? err.message : "搬家失败");
        free(msg);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "搬家失败");
    }
    cdc_task_finish("im_move", "ok", msg ? msg : "搬家完成");
    int r = gui_ok_string(out_json, msg ? msg : "");
    free(msg);
    return r;
}

/* ---------------- 隐私 / 粉碎 / 历史 / 规则 / 路径 / 进程 ---------------- */

static int h_privacy_scan(const char *args_json, char **out_json,
                          char **out_error) {
    (void)args_json;
    char *begin_err = NULL;
    cdc_task_begin("privacy_scan", "隐私清理扫描", 0, 0, &begin_err);
    free(begin_err);
    cdc_err_t err;
    cdc_privacy_item_t *items = NULL;
    size_t n = 0;
    if (cdc_privacy_scan(&items, &n, &err) != CDC_OK) {
        cdc_task_finish("privacy_scan", "failed",
                        err.message[0] ? err.message : "隐私扫描失败");
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "隐私扫描失败");
    }
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "id", items[i].id);
            cJSON_AddStringToObject(o, "section", items[i].section);
            cJSON_AddStringToObject(o, "name", items[i].name);
            cJSON_AddNumberToObject(o, "count", (double)items[i].count);
            cJSON_AddBoolToObject(o, "count_available", items[i].count_available);
            cJSON_AddItemToObject(o, "files",
                                  gui_str_array_json(items[i].files, items[i].nfiles));
            cJSON_AddStringToObject(o, "action", items[i].action);
            cJSON_AddStringToObject(o, "note", items[i].note);
            cJSON_AddItemToArray(arr, o);
        }
    }
    cdc_privacy_items_free(items, n);
    cdc_task_finish("privacy_scan", "ok", "隐私扫描完成");
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static int h_privacy_clean(const char *args_json, char **out_json,
                           char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    size_t nids = 0;
    char **ids = gui_arg_str_array(args, "ids", &nids);
    cJSON_Delete(args);
    if (!ids || nids == 0) {
        gui_free_str_array(ids, nids);
        return gui_fail(out_error, "缺少参数：ids");
    }
    char *begin_err = NULL;
    if (cdc_task_begin("privacy", "隐私清理", 1, 1, &begin_err) != 0) {
        char *msg = begin_err ? begin_err : gui_strdup("任务启动失败");
        free(begin_err);
        int r = gui_fail(out_error, "%s", msg);
        free(msg);
        gui_free_str_array(ids, nids);
        return r;
    }
    cdc_err_t err;
    cdc_privacy_report_t report;
    memset(&report, 0, sizeof(report));
    if (cdc_privacy_clean((const char **)ids, nids, &report, &err) != CDC_OK) {
        cdc_task_finish("privacy", "failed",
                        err.message[0] ? err.message : "隐私清理失败");
        gui_free_str_array(ids, nids);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "隐私清理失败");
    }
    gui_free_str_array(ids, nids);
    {
        char summary[128];
        snprintf(summary, sizeof(summary), "清理 %lld 个文件",
                 (long long)report.files);
        cdc_task_finish("privacy", "ok", summary);
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cdc_privacy_report_free(&report);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddNumberToObject(o, "files", (double)report.files);
    cJSON_AddNumberToObject(o, "sql", (double)report.sql);
    cJSON_AddNumberToObject(o, "registry", (double)report.registry);
    cJSON_AddItemToObject(o, "failed",
                          gui_str_array_json(report.failed, report.nfailed));
    cdc_privacy_report_free(&report);
    return gui_ok_json(out_json, o);
}

static int h_shred(const char *args_json, char **out_json,
                   char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    size_t npaths = 0;
    char **paths = gui_arg_str_array(args, "paths", &npaths);
    uint64_t passes = gui_arg_u64(args, "passes", 1);
    cJSON_Delete(args);
    if (!paths || npaths == 0) {
        gui_free_str_array(paths, npaths);
        return gui_fail(out_error, "缺少参数：paths");
    }
    if (passes < 1) passes = 1;
    if (passes > 3) passes = 3;
    char *begin_err = NULL;
    if (cdc_task_begin("shred", "文件粉碎", 1, 1, &begin_err) != 0) {
        /* Rust 的 shred：begin 失败时返回带 errors 的空报告，不是 Err。 */
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            free(begin_err);
            gui_free_str_array(paths, npaths);
            return gui_fail(out_error, "内存不足");
        }
        cJSON_AddNumberToObject(o, "freed", 0);
        cJSON_AddNumberToObject(o, "files_ok", 0);
        cJSON_AddNumberToObject(o, "files_failed", 0);
        cJSON_AddNumberToObject(o, "finished", 0);
        cJSON *errs = cJSON_CreateArray();
        if (errs) {
            cJSON_AddItemToArray(
                errs, cJSON_CreateString(begin_err ? begin_err : "任务启动失败"));
        }
        cJSON_AddItemToObject(o, "errors", errs);
        free(begin_err);
        gui_free_str_array(paths, npaths);
        return gui_ok_json(out_json, o);
    }
    cdc_task_set_unit("shred", "个文件");
    cdc_task_set_stage("shred", "粉碎文件");
    cdc_err_t err;
    cdc_shred_report_t report;
    memset(&report, 0, sizeof(report));
    if (cdc_shred_paths((const char **)paths, npaths, (uint32_t)passes,
                        &report, &err) != CDC_OK) {
        cdc_task_finish("shred", "failed",
                        err.message[0] ? err.message : "粉碎失败");
        gui_free_str_array(paths, npaths);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "粉碎失败");
    }
    gui_free_str_array(paths, npaths);
    {
        char summary[128];
        snprintf(summary, sizeof(summary), "成功 %llu 个，失败 %llu 个，释放 %.1f MB",
                 (unsigned long long)report.files_ok,
                 (unsigned long long)report.files_failed,
                 (double)report.freed / 1048576.0);
        cdc_task_finish("shred",
                        cdc_task_cancelled("shred") ? "cancelled" : "ok",
                        summary);
    }
    {
        char *stamp = cdc_human_stamp(cdc::now_seconds());
        char line[512];
        snprintf(line, sizeof(line), "%s\tok\t成功 %llu 个，失败 %llu 个",
                 stamp ? stamp : "", (unsigned long long)report.files_ok,
                 (unsigned long long)report.files_failed);
        free(stamp);
        cdc_append_line("shred_history.log", line);
    }
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        cdc_shred_report_free(&report);
        return gui_fail(out_error, "内存不足");
    }
    cJSON_AddNumberToObject(o, "freed", (double)report.freed);
    cJSON_AddNumberToObject(o, "files_ok", (double)report.files_ok);
    cJSON_AddNumberToObject(o, "files_failed", (double)report.files_failed);
    cJSON_AddNumberToObject(o, "finished", (double)report.finished);
    cJSON_AddItemToObject(o, "errors",
                          gui_str_array_json(report.errors, report.nerrors));
    cdc_shred_report_free(&report);
    return gui_ok_json(out_json, o);
}

static int h_history_list(const char *args_json, char **out_json,
                          char **out_error) {
    (void)args_json;
    cdc_err_t err;
    cdc_log_entry_t *entries = NULL;
    size_t n = 0;
    if (cdc_logs_list(&entries, &n, &err) != CDC_OK) {
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "读取日志列表失败");
    }
    cJSON *arr = cJSON_CreateArray();
    if (arr) {
        for (size_t i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "name", entries[i].name);
            cJSON_AddNumberToObject(o, "size", (double)entries[i].size);
            cJSON_AddNumberToObject(o, "mtime", (double)entries[i].mtime);
            cJSON_AddItemToArray(arr, o);
        }
    }
    cdc_logs_list_free(entries, n);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static int h_history_read(const char *args_json, char **out_json,
                          char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *name = gui_arg_str(args, "name");
    char name_buf[256] = "";
    if (name) snprintf(name_buf, sizeof(name_buf), "%s", name);
    cJSON_Delete(args);
    if (!name_buf[0]) return gui_fail(out_error, "缺少参数：name");
    cdc_err_t err;
    char *text = NULL;
    if (cdc_log_read(name_buf, &text, &err) != CDC_OK) {
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "读取日志失败");
    }
    int r = gui_ok_string(out_json, text ? text : "");
    free(text);
    return r;
}

static int h_rules_update(const char *args_json, char **out_json,
                          char **out_error) {
    (void)args_json;
    char *begin_err = NULL;
    if (cdc_task_begin("rules", "更新清理规则", 0, 0, &begin_err) != 0) {
        char *msg = begin_err ? begin_err : gui_strdup("任务启动失败");
        free(begin_err);
        int r = gui_fail(out_error, "%s", msg);
        free(msg);
        return r;
    }
    cdc_err_t err;
    cdc_rules_update_result_t result;
    memset(&result, 0, sizeof(result));
    if (cdc_rules_update(&result, &err) != CDC_OK) {
        cdc_task_finish("rules", "failed",
                        err.message[0] ? err.message : "更新规则失败");
        free(result.message);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "更新规则失败");
    }
    char buf[2048];
    snprintf(buf, sizeof(buf), "新增 %llu 条，共 %llu 条。%s\n下次扫描生效。",
             (unsigned long long)result.accepted,
             (unsigned long long)result.total,
             result.message ? result.message : "");
    free(result.message);
    cdc_task_finish("rules", "ok", buf);
    return gui_ok_string(out_json, buf);
}

static int h_rules_rollback(const char *args_json, char **out_json,
                            char **out_error) {
    (void)args_json;
    cdc_err_t err;
    char *msg = NULL;
    if (cdc_rules_rollback(&msg, &err) != CDC_OK) {
        free(msg);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "回滚规则失败");
    }
    int r = gui_ok_string(out_json, msg ? msg : "");
    free(msg);
    return r;
}

static int h_reveal_path(const char *args_json, char **out_json,
                         char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *path = gui_arg_str(args, "path");
    char *path_copy = path ? gui_strdup(path) : NULL;
    cJSON_Delete(args);
    if (!path_copy) return gui_fail(out_error, "缺少参数：path");
    cdc_err_t err;
    int rc = cdc_reveal_in_explorer(path_copy, &err);
    free(path_copy);
    if (rc != CDC_OK) {
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "定位失败");
    }
    return gui_ok_null(out_json);
}

static int h_open_path(const char *args_json, char **out_json,
                       char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *path = gui_arg_str(args, "path");
    char *path_copy = path ? gui_strdup(path) : NULL;
    cJSON_Delete(args);
    if (!path_copy) return gui_fail(out_error, "缺少参数：path");
    cdc_err_t err;
    int rc = cdc_open_path(path_copy, &err);
    free(path_copy);
    if (rc != CDC_OK) {
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "打开失败");
    }
    return gui_ok_null(out_json);
}

static int h_check_processes(const char *args_json, char **out_json,
                             char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    size_t ntargets = 0;
    char **targets = gui_arg_str_array(args, "targets", &ntargets);
    cJSON_Delete(args);
    if (!targets || ntargets == 0) {
        gui_free_str_array(targets, ntargets);
        return gui_fail(out_error, "缺少参数：targets");
    }
    cdc_err_t err;
    char **running = NULL;
    size_t nrunning = 0;
    if (cdc_check_running((const char **)targets, ntargets, &running,
                          &nrunning, &err) != CDC_OK) {
        gui_free_str_array(targets, ntargets);
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "检测进程失败");
    }
    gui_free_str_array(targets, ntargets);
    cJSON *arr = gui_str_array_json(running, nrunning);
    gui_free_str_array(running, nrunning);
    if (!arr) return gui_fail(out_error, "内存不足");
    return gui_ok_json(out_json, arr);
}

static int h_kill_process(const char *args_json, char **out_json,
                          char **out_error) {
    cJSON *args = gui_parse_args(args_json);
    const char *name = gui_arg_str(args, "name");
    char name_buf[128] = "";
    if (name) snprintf(name_buf, sizeof(name_buf), "%s", name);
    cJSON_Delete(args);
    if (!name_buf[0]) return gui_fail(out_error, "缺少参数：name");
    cdc_err_t err;
    size_t killed = 0;
    if (cdc_kill_process_by_name(name_buf, &killed, &err) != CDC_OK) {
        return gui_fail(out_error, "%s",
                        err.message[0] ? err.message : "结束进程失败");
    }
    return gui_ok_u64(out_json, (uint64_t)killed);
}

/* ---------------- 进度回调（注册给各后端模块） ---------------- */

static void gui_hook_progress(void *ctx, const char *task, uint64_t done,
                              uint64_t total) {
    (void)ctx;
    cdc_task_set_progress(task, done, total);
}

static void gui_hook_stage(void *ctx, const char *task, const char *stage) {
    (void)ctx;
    cdc_task_set_stage(task, stage);
}

static void gui_hook_unit(void *ctx, const char *task, const char *unit) {
    (void)ctx;
    cdc_task_set_unit(task, unit);
}

static int gui_hook_cancel(void *ctx, const char *task) {
    (void)ctx;
    return cdc_task_cancelled(task);
}

static void gui_hook_invalidate(void *ctx) {
    (void)ctx;
    cdc_gui_invalidate_dup_cache();
}

/* ---------------- 命令表 ---------------- */

typedef struct {
    const char *name;
    cdc_command_handler handler;
} gui_command_t;

static const gui_command_t kGuiCommands[] = {
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

int cdc_gui_register_commands(void) {
    /* 进度/取消回调：各模块注册时做值拷贝，栈上结构体即可。 */
    cdc_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.progress = gui_hook_progress;
    hooks.stage = gui_hook_stage;
    hooks.unit = gui_hook_unit;
    hooks.cancel = gui_hook_cancel;
    hooks.invalidate_dup_cache = gui_hook_invalidate;
    hooks.ctx = NULL;
    cdc_shred_set_hooks(&hooks);
    cdc_privacy_set_hooks(&hooks);
    cdc_system_set_hooks(&hooks);
    cdc_im_set_hooks(&hooks);
    size_t n = sizeof(kGuiCommands) / sizeof(kGuiCommands[0]);
    for (size_t i = 0; i < n; i++) {
        if (cdc_bridge_register(kGuiCommands[i].name,
                                kGuiCommands[i].handler) != 0) {
            return -1;
        }
    }
    return 0;
}

#endif /* _WIN32 */

/* ---------------- Linux 下的空操作 ---------------- */

#ifndef _WIN32
int cdc_gui_register_commands(void) {
    return 0;
}
#endif
