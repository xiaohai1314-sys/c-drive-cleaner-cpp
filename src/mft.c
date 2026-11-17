/* mft.c — $MFT 解析 + 快照 + USN Journal 增量，逐行对照 desktop/src/mft.rs。
 *
 * 并行性说明（与 Rust 版的行为差异，仅影响速度不影响结果）：
 *   - MFT 块解析、USN 变更重读、快照后台重建里的并行在 C 版是顺序执行
 *     （Rayon/线程池尚未接入）；解析与读盘也不重叠。
 *   - 快照后台落盘仍走独立线程（与 Rust 的 save_snapshot_in_background 一致）。
 *
 * 文件内动态数组用 util.h 的 cdc_vec_t（ARCHITECTURE.md 契约）：
 *   - 纯内部累加器直接用 cdc_vec_t；
 *   - 存储形状由公共结构体定死（CdcFileEntryList、CdcSnapshot）
 *     的数组，用 vec_grow_raw 经 cdc_vec_reserve 扩容，增长策略仍在 util.c。
 * Windows-only 的静态辅助（u64 哈希表、互斥/线程、内部快照、resolve_dir）
 * 用 #ifdef _WIN32 隔离，Linux 下不编译（否则 -Werror=unused-function 报错）。
 */
#include "mft.h"

#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------- 小工具 ---------------- */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    return p;
}

/* 裸数组扩容（公共结构体的 items/n/cap 形状不能改成 cdc_vec_t 时用）：
 * 用栈上构造的 cdc_vec_t 视图驱动 cdc_vec_reserve，倍增策略在 util.c；
 * 失败时原数组不动。need_len 为扩容前已有的元素个数。 */
static int vec_grow_raw(void **data, size_t *cap, size_t elem_size, size_t need_len) {
    cdc_vec_t v;
    v.data = *data;
    v.len = need_len;
    v.cap = *cap;
    v.elem_size = elem_size;
    if (cdc_vec_reserve(&v, 1) != CDC_OK) return -1;
    *data = v.data;
    *cap = v.cap;
    return 0;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ASCII 小写比较（skip_top 表全是 ASCII，与 Rust 的 to_lowercase 在这批词上等价）。 */
static int ascii_lower_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int name_in_skip_top(const char *name, const char *const *skip_top, size_t n_skip) {
    for (size_t i = 0; i < n_skip; i++)
        if (ascii_lower_eq(name, skip_top[i])) return 1;
    return 0;
}

/* 把 UTF-8 字符串原地转小写（仅 ASCII 段；Rust 的 to_lowercase 在非 ASCII 段的行为
 * 这里不模拟——路径比较只用于 skip_top / root 前缀，都是 ASCII 词）。
 * 仅 Windows 的 resolve_dir 用（Linux 下不编译，否则 -Werror=unused-function）。 */
#ifdef _WIN32
static void ascii_lower_in_place(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}
#endif

const char *const CDC_TOP_SKIP[] = {
    "$recycle.bin",
    "system volume information",
    "windows",
    "program files",
    "program files (x86)",
    "programdata",
    "recovery",
    "perflogs",
    "msocache",
    "config.msi",
    "$windows.~bt",
    "$windows.~ws",
    "windows.old",
};
const size_t CDC_TOP_SKIP_N = sizeof(CDC_TOP_SKIP) / sizeof(CDC_TOP_SKIP[0]);

/* cdc_cache_dir 的唯一实现在 util.c（此前这里的版本把"盘"写了两遍，
 * "C盘盘清理工具"，2026-11-17 切片 4 端到端验证发现；已删）。 */

uint64_t cdc_now_seconds(void) {
    return (uint64_t)time(NULL);
}

/* ---------------- u64 -> uintptr_t 开放寻址哈希表 ---------------- */

/* 仅 Windows 的 $MFT/USN 路径用（内部快照索引、resolve_dir 缓存；Linux 下不编译，
 * 否则 -Werror=unused-function）。 */
#ifdef _WIN32

typedef struct {
    uint64_t *keys;
    uintptr_t *vals;
    uint8_t *used;
    size_t cap, len;
} CdcU64Map;

static uint64_t hash_u64(uint64_t x) {
    /* splitmix64 */
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static int u64map_init(CdcU64Map *m, size_t cap_hint) {
    size_t cap = 16;
    while (cap < cap_hint * 2 && cap < (1u << 24)) cap *= 2;
    m->keys = xmalloc(cap * sizeof(uint64_t));
    m->vals = xmalloc(cap * sizeof(uintptr_t));
    m->used = xmalloc(cap);
    if (!m->keys || !m->vals || !m->used) {
        free(m->keys);
        free(m->vals);
        free(m->used);
        m->keys = NULL;
        m->vals = NULL;
        m->used = NULL;
        m->cap = m->len = 0;
        return -1;
    }
    memset(m->used, 0, cap);
    m->cap = cap;
    m->len = 0;
    return 0;
}

static void u64map_free(CdcU64Map *m) {
    free(m->keys);
    free(m->vals);
    free(m->used);
    m->keys = NULL;
    m->vals = NULL;
    m->used = NULL;
    m->cap = m->len = 0;
}

static int u64map_grow(CdcU64Map *m) {
    size_t ncap = m->cap * 2;
    uint64_t *nkeys = xmalloc(ncap * sizeof(uint64_t));
    uintptr_t *nvals = xmalloc(ncap * sizeof(uintptr_t));
    uint8_t *nused = xmalloc(ncap);
    if (!nkeys || !nvals || !nused) {
        free(nkeys);
        free(nvals);
        free(nused);
        return -1;
    }
    memset(nused, 0, ncap);
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->used[i]) continue;
        size_t j = (size_t)hash_u64(m->keys[i]) & (ncap - 1);
        while (nused[j]) j = (j + 1) & (ncap - 1);
        nused[j] = 1;
        nkeys[j] = m->keys[i];
        nvals[j] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    free(m->used);
    m->keys = nkeys;
    m->vals = nvals;
    m->used = nused;
    m->cap = ncap;
    return 0;
}

static int u64map_put(CdcU64Map *m, uint64_t key, uintptr_t val) {
    if (m->cap == 0 && u64map_init(m, 16) != 0) return -1;
    if ((m->len + 1) * 10 > m->cap * 7 && u64map_grow(m) != 0) return -1;
    size_t i = (size_t)hash_u64(key) & (m->cap - 1);
    while (m->used[i]) {
        if (m->keys[i] == key) {
            m->vals[i] = val;
            return 0;
        }
        i = (i + 1) & (m->cap - 1);
    }
    m->used[i] = 1;
    m->keys[i] = key;
    m->vals[i] = val;
    m->len++;
    return 0;
}

static int u64map_get(const CdcU64Map *m, uint64_t key, uintptr_t *val) {
    if (m->cap == 0) return 0;
    size_t i = (size_t)hash_u64(key) & (m->cap - 1);
    while (m->used[i]) {
        if (m->keys[i] == key) {
            if (val) *val = m->vals[i];
            return 1;
        }
        i = (i + 1) & (m->cap - 1);
    }
    return 0;
}

static int u64map_remove(CdcU64Map *m, uint64_t key) {
    if (m->cap == 0) return 0;
    size_t i = (size_t)hash_u64(key) & (m->cap - 1);
    while (m->used[i]) {
        if (m->keys[i] == key) {
            /* 无 tombstone 的删除：把簇内后续元素逐个重插。 */
            m->used[i] = 0;
            m->len--;
            size_t j = (i + 1) & (m->cap - 1);
            while (m->used[j]) {
                uint64_t k = m->keys[j];
                uintptr_t v = m->vals[j];
                m->used[j] = 0;
                m->len--;
                u64map_put(m, k, v);
                j = (j + 1) & (m->cap - 1);
            }
            return 1;
        }
        i = (i + 1) & (m->cap - 1);
    }
    return 0;
}

#endif /* _WIN32 */

/* ---------------- 互斥与脱离线程 ---------------- */

/* 仅 Windows 的快照缓存/后台落盘用（Linux 下不编译，否则 -Werror=unused-function）。
 * pthread 分支已删：互斥只在 Windows 路径使用。 */
#ifdef _WIN32
typedef CRITICAL_SECTION CdcMutex;
static void cdc_mutex_init(CdcMutex *m) { InitializeCriticalSection(m); }
static void cdc_mutex_lock(CdcMutex *m) { EnterCriticalSection(m); }
static void cdc_mutex_unlock(CdcMutex *m) { LeaveCriticalSection(m); }
#endif

/* 首次使用必定在主线程（后台线程由主线程 spawn），惰性初始化是安全的。
 * 仅 Windows 的后台快照落盘/重建用（Linux 下不编译，否则 -Werror=unused-function）。 */
#ifdef _WIN32
typedef struct {
    void (*run)(void *);
    void *arg;
} ThreadThunk;

#ifdef _WIN32
static DWORD WINAPI thread_entry_win(LPVOID p) {
    ThreadThunk *t = (ThreadThunk *)p;
    t->run(t->arg);
    free(t);
    return 0;
}
#else
static void *thread_entry_posix(void *p) {
    ThreadThunk *t = (ThreadThunk *)p;
    t->run(t->arg);
    free(t);
    return NULL;
}
#endif

static void spawn_detached_thread(void (*run)(void *), void *arg) {
    ThreadThunk *t = xmalloc(sizeof *t);
    if (!t) {
        run(arg); /* OOM：退化为同步执行，保证不丢动作 */
        return;
    }
    t->run = run;
    t->arg = arg;
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, thread_entry_win, t, 0, NULL);
    if (h) {
        CloseHandle(h);
    } else {
        free(t);
        run(arg);
    }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, thread_entry_posix, t) == 0) {
        pthread_detach(th);
    } else {
        free(t);
        run(arg);
    }
#endif
}

#endif /* _WIN32 */

/* ---------------- FileEntry 列表 ---------------- */

static int entry_list_push(CdcFileEntryList *list, CdcFileEntry e) {
    if (list->n == list->cap) {
        void *data = list->items;
        if (vec_grow_raw(&data, &list->cap, sizeof(CdcFileEntry), list->n) != 0)
            return -1;
        list->items = data;
    }
    list->items[list->n++] = e;
    return 0;
}

void cdc_file_entry_list_free(CdcFileEntryList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->n; i++) free(list->items[i].path);
    free(list->items);
    list->items = NULL;
    list->n = list->cap = 0;
}

/* ---------------- parse_record_in_place ---------------- */

void cdc_parsed_record_free(CdcParsedRecord *r) {
    if (!r) return;
    free(r->name);
    free(r->runs);
    r->name = NULL;
    r->runs = NULL;
    r->n_runs = 0;
}

