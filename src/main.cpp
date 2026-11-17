/* main.c — cdc-c 正式入口
 *
 *   cdc selftest | --selftest  移植单元测试（试点 + fileindex + indextask）
 *   cdc --index [--quiet] [--rebuild]
 *                             常驻索引：逐卷增量优先、断档回填（对应 Rust
 *                             main.rs 的 run_index / index_volume；
 *                             非管理员直接 exit 2，不自提权）
 *   cdc big <dir>             目录树大文件扫描（试点演示，对标 Rust scan_big）
 *   cdc probe <C>             [仅 Windows] 打开卷、读 NTFS 卷信息（试点）
 *
 * --index 流程（与 Rust 版一致）：
 *   每卷：volume_usn_state → 游标可用时 USN 增量追赶，否则全量回填；
 *   回填：扫 ≥1MB → 跳过 frn==0 → upsert 元数据 → 写游标（先于哈希，
 *   中断后续跑只补哈希）→ 复用 file_hashes 缓存摘要 → 同 size ≥2 的组
 *   进哈希管线 → 摘要双写（hashcache + index）→ 进度打到
 *   logs/index-YYYYMMDD.log。退出码：0 全卷 OK、1 部分失败、2 未提权。
 *
 * 卷/USN/枚举接口用 mft.h（切片 2 已落地）；hashcache.c / scan.c
 * （切片 3）的接口见 hashcache.h / scan.h（真实现已落地，临时桩已删）。
 * GUI 入口在 main() 默认分支（无 CLI 参数时）：先 cdc_host_prewarm()，
 * 再 cdc_gui_register_commands() 注册 45 个后端命令，最后
 * cdc_host_run() 跑消息循环（见 src/gui.c + host/host.cpp）。
 * 未决事项（见 gui.c 头注释）：GUI 提权自启、计划任务后台重注册、
 * 启动时 recover_pending_move()——Rust main() 里有，C 版尚未决定。
 */
#ifndef _WIN32
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "bigfile.h"
#include "mft_util.h"
#include "fileindex.h"
#include "hashcache.h" /* cdc_hashcache_*（真实现；桩已删） */
#include "indextask.h"
#include "mft.h" /* CdcFileEntry、cdc_scan_files、USN */
#include "scan.h" /* cdc_scan_input_t、cdc_scan_hash_groups（真实现；桩已删） */
#include "gui.h" /* cdc_gui_register_commands（GUI 入口） */

#ifdef _WIN32
#include "bridge.h" /* cdc_host_prewarm / cdc_host_run（extern "C"）；Windows 构建加 -Ihost */
#endif

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h> /* IsUserAnAdmin */
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#endif

/* ================= 自测（试点，保留） ================= */

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_filetime(void) {
    /* 1970-01-01T00:00:00Z 的 FILETIME = 11644473600 * 10^7 */
    CHECK(cdc::mft_filetime_to_unix(116444736000000000ULL) == 0, "filetime epoch");
    /* 2020-01-01T00:00:00Z: unix 1577836800 */
    CHECK(cdc::mft_filetime_to_unix(132223104000000000ULL) == 1577836800ULL, "filetime 2020");
    CHECK(cdc::mft_filetime_to_unix(0) == 0, "filetime 0 saturates");
    CHECK(cdc::mft_filetime_to_unix(50000000ULL) == 0, "filetime pre-epoch saturates");
}

static void test_runlist(void) {
    /* run1: header 0x11 len=2 off=+5 -> (vcn 0, lcn 5, len 2)
     * run2: header 0x21 len=3 off=+16 -> (vcn 2, lcn 21, len 3) */
    const uint8_t buf[] = {0x11, 0x02, 0x05, 0x21, 0x03, 0x10, 0x00, 0x00};
    std::vector<cdc::MftRun> runs = cdc::mft_parse_runlist(buf, sizeof buf);
    size_t n = runs.size();
    CHECK(n == 2, "runlist count");
    if (n == 2) {
        CHECK(runs[0].vcn == 0 && runs[0].lcn == 5 && runs[0].len == 2, "runlist run1");
        CHECK(runs[1].vcn == 2 && runs[1].lcn == 21 && runs[1].len == 3, "runlist run2");
    }
    /* 稀疏 run：header 0x01（len_size=1, off_size=0）-> lcn -1 */
    const uint8_t sparse[] = {0x01, 0x04, 0x00};
    runs = cdc::mft_parse_runlist(sparse, sizeof sparse);
    CHECK(runs.size() == 1 && runs[0].lcn == -1 && runs[0].len == 4, "runlist sparse");
    /* 非法 header（len_size=0）直接停，之前已解析的保留 */
    const uint8_t bad[] = {0x11, 0x02, 0x05, 0x00};
    runs = cdc::mft_parse_runlist(bad, sizeof bad);
    CHECK(runs.size() == 1, "runlist stops on bad header");
}

static void test_first_extent(void) {
    uint64_t lcn = 0, clusters = 0;
    const uint8_t ok[] = {0x21, 0x10, 0x34, 0x12}; /* len 16, lcn 0x1234 */
    cdc::mft_parse_first_extent(ok, sizeof ok, lcn, clusters);
    CHECK(lcn == 0x1234 && clusters == 16, "first_extent ok");
    const uint8_t neg[] = {0x21, 0x01, 0xFF, 0xFF}; /* lcn -1 -> 无效 */
    cdc::mft_parse_first_extent(neg, sizeof neg, lcn, clusters);
    CHECK(lcn == 0 && clusters == 0, "first_extent negative lcn");
    cdc::mft_parse_first_extent(NULL, 0, lcn, clusters);
    CHECK(lcn == 0 && clusters == 0, "first_extent empty");
}

