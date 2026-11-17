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

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bigfile.h"
#include "mft_util.h"
#include "fileindex.h"
#include "hashcache.h" /* cdc_hashcache_*（真实现；桩已删） */
#include "indextask.h"
#include "mft.h" /* CdcFileEntry、cdc_scan_files、USN */
#include "scan.h" /* cdc_scan_input_t、cdc_scan_hash_groups（真实现；桩已删） */
#include "gui.h" /* cdc_gui_register_commands（GUI 入口） */
#include "raii.h"

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
    total_fail += cdc::fileindex_selftest();
    total_fail += cdc_indextask_selftest();
    printf("selftest total: %d failures\n", total_fail);
    return total_fail == 0 ? 0 : 1;
}

/* ================= big 扫描演示（试点，保留；POSIX 目录遍历） ================= */

#ifndef _WIN32
#include <dirent.h>

struct BigRow {
    std::string path;
    uint64_t size = 0;
};

static void walk(const char *dir, std::vector<BigRow> &rows) {
    cdc::UniqueDir d(opendir(dir));
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d.get())) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string full = std::string(dir) + "/" + e->d_name;
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk(full.c_str(), rows);
        } else if (S_ISREG(st.st_mode)) {
            uint64_t size = (uint64_t)st.st_size;
            /* 与 Rust scan_big 相同：2MB 预过滤 + 分档阈值 + 跳过系统文件 */
            if (size >= cdc::kScanMinBytes &&
                size >= cdc::big_file_threshold_bytes(full.c_str()) &&
                !cdc::is_system_file_name(full.c_str()))
                rows.push_back({std::move(full), size});
        }
    }
}