int cdc_parse_record_in_place(uint8_t *rec, size_t rec_len, uint64_t index,
                              size_t bytes_per_sector, size_t bytes_per_cluster,
                              uint64_t min_size, CdcParsedRecord *out) {
    memset(out, 0, sizeof *out);
    out->kind = CDC_REC_SKIP;
    out->index = index;
    if (rec_len < 48 || memcmp(rec, "FILE", 4) != 0) return 0;
    mft_apply_fixups(rec, rec_len, bytes_per_sector);

    uint16_t flags = 0;
    if (!mft_u16_at(rec, rec_len, 22, &flags)) return 0;
    if ((flags & 1) == 0) return 0; /* 未在使用中 */
    if (index != 0) {
        uint64_t base = 0;
        if (mft_u64_at(rec, rec_len, 32, &base) && base != 0) return 0; /* 扩展记录 */
    }

    int is_dir = (flags & 2) != 0;
    uint16_t sequence = 0;
    if (!mft_u16_at(rec, rec_len, 16, &sequence)) return 0;
    uint64_t frn = ((uint64_t)sequence << 48) | index;
    uint16_t first_attr = 0;
    if (!mft_u16_at(rec, rec_len, 20, &first_attr)) return 0;

    /* 只记名字字节切片，等确认 size >= min_size 后再解码 UTF-16，
     * 避免百万小文件无效堆分配（与 Rust 版同款剪枝）。 */
    int have_name_span = 0;
    size_t name_off = 0, name_byte_len = 0;
    int name_is_dos = 0;
    uint64_t parent_frn = 0;
    uint64_t size = 0, mtime = 0;
    uint32_t attrs = 0;
    uint16_t data_flags = 0;
    uint64_t first_lcn = 0, first_run_clusters = 0;
    MftRun *mft_runs = NULL;
    size_t n_mft_runs = 0;

    size_t pos = first_attr;
    while (pos + 8 <= rec_len) {
        uint32_t attr_type = 0;
        if (!mft_u32_at(rec, rec_len, pos, &attr_type)) break;
        if (attr_type == 0xFFFFFFFFu) break;
        uint32_t attr_len = 0;
        if (!mft_u32_at(rec, rec_len, pos + 4, &attr_len)) break;
        if (attr_len < 16 || pos + attr_len > rec_len) break;
        uint8_t non_resident = rec[pos + 8];
        uint8_t name_len = rec[pos + 9];

        if (attr_type == 0x10 && non_resident == 0) {
            /* $STANDARD_INFORMATION */
            uint16_t val_off = 0;
            if (mft_u16_at(rec, rec_len, pos + 20, &val_off)) {
                size_t v = pos + val_off;
                if (v + 36 <= pos + attr_len) {
                    uint64_t ft = 0;
                    if (mft_u64_at(rec, rec_len, v + 8, &ft)) mtime = mft_filetime_to_unix(ft);
                    uint32_t a = 0;
                    if (mft_u32_at(rec, rec_len, v + 32, &a)) {
                        attrs = a;
                        if (!is_dir && (attrs & CDC_MFT_SKIP_FILE_ATTRS) != 0) {
                            free(mft_runs);
                            return 0;
                        }
                    }
                }
            }
        } else if (attr_type == 0x30 && non_resident == 0) {
            /* $FILE_NAME */
            uint16_t val_off = 0;
            if (mft_u16_at(rec, rec_len, pos + 20, &val_off)) {
                size_t v = pos + val_off;
                if (v + 66 <= pos + attr_len) {
                    uint64_t parent_ref = 0;
                    if (mft_u64_at(rec, rec_len, v, &parent_ref)) {
                        size_t nlen = rec[v + 64];
                        uint8_t namespace = rec[v + 65];
                        size_t noff = v + 66;
                        size_t blen = nlen * 2;
                        if (noff + blen <= pos + attr_len) {
                            int is_dos = namespace == 2;
                            if (!have_name_span || (name_is_dos && !is_dos) ||
                                namespace == 0 || namespace == 1) {
                                have_name_span = 1;
                                name_off = noff;
                                name_byte_len = blen;
                                parent_frn = parent_ref;
                                name_is_dos = is_dos;
                            }
                        }
                    }
                }
            }
        } else if (attr_type == 0x80 && name_len == 0) {
            /* $DATA（未命名） */
            data_flags = 0;
            uint16_t df = 0;
            if (mft_u16_at(rec, rec_len, pos + 12, &df)) data_flags = df;
            if (non_resident != 0) {
                uint64_t lowest_vcn = 0;
                if (pos + 56 <= rec_len &&
                    mft_u64_at(rec, rec_len, pos + 16, &lowest_vcn) && lowest_vcn == 0) {
                    uint64_t real_size = 0;
                    if (mft_u64_at(rec, rec_len, pos + 48, &real_size)) size = real_size;
                    uint16_t run_off = 0;
                    if (mft_u16_at(rec, rec_len, pos + 32, &run_off)) {
                        size_t start = pos + run_off;
                        if (start <= pos + attr_len) {
                            if (index == 0) {
                                free(mft_runs);
                                mft_runs = mft_parse_runlist(rec + start,
                                                            pos + attr_len - start, &n_mft_runs);
                            } else {
                                mft_parse_first_extent(rec + start, pos + attr_len - start,
                                                       &first_lcn, &first_run_clusters);
                            }
                        }
                    }
                }
            } else {
                uint32_t vlen = 0;
                if (mft_u32_at(rec, rec_len, pos + 16, &vlen)) size = vlen;
            }
        }

        pos += attr_len;
    }

    if (index == 0) {
        if (mft_runs && n_mft_runs > 0) {
            out->kind = CDC_REC_MFT_ROOT;
            out->runs = mft_runs;
            out->n_runs = n_mft_runs;
            return 0;
        }
        free(mft_runs);
        return 0;
    }

    if (!have_name_span) {
        free(mft_runs);
        return 0;
    }

    char *name = mft_utf16le_to_utf8(rec + name_off, name_byte_len);
    if (!name) {
        free(mft_runs);
        return -1;
    }

    if (is_dir) {
        out->kind = CDC_REC_DIR;
        out->name = name;
        out->parent_frn = parent_frn;
        out->seq = sequence;
        out->reparse = (attrs & CDC_FILE_ATTR_REPARSE_POINT) != 0;
        free(mft_runs);
        return 0;
    }

    if (size < min_size || (attrs & CDC_MFT_SKIP_FILE_ATTRS) != 0) {
        free(name);
        free(mft_runs);
        return 0;
    }
    if (name[0] == '$') {
        free(name);
        free(mft_runs);
        return 0;
    }

    uint64_t first_extent_bytes;
    if (first_run_clusters > UINT64_MAX / (uint64_t)bytes_per_cluster)
        first_extent_bytes = UINT64_MAX;
    else
        first_extent_bytes = first_run_clusters * (uint64_t)bytes_per_cluster;
    uint64_t need = size < 4096 ? size : 4096;
    int can_raw_read = (attrs & CDC_MFT_NO_RAW_ATTRS) == 0 &&
                       (data_flags & (0x0001u | 0x4000u | 0x8000u)) == 0 &&
                       first_lcn > 0 && first_extent_bytes >= need;

    out->kind = CDC_REC_FILE;
    out->name = name;
    out->frn = frn;
    out->parent_frn = parent_frn;
    out->size = size;
    out->mtime = mtime;
    out->first_lcn = first_lcn;
    out->sort_key = first_lcn > 0 ? first_lcn : index;
    out->can_raw_read = can_raw_read;
    free(mft_runs);
    return 0;
}

/* ---------------- 快照编解码（与 Rust 版字节一致） ---------------- */

static void put_u16_le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get_u16_le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

typedef struct {
    FILE *f;
    int err;
} SnapWriter;

static void w_bytes(SnapWriter *w, const void *p, size_t n) {
    if (!w->err && fwrite(p, 1, n, w->f) != n) w->err = 1;
}
static void w_u16(SnapWriter *w, uint16_t v) {
    uint8_t b[2];
    put_u16_le(b, v);
    w_bytes(w, b, 2);
}
static void w_u32(SnapWriter *w, uint32_t v) {
    uint8_t b[4];
    put_u32_le(b, v);
    w_bytes(w, b, 4);
}
static void w_u64(SnapWriter *w, uint64_t v) {
    uint8_t b[8];
    put_u64_le(b, v);
    w_bytes(w, b, 8);
}
static void w_str(SnapWriter *w, const char *s) {
    uint32_t n = (uint32_t)strlen(s ? s : "");
    w_u32(w, n);
    if (n) w_bytes(w, s, n);
}

typedef struct {
    FILE *f;
    int err;
} SnapReader;

static int r_bytes(SnapReader *r, void *p, size_t n) {
    if (r->err) return 0;
    if (fread(p, 1, n, r->f) != n) {
        r->err = 1;
        return 0;
    }
    return 1;
}
static int r_u16(SnapReader *r, uint16_t *v) {
    uint8_t b[2];
    if (!r_bytes(r, b, 2)) return 0;
    *v = get_u16_le(b);
    return 1;
}
static int r_u32(SnapReader *r, uint32_t *v) {
    uint8_t b[4];
    if (!r_bytes(r, b, 4)) return 0;
    *v = get_u32_le(b);
    return 1;
}
static int r_u64(SnapReader *r, uint64_t *v) {
    uint8_t b[8];
    if (!r_bytes(r, b, 8)) return 0;
    *v = get_u64_le(b);
    return 1;
}
static char *r_str(SnapReader *r) {
    uint32_t n = 0;
    if (!r_u32(r, &n)) return NULL;
    if (n > 32 * 1024) {
        r->err = 1;
        return NULL;
    }
    char *s = xmalloc(n + 1);
    if (!s) {
        r->err = 1;
        return NULL;
    }
    if (n && !r_bytes(r, s, n)) {
        free(s);
        return NULL;
    }
    s[n] = '\0';
    return s;
}

int cdc_snapshot_save(const char *path, const CdcSnapshot *snap) {
    size_t n = strlen(path) + 5;
    char *tmp = xmalloc(n);
    if (!tmp) return -1;
    snprintf(tmp, n, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return -1;
    }
    SnapWriter w = {f, 0};
    w_bytes(&w, "MFT3", 4);
    w_u64(&w, snap->volume_serial);
    w_u64(&w, snap->journal_id);
    w_u64(&w, (uint64_t)snap->next_usn);
    w_u64(&w, snap->min_size);
    w_u32(&w, snap->bytes_per_cluster);
    w_u32(&w, (uint32_t)snap->n_dirs);
    for (size_t i = 0; i < snap->n_dirs; i++) {
        const CdcDirNode *d = &snap->dirs[i];
        w_u64(&w, d->index);
        w_u16(&w, d->seq);
        uint8_t rep = d->reparse ? 1 : 0;
        w_bytes(&w, &rep, 1);
        w_u64(&w, d->parent_frn);
        w_str(&w, d->name);
    }
    w_u32(&w, (uint32_t)snap->n_files);
    for (size_t i = 0; i < snap->n_files; i++) {
        const CdcRawFile *rf = &snap->files[i];
        w_u64(&w, rf->frn & 0xFFFFFFFFFFFFULL);
        w_u64(&w, rf->frn);
        w_u64(&w, rf->parent_frn);
        w_u64(&w, rf->size);
        w_u64(&w, rf->mtime);
        w_u64(&w, rf->first_lcn);
        w_u64(&w, rf->sort_key);
        uint8_t raw = rf->can_raw_read ? 1 : 0;
        w_bytes(&w, &raw, 1);
        w_str(&w, rf->name);
    }
    if (fclose(f) != 0) w.err = 1;
    int rc = 0;
    if (w.err) {
        remove(tmp);
        rc = -1;
    } else {
#ifdef _WIN32
        if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) rc = -1;
#else
        if (rename(tmp, path) != 0) rc = -1;
#endif
        if (rc != 0) remove(tmp);
    }
    free(tmp);
    return rc;
}

int cdc_snapshot_load(const char *path, CdcSnapshot *out) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    SnapReader r = {f, 0};
    char magic[4];
    if (!r_bytes(&r, magic, 4) || memcmp(magic, "MFT3", 4) != 0) {
        fclose(f);
        return -1;
    }
    uint64_t serial = 0, journal = 0, next = 0, min_size = 0;
    uint32_t bpc = 0, dir_count = 0, file_count = 0;
    if (!r_u64(&r, &serial) || !r_u64(&r, &journal) || !r_u64(&r, &next) ||
        !r_u64(&r, &min_size) || !r_u32(&r, &bpc) || !r_u32(&r, &dir_count)) {
        fclose(f);
        return -1;
    }
    out->volume_serial = serial;
    out->journal_id = journal;
    out->next_usn = (int64_t)next;
    out->min_size = min_size;
    out->bytes_per_cluster = bpc;
    /* 先在 vec 里攒，读完再整体交给 out（读一半失败时 out 保持干净）。 */
    cdc_vec_t dv, fv;
    if (cdc_vec_init(&dv, sizeof(CdcDirNode)) != CDC_OK ||
        cdc_vec_init(&fv, sizeof(CdcRawFile)) != CDC_OK) {
        fclose(f);
        return -1;
    }
    for (uint32_t i = 0; i < dir_count; i++) {
        uint64_t idx = 0, parent = 0;
        uint16_t seq = 0;
        uint8_t rep = 0;
        if (!r_u64(&r, &idx) || !r_u16(&r, &seq) || !r_bytes(&r, &rep, 1) ||
            !r_u64(&r, &parent)) {
            goto fail;
        }
        char *name = r_str(&r);
        if (!name) goto fail;
        CdcDirNode node;
        node.index = idx;
        node.seq = seq;
        node.parent_frn = parent;
        node.name = name;
        node.reparse = rep != 0;
        if (cdc_vec_push(&dv, &node) != CDC_OK) {
            free(name);
            goto fail;
        }
    }
    if (!r_u32(&r, &file_count)) goto fail;
    for (uint32_t i = 0; i < file_count; i++) {
        uint64_t idx = 0, frn = 0, parent = 0, size = 0, mtime = 0, lcn = 0, skey = 0;
        uint8_t raw = 0;
        if (!r_u64(&r, &idx) || !r_u64(&r, &frn) || !r_u64(&r, &parent) ||
            !r_u64(&r, &size) || !r_u64(&r, &mtime) || !r_u64(&r, &lcn) ||
            !r_u64(&r, &skey) || !r_bytes(&r, &raw, 1)) {
            goto fail;
        }
        char *name = r_str(&r);
        if (!name) goto fail;
        (void)idx; /* 文件的键就是 frn 的低 48 位；idx 与之一致 */
        CdcRawFile rf;
        rf.frn = frn;
        rf.parent_frn = parent;
        rf.name = name;
        rf.size = size;
        rf.mtime = mtime;
        rf.first_lcn = lcn;
        rf.sort_key = skey;
        rf.can_raw_read = raw != 0;
        if (cdc_vec_push(&fv, &rf) != CDC_OK) {
            free(name);
            goto fail;
        }
    }
    fclose(f);
    f = NULL;
    if (r.err) goto fail;
    out->dirs = dv.data;
    out->n_dirs = dv.len;
    dv.data = NULL; /* 交给 out，vec 不再拥有 */
    out->files = fv.data;
    out->n_files = fv.len;
    fv.data = NULL;
    cdc_vec_free(&dv);
    cdc_vec_free(&fv);
    return 0;