static void test_fixups(void) {
    uint8_t rec[1024];
    memset(rec, 0, sizeof rec);
    /* usa_off=8, usa_count=3；usa[1]=AA BB, usa[2]=CC DD */
    rec[4] = 8; rec[5] = 0; rec[6] = 3; rec[7] = 0;
    rec[8] = 0x55; rec[9] = 0x55;
    rec[10] = 0xAA; rec[11] = 0xBB;
    rec[12] = 0xCC; rec[13] = 0xDD;
    rec[510] = 0x11; rec[511] = 0x22;
    rec[1022] = 0x33; rec[1023] = 0x44;
    cdc::mft_apply_fixups(rec, sizeof rec, 512);
    CHECK(rec[510] == 0xAA && rec[511] == 0xBB, "fixup sector 1");
    CHECK(rec[1022] == 0xCC && rec[1023] == 0xDD, "fixup sector 2");
    CHECK(rec[8] == 0x55, "fixup leaves usa array alone");
}

static void test_utf16(void) {
    /* "A" + U+4E2D(中) + 代理对 U+1F600(😀) */
    const uint8_t bytes[] = {0x41, 0x00, 0x2D, 0x4E, 0x3D, 0xD8, 0x00, 0xDE};
    std::string s = cdc::mft_utf16le_to_utf8(bytes, sizeof bytes);
    CHECK(s == "A\xe4\xb8\xad\xf0\x9f\x98\x80", "utf16 incl. surrogate pair");
}

static void test_thresholds(void) {
    /* 与 Rust main.rs big_file_thresholds_follow_kbigfile_tiers 逐条一致 */
    const uint64_t MB = cdc::kMB;
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\setup.exe") == 100 * MB, "thr exe");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\setup.msi") == 100 * MB, "thr msi");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\movie.mp4") == 20 * MB, "thr mp4");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\movie.MP4") == 20 * MB, "thr MP4 case");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\clip.tmp") == 20 * MB, "thr tmp");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\song.mp3") == 5 * MB, "thr mp3");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\pack.zip") == 5 * MB, "thr zip");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\app.apk") == 5 * MB, "thr apk");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\photo.jpg") == 2 * MB, "thr jpg");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\doc.pdf") == 2 * MB, "thr pdf");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\app.log") == 2 * MB, "thr log");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\data.bin") == 50 * MB, "thr bin fallback");
    CHECK(cdc::big_file_threshold_bytes("C:\\a\\README") == 50 * MB, "thr no-ext fallback");
    CHECK(cdc::is_system_file_name("C:\\pagefile.sys"), "sys pagefile");
    CHECK(!cdc::is_system_file_name("C:\\a\\movie.mp4"), "sys negative");
}

static int run_selftest(void) {
    test_filetime();
    test_runlist();
    test_first_extent();
    test_fixups();
    test_utf16();
    test_thresholds();
    printf("selftest: %d checks, %d failures\n", checks, failures);
    int total_fail = failures;
    total_fail += cdc_fileindex_selftest();
    total_fail += cdc_indextask_selftest();
    printf("selftest total: %d failures\n", total_fail);
    return total_fail == 0 ? 0 : 1;
}

/* ================= big 扫描演示（试点，保留；POSIX 目录遍历） ================= */

#ifndef _WIN32
#include <dirent.h>

typedef struct { char *path; uint64_t size; } BigRow;
static BigRow *rows = NULL;
static size_t nrows = 0, caprows = 0;

static void add_row(const char *path, uint64_t size) {
    if (nrows == caprows) {
        caprows = caprows ? caprows * 2 : 256;
        rows = (BigRow *)realloc(rows, caprows * sizeof(BigRow));
        if (!rows) { perror("realloc"); exit(1); }
    }
    rows[nrows].path = strdup(path);
    rows[nrows].size = size;
    nrows++;
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *full = (char *)malloc(strlen(dir) + strlen(e->d_name) + 2);
        sprintf(full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) { free(full); continue; }
        if (S_ISDIR(st.st_mode)) {
            walk(full);
        } else if (S_ISREG(st.st_mode)) {
            uint64_t size = (uint64_t)st.st_size;
            /* 与 Rust scan_big 相同：2MB 预过滤 + 分档阈值 + 跳过系统文件 */
            if (size >= cdc::kScanMinBytes &&
                size >= cdc::big_file_threshold_bytes(full) &&
                !cdc::is_system_file_name(full))
                add_row(full, size);
        }
        free(full);
    }
    closedir(d);
}

static int cmp_desc(const void *a, const void *b) {
    uint64_t x = ((const BigRow *)a)->size, y = ((const BigRow *)b)->size;
    return (x < y) - (x > y);
}

static int run_big(const char *dir) {
    walk(dir);
    qsort(rows, nrows, sizeof(BigRow), cmp_desc);
    for (size_t i = 0; i < nrows; i++)
        printf("%llu\t%s\n", (unsigned long long)rows[i].size, rows[i].path);
    fprintf(stderr, "%zu big files\n", nrows);
    return 0;
}
#endif

/* ================= --index 编排 ================= */

/* 顶层跳过目录用 mft.h 的 CDC_TOP_SKIP / CDC_TOP_SKIP_N
 *（与 Rust main.rs 的 TOP_SKIP 逐字一致）。CDC_TOP_SKIP 是
 * const char *const[]，接口参数是 const char *const *，无需 cast。 */