static int run_big(const char *dir) {
    std::vector<BigRow> rows;
    walk(dir, rows);
    std::sort(rows.begin(), rows.end(), [](const BigRow &a, const BigRow &b) {
        return a.size > b.size;
    });
    for (const BigRow &row : rows)
        printf("%llu\t%s\n", (unsigned long long)row.size, row.path.c_str());
    fprintf(stderr, "%zu big files\n", rows.size());
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
 * 返回固定磁盘路径的 C++ 字符串列表；非 Windows 返回空列表。 */
static std::vector<std::string> fixed_drives() {
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    std::vector<std::string> drives;
    for (int i = 0; i < 26; i++) {
        if ((mask & (1u << i)) == 0) continue;
        char root[4] = {(char)('A' + i), ':', '\\', '\0'};
        wchar_t *w = cdc_utf8_to_wide(root);
        if (!w) continue;
        int fixed = GetDriveTypeW(w) == DRIVE_FIXED;
        free(w);
        if (!fixed) continue;
        drives.emplace_back(root);
    }
    if (drives.empty()) {
        /* 回落：与 Rust 一致，哪怕没有固定盘也试 C:。 */
        drives.emplace_back("C:\\");
    }
    return drives;
#else
    return {};
#endif
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

enum class IdxStatus { Backfilled, Incremented, Failed };

struct IdxOutcome {
    IdxStatus status = IdxStatus::Failed;
    uint64_t files = 0;
    uint64_t deleted = 0;
    uint64_t renamed = 0;
    uint64_t reused_full = 0;
    uint64_t hashed_new = 0;
    double elapsed_secs = 0;
};

/* 合并两组摘要：a 优先，a 没有的用 b 补（对应 Rust 的 or_else 链）。
 * xxh 三档与 BLAKE3 四档都按同一规则合并。 */
static cdc::DigestSet merge_digests(const cdc::DigestSet &a, const cdc::DigestSet &b) {
    cdc::DigestSet d = a;
    auto fill32 = [](bool &has, cdc::Digest &v, bool b_has, const cdc::Digest &b_v) {
        if (!has && b_has) has = true, v = b_v;
    };
    auto fill64 = [](bool &has, uint64_t &v, bool b_has, uint64_t b_v) {
        if (!has && b_has) has = true, v = b_v;
    };
    fill32(d.has_head, d.head, b.has_head, b.head);
    fill32(d.has_sample, d.sample, b.has_sample, b.sample);
    fill32(d.has_strong, d.strong, b.has_strong, b.strong);
    fill32(d.has_full, d.full, b.has_full, b.full);
    fill64(d.has_head_xxh, d.head_xxh, b.has_head_xxh, b.head_xxh);
    fill64(d.has_sample_xxh, d.sample_xxh, b.has_sample_xxh, b.sample_xxh);
    fill64(d.has_strong_xxh, d.strong_xxh, b.has_strong_xxh, b.strong_xxh);
    return d;
}

static bool any_digest(const cdc::DigestSet &d) {
    return d.has_head || d.has_sample || d.has_strong || d.has_full || d.has_head_xxh ||
           d.has_sample_xxh || d.has_strong_xxh;
}

/* idx_dig 按 frn 升序：二分查找。 */
static const cdc::IndexDigestEntry *find_idx_digest(
    const std::vector<cdc::IndexDigestEntry> &arr, uint64_t frn) {
    auto it = std::lower_bound(arr.begin(), arr.end(), frn,
                               [](const cdc::IndexDigestEntry &e, uint64_t f) { return e.frn < f; });
    return it != arr.end() && it->frn == frn ? &*it : nullptr;
}

/* 跑哈希管线并把新摘要按卷写回索引；返回 updates 里带 full 的个数。
 * scan 的 updates 里 volume_serial 填 0（inputs 里没有卷号）；写回前
 * 必须按卷填好，否则 WHERE volume_serial=?1 匹配不到行。 */
static uint64_t hash_and_store(cdc::FileIndex &idx, uint64_t serial, uint64_t stamp_now,
                               std::span<const cdc::ScanInput> inputs, cdc::HashCache *cache) {
    if (inputs.empty()) return 0;
    std::vector<cdc::DigestUpdate> updates;
    if (cdc::scan_hash_groups(inputs, cache, stamp_now, updates) != 0 || updates.empty())
        return 0;
    for (cdc::DigestUpdate &u : updates) u.volume_serial = serial;
    idx.set_digests_batch(updates, stamp_now);
    return static_cast<uint64_t>(std::count_if(
        updates.begin(), updates.end(), [](const cdc::DigestUpdate &u) { return u.digests.has_full; }));
}

/* 增量维护的哈希补算：对"同 size ≥ 2 且组内至少一个缺 full"的组跑哈希管线。
 * 摘要来源优先级：索引行已有 > file_hashes 缓存 > 管线新算；新摘要双写
 * （索引 + 缓存，缓存的写回由管线内部做）。
 * 返回（从缓存复用的 full 个数，管线新算的 full 个数）。 */
static void hash_index_groups(cdc::FileIndex &idx, uint64_t serial, uint64_t stamp_now,
                              uint64_t &out_reused, uint64_t &out_new) {
    out_reused = 0;
    out_new = 0;
    std::vector<cdc::IndexHashRow> rows = idx.rows_needing_full(serial);
    if (rows.empty()) return;
    std::vector<const char *> cpaths;
    cpaths.reserve(rows.size());
    for (const cdc::IndexHashRow &r : rows) cpaths.push_back(r.path.c_str());
    std::unique_ptr<cdc::HashCache> cache = cdc::HashCache::load_for_paths(cpaths);

    std::vector<cdc::ScanInput> inputs(rows.size());
    uint64_t merged_full = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        cdc::DigestSet merged = rows[i].digests;
        if (cache) {
            if (std::optional<cdc::DigestSet> ce =
                    cache->entry_for(rows[i].path, rows[i].size, rows[i].mtime)) {
                if (!rows[i].digests.has_full && ce->has_full) out_reused++;
                merged = merge_digests(rows[i].digests, *ce);
            }
        }
        if (merged.has_full) merged_full++;
        inputs[i].path = rows[i].path.c_str();
        inputs[i].size = rows[i].size;
        inputs[i].mtime = rows[i].mtime;
        inputs[i].frn = rows[i].frn;
        inputs[i].digests = merged;
    }
    uint64_t upd_full = hash_and_store(idx, serial, stamp_now, inputs, cache.get());
    out_new = upd_full > merged_full ? upd_full - merged_full : 0;
}

/* 增量维护：把一个 USN 变化集应用到索引（删行 / 路径前缀改写 / 元数据
 * upsert），再对缺失摘要的组做哈希补算。游标在元数据落库后、哈希之前写
 * ——中断后续跑只补缺失摘要，不重扫。 */
static IdxOutcome index_volume_incremental(cdc::FileIndex &idx, const char *drive,
                                           uint64_t serial, const cdc::UsnChangeSet &changes,
                                           bool quiet, bool do_hash, double started) {
    IdxOutcome o;
    uint64_t stamp_now = cdc::now_seconds();
    o.deleted = idx.delete_rows(serial, changes.deleted_frns);
    for (size_t i = 0; i < changes.renamed_old.size(); i++)
        o.renamed += idx.update_path_prefix(serial, changes.renamed_old[i], changes.renamed_new[i]);
    std::vector<cdc::IndexRow> rows;
    rows.reserve(changes.files.size());
    for (const auto &f : changes.files)
        rows.push_back({serial, f.frn, f.path, f.size, f.mtime, stamp_now});
    o.files = rows.size();
    if (!rows.empty() && idx.upsert_files(rows) == 0) {
        /* 事务失败：游标不写，下次重新追赶。 */
        std::string msg = cdc::format("--index：%s 增量元数据写入失败。", drive);
        std::fprintf(stderr, "%s\n", msg.c_str());
        index_log(msg.c_str());
        o.files = 0;
        o.elapsed_secs = now_mono() - started;
        return o;
    }
    idx.set_usn_state(serial, changes.journal_id, changes.next_usn);
    if (do_hash) hash_index_groups(idx, serial, stamp_now, o.reused_full, o.hashed_new);
    o.elapsed_secs = now_mono() - started;
    std::string line = cdc::format(
        "index: %s 增量：删 %llu 行，路径修正 %llu 个，新增/更新 %llu 个文件，"
        "复用缓存 %llu 个，新算 %llu 个，用时 %.1fs",
        drive, (unsigned long long)o.deleted, (unsigned long long)o.renamed,
        (unsigned long long)o.files, (unsigned long long)o.reused_full,
        (unsigned long long)o.hashed_new, o.elapsed_secs);
    if (!quiet) std::printf("%s\n", line.c_str());
    index_log(line.c_str());
    o.status = IdxStatus::Incremented;
    return o;
}

/* 全量回填一卷：MFT 枚举 → 元数据 upsert → 游标写入 → 摘要复用/哈希管线
 * → 摘要写回。游标在元数据落库后、哈希之前写（断点续跑）。 */
static IdxOutcome index_volume_backfill(cdc::FileIndex &idx, const char *drive, uint64_t serial,
                                        uint64_t journal_id, int64_t next_usn, bool quiet) {
    double started = now_mono();
    uint64_t stamp_now = cdc::now_seconds();
    IdxOutcome o;
    cdc::FileEntryList list;
    if (cdc::scan_files(drive, cdc::kIndexMinSize, cdc::kTopSkip, nullptr, list) != 0) {
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
    size_t skipped = total_entries - res.size();
    {
        std::vector<cdc::IndexRow> rows;
        rows.reserve(res.size());
        for (const cdc::FileEntry &e : res)
            rows.push_back({serial, e.frn, e.path, e.size, e.mtime, stamp_now});
        idx.upsert_files(rows);
    }
    /* 游标先写（元数据之后、哈希之前）：中断后续跑走增量，只补缺失摘要。 */
    idx.set_usn_state(serial, journal_id, next_usn);

    /* 复用摘要缓存：已有 (path,size,mtime) 有效摘要的直接拷进索引。 */
    std::vector<const char *> paths;
    paths.reserve(res.size());
    for (const cdc::FileEntry &e : res) paths.push_back(e.path.c_str());
    std::unique_ptr<cdc::HashCache> cache = cdc::HashCache::load_for_paths(paths);

    /* 种子补充：索引里已有的摘要（见 FileIndex::all_digests 注释）。
     * upsert 已保证 size/mtime 变了的行摘要已清 NULL，读回的摘要与
     * 当前文件一致。合并顺序与 hash_index_groups 一致：索引优先，
     * 缓存补缺。 */
    std::vector<cdc::IndexDigestEntry> idx_dig = idx.all_digests(serial);
    std::vector<cdc::DigestUpdate> reuse;
    std::vector<cdc::ScanInput> inputs(res.size());
    uint64_t reused_full = 0;
    for (size_t i = 0; i < res.size(); i++) {
        cdc::DigestSet merged{};
        if (const cdc::IndexDigestEntry *e = find_idx_digest(idx_dig, res[i].frn))
            merged = e->digests;
        if (cache) {
            if (std::optional<cdc::DigestSet> ce =
                    cache->entry_for(res[i].path, res[i].size, res[i].mtime)) {
                merged = merge_digests(merged, *ce);
                if (any_digest(*ce))
                    reuse.push_back({serial, res[i].frn, res[i].size, res[i].mtime, *ce});
            }
        }
        if (merged.has_full) reused_full++;
        /* 按 size 分组，只有 ≥2 个文件同 size 的组才进哈希管线（分组在管线内部做）。 */
        inputs[i].path = res[i].path.c_str();
        inputs[i].size = res[i].size;
        inputs[i].mtime = res[i].mtime;
        inputs[i].frn = res[i].frn;
        inputs[i].digests = merged;
    }
    if (!reuse.empty()) idx.set_digests_batch(reuse, stamp_now);
    uint64_t upd_full = hash_and_store(idx, serial, stamp_now, inputs, cache.get());
    uint64_t newly = upd_full > reused_full ? upd_full - reused_full : 0;
    double elapsed = now_mono() - started;
    std::string line = cdc::format(
        "index: %s 文件 %llu 个（跳过无 FRN %llu 个），复用摘要 %llu 个，"
        "新算 %llu 个，用时 %.1fs",
        drive, (unsigned long long)res.size(), (unsigned long long)skipped,
        (unsigned long long)reused_full, (unsigned long long)newly, elapsed);
    if (!quiet) std::printf("%s\n", line.c_str());
    index_log(line.c_str());
    o.status = IdxStatus::Backfilled;
    o.files = res.size();
    o.reused_full = reused_full;
    o.hashed_new = newly;
    o.elapsed_secs = elapsed;
    return o;
}

/* 卷当前的 USN 状态（打不开返回 nullopt）。 */
struct VolumeUsn {
    uint64_t serial = 0, journal_id = 0;
    int64_t first_usn = 0, next_usn = 0;
};
static std::optional<VolumeUsn> volume_usn(const char *drive) {
    VolumeUsn v;
    if (cdc::volume_usn_state(drive, v.serial, v.journal_id, v.first_usn, v.next_usn) != 0)
        return std::nullopt;
    return v;
}

/* 已存游标可用时取变化集；不可用或取不到返回 false。 */
static bool usable_changes(cdc::FileIndex &idx, const char *drive, const VolumeUsn &v,
                           cdc::UsnChangeSet &changes) {
    std::optional<cdc::UsnState> stored = idx.get_usn_state(v.serial);
    if (!stored || !cdc::usn_state_usable(stored->journal_id, stored->next_usn, v.journal_id,
                                          v.first_usn, v.next_usn))
        return false;
    return cdc::usn_changes_since(drive, stored->journal_id, stored->next_usn, cdc::kTopSkip,
                                  changes) == 0;
}

/* 单卷索引入口：游标可用时走 USN 增量追赶，否则全量回填。
 * rebuild 为真时先清掉该卷的索引行和游标，再全量回填。 */
static IdxOutcome index_volume(cdc::FileIndex &idx, const char *drive, bool quiet, bool do_hash,
                               bool rebuild) {
    double started = now_mono();
    std::optional<VolumeUsn> v = volume_usn(drive);
    if (!v) {
        std::string msg = cdc::format("--index：%s 打不开卷或 USN 日志。", drive);
        std::fprintf(stderr, "%s\n", msg.c_str());
        index_log(msg.c_str());
        IdxOutcome fail;
        fail.elapsed_secs = now_mono() - started;
        return fail;
    }
    if (rebuild) {
        idx.delete_volume(v->serial);
    } else if (idx.get_usn_state(v->serial)) {
        cdc::UsnChangeSet changes;
        if (usable_changes(idx, drive, *v, changes))
            return index_volume_incremental(idx, drive, v->serial, changes, quiet, do_hash,
                                            started);
        std::optional<cdc::UsnState> st = idx.get_usn_state(v->serial);
        bool usable = st && cdc::usn_state_usable(st->journal_id, st->next_usn, v->journal_id,
                                                  v->first_usn, v->next_usn);
        /* 变化集取不到（无快照 / 变化太多 / 日志读失败）：回落全量回填。 */
        if (usable && !quiet) std::printf("index: %s 增量不可用（变化集取不到），全量回填\n", drive);
    }
    return index_volume_backfill(idx, drive, v->serial, v->journal_id, v->next_usn, quiet);
}

/* 只做增量追赶，不回填（页面查询用：查询前确保新鲜，绝不触发全量回填）。
 * 返回 true=追赶成功，false=增量不可用。 */
static bool index_volume_incremental_only(cdc::FileIndex &idx, const char *drive, bool quiet,
                                          bool do_hash) {
    std::optional<VolumeUsn> v = volume_usn(drive);
    if (!v) return false;
    cdc::UsnChangeSet changes;
    if (!usable_changes(idx, drive, *v, changes)) return false;
    return index_volume_incremental(idx, drive, v->serial, changes, quiet, do_hash, now_mono())
               .status == IdxStatus::Incremented;
}

/* 只读新鲜度（Rust freshness_from_state；index_status 用，不触发同步）。
 * state_out 填 fresh | behind | stale | missing | unavailable 之一；
 * serial_out 填卷序列号（卷打不开时填 0）。返回 0。 */
int cdc_index_freshness(cdc::FileIndex *idx, const char *drive, uint64_t *serial_out,
                        char *state_out, size_t cap) {
    const char *state = "unavailable";
    if (serial_out) *serial_out = 0;
    if (std::optional<VolumeUsn> v = volume_usn(drive)) {
        if (serial_out) *serial_out = v->serial;
        std::optional<cdc::UsnState> stored = idx->get_usn_state(v->serial);
        if (!stored)
            state = "missing";
        else if (!cdc::usn_state_usable(stored->journal_id, stored->next_usn, v->journal_id,
                                        v->first_usn, v->next_usn))
            state = "stale";
        else
            state = stored->next_usn == v->next_usn ? "fresh" : "behind";
    }
    if (state_out && cap) std::snprintf(state_out, cap, "%s", state);
    return 0;
}

/* 查询前确保一卷新鲜（Rust ensure_index_scope_fresh）：Fresh 直接用；
 * Behind 跑一次增量（仅增量，不回填；do_hash=1 时补算摘要）。
 * 返回 1=新鲜（serial_out 已填），0=不新鲜（serial_out 填卷序列号，
 * 卷打不开时填 0）。 */
int cdc_index_scope_fresh(cdc::FileIndex *idx, const char *drive, int do_hash,
                          uint64_t *serial_out) {
    char state[16] = "";
    uint64_t serial = 0;
    cdc_index_freshness(idx, drive, &serial, state, sizeof(state));
    if (std::strcmp(state, "behind") == 0) {
        /* 追赶一次增量，再判一次新鲜度。 */
        if (index_volume_incremental_only(*idx, drive, true, do_hash != 0))
            cdc_index_freshness(idx, drive, &serial, state, sizeof(state));
    }
    if (serial_out) *serial_out = serial;
    return std::strcmp(state, "fresh") == 0 ? 1 : 0;
}

/* 逐卷索引并返回汇总行。返回 0=全部成功，1=有卷失败。 */
static int index_all_drives(cdc::FileIndex &idx, const std::vector<std::string> &drives, bool quiet,
                            bool rebuild, std::string &summary) {
    uint64_t total_files = 0, total_hashed = 0, total_reused = 0, total_incremented = 0;
    uint64_t total_deleted = 0, total_renamed = 0;
    double total_elapsed = 0;
    std::string failed_list;
    for (const std::string &drive : drives) {
        IdxOutcome o = index_volume(idx, drive.c_str(), quiet, true, rebuild);
        total_files += o.files;
        total_hashed += o.hashed_new;
        total_reused += o.reused_full;
        total_deleted += o.deleted;
        total_renamed += o.renamed;
        total_elapsed += o.elapsed_secs;
        if (o.status == IdxStatus::Incremented) total_incremented++;
        if (o.status == IdxStatus::Failed) {
            if (!failed_list.empty()) failed_list += "、";
            failed_list += drive;
        }
    }
    cdc::IndexCounts c = idx.counts();
    std::string suffix = failed_list.empty() ? std::string() : "；失败的卷：" + failed_list;
    summary = cdc::format(
        "index: 本次 %llu 个文件（其中 %llu 卷增量，删 %llu 行，路径修正 %llu 个），"
        "复用缓存 %llu 个，新算 %llu 个，用时 %.1fs；库中共 %llu 个文件"
        "（%llu 个有 full 摘要）%s",
        (unsigned long long)total_files, (unsigned long long)total_incremented,
        (unsigned long long)total_deleted, (unsigned long long)total_renamed,
        (unsigned long long)total_reused, (unsigned long long)total_hashed, total_elapsed,
        (unsigned long long)c.total, (unsigned long long)c.with_full, suffix.c_str());
    return failed_list.empty() ? 0 : 1;
}

/* --index 编排（增量优先，断档回填；CLI 与 GUI 共用）：
 * quiet=1 时不打印到 stdout；summary 总是填一条中文汇总（失败时为
 * 失败原因）；进度同时打到 logs/index-YYYYMMDD.log。
 * 退出码：0 全部成功，1 部分卷失败，2 非管理员。 */
int cdc_run_index_summary(int quiet, int rebuild, char *summary, size_t cap) {
    auto put = [&](const std::string &s) {
        if (summary && cap) std::snprintf(summary, cap, "%s", s.c_str());
    };
    if (!is_admin()) {
        put("--index 需要管理员权限。");
        return 2;
    }
    std::vector<std::string> drives = fixed_drives();
    if (drives.empty()) {
        put("--index：没有找到固定磁盘。");
        return 1;
    }
    cdc::Error err;
    std::unique_ptr<cdc::FileIndex> idx = cdc::FileIndex::open_default(&err);
    if (!idx) {
        put("--index：打不开索引库。");
        return 1;
    }
    std::string text;
    int rc = index_all_drives(*idx, drives, quiet != 0, rebuild != 0, text);
    put(text);
    index_log(text.c_str());
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