fail:
    if (f) fclose(f);
    for (size_t i = 0; i < dv.len; i++) free(CDC_VEC_AT(&dv, CdcDirNode, i).name);
    for (size_t i = 0; i < fv.len; i++) free(CDC_VEC_AT(&fv, CdcRawFile, i).name);
    cdc_vec_free(&dv);
    cdc_vec_free(&fv);
    memset(out, 0, sizeof *out);
    return -1;
}

void cdc_snapshot_free(CdcSnapshot *snap) {
    if (!snap) return;
    for (size_t i = 0; i < snap->n_dirs; i++) free(snap->dirs[i].name);
    for (size_t i = 0; i < snap->n_files; i++) free(snap->files[i].name);
    free(snap->dirs);
    free(snap->files);
    snap->dirs = NULL;
    snap->files = NULL;
    snap->n_dirs = snap->n_files = 0;
}

/* ---------------- 内部快照（数组 + 索引表） ---------------- */

/* 仅 Windows 的 $MFT/USN 路径用（Linux 下不编译，否则 -Werror=unused-function）。 */
#ifdef _WIN32

typedef struct {
    CdcSnapshot snap;
    size_t dir_cap;   /* snap.dirs 的实际容量（CdcSnapshot 本身不存 cap） */
    size_t file_cap;  /* snap.files 的实际容量 */
    CdcU64Map dir_index;  /* 记录 index -> 在 snap.dirs 中的下标 */
    CdcU64Map file_index; /* 记录 index -> 在 snap.files 中的下标 */
} SnapInternal;

static void snap_internal_free(SnapInternal *s) {
    if (!s) return;
    cdc_snapshot_free(&s->snap);
    u64map_free(&s->dir_index);
    u64map_free(&s->file_index);
    free(s);
}

static SnapInternal *snap_internal_from_owned(CdcSnapshot snap) {
    SnapInternal *s = xmalloc(sizeof *s);
    if (!s) {
        cdc_snapshot_free(&snap);
        return NULL;
    }
    memset(s, 0, sizeof *s);
    s->snap = snap;
    s->dir_cap = snap.n_dirs;
    s->file_cap = snap.n_files;
    if (u64map_init(&s->dir_index, snap.n_dirs ? snap.n_dirs : 16) != 0 ||
        u64map_init(&s->file_index, snap.n_files ? snap.n_files : 16) != 0) {
        snap_internal_free(s);
        return NULL;
    }
    for (size_t i = 0; i < snap.n_dirs; i++)
        u64map_put(&s->dir_index, snap.dirs[i].index, (uintptr_t)i);
    for (size_t i = 0; i < snap.n_files; i++)
        u64map_put(&s->file_index, snap.files[i].frn & 0xFFFFFFFFFFFFULL, (uintptr_t)i);
    return s;
}

static CdcDirNode *snap_dir_get(SnapInternal *s, uint64_t index) {
    uintptr_t pos = 0;
    if (!u64map_get(&s->dir_index, index, &pos)) return NULL;
    if (pos >= s->snap.n_dirs) return NULL;
    return &s->snap.dirs[pos];
}

static int snap_dir_put(SnapInternal *s, CdcDirNode node) {
    uintptr_t pos = 0;
    if (u64map_get(&s->dir_index, node.index, &pos) && pos < s->snap.n_dirs) {
        free(s->snap.dirs[pos].name);
        s->snap.dirs[pos] = node;
        return 0;
    }
    size_t at = s->snap.n_dirs;
    if (at == s->dir_cap) {
        void *data = s->snap.dirs;
        if (vec_grow_raw(&data, &s->dir_cap, sizeof(CdcDirNode), at) != 0) return -1;
        s->snap.dirs = data;
    }
    s->snap.dirs[at] = node;
    s->snap.n_dirs++;
    return u64map_put(&s->dir_index, node.index, (uintptr_t)at);
}

static int snap_file_put(SnapInternal *s, CdcRawFile f) {
    uint64_t index = f.frn & 0xFFFFFFFFFFFFULL;
    uintptr_t pos = 0;
    if (u64map_get(&s->file_index, index, &pos) && pos < s->snap.n_files) {
        free(s->snap.files[pos].name);
        s->snap.files[pos] = f;
        return 0;
    }
    size_t at = s->snap.n_files;
    if (at == s->file_cap) {
        void *data = s->snap.files;
        if (vec_grow_raw(&data, &s->file_cap, sizeof(CdcRawFile), at) != 0) return -1;
        s->snap.files = data;
    }
    s->snap.files[at] = f;
    s->snap.n_files++;
    return u64map_put(&s->file_index, index, (uintptr_t)at);
}

/* swap-remove：把最后一个元素挪到被删位置，并修正它的索引。 */
static void snap_remove_at(SnapInternal *s, int is_dir, size_t pos) {
    if (is_dir) {
        free(s->snap.dirs[pos].name);
        uint64_t removed_index = s->snap.dirs[pos].index;
        size_t last = s->snap.n_dirs - 1;
        if (pos != last) {
            s->snap.dirs[pos] = s->snap.dirs[last];
            u64map_put(&s->dir_index, s->snap.dirs[pos].index, (uintptr_t)pos);
        }
        s->snap.n_dirs--;
        u64map_remove(&s->dir_index, removed_index);
    } else {
        free(s->snap.files[pos].name);
        uint64_t removed_index = s->snap.files[pos].frn & 0xFFFFFFFFFFFFULL;
        size_t last = s->snap.n_files - 1;
        if (pos != last) {
            s->snap.files[pos] = s->snap.files[last];
            u64map_put(&s->file_index, s->snap.files[pos].frn & 0xFFFFFFFFFFFFULL,
                       (uintptr_t)pos);
        }
        s->snap.n_files--;
        u64map_remove(&s->file_index, removed_index);
    }
}

static void snap_dir_remove(SnapInternal *s, uint64_t index) {
    uintptr_t pos = 0;
    if (u64map_get(&s->dir_index, index, &pos) && pos < s->snap.n_dirs)
        snap_remove_at(s, 1, (size_t)pos);
}

static void snap_file_remove(SnapInternal *s, uint64_t index) {
    uintptr_t pos = 0;
    if (u64map_get(&s->file_index, index, &pos) && pos < s->snap.n_files)
        snap_remove_at(s, 0, (size_t)pos);
}

static SnapInternal *snap_clone(const SnapInternal *src) {
    CdcSnapshot snap;
    memset(&snap, 0, sizeof snap);
    snap.volume_serial = src->snap.volume_serial;
    snap.journal_id = src->snap.journal_id;
    snap.next_usn = src->snap.next_usn;
    snap.min_size = src->snap.min_size;
    snap.bytes_per_cluster = src->snap.bytes_per_cluster;
    snap.dirs = xmalloc((src->snap.n_dirs ? src->snap.n_dirs : 1) * sizeof(CdcDirNode));
    snap.files = xmalloc((src->snap.n_files ? src->snap.n_files : 1) * sizeof(CdcRawFile));
    if (!snap.dirs || !snap.files) {
        free(snap.dirs);
        free(snap.files);
        return NULL;
    }
    for (size_t i = 0; i < src->snap.n_dirs; i++) {
        snap.dirs[i] = src->snap.dirs[i];
        snap.dirs[i].name = xstrdup(src->snap.dirs[i].name);
        if (!snap.dirs[i].name) {
            snap.n_dirs = i;
            snap.n_files = 0;
            cdc_snapshot_free(&snap);
            return NULL;
        }
    }
    snap.n_dirs = src->snap.n_dirs;
    for (size_t i = 0; i < src->snap.n_files; i++) {
        snap.files[i] = src->snap.files[i];
        snap.files[i].name = xstrdup(src->snap.files[i].name);
        if (!snap.files[i].name) {
            snap.n_files = i;
            cdc_snapshot_free(&snap);
            return NULL;
        }
    }
    snap.n_files = src->snap.n_files;
    return snap_internal_from_owned(snap);
}

/* ---------------- 进程内快照缓存 ---------------- */

#define CDC_MEM_SNAPS_MAX 8
static SnapInternal *g_mem_snaps[CDC_MEM_SNAPS_MAX];
static uint64_t g_mem_serials[CDC_MEM_SNAPS_MAX];
static size_t g_mem_n = 0;
static CdcMutex g_mem_lock;
static int g_mem_lock_init = 0;

static void mem_lock(void) {
    if (!g_mem_lock_init) {
        cdc_mutex_init(&g_mem_lock);
        g_mem_lock_init = 1;
    }
    cdc_mutex_lock(&g_mem_lock);
}

static void mem_unlock(void) { cdc_mutex_unlock(&g_mem_lock); }

static SnapInternal *mem_snapshot_get_borrowed(uint64_t serial) {
    mem_lock();
    SnapInternal *found = NULL;
    for (size_t i = 0; i < g_mem_n; i++) {
        if (g_mem_serials[i] == serial) {
            found = g_mem_snaps[i];
            break;
        }
    }
    mem_unlock();
    return found;
}

static SnapInternal *mem_snapshot_take(uint64_t serial) {
    mem_lock();
    SnapInternal *found = NULL;
    for (size_t i = 0; i < g_mem_n; i++) {
        if (g_mem_serials[i] == serial) {
            found = g_mem_snaps[i];
            g_mem_snaps[i] = g_mem_snaps[--g_mem_n];
            g_mem_serials[i] = g_mem_serials[g_mem_n];
            break;
        }
    }
    mem_unlock();
    return found;
}

static void mem_snapshot_put(uint64_t serial, SnapInternal *snap) {
    mem_lock();
    for (size_t i = 0; i < g_mem_n; i++) {
        if (g_mem_serials[i] == serial) {
            snap_internal_free(g_mem_snaps[i]);
            g_mem_snaps[i] = snap;
            mem_unlock();
            return;
        }
    }
    if (g_mem_n == CDC_MEM_SNAPS_MAX) {
        snap_internal_free(g_mem_snaps[0]);
        g_mem_snaps[0] = g_mem_snaps[--g_mem_n];
        g_mem_serials[0] = g_mem_serials[g_mem_n];
    }
    g_mem_snaps[g_mem_n] = snap;
    g_mem_serials[g_mem_n] = serial;
    g_mem_n++;
    mem_unlock();
}

/* ---------------- resolve_dir ---------------- */

/* resolve 缓存里"已解析为无"的哨兵（有效路径永不为空串）。 */
static const char g_resolve_none[] = "";

/* 目录路径解析：返回缓存内字符串的借用指针（调用期间有效，调用方
 * 不得 free、不得修改）；未解析出返回 NULL。
 * 缓存每 FRN 存一份完整路径；借用路径省掉"命中缓存还要 xstrdup
 * 一份"的逐条拷贝（主循环每条记录一次）。需要长期持有的调用方
 * 用下面的 resolve_dir（独立拷贝）。 */