static double now_mono(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static int is_admin(void) {
#ifdef _WIN32
    return IsUserAnAdmin() != 0;
#else
    return geteuid() == 0;
#endif
}

/* 固定磁盘列表（如 "C:\"）。与 Rust main.rs 的 fixed_drives 一致：
 * GetLogicalDrives + DRIVE_FIXED；一个都没有时回落到 ["C:\"]。
 * 非 Windows 返回空列表（没有 NTFS 固定盘的概念）。
 * 返回 malloc 的字符串数组（可能为 NULL 且 *out_n=0），调用方 free。 */
static char **fixed_drives(size_t *out_n) {
    if (out_n) *out_n = 0;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    char **drives = NULL;
    size_t n = 0, cap = 0;
    for (int i = 0; i < 26; i++) {
        if ((mask & (1u << i)) == 0) continue;
        char root[4] = {(char)('A' + i), ':', '\\', '\0'};
        wchar_t *w = cdc_utf8_to_wide(root);
        if (!w) continue;
        int fixed = GetDriveTypeW(w) == DRIVE_FIXED;
        free(w);
        if (!fixed) continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4;
            char **np = (char **)realloc(drives, ncap * sizeof(char *));
            if (!np) break;
            drives = np;
            cap = ncap;
        }
        drives[n] = strdup(root);
        if (!drives[n]) break;
        n++;
    }
    if (n == 0) {
        /* 回落：与 Rust 一致，哪怕没有固定盘也试 C:。 */
        free(drives);
        drives = (char **)malloc(sizeof(char *));
        if (drives) {
            drives[0] = strdup("C:\\");
            if (!drives[0]) {
                free(drives);
                drives = NULL;
            } else {
                n = 1;
            }
        }
    }
    if (out_n) *out_n = n;
    return drives;
#else
    (void)0;
    return NULL;
#endif
}

static void free_drives(char **drives, size_t n) {
    if (!drives) return;
    for (size_t i = 0; i < n; i++) free(drives[i]);
    free(drives);
}

/* 文件的 mtime（unix 秒），取不到返回 0（对应 Rust file_mtime）。 */
static uint64_t file_mtime(const char *path) {
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    struct _stat st;
    int rc = _wstat(w, &st);
    free(w);
    return rc == 0 && st.st_mtime > 0 ? (uint64_t)st.st_mtime : 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime > 0 ? (uint64_t)st.st_mtime : 0;
#endif
}

/* 索引进度日志：logs/index-YYYYMMDD.log（本地日期）。写失败静默忽略。
 * 目录用 util.h 的 cdc_log_dir（与 Rust logs::log_dir() 同规则，
 * 且内部已创建目录）。 */
static void index_log(const char *msg) {
    char dir[4096];
    if (cdc_log_dir(dir, sizeof(dir)) != CDC_OK) return;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char datestr[16] = "19700101";
    if (lt) {
        /* 显式收紧区间，让 -Wformat-truncation 能证明不截断
         *（值本身已是合法日期；与 indextask.c 的同款处理一致）。 */
        int y = lt->tm_year + 1900;
        int mo = lt->tm_mon + 1;
        int d = lt->tm_mday;
        if (y < 0) y = 0;
        if (y > 9999) y = 9999;
        if (mo < 1) mo = 1;
        if (mo > 12) mo = 12;
        if (d < 1) d = 1;
        if (d > 31) d = 31;
        snprintf(datestr, sizeof(datestr), "%04d%02d%02d", y, mo, d);
    }
#ifdef _WIN32
    const char *sep = "\\";
#else
    const char *sep = "/";
#endif
    char path[4200];
    if (snprintf(path, sizeof(path), "%s%sindex-%s.log", dir, sep, datestr) >=
        (int)sizeof(path))
        return;
    FILE *f = NULL;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (w) {
        f = _wfopen(w, L"a");
        free(w);
    }
#else
    f = fopen(path, "a");
#endif
    if (!f) return;
    fprintf(f, "%s\n", msg);
    fclose(f);
}

/* ---------- 单卷索引结果 ---------- */

typedef enum { IDX_BACKFILLED, IDX_INCREMENTED, IDX_FAILED } idx_status_t;

typedef struct {
    idx_status_t status;
    uint64_t files;
    uint64_t deleted;
    uint64_t renamed;
    uint64_t reused_full;
    uint64_t hashed_new;
    double elapsed_secs;
} idx_outcome_t;

/* 合并两组摘要：a 优先，a 没有的用 b 补（对应 Rust 的 or_else 链）。
 * xxh 三档与 BLAKE3 四档都按同一规则合并。 */
static void merge_digests(cdc_digest_set_t *dst, const cdc_digest_set_t *a,
                          const cdc_digest_set_t *b) {
    dst->has_head = a->has_head;
    dst->head = a->head;
    if (!dst->has_head && b->has_head) {
        dst->has_head = 1;
        dst->head = b->head;
    }
    dst->has_sample = a->has_sample;
    dst->sample = a->sample;
    if (!dst->has_sample && b->has_sample) {
        dst->has_sample = 1;
        dst->sample = b->sample;
    }
    dst->has_strong = a->has_strong;
    dst->strong = a->strong;
    if (!dst->has_strong && b->has_strong) {
        dst->has_strong = 1;
        dst->strong = b->strong;
    }
    dst->has_full = a->has_full;
    dst->full = a->full;
    if (!dst->has_full && b->has_full) {
        dst->has_full = 1;
        dst->full = b->full;
    }
    dst->has_head_xxh = a->has_head_xxh;
    dst->head_xxh = a->head_xxh;
    if (!dst->has_head_xxh && b->has_head_xxh) {
        dst->has_head_xxh = 1;
        dst->head_xxh = b->head_xxh;
    }
    dst->has_sample_xxh = a->has_sample_xxh;
    dst->sample_xxh = a->sample_xxh;
    if (!dst->has_sample_xxh && b->has_sample_xxh) {
        dst->has_sample_xxh = 1;
        dst->sample_xxh = b->sample_xxh;
    }
    dst->has_strong_xxh = a->has_strong_xxh;
    dst->strong_xxh = a->strong_xxh;
    if (!dst->has_strong_xxh && b->has_strong_xxh) {
        dst->has_strong_xxh = 1;
        dst->strong_xxh = b->strong_xxh;
    }
}

static int any_digest(const cdc_digest_set_t *d) {
    return d->has_head || d->has_sample || d->has_strong || d->has_full ||
           d->has_head_xxh || d->has_sample_xxh || d->has_strong_xxh;
}

/* 增量维护的哈希补算：对"同 size ≥ 2 且组内至少一个缺 full"的组跑哈希管线。
 * 摘要来源优先级：索引行已有 > file_hashes 缓存 > 管线新算；新摘要双写
 * （索引 + 缓存，缓存的写回由管线内部做）。
 * 返回（从缓存复用的 full 个数，管线新算的 full 个数）。 */
/* idx_dig 按 frn 升序：二分查找。 */
static const cdc_index_digest_entry_t *find_idx_digest(
    const cdc_index_digest_entry_t *arr, size_t n, uint64_t frn) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (arr[mid].frn == frn) return &arr[mid];
        if (arr[mid].frn < frn) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static void hash_index_groups(cdc_fileindex_t *idx, uint64_t serial, uint64_t stamp_now,
                              uint64_t *out_reused, uint64_t *out_new) {
    *out_reused = 0;
    *out_new = 0;
    size_t n = 0;
    cdc_index_hash_row_t *rows = cdc_fileindex_rows_needing_full(idx, serial, &n);
    if (!rows || n == 0) {
        cdc_index_hash_rows_free(rows, n);
        return;
    }
    const char **paths = (const char **)malloc(n * sizeof(char *));
    if (!paths) {
        cdc_index_hash_rows_free(rows, n);
        return;
    }
    for (size_t i = 0; i < n; i++) paths[i] = rows[i].path;
    cdc_hashcache_t *cache = cdc_hashcache_load_for_paths(paths, n);
    free(paths);

    cdc_scan_input_t *inputs = (cdc_scan_input_t *)malloc(n * sizeof(*inputs));
    if (!inputs) {
        if (cache) cdc_hashcache_close(cache);
        cdc_index_hash_rows_free(rows, n);
        return;
    }
    uint64_t merged_full = 0;
    for (size_t i = 0; i < n; i++) {
        cdc_digest_set_t merged = rows[i].digests;
        if (cache) {
            cdc_digest_set_t ce;
            memset(&ce, 0, sizeof(ce));
            if (cdc_hashcache_entry_for(cache, rows[i].path, rows[i].size,
                                        rows[i].mtime, &ce) == 1) {
                if (!rows[i].digests.has_full && ce.has_full) (*out_reused)++;
                cdc_digest_set_t tmp;
                merge_digests(&tmp, &rows[i].digests, &ce);
                merged = tmp;
            }
        }
        if (merged.has_full) merged_full++;
        inputs[i].path = rows[i].path;
        inputs[i].size = rows[i].size;
        inputs[i].mtime = rows[i].mtime;
        inputs[i].frn = rows[i].frn;
        inputs[i].digests = merged;
    }
    cdc_digest_update_t *updates = NULL;
    size_t n_upd = 0;
    if (cdc_scan_hash_groups(inputs, n, cache, stamp_now, &updates, &n_upd) == 0 &&
        n_upd > 0) {
        /* scan.c 的 updates 里 volume_serial 填 0（inputs 里没有卷号）；
         * 写回前必须按卷填好，否则 WHERE volume_serial=?1 匹配不到行。 */
        for (size_t i = 0; i < n_upd; i++) updates[i].volume_serial = serial;
        cdc_fileindex_set_digests_batch(idx, updates, n_upd, stamp_now);
        uint64_t upd_full = 0;
        for (size_t i = 0; i < n_upd; i++) {
            if (updates[i].digests.has_full) upd_full++;
        }
        *out_new = upd_full > merged_full ? upd_full - merged_full : 0;
        free(updates);
    }
    free(inputs);
    if (cache) cdc_hashcache_close(cache);
    cdc_index_hash_rows_free(rows, n);
}

/* 增量维护：把一个 USN 变化集应用到索引（删行 / 路径前缀改写 / 元数据
 * upsert），再对缺失摘要的组做哈希补算。游标在元数据落库后、哈希之前写
 * ——中断后续跑只补缺失摘要，不重扫。 */