static const char *resolve_dir_borrowed(uint64_t frn, SnapInternal *snap,
                                        CdcU64Map *cache, const char *drive,
                                        int skip_top_at_root,
                                        const char *const *skip_top, size_t n_skip) {
    uintptr_t cv = 0;
    if (u64map_get(cache, frn, &cv)) {
        if (cv == (uintptr_t)g_resolve_none) return NULL;
        return (const char *)cv;
    }
    uint64_t root_idx = frn & 0xFFFFFFFFFFFFULL;
    if (root_idx == 5) {
        size_t n = strlen(drive) + 2;
        char *p = xmalloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s\\", drive);
        u64map_put(cache, frn, (uintptr_t)p);
        return p;
    }

    cdc_vec_t chain;
    if (cdc_vec_init(&chain, sizeof(uint64_t)) != CDC_OK) return NULL;
    uint64_t cur_frn = frn;
    const char *base = NULL;

    while (1) {
        if (u64map_get(cache, cur_frn, &cv)) {
            if (cv == (uintptr_t)g_resolve_none) break;
            base = (const char *)cv; /* 已入缓存，借用 */
            break;
        }
        uint64_t cur_idx = cur_frn & 0xFFFFFFFFFFFFULL;
        uint16_t cur_seq = (uint16_t)(cur_frn >> 48);
        CdcDirNode *node = snap_dir_get(snap, cur_idx);
        if (!node) break;
        if (cur_seq != 0 && node->seq != cur_seq) break;
        if (node->reparse || (node->name && node->name[0] == '$')) break;
        uint64_t parent_idx = node->parent_frn & 0xFFFFFFFFFFFFULL;
        if (node->parent_frn == 0 || parent_idx == 5) {
            if (skip_top_at_root && name_in_skip_top(node->name, skip_top, n_skip)) break;
            size_t n = strlen(drive) + 1 + strlen(node->name) + 1;
            char *b = xmalloc(n);
            if (!b) break;
            snprintf(b, n, "%s\\%s", drive, node->name);
            u64map_put(cache, cur_frn, (uintptr_t)b);
            base = b; /* 刚入缓存，借用 */
            break;
        }
        if (cdc_vec_push(&chain, &cur_frn) != CDC_OK) break;
        cur_frn = node->parent_frn;
        if (chain.len > 4096) break;
    }

    if (!base) {
        for (size_t i = 0; i < chain.len; i++)
            u64map_put(cache, CDC_VEC_AT(&chain, uint64_t, i), (uintptr_t)g_resolve_none);
        u64map_put(cache, frn, (uintptr_t)g_resolve_none);
        cdc_vec_free(&chain);
        return NULL;
    }

    if (chain.len == 0) {
        /* base 已入缓存，直接借用（无拷贝）。 */
        cdc_vec_free(&chain);
        return base;
    }

    /* 组装：base 已入缓存，不能原地改，先拷一份工作串；每段拼完后
     * 把当前完整路径存一份进缓存；最后一段（frn 本身）直接把工作串
     * 入缓存、不另拷贝，返回的即缓存串。 */
    char *path = xstrdup(base);
    if (!path) {
        cdc_vec_free(&chain);
        return NULL;
    }
    for (size_t ri = chain.len; ri-- > 0;) {
        uint64_t idx = CDC_VEC_AT(&chain, uint64_t, ri) & 0xFFFFFFFFFFFFULL;
        CdcDirNode *node = snap_dir_get(snap, idx);
        if (!node) {
            free(path);
            cdc_vec_free(&chain);
            u64map_put(cache, frn, (uintptr_t)g_resolve_none);
            return NULL;
        }
        size_t plen = strlen(path);
        while (plen > 0 && path[plen - 1] == '\\') plen--;
        size_t n = plen + 1 + strlen(node->name) + 1;
        char *np = xmalloc(n);
        if (!np) {
            free(path);
            cdc_vec_free(&chain);
            return NULL;
        }
        memcpy(np, path, plen);
        np[plen] = '\\';
        memcpy(np + plen + 1, node->name, strlen(node->name) + 1);
        free(path);
        path = np;
        if (ri == 0) {
            u64map_put(cache, CDC_VEC_AT(&chain, uint64_t, ri), (uintptr_t)path);
        } else {
            char *cached = xstrdup(path);
            if (cached)
                u64map_put(cache, CDC_VEC_AT(&chain, uint64_t, ri), (uintptr_t)cached);
        }
    }
    cdc_vec_free(&chain);
    return path;
}

/* resolve_dir_borrowed 的独立拷贝版（调用方 free）：供需要长期持有
 * 字符串的调用方（USN 第 2 遍的改名新旧路径，缓存释放后仍在用）。 */
static char *resolve_dir(uint64_t frn, SnapInternal *snap, CdcU64Map *cache,
                         const char *drive, int skip_top_at_root,
                         const char *const *skip_top, size_t n_skip) {
    const char *borrowed = resolve_dir_borrowed(frn, snap, cache, drive,
                                                skip_top_at_root, skip_top, n_skip);
    return borrowed ? xstrdup(borrowed) : NULL;
}

#endif /* _WIN32 */

/* ---------------- Windows 专属：卷 / USN / $MFT ---------------- */

#ifdef _WIN32

typedef struct {
    int64_t volume_serial_number;
    int64_t number_sectors;
    int64_t total_clusters;
    int64_t free_clusters;
    int64_t total_reserved;
    uint32_t bytes_per_sector;
    uint32_t bytes_per_cluster;
    uint32_t bytes_per_file_record_segment;
    uint32_t clusters_per_file_record_segment;
    int64_t mft_valid_data_length;
    int64_t mft_start_lcn;
    int64_t mft2_start_lcn;
    int64_t mft_zone_start;
    int64_t mft_zone_end;
} NtfsVolumeData;

typedef struct {
    uint64_t usn_journal_id;
    int64_t first_usn;
    int64_t next_usn;
    int64_t lowest_valid_usn;
    int64_t max_usn;
    uint64_t maximum_size;
    uint64_t allocation_delta;
} UsnJournalData;

typedef struct {
    int64_t start_usn;
    uint32_t reason_mask;
    uint32_t return_only_on_close;
    uint64_t timeout;
    uint64_t bytes_to_wait_for;
    uint64_t usn_journal_id;
} ReadUsnJournalDataV0;

typedef struct {
    int64_t file_reference_number;
} NtfsFileRecordInput;

/* root 如 "C:\\" 或 "C:"；成功时 drive_out="C:"，返回 1。 */
static int parse_drive_root(const char *root, char *drive_out) {
    size_t len = strlen(root);
    while (len > 0 && root[len - 1] == '\\') len--;
    if (len < 2 || root[1] != ':') return 0;
    char letter = root[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))) return 0;
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    drive_out[0] = letter;
    drive_out[1] = ':';
    drive_out[2] = '\0';
    return 1;
}

static HANDLE open_volume_handle(const char *volume_path) {
    wchar_t *w = cdc_utf8_to_wide(volume_path);
    if (!w) return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    free(w);
    return h;
}

static int get_volume_data(HANDLE h, NtfsVolumeData *out) {
    DWORD ret = 0;
    memset(out, 0, sizeof *out);
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, out,
                         (DWORD)sizeof *out, &ret, NULL))
        return -1;
    return 0;
}

static int query_usn_journal(HANDLE h, UsnJournalData *out) {
    DWORD ret = 0;
    memset(out, 0, sizeof *out);
    if (!DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, NULL, 0, out,
                         (DWORD)sizeof *out, &ret, NULL))
        return -1;
    return 0;
}

static size_t read_exact_at(HANDLE h, int64_t offset, uint8_t *buf, size_t len) {
    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > 8u * 1024 * 1024) chunk = 8u * 1024 * 1024;
        DWORD read = 0;
        if (!ReadFile(h, buf + total, (DWORD)chunk, &read, NULL) || read == 0) break;
        total += read;
    }
    return total;
}

static int read_single_mft_record(HANDLE h, uint64_t index, size_t record_size,
                                  uint8_t *out_buf, uint64_t *actual_frn_out) {
    NtfsFileRecordInput in;
    in.file_reference_number = (int64_t)index;
    DWORD ret = 0;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_FILE_RECORD, &in, (DWORD)sizeof in,
                         out_buf, (DWORD)(16 + record_size), &ret, NULL))
        return -1;
    if (ret < 12 + record_size) return -1;
    uint64_t actual_frn = get_u64_le(out_buf);
    uint32_t rec_len = get_u32_le(out_buf + 8);
    if (rec_len < record_size) return -1;
    *actual_frn_out = actual_frn;
    return 0;
}

/* ---- USN 变化收集 ---- */

typedef enum {
    CDC_DELTA_CHANGED,
    CDC_DELTA_TOO_MANY,
    CDC_DELTA_FAILED
} CdcUsnDeltaKind;

typedef struct {
    CdcUsnDeltaKind kind;
    uint64_t *frns;
    size_t n, cap;
} CdcUsnDelta;

static void usn_delta_free(CdcUsnDelta *d) {
    free(d->frns);
    d->frns = NULL;
    d->n = d->cap = 0;
}

static CdcUsnDelta collect_usn_changed_indices(HANDLE h, const UsnJournalData *journal,
                                               int64_t start_usn) {
    CdcUsnDelta d;
    memset(&d, 0, sizeof d);
    d.kind = CDC_DELTA_FAILED;
    ReadUsnJournalDataV0 input;
    input.start_usn = start_usn;
    input.reason_mask = 0xFFFFFFFFu;
    input.return_only_on_close = 0;
    input.timeout = 0;
    input.bytes_to_wait_for = 0;
    input.usn_journal_id = journal->usn_journal_id;
    uint8_t *buf = xmalloc(64 * 1024);
    if (!buf) return d;
    CdcU64Map seen;
    memset(&seen, 0, sizeof seen);
    if (u64map_init(&seen, 1024) != 0) {
        free(buf);
        return d;
    }
    int failed = 0;
    while (1) {
        DWORD returned = 0;
        if (!DeviceIoControl(h, FSCTL_READ_USN_JOURNAL, &input, (DWORD)sizeof input,
                             buf, 64 * 1024, &returned, NULL) ||
            returned < 8) {
            failed = 1;
            break;
        }
        size_t len = returned;
        int64_t next_cursor = (int64_t)get_u64_le(buf);
        if (len == 8) break;
        size_t off = 8;
        while (off + 60 <= len) {
            uint32_t rec_len = get_u32_le(buf + off);
            if (rec_len < 60 || off + rec_len > len) break;
            uint16_t major = get_u16_le(buf + off + 4);
            if (major == 2 || major == 3) {
                uint64_t frn = get_u64_le(buf + off + 8) & 0xFFFFFFFFFFFFULL;
                uintptr_t dummy = 0;
                if (!u64map_get(&seen, frn, &dummy)) {
                    if (u64map_put(&seen, frn, 1) != 0) {
                        failed = 1;
                        break;
                    }
                    if (seen.len > CDC_MFT_MAX_USN_DELTA_FRNS) {
                        d.kind = CDC_DELTA_TOO_MANY;
                        goto done;
                    }
                }
            }
            off += rec_len;
        }
        if (failed) break;
        if (next_cursor <= input.start_usn || next_cursor >= journal->next_usn) break;
        input.start_usn = next_cursor;
    }
    if (!failed && d.kind != CDC_DELTA_TOO_MANY) {
        d.kind = CDC_DELTA_CHANGED;
        d.frns = xmalloc((seen.len ? seen.len : 1) * sizeof(uint64_t));
        if (!d.frns) {
            d.kind = CDC_DELTA_FAILED;
        } else {
            for (size_t i = 0; i < seen.cap; i++) {
                if (seen.used[i]) d.frns[d.n++] = seen.keys[i];
            }
        }
    }
done:
    u64map_free(&seen);
    free(buf);
    return d;
}

/* ---- USN 变更记录重读（顺序版；Rust 版 >=64 条时 8 线程并行） ---- */

typedef struct {
    uint64_t idx;
    CdcParsedRecord rec;
} ParsedDeltaItem;

typedef struct {
    cdc_vec_t vec; /* 元素 ParsedDeltaItem */
} ParsedDeltaList;