static idx_outcome_t index_volume_incremental(cdc_fileindex_t *idx, const char *drive,
                                              uint64_t serial,
                                              cdc::UsnChangeSet *changes,
                                              int quiet, int do_hash, double started) {
    idx_outcome_t o;
    memset(&o, 0, sizeof(o));
    o.status = IDX_FAILED;
    uint64_t stamp_now = cdc::now_seconds();
    o.deleted = cdc_fileindex_delete_rows(idx, serial, changes->deleted_frns.data(),
                                          changes->deleted_frns.size());
    for (size_t i = 0; i < changes->renamed_old.size(); i++) {
        o.renamed += cdc_fileindex_update_path_prefix(idx, serial, changes->renamed_old[i].c_str(),
                                                      changes->renamed_new[i].c_str());
    }
    size_t n = changes->files.size();
    cdc_index_row_t *rows = NULL;
    if (n > 0) {
        rows = (cdc_index_row_t *)malloc(n * sizeof(*rows));
        if (!rows) {
            o.elapsed_secs = now_mono() - started;
            return o;
        }
        for (size_t i = 0; i < n; i++) {
            rows[i].volume_serial = serial;
            rows[i].frn = changes->files[i].frn;
            rows[i].path = (char *)changes->files[i].path.c_str();
            rows[i].size = changes->files[i].size;
            rows[i].mtime = changes->files[i].mtime;
            rows[i].last_seen = stamp_now;
        }
    }
    o.files = (uint64_t)n;
    if (n > 0 && cdc_fileindex_upsert_files(idx, rows, n) == 0) {
        /* 事务失败：游标不写，下次重新追赶。 */
        char msg[1024];
        snprintf(msg, sizeof(msg), "--index：%s 增量元数据写入失败。", drive);
        fprintf(stderr, "%s\n", msg);
        index_log(msg);
        free(rows);
        o.files = 0;
        o.elapsed_secs = now_mono() - started;
        return o;
    }
    free(rows);
    cdc_fileindex_set_usn_state(idx, serial, changes->journal_id, changes->next_usn);
    if (do_hash) {
        hash_index_groups(idx, serial, stamp_now, &o.reused_full, &o.hashed_new);
    }
    o.elapsed_secs = now_mono() - started;
    char line[1024];
    snprintf(line, sizeof(line),
             "index: %s 增量：删 %llu 行，路径修正 %llu 个，新增/更新 %llu 个文件，"
             "复用缓存 %llu 个，新算 %llu 个，用时 %.1fs",
             drive, (unsigned long long)o.deleted, (unsigned long long)o.renamed,
             (unsigned long long)o.files, (unsigned long long)o.reused_full,
             (unsigned long long)o.hashed_new, o.elapsed_secs);
    if (!quiet) printf("%s\n", line);
    index_log(line);
    o.status = IDX_INCREMENTED;
    return o;
}

/* 全量回填一卷：MFT 枚举 → 元数据 upsert → 游标写入 → 摘要复用/哈希管线
 * → 摘要写回。游标在元数据落库后、哈希之前写（断点续跑）。 */
static idx_outcome_t index_volume_backfill(cdc_fileindex_t *idx, const char *drive,
                                           uint64_t serial, uint64_t journal_id,
                                           int64_t next_usn, int quiet) {
    double started = now_mono();
    uint64_t stamp_now = cdc::now_seconds();
    idx_outcome_t o;
    memset(&o, 0, sizeof(o));
    o.status = IDX_FAILED;
    cdc::FileEntryList list;
    if (cdc::scan_files(drive, CDC_INDEX_MIN_SIZE, cdc::kTopSkip, nullptr, list) != 0) {
        /* 枚举失败：按空卷处理（与 Rust 的 Vec 为空同义；游标照常写）。 */
        list.clear();
    }
    size_t total_entries = list.size();
    /* 跳过无 FRN 的条目（遍历回退的条目没有 FRN，进不了索引）；MFT 里
     * 没有 mtime 的读文件属性（与 scan_dups 一致）。 */
    std::vector<cdc::FileEntry> res;
    for (cdc::FileEntry &e : list) {
        if (e.frn == 0) continue;
        if (e.mtime == 0) e.mtime = file_mtime(e.path.c_str());
        res.push_back(std::move(e));
    }
    size_t n_res = res.size();
    size_t skipped = total_entries - n_res;
    cdc_index_row_t *rows = NULL;
    if (n_res > 0) {
        rows = (cdc_index_row_t *)malloc(n_res * sizeof(*rows));
        if (rows) {
            for (size_t i = 0; i < n_res; i++) {
                rows[i].volume_serial = serial;
                rows[i].frn = res[i].frn;
                rows[i].path = (char *)res[i].path.c_str();
                rows[i].size = res[i].size;
                rows[i].mtime = res[i].mtime;
                rows[i].last_seen = stamp_now;
            }
            cdc_fileindex_upsert_files(idx, rows, n_res);
            free(rows);
        }
    }
    /* 游标先写（元数据之后、哈希之前）：中断后续跑走增量，只补缺失摘要。 */
    cdc_fileindex_set_usn_state(idx, serial, journal_id, next_usn);

    /* 复用摘要缓存：已有 (path,size,mtime) 有效摘要的直接拷进索引。 */
    const char **paths = NULL;
    if (n_res > 0) {
        paths = (const char **)malloc(n_res * sizeof(char *));
        if (paths) {
            for (size_t i = 0; i < n_res; i++) paths[i] = res[i].path.c_str();
        }
    }
    cdc_hashcache_t *cache = cdc_hashcache_load_for_paths(paths, n_res);
    free(paths);
    cdc_digest_set_t *seeded = NULL;
    if (n_res > 0) {
        seeded = (cdc_digest_set_t *)calloc(n_res, sizeof(*seeded));
        if (!seeded) {
            if (cache) cdc_hashcache_close(cache);
            o.elapsed_secs = now_mono() - started;
            return o;
        }
    }
    uint64_t reused_full = 0;
    cdc_vec_t reuse_v;
    if (cdc_vec_init(&reuse_v, sizeof(cdc_digest_update_t)) != CDC_OK) {
        if (cache) cdc_hashcache_close(cache);
        free(seeded);
        o.elapsed_secs = now_mono() - started;
        return o;
    }
    /* 种子补充：索引里已有的摘要（见 cdc_fileindex_all_digests 注释）。
     * upsert 已保证 size/mtime 变了的行摘要已清 NULL，读回的摘要与
     * 当前文件一致。合并顺序与 hash_index_groups 一致：索引优先，
     * 缓存补缺。没有缓存（cache==NULL）时索引摘要仍可复用。 */
    size_t n_idx_dig = 0;
    cdc_index_digest_entry_t *idx_dig =
        cdc_fileindex_all_digests(idx, serial, &n_idx_dig);
    for (size_t i = 0; i < n_res; i++) {
        cdc_digest_set_t merged;
        memset(&merged, 0, sizeof(merged));
        if (idx_dig) {
            const cdc_index_digest_entry_t *e =
                find_idx_digest(idx_dig, n_idx_dig, res[i].frn);
            if (e) merged = e->digests;
        }
        if (cache) {
            cdc_digest_set_t ce;
            memset(&ce, 0, sizeof(ce));
            if (cdc_hashcache_entry_for(cache, res[i].path.c_str(), res[i].size,
                                        res[i].mtime, &ce) == 1) {
                cdc_digest_set_t tmp;
                merge_digests(&tmp, &merged, &ce);
                merged = tmp;
                if (any_digest(&ce)) {
                    cdc_digest_update_t u;
                    memset(&u, 0, sizeof(u));
                    u.volume_serial = serial;
                    u.frn = res[i].frn;
                    u.size = res[i].size;
                    u.mtime = res[i].mtime;
                    u.digests = ce;
                    cdc_vec_push(&reuse_v, &u);
                }
            }
        }
        seeded[i] = merged;
        if (merged.has_full) reused_full++;
    }
    free(idx_dig);
    if (reuse_v.len > 0) {
        cdc_fileindex_set_digests_batch(idx, (const cdc_digest_update_t *)reuse_v.data, reuse_v.len, stamp_now);
    }
    cdc_vec_free(&reuse_v);

    /* 按 size 分组，只有 ≥2 个文件同 size 的组才进哈希管线（大小唯一的
     * 零 I/O 排除，与 scan_dups 同一条规则；分组在管线内部做）。 */
    cdc_scan_input_t *inputs = NULL;
    if (n_res > 0) {
        inputs = (cdc_scan_input_t *)malloc(n_res * sizeof(*inputs));
        if (inputs) {
            for (size_t i = 0; i < n_res; i++) {
                inputs[i].path = res[i].path.c_str();
                inputs[i].size = res[i].size;
                inputs[i].mtime = res[i].mtime;
                inputs[i].frn = res[i].frn;
                inputs[i].digests = seeded[i];
            }
        }
    }
    uint64_t upd_full = 0;
    if (inputs) {
        cdc_digest_update_t *updates = NULL;
        size_t n_upd = 0;
        if (cdc_scan_hash_groups(inputs, n_res, cache, stamp_now, &updates, &n_upd) ==
                0 &&
            n_upd > 0) {
            /* 同上：scan.c 的 updates 里 volume_serial 为 0，写回前按卷填好。 */
            for (size_t i = 0; i < n_upd; i++) updates[i].volume_serial = serial;
            cdc_fileindex_set_digests_batch(idx, updates, n_upd, stamp_now);
            for (size_t i = 0; i < n_upd; i++) {
                if (updates[i].digests.has_full) upd_full++;
            }
            free(updates);
        }
        free(inputs);
    }
    uint64_t newly = upd_full > reused_full ? upd_full - reused_full : 0;
    if (cache) cdc_hashcache_close(cache);
    double elapsed = now_mono() - started;
    char line[1024];
    snprintf(line, sizeof(line),
             "index: %s 文件 %llu 个（跳过无 FRN %llu 个），复用摘要 %llu 个，"
             "新算 %llu 个，用时 %.1fs",
             drive, (unsigned long long)n_res, (unsigned long long)skipped,
             (unsigned long long)reused_full, (unsigned long long)newly, elapsed);
    if (!quiet) printf("%s\n", line);
    index_log(line);
    free(seeded);
    o.status = IDX_BACKFILLED;
    o.files = (uint64_t)n_res;
    o.reused_full = reused_full;
    o.hashed_new = newly;
    o.elapsed_secs = elapsed;
    return o;
}

/* 单卷索引入口：游标可用时走 USN 增量追赶，否则全量回填。
 * rebuild 为真时先清掉该卷的索引行和游标，再全量回填。 */