static void parsed_delta_list_free(ParsedDeltaList *l) {
    if (!l) return;
    for (size_t i = 0; i < l->vec.len; i++)
        cdc_parsed_record_free(&CDC_VEC_AT(&l->vec, ParsedDeltaItem, i).rec);
    cdc_vec_free(&l->vec);
}

static int apply_usn_delta(HANDLE h, const uint64_t *changed, size_t n_changed,
                           size_t record_size, size_t bytes_per_sector,
                           size_t bytes_per_cluster, uint64_t min_size,
                           ParsedDeltaList *out) {
    uint8_t *buf = xmalloc(16 + record_size);
    if (!buf) return -1;
    for (size_t i = 0; i < n_changed; i++) {
        uint64_t idx = changed[i];
        uint64_t actual = 0;
        if (read_single_mft_record(h, idx, record_size, buf, &actual) != 0) continue;
        if ((actual & 0xFFFFFFFFFFFFULL) != idx) continue;
        CdcParsedRecord rec;
        if (cdc_parse_record_in_place(buf + 12, record_size, idx, bytes_per_sector,
                                      bytes_per_cluster, min_size, &rec) != 0)
            continue;
        ParsedDeltaItem item;
        item.idx = idx;
        item.rec = rec;
        if (cdc_vec_push(&out->vec, &item) != CDC_OK) {
            cdc_parsed_record_free(&rec);
            free(buf);
            return -1;
        }
    }
    free(buf);
    return 0;
}

/* ---- $MFT 全量读取（顺序版；Rust 版用 Rayon 并行解析 + 读盘重叠） ---- */

static void insert_parsed(SnapInternal *snap, CdcParsedRecord *rec) {
    if (rec->kind == CDC_REC_DIR) {
        CdcDirNode node;
        node.index = rec->index;
        node.seq = rec->seq;
        node.parent_frn = rec->parent_frn;
        node.name = rec->name;
        node.reparse = rec->reparse;
        rec->name = NULL;
        snap_dir_put(snap, node);
    } else if (rec->kind == CDC_REC_FILE) {
        CdcRawFile f;
        f.frn = rec->frn;
        f.parent_frn = rec->parent_frn;
        f.name = rec->name;
        f.size = rec->size;
        f.mtime = rec->mtime;
        f.first_lcn = rec->first_lcn;
        f.sort_key = rec->sort_key;
        f.can_raw_read = rec->can_raw_read;
        rec->name = NULL;
        snap_file_put(snap, f);
    }
}

static void parse_chunk(const uint8_t *data, size_t len, uint64_t base_idx,
                        size_t record_size, size_t bytes_per_sector,
                        size_t bytes_per_cluster, uint64_t min_size,
                        SnapInternal *out) {
    uint8_t *scratch = xmalloc(record_size);
    if (!scratch) return;
    for (size_t off = 0; off + record_size <= len; off += record_size) {
        if (memcmp(data + off, "FILE", 4) != 0) continue;
        memcpy(scratch, data + off, record_size);
        CdcParsedRecord rec;
        if (cdc_parse_record_in_place(scratch, record_size, base_idx + off / record_size,
                                      bytes_per_sector, bytes_per_cluster, min_size,
                                      &rec) != 0)
            continue;
        insert_parsed(out, &rec);
        cdc_parsed_record_free(&rec);
    }
    free(scratch);
}

static int full_mft_read(HANDLE h, const NtfsVolumeData *vol, const MftRun *runs,
                         size_t n_runs, uint64_t store_min_size, SnapInternal *out) {
    size_t record_size = vol->bytes_per_file_record_segment;
    size_t bytes_per_cluster = vol->bytes_per_cluster;
    size_t bytes_per_sector = vol->bytes_per_sector;
    uint64_t max_mft_bytes = vol->mft_valid_data_length > 0
                                 ? (uint64_t)vol->mft_valid_data_length
                                 : UINT64_MAX;
    const size_t CHUNK = 8 * 1024 * 1024;
    uint8_t *read_buf = xmalloc(CHUNK);
    uint8_t *parse_buf = xmalloc(CHUNK);
    uint8_t *scratch = xmalloc(record_size);
    uint8_t *carry = xmalloc(record_size);
    size_t carry_len = 0;
    if (!read_buf || !parse_buf || !scratch || !carry) {
        free(read_buf);
        free(parse_buf);
        free(scratch);
        free(carry);
        return -1;
    }
    size_t pending_len = 0;
    uint64_t pending_base_idx = 0;
    uint64_t total_offset = 0;

    for (size_t ri = 0; ri < n_runs; ri++) {
        int64_t lcn = runs[ri].lcn;
        int64_t length = runs[ri].len;
        if (total_offset >= max_mft_bytes) break;
        if (length <= 0) continue;
        uint64_t run_bytes = (uint64_t)length * (uint64_t)bytes_per_cluster;
        if (lcn < 0) {
            if (pending_len > 0) {
                parse_chunk(parse_buf, pending_len, pending_base_idx, record_size,
                            bytes_per_sector, bytes_per_cluster, store_min_size, out);
                pending_len = 0;
            }
            total_offset = run_bytes > UINT64_MAX - total_offset ? UINT64_MAX
                                                                 : total_offset + run_bytes;
            carry_len = 0;
            continue;
        }
        int64_t disk_offset = lcn * (int64_t)bytes_per_cluster;
        uint64_t remain = max_mft_bytes - total_offset;
        uint64_t to_process = run_bytes < remain ? run_bytes : remain;
        uint64_t done_in_run = 0;
        while (done_in_run < to_process) {
            uint64_t left = to_process - done_in_run;
            size_t want = left < CHUNK ? (size_t)left : CHUNK;
            size_t got;
            if (pending_len > 0) {
                parse_chunk(parse_buf, pending_len, pending_base_idx, record_size,
                            bytes_per_sector, bytes_per_cluster, store_min_size, out);
                pending_len = 0;
                got = read_exact_at(h, disk_offset + (int64_t)done_in_run, read_buf, want);
            } else {
                got = read_exact_at(h, disk_offset + (int64_t)done_in_run, read_buf, want);
            }
            if (got == 0) break;
            size_t cursor = 0;
            if (carry_len > 0) {
                size_t need = record_size - carry_len;
                if (got >= need) {
                    uint64_t rec_idx = (total_offset - carry_len) / record_size;
                    memcpy(scratch, carry, carry_len);
                    memcpy(scratch + carry_len, read_buf, need);
                    CdcParsedRecord rec;
                    if (cdc_parse_record_in_place(scratch, record_size, rec_idx,
                                                  bytes_per_sector, bytes_per_cluster,
                                                  store_min_size, &rec) == 0) {
                        insert_parsed(out, &rec);
                        cdc_parsed_record_free(&rec);
                    }
                    carry_len = 0;
                    cursor = need;
                } else {
                    memcpy(carry + carry_len, read_buf, got);
                    carry_len += got;
                    done_in_run += got;
                    total_offset += got;
                    continue;
                }
            }
            size_t aligned_len = ((got - cursor) / record_size) * record_size;
            if (aligned_len > 0) {
                memcpy(parse_buf, read_buf + cursor, aligned_len);
                pending_len = aligned_len;
                pending_base_idx = (total_offset + cursor) / record_size;
                cursor += aligned_len;
            }
            if (cursor < got) {
                memcpy(carry, read_buf + cursor, got - cursor);
                carry_len = got - cursor;
            }
            done_in_run += got;
            total_offset += got;
        }
    }
    if (pending_len > 0) {
        parse_chunk(parse_buf, pending_len, pending_base_idx, record_size,
                    bytes_per_sector, bytes_per_cluster, store_min_size, out);
    }
    free(read_buf);
    free(parse_buf);
    free(scratch);
    free(carry);
    if (out->snap.n_dirs == 0 && out->snap.n_files == 0) return 1;
    return 0;
}

/* ---- 快照路径与后台落盘 / 后台重建 ---- */

static char *snapshot_path(uint64_t serial) {
    char *dir = cdc_cache_dir();
    if (!dir) return NULL;
    size_t n = strlen(dir) + 32;
    char *p = xmalloc(n);
    if (!p) {
        free(dir);
        return NULL;
    }
    snprintf(p, n, "%s\\mft-v3-%016llx.bin", dir, (unsigned long long)serial);
    free(dir);
    return p;
}

static CdcMutex g_save_lock;
static int g_save_lock_init = 0;

static void save_lock_ensure(void) {
    if (!g_save_lock_init) {
        cdc_mutex_init(&g_save_lock);
        g_save_lock_init = 1;
    }
}

typedef struct {
    char *path;
    SnapInternal *snap; /* 深拷贝的所有权归线程 */
} BgSaveArg;

static void bg_save_run(void *arg) {
    BgSaveArg *a = (BgSaveArg *)arg;
    cdc_mutex_lock(&g_save_lock);
    cdc_snapshot_save(a->path, &a->snap->snap);
    cdc_mutex_unlock(&g_save_lock);
    snap_internal_free(a->snap);
    free(a->path);
    free(a);
}

static void save_snapshot_in_background(const char *path, SnapInternal *snap) {
    save_lock_ensure();
    SnapInternal *copy = snap_clone(snap);
    if (!copy) return;
    BgSaveArg *a = xmalloc(sizeof *a);
    if (!a) {
        snap_internal_free(copy);
        return;
    }
    a->path = xstrdup(path);
    a->snap = copy;
    if (!a->path) {
        snap_internal_free(copy);
        free(a);
        return;
    }
    spawn_detached_thread(bg_save_run, a);
}

#define CDC_REBUILD_MAX 8
static uint64_t g_rebuild_in_progress[CDC_REBUILD_MAX];
static size_t g_rebuild_n = 0;

typedef struct {
    char *volume_path;
    uint64_t serial;
    NtfsVolumeData volume;
    uint64_t store_min_size;
    char *snap_file;
    size_t record_size;
} BgRebuildArg;

static void bg_rebuild_run(void *arg) {
    BgRebuildArg *a = (BgRebuildArg *)arg;
    SnapInternal *rebuilt = NULL;
    HANDLE h = open_volume_handle(a->volume_path);
    if (h != INVALID_HANDLE_VALUE) {
        /* 先取 USN 游标再读 MFT：读取期间的变更在下次增量里补上。 */
        UsnJournalData journal;
        int have_journal = query_usn_journal(h, &journal) == 0;
        uint8_t *first = xmalloc(a->record_size);
        if (first &&
            read_exact_at(h, a->volume.mft_start_lcn * (int64_t)a->volume.bytes_per_cluster,
                          first, a->record_size) >= a->record_size) {
            CdcParsedRecord rec;
            if (cdc_parse_record_in_place(first, a->record_size, 0,
                                          a->volume.bytes_per_sector,
                                          a->volume.bytes_per_cluster,
                                          a->store_min_size, &rec) == 0 &&
                rec.kind == CDC_REC_MFT_ROOT) {
                CdcSnapshot s;
                memset(&s, 0, sizeof s);
                s.volume_serial = a->serial;
                s.journal_id = have_journal ? journal.usn_journal_id : 0;
                s.next_usn = have_journal ? journal.next_usn : 0;
                s.min_size = a->store_min_size;
                s.bytes_per_cluster = a->volume.bytes_per_cluster;
                SnapInternal *si = snap_internal_from_owned(s);
                if (si) {
                    if (full_mft_read(h, &a->volume, rec.runs, rec.n_runs,
                                      a->store_min_size, si) == 0)
                        rebuilt = si;
                    else
                        snap_internal_free(si);
                }
            }
            cdc_parsed_record_free(&rec);
        }
        free(first);
        CloseHandle(h);
    }
    if (rebuilt) {
        /* 先落盘再进缓存：落盘用的是本线程拥有的指针，无竞态。 */
        cdc_snapshot_save(a->snap_file, &rebuilt->snap);
        mem_snapshot_put(a->serial, rebuilt);
    }
    mem_lock();
    for (size_t i = 0; i < g_rebuild_n; i++) {
        if (g_rebuild_in_progress[i] == a->serial) {
            g_rebuild_in_progress[i] = g_rebuild_in_progress[--g_rebuild_n];
            break;
        }
    }
    mem_unlock();
    free(a->volume_path);
    free(a->snap_file);
    free(a);
}