static idx_outcome_t index_volume(cdc_fileindex_t *idx, const char *drive, int quiet,
                                  int do_hash, int rebuild) {
    double started = now_mono();
    idx_outcome_t fail;
    memset(&fail, 0, sizeof(fail));
    fail.status = IDX_FAILED;
    uint64_t serial = 0, journal_id = 0;
    int64_t first_usn = 0, next_usn = 0;
    if (cdc::volume_usn_state(drive, serial, journal_id, first_usn, next_usn) !=
        0) {
        char msg[1024];
        snprintf(msg, sizeof(msg), "--index：%s 打不开卷或 USN 日志。", drive);
        fprintf(stderr, "%s\n", msg);
        index_log(msg);
        fail.elapsed_secs = now_mono() - started;
        return fail;
    }
    if (rebuild) {
        cdc_fileindex_delete_volume(idx, serial);
    }
    if (!rebuild) {
        uint64_t stored_journal = 0;
        int64_t stored_next = 0;
        if (cdc_fileindex_get_usn_state(idx, serial, &stored_journal, &stored_next) ==
                1 &&
            cdc::usn_state_usable(stored_journal, stored_next, journal_id,
                                           first_usn, next_usn)) {
            cdc::UsnChangeSet changes;
            int rc = cdc::usn_changes_since(drive, stored_journal, stored_next,
                                            cdc::kTopSkip, changes);
            if (rc == 0) {
                idx_outcome_t o = index_volume_incremental(idx, drive, serial, &changes,
                                                           quiet, do_hash, started);
                return o;
            }
            /* 变化集取不到（无快照 / 变化太多 / 日志读失败）：回落全量回填。 */
            if (!quiet) {
                printf("index: %s 增量不可用（变化集取不到），全量回填\n", drive);
            }
        }
    }
    return index_volume_backfill(idx, drive, serial, journal_id, next_usn, quiet);
}

/* 只做增量追赶，不回填（页面查询用：查询前确保新鲜，绝不触发全量回填）。
 * 返回 1=追赶成功，0=增量不可用。 */
static int index_volume_incremental_only(cdc_fileindex_t *idx, const char *drive,
                                         int quiet, int do_hash) {
    uint64_t serial = 0, journal_id = 0;
    int64_t first_usn = 0, next_usn = 0;
    if (cdc::volume_usn_state(drive, serial, journal_id, first_usn,
                             next_usn) != 0)
        return 0;
    uint64_t stored_journal = 0;
    int64_t stored_next = 0;
    if (cdc_fileindex_get_usn_state(idx, serial, &stored_journal,
                                    &stored_next) != 1)
        return 0;
    if (!cdc::usn_state_usable(stored_journal, stored_next, journal_id,
                              first_usn, next_usn))
        return 0;
    cdc::UsnChangeSet changes;
    if (cdc::usn_changes_since(drive, stored_journal, stored_next, cdc::kTopSkip, changes) != 0)
        return 0;
    idx_outcome_t o = index_volume_incremental(idx, drive, serial, &changes,
                                               quiet, do_hash, now_mono());
    return o.status == IDX_INCREMENTED ? 1 : 0;
}

/* 只读新鲜度（Rust freshness_from_state；index_status 用，不触发同步）。
 * state_out 填 fresh | behind | stale | missing | unavailable 之一；
 * serial_out 填卷序列号（卷打不开时填 0）。返回 0。 */
int cdc_index_freshness(cdc_fileindex_t *idx, const char *drive,
                        uint64_t *serial_out, char *state_out, size_t cap) {
    uint64_t serial = 0, journal_id = 0;
    int64_t first_usn = 0, next_usn = 0;
    const char *state = "unavailable";
    if (serial_out) *serial_out = 0;
    if (cdc::volume_usn_state(drive, serial, journal_id, first_usn,
                             next_usn) == 0) {
        if (serial_out) *serial_out = serial;
        uint64_t stored_journal = 0;
        int64_t stored_next = 0;
        if (cdc_fileindex_get_usn_state(idx, serial, &stored_journal,
                                        &stored_next) != 1) {
            state = "missing";
        } else if (!cdc::usn_state_usable(stored_journal, stored_next,
                                         journal_id, first_usn, next_usn)) {
            state = "stale";
        } else {
            state = stored_next == next_usn ? "fresh" : "behind";
        }
    }
    if (state_out && cap) snprintf(state_out, cap, "%s", state);
    return 0;
}

/* 查询前确保一卷新鲜（Rust ensure_index_scope_fresh）：Fresh 直接用；
 * Behind 跑一次增量（仅增量，不回填；do_hash=1 时补算摘要）。
 * 返回 1=新鲜（serial_out 已填），0=不新鲜（serial_out 填卷序列号，
 * 卷打不开时填 0）。 */
int cdc_index_scope_fresh(cdc_fileindex_t *idx, const char *drive, int do_hash,
                          uint64_t *serial_out) {
    char state[16] = "";
    uint64_t serial = 0;
    cdc_index_freshness(idx, drive, &serial, state, sizeof(state));
    if (strcmp(state, "behind") == 0) {
        /* 追赶一次增量，再判一次新鲜度。 */
        if (index_volume_incremental_only(idx, drive, 1, do_hash)) {
            cdc_index_freshness(idx, drive, &serial, state, sizeof(state));
        }
    }
    if (serial_out) *serial_out = serial;
    return strcmp(state, "fresh") == 0 ? 1 : 0;
}

/* 逐卷索引并返回汇总行。返回 0=全部成功，1=有卷失败。 */
static int index_all_drives(cdc_fileindex_t *idx, char **drives, size_t n_drives,
                            int quiet, int rebuild, char *summary, size_t cap) {
    uint64_t total_files = 0, total_hashed = 0, total_reused = 0, total_incremented = 0;
    uint64_t total_deleted = 0, total_renamed = 0;
    double total_elapsed = 0;
    char failed_list[1024] = "";
    int any_failed = 0;
    for (size_t i = 0; i < n_drives; i++) {
        idx_outcome_t o = index_volume(idx, drives[i], quiet, 1, rebuild);
        total_files += o.files;
        total_hashed += o.hashed_new;
        total_reused += o.reused_full;
        total_deleted += o.deleted;
        total_renamed += o.renamed;
        total_elapsed += o.elapsed_secs;
        if (o.status == IDX_INCREMENTED) total_incremented++;
        if (o.status == IDX_FAILED) {
            if (any_failed) {
                strncat(failed_list, "、", sizeof(failed_list) - strlen(failed_list) - 1);
            }
            strncat(failed_list, drives[i],
                    sizeof(failed_list) - strlen(failed_list) - 1);
            any_failed = 1;
        }
    }
    uint64_t files = 0, with_full = 0;
    cdc_fileindex_counts(idx, &files, &with_full);
    char suffix[1100] = "";
    if (any_failed) {
        snprintf(suffix, sizeof(suffix), "；失败的卷：%s", failed_list);
    }
    snprintf(summary, cap,
             "index: 本次 %llu 个文件（其中 %llu 卷增量，删 %llu 行，路径修正 %llu 个），"
             "复用缓存 %llu 个，新算 %llu 个，用时 %.1fs；库中共 %llu 个文件"
             "（%llu 个有 full 摘要）%s",
             (unsigned long long)total_files, (unsigned long long)total_incremented,
             (unsigned long long)total_deleted, (unsigned long long)total_renamed,
             (unsigned long long)total_reused, (unsigned long long)total_hashed,
             total_elapsed, (unsigned long long)files, (unsigned long long)with_full,
             suffix);
    return any_failed ? 1 : 0;
}

/* --index 编排（增量优先，断档回填；CLI 与 GUI 共用）：
 * quiet=1 时不打印到 stdout；summary 总是填一条中文汇总（失败时为
 * 失败原因）；进度同时打到 logs/index-YYYYMMDD.log。
 * 退出码：0 全部成功，1 部分卷失败，2 非管理员。 */
int cdc_run_index_summary(int quiet, int rebuild, char *summary, size_t cap) {
    if (!is_admin()) {
        if (summary && cap)
            snprintf(summary, cap, "--index 需要管理员权限。");
        return 2;
    }
    size_t n_drives = 0;
    char **drives = fixed_drives(&n_drives);
    if (!drives || n_drives == 0) {
        if (summary && cap)
            snprintf(summary, cap, "--index：没有找到固定磁盘。");
        free_drives(drives, n_drives);
        return 1;
    }
    cdc_err_t err;
    cdc_fileindex_t *idx = cdc_fileindex_open_default(&err);
    if (!idx) {
        if (summary && cap)
            snprintf(summary, cap, "--index：打不开索引库。");
        free_drives(drives, n_drives);
        return 1;
    }
    int rc = index_all_drives(idx, drives, n_drives, quiet, rebuild, summary,
                              cap);
    index_log(summary);
    cdc_fileindex_close(idx);
    free_drives(drives, n_drives);
    return rc;
}

/* --index 的 CLI 包装：退出码与原来一致（0 全部成功，1 部分卷失败，
 * 2 非管理员），失败原因打 stderr，成功汇总在非 quiet 时打 stdout。 */
static int run_index(int quiet, int rebuild) {
    char summary[2048] = "";
    int rc = cdc_run_index_summary(quiet, rebuild, summary, sizeof(summary));
    if (rc == 2) {
        fprintf(stderr,
                "--index 需要管理员权限：请用管理员身份运行（计划任务请用最高权限）。\n");
    } else if (rc != 0) {
        fprintf(stderr, "%s\n", summary);
    } else if (!quiet) {
        printf("%s\n", summary);
    }
    return rc;
}

/* ================= 入口 ================= */

static int has_arg(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (has_arg(argc, argv, "--selftest") ||
        (argc >= 2 && strcmp(argv[1], "selftest") == 0))
        return run_selftest();
    if (has_arg(argc, argv, "--index")) {
        int quiet = has_arg(argc, argv, "--quiet");
        int rebuild = has_arg(argc, argv, "--rebuild");
        return run_index(quiet, rebuild);
    }
    if (argc >= 3 && strcmp(argv[1], "big") == 0) {
#ifdef _WIN32
        fprintf(stderr, "big: POSIX walk build only in this pilot; Windows build uses MFT path (see README)\n");
        return 2;
#else
        return run_big(argv[2]);
#endif
    }
#ifdef _WIN32
    if (argc >= 3 && strcmp(argv[1], "probe") == 0) {
        wchar_t drive[4] = {0};
        drive[0] = (wchar_t)argv[2][0];
        return cdc::mft_probe_volume(drive);
    }
#endif
    if (argc == 1) {
        /* 默认分支：GUI（无 CLI 参数时）。
         * 预热先行：浏览器进程拉起与后端初始化重叠；失败不致命，
         * cdc_host_run() 里还会再试一次。 */
#ifdef _WIN32
        (void)cdc_host_prewarm();
        if (cdc_gui_register_commands() != 0) {
            fprintf(stderr, "注册后端命令失败。\n");
            return 1;
        }
        return cdc_host_run(GetModuleHandleW(NULL), SW_SHOWDEFAULT);
#else
        fprintf(stderr, "GUI 仅支持 Windows（本机为 Linux 构建）。\n");
        return 2;
#endif
    }
    fprintf(stderr,
            "usage:\n"
            "  %s                         启动图形界面（仅 Windows）\n"
            "  %s selftest | --selftest   移植单元测试\n"
            "  %s --index [--quiet] [--rebuild]  常驻索引（需管理员；exit 2=未提权）\n"
            "  %s big <dir>               大文件扫描（分档阈值过滤，按大小降序）\n"
#ifdef _WIN32
            "  %s probe <C>               打开卷并读 NTFS 卷信息\n"
#endif
            ,
            argv[0], argv[0], argv[0], argv[0]
#ifdef _WIN32
            ,
            argv[0]
#endif
            );
    return 2;
}