static void spawn_background_rebuild(const char *volume_path, uint64_t serial,
                                     const NtfsVolumeData *volume, uint64_t store_min_size,
                                     const char *snap_file, size_t record_size) {
    mem_lock();
    for (size_t i = 0; i < g_rebuild_n; i++) {
        if (g_rebuild_in_progress[i] == serial) {
            mem_unlock();
            return;
        }
    }
    if (g_rebuild_n == CDC_REBUILD_MAX) {
        mem_unlock();
        return;
    }
    g_rebuild_in_progress[g_rebuild_n++] = serial;
    mem_unlock();
    BgRebuildArg *a = xmalloc(sizeof *a);
    if (!a) goto rollback;
    a->volume_path = xstrdup(volume_path);
    a->snap_file = xstrdup(snap_file);
    if (!a->volume_path || !a->snap_file) {
        free(a->volume_path);
        free(a->snap_file);
        free(a);
        goto rollback;
    }
    a->serial = serial;
    a->volume = *volume;
    a->store_min_size = store_min_size;
    a->record_size = record_size;
    spawn_detached_thread(bg_rebuild_run, a);
    return;
rollback:
    mem_lock();
    for (size_t i = 0; i < g_rebuild_n; i++) {
        if (g_rebuild_in_progress[i] == serial) {
            g_rebuild_in_progress[i] = g_rebuild_in_progress[--g_rebuild_n];
            break;
        }
    }
    mem_unlock();
}

#endif /* _WIN32 */

/* ---------------- 公共入口 ---------------- */

int cdc_usn_state_usable(uint64_t stored_journal_id, int64_t stored_next_usn,
                         uint64_t journal_id, int64_t first_usn, int64_t next_usn) {
    return stored_journal_id == journal_id && stored_next_usn >= first_usn &&
           stored_next_usn <= next_usn;
}

#ifdef _WIN32

int cdc_volume_usn_state(const char *root, uint64_t *volume_serial, uint64_t *journal_id,
                         int64_t *first_usn, int64_t *next_usn) {
    char drive[3];
    if (!parse_drive_root(root, drive)) return -1;
    char volume_path[16];
    snprintf(volume_path, sizeof volume_path, "\\\\.\\%s", drive);
    HANDLE h = open_volume_handle(volume_path);
    if (h == INVALID_HANDLE_VALUE) return -1;
    NtfsVolumeData volume;
    if (get_volume_data(h, &volume) != 0) {
        CloseHandle(h);
        return -1;
    }
    UsnJournalData journal;
    if (query_usn_journal(h, &journal) != 0) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    *volume_serial = (uint64_t)volume.volume_serial_number;
    *journal_id = journal.usn_journal_id;
    *first_usn = journal.first_usn;
    *next_usn = journal.next_usn;
    return 0;
}

/* 释放 resolve 缓存里的字符串（哨兵除外）并释放表本身。 */
static void resolve_cache_free(CdcU64Map *cache) {
    for (size_t i = 0; i < cache->cap; i++) {
        if (cache->used[i] && cache->vals[i] != (uintptr_t)g_resolve_none)
            free((void *)cache->vals[i]);
    }
    u64map_free(cache);
}

/* 逐字符小写前缀比较：full 的前 n 个字节与小写的 prefix 相等
 * （prefix 须事先小写；ASCII 大小写差 32，逐字符就地小写等价于整串
 * ascii_lower_in_place 后比较，但不另分配拷贝）。full 短于 n 时
 * 读到 NUL 即返回 0（不是前缀），不越界。 */
static int lower_is_prefix(const char *full, const char *prefix, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)full[i];
        if (a == '\0') return 0;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (a != (unsigned char)prefix[i]) return 0;
    }
    return 1;
}

/* rest（已去掉 root 前缀，指向 full 串内的第一段）与 skip_top 逐段
 * 比较：只读第一段，不拷贝、不分配；两侧逐字符就地小写。 */
static int lower_has_skip_segment(const char *rest,
                                  const char *const *skip_top, size_t n_skip) {
    const char *slash = strchr(rest, '\\');
    size_t seg = slash ? (size_t)(slash - rest) : strlen(rest);
    for (size_t i = 0; i < n_skip; i++) {
        size_t k = strlen(skip_top[i]);
        size_t j;
        int eq = 1;
        if (k != seg) continue;
        for (j = 0; j < k; j++) {
            unsigned char a = (unsigned char)rest[j];
            unsigned char b = (unsigned char)skip_top[i][j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) {
                eq = 0;
                break;
            }
        }
        if (eq) return 1;
    }
    return 0;
}

int cdc_mft_try_scan(const char *root, uint64_t min_size,
                     const char *const *skip_top, size_t n_skip,
                     cdc_keep_fn keep, void *keep_ctx, CdcFileEntryList *out) {
    char drive[3];
    if (!parse_drive_root(root, drive)) return 1;
    size_t trimmed_len = strlen(root);
    while (trimmed_len > 0 && root[trimmed_len - 1] == '\\') trimmed_len--;
    int is_drive_root = trimmed_len == 2;

    char volume_path[16];
    snprintf(volume_path, sizeof volume_path, "\\\\.\\%s", drive);
    HANDLE h = open_volume_handle(volume_path);
    if (h == INVALID_HANDLE_VALUE) return 1;

    NtfsVolumeData volume;
    if (get_volume_data(h, &volume) != 0) {
        CloseHandle(h);
        return 1;
    }
    size_t record_size = volume.bytes_per_file_record_segment;
    size_t bytes_per_cluster = volume.bytes_per_cluster;
    size_t bytes_per_sector = volume.bytes_per_sector;
    if ((record_size != 1024 && record_size != 4096) || bytes_per_cluster == 0 ||
        bytes_per_sector == 0) {
        CloseHandle(h);
        return 1;
    }
    uint64_t serial = (uint64_t)volume.volume_serial_number;
    UsnJournalData journal;
    int have_journal = query_usn_journal(h, &journal) == 0;
    char *snap_file = snapshot_path(serial);
    uint64_t store_min_size = min_size >= CDC_MFT_SNAPSHOT_FLOOR_MIN_SIZE
                                  ? CDC_MFT_SNAPSHOT_FLOOR_MIN_SIZE
                                  : min_size;

    SnapInternal *snap = NULL;     /* 本次扫描用的快照 */
    SnapInternal *snap_owned = NULL; /* 未进 mem 缓存时由本函数释放 */
    int snap_cached = 0;

    if (have_journal && snap_file) {
        SnapInternal *cached = mem_snapshot_get_borrowed(serial);
        SnapInternal *disk = NULL;
        if (!cached) {
            CdcSnapshot loaded;
            if (cdc_snapshot_load(snap_file, &loaded) == 0) {
                disk = snap_internal_from_owned(loaded);
                if (!disk) cdc_snapshot_free(&loaded);
            }
        }
        SnapInternal *cand = cached ? cached : disk;
        if (cand && cand->snap.volume_serial == serial &&
            cand->snap.journal_id == journal.usn_journal_id &&
            cand->snap.bytes_per_cluster == (uint32_t)bytes_per_cluster &&
            cand->snap.min_size <= min_size) {
            if (cand->snap.next_usn == journal.next_usn) {
                if (cached) {
                    snap = cached;
                } else {
                    snap = disk;
                    disk = NULL;
                    mem_snapshot_put(serial, snap);
                }
                snap_cached = 1;
            } else if (cand->snap.next_usn >= journal.first_usn &&
                       cand->snap.next_usn < journal.next_usn) {
                CdcUsnDelta delta =
                    collect_usn_changed_indices(h, &journal, cand->snap.next_usn);
                if (delta.kind == CDC_DELTA_CHANGED) {
                    SnapInternal *owned = cached ? mem_snapshot_take(serial) : disk;
                    disk = NULL; /* 所有权已转移给 owned（或 owned 为 NULL 时 disk 本就是 NULL） */
                    if (owned) {
                        ParsedDeltaList parsed;
                        memset(&parsed, 0, sizeof parsed);
                        if (cdc_vec_init(&parsed.vec, sizeof(ParsedDeltaItem)) !=
                            CDC_OK) {
                            snap_internal_free(owned);
                        } else if (apply_usn_delta(h, delta.frns, delta.n, record_size,
                                            bytes_per_sector, bytes_per_cluster,
                                            owned->snap.min_size, &parsed) == 0) {
                            for (size_t i = 0; i < parsed.vec.len; i++) {
                                uint64_t idx = CDC_VEC_AT(&parsed.vec, ParsedDeltaItem, i).idx;
                                snap_dir_remove(owned, idx);
                                snap_file_remove(owned, idx);
                                CdcParsedRecord *rec =
                                    &CDC_VEC_AT(&parsed.vec, ParsedDeltaItem, i).rec;
                                if (rec->kind == CDC_REC_DIR ||
                                    rec->kind == CDC_REC_FILE)
                                    insert_parsed(owned, rec);
                            }
                            owned->snap.next_usn = journal.next_usn;
                            snap = owned;
                            mem_snapshot_put(serial, snap);
                            snap_cached = 1;
                            save_snapshot_in_background(snap_file, snap);
                        } else {
                            snap_internal_free(owned);
                        }
                        parsed_delta_list_free(&parsed);
                    }
                } else if (delta.kind == CDC_DELTA_TOO_MANY) {
                    /* 变更太多：本次先用旧快照，后台重建，下一次扫描再换新。 */
                    if (cached) {
                        snap = cached;
                    } else {
                        snap = disk;
                        disk = NULL;
                        mem_snapshot_put(serial, snap);
                    }
                    snap_cached = 1;
                    spawn_background_rebuild(volume_path, serial, &volume, store_min_size,
                                             snap_file, record_size);
                }
                /* Failed：落到下面的同步全量重建。 */
                usn_delta_free(&delta);
            }
        }
        if (disk) snap_internal_free(disk);
    }

    if (!snap) {
        uint8_t *first = xmalloc(record_size);
        int ok = 0;
        if (first &&
            read_exact_at(h, volume.mft_start_lcn * (int64_t)bytes_per_cluster, first,
                          record_size) >= record_size) {
            CdcParsedRecord rec;
            if (cdc_parse_record_in_place(first, record_size, 0, bytes_per_sector,
                                          bytes_per_cluster, store_min_size,
                                          &rec) == 0 &&
                rec.kind == CDC_REC_MFT_ROOT) {
                CdcSnapshot s;
                memset(&s, 0, sizeof s);
                s.volume_serial = serial;
                s.journal_id = have_journal ? journal.usn_journal_id : 0;
                s.next_usn = have_journal ? journal.next_usn : 0;
                s.min_size = store_min_size;
                s.bytes_per_cluster = (uint32_t)bytes_per_cluster;
                SnapInternal *si = snap_internal_from_owned(s);
                if (si) {
                    if (full_mft_read(h, &volume, rec.runs, rec.n_runs, store_min_size,
                                      si) == 0) {
                        snap = si;
                        if (have_journal &&
                            store_min_size >= CDC_MFT_SNAPSHOT_FLOOR_MIN_SIZE) {
                            mem_snapshot_put(serial, si);
                            snap_cached = 1;
                            save_snapshot_in_background(snap_file, si);
                        } else {
                            snap_owned = si;
                        }
                        ok = 1;
                    } else {
                        snap_internal_free(si);
                    }
                }
            }
            cdc_parsed_record_free(&rec);
        }
        free(first);
        if (!ok) {
            free(snap_file);
            CloseHandle(h);
            return 1;
        }
    }

    /* 拼路径、过滤、拼 FileEntry。 */
    char *root_lower = xstrdup(root);
    if (!root_lower) {
        free(snap_file);
        CloseHandle(h);
        if (!snap_cached && snap_owned) snap_internal_free(snap_owned);
        return -1;
    }
    size_t rl = strlen(root_lower);
    while (rl > 0 && root_lower[rl - 1] == '\\') root_lower[--rl] = '\0';
    ascii_lower_in_place(root_lower);
    size_t root_len = strlen(root_lower) + 1; /* root_lower + "\\" */

    CdcU64Map dir_cache;
    memset(&dir_cache, 0, sizeof dir_cache);
    size_t hint = snap->snap.n_dirs < 65536 ? snap->snap.n_dirs : 65536;
    u64map_init(&dir_cache, hint ? hint : 16);
    uint64_t now = cdc_now_seconds();
    int rc = 0;
    for (size_t i = 0; i < snap->snap.n_files; i++) {
        CdcRawFile *raw = &snap->snap.files[i];
        if (raw->size < min_size) continue;
        if (keep && !keep(raw->name, raw->size, keep_ctx)) continue;
        uint64_t parent_idx = raw->parent_frn & 0xFFFFFFFFFFFFULL;
        /* dir_path 是借用指针（resolve 缓存内字符串，或栈上的盘根），
         * 本次迭代内有效，调用方不 free。 */
        const char *dir_path;
        char drive_root[64]; /* "C:\" 形，栈上足够 */
        if (parent_idx == 5) {
            snprintf(drive_root, sizeof drive_root, "%s\\", drive);
            dir_path = drive_root;
        } else {
            dir_path = resolve_dir_borrowed(raw->parent_frn, snap, &dir_cache,
                                            drive, is_drive_root, skip_top, n_skip);
            if (!dir_path) continue;
        }
        size_t dl = strlen(dir_path);
        int need_sep = dl == 0 || dir_path[dl - 1] != '\\';
        size_t n = dl + (size_t)(need_sep ? 1 : 0) + strlen(raw->name) + 1;
        char *full = xmalloc(n);
        if (!full) {
            rc = -1;
            break;
        }
        if (need_sep)
            snprintf(full, n, "%s\\%s", dir_path, raw->name);
        else
            snprintf(full, n, "%s%s", dir_path, raw->name);
        if (!is_drive_root) {
            /* 与旧实现同款判定，但不拷贝 lower、不截段串：前缀与首段
             * 都逐字符比（full 原串不改，比较时就地小写）。 */
            int skip = 0;
            if (!lower_is_prefix(full, root_lower, strlen(root_lower)) ||
                full[strlen(root_lower)] != '\\') {
                skip = 1;
            } else if (strlen(full) > root_len) {
                if (lower_has_skip_segment(full + root_len, skip_top, n_skip))
                    skip = 1;
            }
            if (skip) {
                free(full);
                continue;
            }
        }
        uint64_t age = now >= raw->mtime ? now - raw->mtime : 0;
        int recent = raw->mtime == 0 || age < CDC_MFT_RAW_READ_MIN_AGE_SECS;
        CdcFileEntry e;
        e.path = full;
        e.size = raw->size;
        e.mtime = raw->mtime;
        e.sort_key = raw->sort_key;
        e.frn = raw->frn;
        e.first_lcn = raw->first_lcn;
        e.bytes_per_cluster = snap->snap.bytes_per_cluster; /* Rust：取自卷，非记录 */
        e.can_raw_read = raw->can_raw_read && !recent;
        if (entry_list_push(out, e) != 0) {
            free(full);
            rc = -1;
            break;
        }
    }
    resolve_cache_free(&dir_cache);
    free(root_lower);
    free(snap_file);
    CloseHandle(h);
    if (!snap_cached && snap_owned) snap_internal_free(snap_owned);
    return rc;
}

int cdc_usn_changes_since(const char *root, uint64_t stored_journal_id,
                          int64_t stored_next_usn, const char *const *skip_top, size_t n_skip,
                          CdcUsnChangeSet *out) {
    memset(out, 0, sizeof *out);
    char drive[3];
    if (!parse_drive_root(root, drive)) return 1;
    char volume_path[16];
    snprintf(volume_path, sizeof volume_path, "\\\\.\\%s", drive);
    HANDLE h = open_volume_handle(volume_path);
    if (h == INVALID_HANDLE_VALUE) return 1;
    NtfsVolumeData volume;
    if (get_volume_data(h, &volume) != 0) {
        CloseHandle(h);
        return 1;
    }
    size_t record_size = volume.bytes_per_file_record_segment;
    size_t bytes_per_cluster = volume.bytes_per_cluster;
    size_t bytes_per_sector = volume.bytes_per_sector;
    if ((record_size != 1024 && record_size != 4096) || bytes_per_cluster == 0 ||
        bytes_per_sector == 0) {
        CloseHandle(h);
        return 1;
    }
    uint64_t serial = (uint64_t)volume.volume_serial_number;
    UsnJournalData journal;
    if (query_usn_journal(h, &journal) != 0) {
        CloseHandle(h);
        return 1;
    }
    if (!cdc_usn_state_usable(stored_journal_id, stored_next_usn, journal.usn_journal_id,
                              journal.first_usn, journal.next_usn)) {
        CloseHandle(h);
        return 1;
    }
    CdcUsnDelta changed = collect_usn_changed_indices(h, &journal, stored_next_usn);
    if (changed.kind != CDC_DELTA_CHANGED) {
        usn_delta_free(&changed);
        CloseHandle(h);
        return 1;
    }
    out->journal_id = journal.usn_journal_id;
    out->next_usn = journal.next_usn;
    if (changed.n == 0) {
        usn_delta_free(&changed);
        CloseHandle(h);
        return 0;
    }

    /* 路径解析需要快照的目录表（没有快照就全量重建）。 */
    char *snap_file = snapshot_path(serial);
    SnapInternal *old_snap = NULL; /* 仅目录表的旧快照 */
    SnapInternal *borrowed = mem_snapshot_get_borrowed(serial);
    if (borrowed) {
        CdcSnapshot s;
        memset(&s, 0, sizeof s);
        s.volume_serial = borrowed->snap.volume_serial;
        s.journal_id = borrowed->snap.journal_id;
        s.next_usn = borrowed->snap.next_usn;
        s.min_size = borrowed->snap.min_size;
        s.bytes_per_cluster = borrowed->snap.bytes_per_cluster;
        s.dirs = xmalloc((borrowed->snap.n_dirs ? borrowed->snap.n_dirs : 1) *
                         sizeof(CdcDirNode));
        if (!s.dirs) {
            usn_delta_free(&changed);
            free(snap_file);
            CloseHandle(h);
            return -1;
        }
        for (size_t i = 0; i < borrowed->snap.n_dirs; i++) {
            s.dirs[i] = borrowed->snap.dirs[i];
            s.dirs[i].name = xstrdup(borrowed->snap.dirs[i].name);
            if (!s.dirs[i].name) {
                s.n_dirs = i;
                cdc_snapshot_free(&s);
                usn_delta_free(&changed);
                free(snap_file);
                CloseHandle(h);
                return -1;
            }
        }
        s.n_dirs = borrowed->snap.n_dirs;
        old_snap = snap_internal_from_owned(s);
        if (!old_snap) cdc_snapshot_free(&s);
    } else if (snap_file) {
        CdcSnapshot loaded;
        if (cdc_snapshot_load(snap_file, &loaded) == 0) {
            /* 只留目录表，文件表丢掉。 */
            for (size_t i = 0; i < loaded.n_files; i++) free(loaded.files[i].name);
            free(loaded.files);
            loaded.files = NULL;
            loaded.n_files = 0;
            old_snap = snap_internal_from_owned(loaded);
            if (!old_snap) cdc_snapshot_free(&loaded);
        }
    }
    free(snap_file);
    if (!old_snap) {
        usn_delta_free(&changed);
        CloseHandle(h);
        return 1;
    }
    /* 工作目录表 = 旧表的一份深拷贝。 */
    SnapInternal *dirs = snap_clone(old_snap);
    if (!dirs) {
        snap_internal_free(old_snap);
        usn_delta_free(&changed);
        CloseHandle(h);
        return -1;
    }

    /* 第 1 遍：重读变化 FRN 的 MFT 记录（1MB 下限；小于下限的按消失处理）。 */
    ParsedDeltaList parsed;
    memset(&parsed, 0, sizeof parsed);
    int rc = 0;
    if (cdc_vec_init(&parsed.vec, sizeof(ParsedDeltaItem)) != CDC_OK ||
        apply_usn_delta(h, changed.frns, changed.n, record_size, bytes_per_sector,
                        bytes_per_cluster, CDC_MFT_SNAPSHOT_FLOOR_MIN_SIZE,
                        &parsed) != 0) {
        rc = -1;
        goto cleanup;
    }
    cdc_vec_t deleted_v, file_recs_v, dir_frn_list_v;
    memset(&deleted_v, 0, sizeof deleted_v);
    memset(&file_recs_v, 0, sizeof file_recs_v);
    memset(&dir_frn_list_v, 0, sizeof dir_frn_list_v);
    if (cdc_vec_init(&deleted_v, sizeof(uint64_t)) != CDC_OK ||
        cdc_vec_init(&file_recs_v, sizeof(CdcRawFile)) != CDC_OK ||
        cdc_vec_init(&dir_frn_list_v, sizeof(uint64_t)) != CDC_OK) {
        rc = -1;
        goto cleanup;
    }
    CdcU64Map seen;
    memset(&seen, 0, sizeof seen);
    if (u64map_init(&seen, parsed.vec.len ? parsed.vec.len : 16) != 0) {
        rc = -1;
        goto cleanup;
    }
    for (size_t i = 0; i < parsed.vec.len; i++) {
        uint64_t idx = CDC_VEC_AT(&parsed.vec, ParsedDeltaItem, i).idx;
        u64map_put(&seen, idx, 1);
        CdcParsedRecord *rec = &CDC_VEC_AT(&parsed.vec, ParsedDeltaItem, i).rec;
        if (rec->kind == CDC_REC_DIR) {
            CdcDirNode node;
            node.index = rec->index;
            node.seq = rec->seq;
            node.parent_frn = rec->parent_frn;
            node.name = rec->name;
            node.reparse = rec->reparse;
            rec->name = NULL;
            snap_dir_put(dirs, node);
            if (cdc_vec_push(&dir_frn_list_v, &idx) != CDC_OK) {
                rc = -1;
                break;
            }
        } else if (rec->kind == CDC_REC_FILE) {
            CdcRawFile fr;
            fr.frn = rec->frn;
            fr.parent_frn = rec->parent_frn;
            fr.name = rec->name;
            fr.size = rec->size;
            fr.mtime = rec->mtime;
            fr.first_lcn = rec->first_lcn;
            fr.sort_key = rec->sort_key;
            fr.can_raw_read = rec->can_raw_read;
            if (cdc_vec_push(&file_recs_v, &fr) != CDC_OK) {
                rc = -1;
                break;
            }
            rec->name = NULL;
        } else {
            if (cdc_vec_push(&deleted_v, &idx) != CDC_OK) {
                rc = -1;
                break;
            }
        }
    }
    if (rc == 0) {
        for (size_t i = 0; i < changed.n; i++) {
            uintptr_t dummy = 0;
            if (!u64map_get(&seen, changed.frns[i], &dummy)) {
                if (cdc_vec_push(&deleted_v, &changed.frns[i]) != CDC_OK) {
                    rc = -1;
                    break;
                }
            }
        }
    }
    u64map_free(&seen);

    /* 第 2 遍：用旧/新两份目录表解析本次变化目录的改名。 */
    cdc_vec_t ren_old_v, ren_new_v;
    memset(&ren_old_v, 0, sizeof ren_old_v);
    memset(&ren_new_v, 0, sizeof ren_new_v);
    if (cdc_vec_init(&ren_old_v, sizeof(char *)) != CDC_OK ||
        cdc_vec_init(&ren_new_v, sizeof(char *)) != CDC_OK) {
        rc = -1;
    }
    if (rc == 0) {
        CdcU64Map old_cache, new_cache;
        memset(&old_cache, 0, sizeof old_cache);
        memset(&new_cache, 0, sizeof new_cache);
        u64map_init(&old_cache, 64);
        u64map_init(&new_cache, 64);
        for (size_t i = 0; i < dir_frn_list_v.len; i++) {
            char *o = resolve_dir(CDC_VEC_AT(&dir_frn_list_v, uint64_t, i), old_snap, &old_cache, drive, 1,
                                  skip_top, n_skip);
            char *n = resolve_dir(CDC_VEC_AT(&dir_frn_list_v, uint64_t, i), dirs, &new_cache, drive, 1, skip_top,
                                  n_skip);
            if (o && n && strcmp(o, n) != 0) {
                if (cdc_vec_push(&ren_old_v, &o) != CDC_OK) {
                    free(o);
                    free(n);
                    rc = -1;
                    break;
                }
                if (cdc_vec_push(&ren_new_v, &n) != CDC_OK) {
                    /* 新串没进去，旧串已入队：弹出旧串释放，保持两队等长。 */
                    char *dropped = NULL;
                    cdc_vec_pop(&ren_old_v, &dropped);
                    free(dropped);
                    free(n);
                    rc = -1;
                    break;
                }
            } else {
                free(o);
                free(n);
            }
        }
        resolve_cache_free(&old_cache);
        resolve_cache_free(&new_cache);
    }

    /* 第 3 遍：用更新后的目录表解析变化文件的完整路径。 */
    cdc_vec_t files_v;
    memset(&files_v, 0, sizeof files_v);
    if (cdc_vec_init(&files_v, sizeof(CdcUsnChangedFile)) != CDC_OK) rc = -1;
    if (rc == 0) {
        CdcU64Map dir_cache;
        memset(&dir_cache, 0, sizeof dir_cache);
        u64map_init(&dir_cache, 64);
        for (size_t i = 0; i < file_recs_v.len; i++) {
            CdcRawFile *raw = &CDC_VEC_AT(&file_recs_v, CdcRawFile, i);
            uint64_t parent = raw->parent_frn & 0xFFFFFFFFFFFFULL;
            /* dir_path 是借用指针（resolve 缓存内字符串，或栈上的盘根），
             * 本次迭代内有效，调用方不 free。 */
            const char *dir_path = NULL;
            char drive_root[64]; /* "C:\" 形，栈上足够 */
            if (parent == 5) {
                snprintf(drive_root, sizeof drive_root, "%s\\", drive);
                dir_path = drive_root;
            } else {
                dir_path = resolve_dir_borrowed(raw->parent_frn, dirs, &dir_cache,
                                                drive, 1, skip_top, n_skip);
            }
            if (!dir_path) {
                uint64_t gone = raw->frn & 0xFFFFFFFFFFFFULL;
                if (cdc_vec_push(&deleted_v, &gone) != CDC_OK) {
                    rc = -1;
                    break;
                }
                continue;
            }
            size_t dl = strlen(dir_path);
            int need_sep = dl == 0 || dir_path[dl - 1] != '\\';
            size_t n = dl + (size_t)(need_sep ? 1 : 0) + strlen(raw->name) + 1;
            char *full = xmalloc(n);
            if (!full) {
                rc = -1;
                break;
            }
            if (need_sep)
                snprintf(full, n, "%s\\%s", dir_path, raw->name);
            else
                snprintf(full, n, "%s%s", dir_path, raw->name);
            CdcUsnChangedFile cf;
            cf.frn = raw->frn;
            cf.path = full;
            cf.size = raw->size;
            cf.mtime = raw->mtime;
            if (cdc_vec_push(&files_v, &cf) != CDC_OK) {
                free(full);
                rc = -1;
                break;
            }
        }
        resolve_cache_free(&dir_cache);
    }

    if (rc == 0) {
        out->deleted_frns = deleted_v.data;
        out->n_deleted = deleted_v.len;
        deleted_v.data = NULL; /* 交给 out */
        out->files = files_v.data;
        out->n_files = files_v.len;
        files_v.data = NULL;
        out->renamed_old = ren_old_v.data;
        out->renamed_new = ren_new_v.data;
        out->n_renamed = ren_old_v.len;
        ren_old_v.data = NULL;
        ren_new_v.data = NULL;
    } else {
        for (size_t i = 0; i < files_v.len; i++)
            free(CDC_VEC_AT(&files_v, CdcUsnChangedFile, i).path);
        for (size_t i = 0; i < ren_old_v.len; i++) {
            free(CDC_VEC_AT(&ren_old_v, char *, i));
            free(CDC_VEC_AT(&ren_new_v, char *, i));
        }
        memset(out, 0, sizeof *out);
    }
    for (size_t i = 0; i < file_recs_v.len; i++)
        free(CDC_VEC_AT(&file_recs_v, CdcRawFile, i).name);
    cdc_vec_free(&deleted_v);
    cdc_vec_free(&files_v);
    cdc_vec_free(&ren_old_v);
    cdc_vec_free(&ren_new_v);
    cdc_vec_free(&file_recs_v);
    cdc_vec_free(&dir_frn_list_v);
    parsed_delta_list_free(&parsed);
cleanup:
    snap_internal_free(old_snap);
    snap_internal_free(dirs);
    usn_delta_free(&changed);
    CloseHandle(h);
    return rc == 0 ? 0 : -1;
}

#else /* ! _WIN32 */

int cdc_volume_usn_state(const char *root, uint64_t *volume_serial, uint64_t *journal_id,
                         int64_t *first_usn, int64_t *next_usn) {
    (void)root;
    (void)volume_serial;
    (void)journal_id;
    (void)first_usn;
    (void)next_usn;
    return -1;
}

int cdc_mft_try_scan(const char *root, uint64_t min_size,
                     const char *const *skip_top, size_t n_skip,
                     cdc_keep_fn keep, void *keep_ctx, CdcFileEntryList *out) {
    (void)root;
    (void)min_size;
    (void)skip_top;
    (void)n_skip;
    (void)keep;
    (void)keep_ctx;
    (void)out;
    return 1; /* 不适用：调用方退回目录遍历 */
}

int cdc_usn_changes_since(const char *root, uint64_t stored_journal_id,
                          int64_t stored_next_usn, const char *const *skip_top, size_t n_skip,
                          CdcUsnChangeSet *out) {
    (void)root;
    (void)stored_journal_id;
    (void)stored_next_usn;
    (void)skip_top;
    (void)n_skip;
    (void)out;
    return 1; /* 无 USN：调用方全量重建 */
}

#endif /* _WIN32 */

void cdc_usn_change_set_free(CdcUsnChangeSet *s) {
    if (!s) return;
    free(s->deleted_frns);
    for (size_t i = 0; i < s->n_files; i++) free(s->files[i].path);
    free(s->files);
    for (size_t i = 0; i < s->n_renamed; i++) {
        free(s->renamed_old[i]);
        free(s->renamed_new[i]);
    }
    free(s->renamed_old);
    free(s->renamed_new);
    memset(s, 0, sizeof *s);
}

/* ---------------- 目录遍历退路 ---------------- */

typedef struct {
    char *path;
    int is_top;
} WalkDir;

typedef struct {
    cdc_vec_t vec; /* 元素 WalkDir */
} WalkStack;

static int walk_stack_push(WalkStack *st, const char *path, int is_top) {
    WalkDir wd;
    wd.path = xstrdup(path);
    if (!wd.path) return -1;
    wd.is_top = is_top;
    if (cdc_vec_push(&st->vec, &wd) != CDC_OK) {
        free(wd.path);
        return -1;
    }
    return 0;
}

static char *join_path(const char *dir, const char *name) {
    size_t dl = strlen(dir);
    int need_sep = dl == 0 || (dir[dl - 1] != '/' && dir[dl - 1] != '\\');
    size_t n = dl + (size_t)(need_sep ? 1 : 0) + strlen(name) + 1;
    char *p = xmalloc(n);
    if (!p) return NULL;
    if (need_sep)
        snprintf(p, n, "%s/%s", dir, name);
    else
        snprintf(p, n, "%s%s", dir, name);
    return p;
}

typedef struct {
    WalkStack *st;
    CdcFileEntryList *out;
    uint64_t min_size;
    const char *const *skip_top;
    size_t n_skip;
    const char *cur_path; /* 当前目录（借用，不拥有） */
    int cur_is_top;
} WalkCbCtx;

/* 回调返回 0 继续、-1 致命错误（NOMEM）；cdc_enum_dir 原样返回它。
 * 过滤语义与旧的两份实现逐项一致：跳过 "$" 开头名、不跟随链接
 * （POSIX 符号链接 / Windows 重解析点）、Windows 再跳过离线/
 * 按需取回属性；顶层目录才比 skip_top。 */
static int walk_enum_cb(const char *name, int is_dir, int is_symlink, uint64_t size,
                        uint64_t mtime, uint32_t attrs, void *ctx) {
    WalkCbCtx *c = (WalkCbCtx *)ctx;
    char *full;
    if (name[0] == '$') return 0;
    if (is_symlink) return 0; /* 不跟随链接 */
#ifdef _WIN32
    if ((attrs & CDC_MFT_SKIP_FILE_ATTRS) != 0) return 0; /* 离线/按需取回 */
#else
    (void)attrs;
#endif
    full = join_path(c->cur_path, name);
    if (!full) return -1;
    if (is_dir) {
        if (c->cur_is_top && name_in_skip_top(name, c->skip_top, c->n_skip)) {
            free(full);
            return 0;
        }
        if (walk_stack_push(c->st, full, 0) != 0) {
            free(full);
            return -1;
        }
        free(full);
        return 0;
    }
    if (size >= c->min_size) {
        CdcFileEntry en;
        en.path = full;
        en.size = size;
        en.mtime = mtime;
        en.sort_key = 0;
        en.frn = 0;
        en.first_lcn = 0;
        en.bytes_per_cluster = 0;
        en.can_raw_read = 0;
        if (entry_list_push(c->out, en) != 0) {
            free(full);
            return -1;
        }
        return 0;
    }
    free(full);
    return 0;
}

/* 单一实现走 cdc_enum_dir（util.c）：Windows 用 NtQueryDirectoryFile
 * 批量取，POSIX 用 opendir/readdir。打不开的目录静默跳过。 */
int cdc_fallback_walk_files(const char *root, uint64_t min_size,
                            const char *const *skip_top, size_t n_skip,
                            CdcFileEntryList *out) {
    WalkStack st;
    WalkCbCtx ctx;
    int rc = 0;
    memset(&st, 0, sizeof st);
    if (cdc_vec_init(&st.vec, sizeof(WalkDir)) != CDC_OK) return -1;
    if (walk_stack_push(&st, root, 1) != 0) {
        cdc_vec_free(&st.vec);
        return -1;
    }
    ctx.st = &st;
    ctx.out = out;
    ctx.min_size = min_size;
    ctx.skip_top = skip_top;
    ctx.n_skip = n_skip;
    ctx.cur_path = NULL;
    ctx.cur_is_top = 0;
    while (st.vec.len > 0) {
        WalkDir cur;
        int r;
        cdc_vec_pop(&st.vec, &cur);
        ctx.cur_path = cur.path;
        ctx.cur_is_top = cur.is_top;
        r = cdc_enum_dir(cur.path, walk_enum_cb, &ctx);
        free(cur.path);
        /* 打开失败（缺失/不可读）的目录直接跳过，与旧实现一致；
         * 其它返回（回调 -1、NOMEM）为致命错误。 */
        if (r != CDC_OK && r != CDC_ERR_NOT_FOUND && r != CDC_ERR_IO) {
            rc = -1;
            break;
        }
    }
    while (st.vec.len > 0) {
        WalkDir wd;
        cdc_vec_pop(&st.vec, &wd);
        free(wd.path);
    }
    cdc_vec_free(&st.vec);
    return rc;
}

int cdc_scan_files(const char *root, uint64_t min_size,
                   const char *const *skip_top, size_t n_skip,
                   cdc_keep_fn keep, void *keep_ctx, CdcFileEntryList *out) {
    int r = cdc_mft_try_scan(root, min_size, skip_top, n_skip, keep, keep_ctx, out);
    if (r == 0) return 0;
    if (r < 0) return r;
    /* r == 1：退回遍历，keep 在名字层再过滤一次（Rust 的 scan_files_where 同款）。 */
    CdcFileEntryList walked;
    memset(&walked, 0, sizeof walked);
    if (cdc_fallback_walk_files(root, min_size, skip_top, n_skip, &walked) != 0)
        return -1;
    int rc = 0;
    for (size_t i = 0; i < walked.n; i++) {
        CdcFileEntry *e = &walked.items[i];
        const char *base = e->path;
        for (const char *p = e->path; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        if (keep && !keep(base, e->size, keep_ctx)) {
            free(e->path);
            e->path = NULL;
            continue;
        }
        if (entry_list_push(out, *e) != 0) {
            free(e->path);
            e->path = NULL;
            rc = -1;
            break;
        }
        e->path = NULL; /* 所有权已转移给 out */
    }
    for (size_t i = 0; i < walked.n; i++) free(walked.items[i].path);
    free(walked.items);
    return rc;
}
