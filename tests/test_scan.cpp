/* test_scan.cpp — mft / fastio / hashcache / scan 自测（C++）。
 *
 * 只在 Linux 下跑（Windows 专属路径由 MinGW 交叉编译覆盖）。
 * 用法：./test_scan [tests/fixtures 目录，默认 tests/fixtures]
 * 退出码 = 失败断言数。
 *
 * 依赖：src/mft.cpp、fastio.cpp、hashcache.cpp、scan.cpp、util.cpp、bigfile.cpp、
 *       mft_util.cpp、sampling.cpp、third_party/blake3（intrinsics 版）、
 *       third_party/xxhash、third_party/sqlite3、
 *       third_party/cjson（CDC_HAVE_CJSON 时）。
 * 不依赖 fileindex.c / indextask.c / mft_raw.c（只用 fileindex.h 的类型）。
 */

#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "blake3.h"
#include "fastio.h"
#include "hashcache.h"
#include "mft.h"
#include "sampling.h"
#include "scan.h"
#include "sqlite3.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define CHECK_STREQ(a, b, what) CHECK((a) && strcmp((a), (b)) == 0, "%s: got [%s] want [%s]", (what), (a) ? (a) : "(null)", (b))
#define CHECK_U64(a, b, what) CHECK((a) == (b), "%s: got %llu want %llu", (what), (unsigned long long)(a), (unsigned long long)(b))
#define CHECK_INT(a, b, what) CHECK((a) == (b), "%s: got %lld want %lld", (what), (long long)(a), (long long)(b))

/* ---------------- 小工具 ---------------- */

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void w64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* unix 秒 -> Windows FILETIME（100ns，1601 纪元）。 */
static uint64_t ft_unix(int64_t unix) {
    return (uint64_t)(unix + 11644473600LL) * 10000000ULL;
}

static void put_utf16(uint8_t *dst, const char *ascii) {
    for (size_t i = 0; ascii[i]; i++) {
        dst[2 * i] = (uint8_t)ascii[i];
        dst[2 * i + 1] = 0;
    }
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void write_repeat(const char *path, uint64_t size, char c) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(2); }
    static char buf[65536];
    memset(buf, (unsigned char)c, sizeof buf);
    uint64_t left = size;
    while (left) {
        size_t n = left > sizeof buf ? sizeof buf : (size_t)left;
        if (write(fd, buf, n) != (ssize_t)n) { perror("write"); exit(2); }
        left -= n;
    }
    if (close(fd) != 0) { perror("close"); exit(2); }
}

static void make_sparse(const char *path, uint64_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(2); }
    if (ftruncate(fd, (off_t)size) != 0) { perror("ftruncate"); exit(2); }
    if (close(fd) != 0) { perror("close"); exit(2); }
}

static void pwrite_byte(const char *path, uint64_t off, char c) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { perror("open"); exit(2); }
    if (pwrite(fd, &c, 1, (off_t)off) != 1) { perror("pwrite"); exit(2); }
    if (close(fd) != 0) { perror("close"); exit(2); }
}

/* 把文件 mtime/atime 往后推 delta_sec：同秒内重写 mtime 不变，
 * (size, mtime) 校验的缓存仍会命中，必须显式推后才能模拟"文件变了"。 */
static void bump_mtime(const char *path, int delta_sec) {
    struct stat st;
    if (stat(path, &st) != 0) { perror("stat"); exit(2); }
    struct timespec ts[2];
    ts[0].tv_sec = st.st_atime + delta_sec;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = st.st_mtime + delta_sec;
    ts[1].tv_nsec = 0;
    if (utimensat(AT_FDCWD, path, ts, 0) != 0) { perror("utimensat"); exit(2); }
}

/* SQL 直查（open_at 不预加载任何路径，entry_for 查不到未加载的条目，
 * 表级断言得直接读库）。 */
static int64_t db_query_int(const char *dbpath, const char *sql) {
    sqlite3 *db = nullptr;
    int64_t v = -1;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return v;
}

static int db_blob_equals(const char *dbpath, const char *sql, const uint8_t *want,
                          size_t n) {
    sqlite3 *db = nullptr;
    int ok = 0;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const void *blob = sqlite3_column_blob(st, 0);
            int bytes = sqlite3_column_bytes(st, 0);
            ok = blob != nullptr && bytes == (int)n && memcmp(blob, want, n) == 0;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

static void join_path(char *buf, size_t cap, const char *dir, const char *name) {
    snprintf(buf, cap, "%s/%s", dir, name);
}

static cdc::Digest blake3_of(const void *data, size_t n) {
    cdc::Digest d;
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, n);
    blake3_hasher_finalize(&h, d.bytes, 32);
    return d;
}

/* ---------------- 合成 FILE 记录 ---------------- */

typedef struct {
    uint8_t buf[1024];
    size_t pos; /* 下一个属性的偏移 */
} Rec;

static void rec_init(Rec *r, uint16_t seq, uint16_t flags) {
    memset(r->buf, 0, sizeof r->buf);
    memcpy(r->buf, "FILE", 4);
    w16(r->buf + 4, 40);  /* usa_off */
    w16(r->buf + 6, 3);   /* usa_count：1 + 2 扇区 */
    w16(r->buf + 16, seq);
    w16(r->buf + 18, 1);  /* links */
    w16(r->buf + 20, 56); /* first_attr */
    w16(r->buf + 22, flags);
    w64(r->buf + 32, 0);  /* base record */
    w16(r->buf + 40, 0x1111);
    w16(r->buf + 42, 0x2222);
    w16(r->buf + 44, 0x3333);
    r->pos = 56;
}

/* $STANDARD_INFORMATION：mtime FILETIME @+8，attrs u32 @+32。 */
static void rec_si(Rec *r, uint64_t mtime_ft, uint32_t attrs) {
    uint8_t *a = r->buf + r->pos;
    w32(a + 0, 0x10);
    w32(a + 4, 60);
    a[8] = 0; /* resident */
    w16(a + 16, 36); /* content_len */
    w16(a + 20, 24); /* content_off */
    w64(a + 24 + 8, mtime_ft);
    w32(a + 24 + 32, attrs);
    r->pos += 60;
}

/* $FILE_NAME：parent @0，name_len @64，namespace @65，UTF-16 @66。 */
static void rec_fn(Rec *r, uint64_t parent_frn, const char *name, uint8_t ns) {
    size_t nlen = strlen(name);
    uint8_t *a = r->buf + r->pos;
    uint32_t len = (uint32_t)(24 + 66 + 2 * nlen);
    w32(a + 0, 0x30);
    w32(a + 4, len);
    a[8] = 0; /* resident */
    w16(a + 16, (uint16_t)(66 + 2 * nlen));
    w16(a + 20, 24);
    w64(a + 24, parent_frn);
    a[24 + 64] = (uint8_t)nlen;
    a[24 + 65] = ns;
    put_utf16(a + 24 + 66, name);
    r->pos += len;
}

/* $DATA（非 resident）：lowest VCN 0，real size，runlist 在 +64。 */
static void rec_data_nonres(Rec *r, uint64_t size, const uint8_t *runs, size_t n_runs,
                            uint16_t data_flags) {
    uint8_t *a = r->buf + r->pos;
    uint32_t len = (uint32_t)(64 + n_runs);
    w32(a + 0, 0x80);
    w32(a + 4, len);
    a[8] = 1; /* non-resident */
    w16(a + 12, data_flags);
    w64(a + 16, 0); /* lowest VCN */
    w64(a + 24, 0); /* highest VCN */
    w16(a + 32, 64); /* run_off */
    w64(a + 40, size); /* alloc */
    w64(a + 48, size); /* real */
    w64(a + 56, size); /* initialized */
    memcpy(a + 64, runs, n_runs);
    r->pos += len;
}

/* $DATA（resident）：content 在 +24。 */
static void rec_data_res(Rec *r, const uint8_t *content, uint32_t size) {
    uint8_t *a = r->buf + r->pos;
    uint32_t len = 24 + size;
    w32(a + 0, 0x80);
    w32(a + 4, len);
    a[8] = 0; /* resident */
    w32(a + 16, size);
    w16(a + 20, 24);
    memcpy(a + 24, content, size);
    r->pos += len;
}

static void rec_end(Rec *r) {
    w32(r->buf + r->pos, 0xFFFFFFFFu);
    r->pos += 4;
}

/* ---------------- 1. parse_record_in_place ---------------- */

static void test_parse() {
    printf("== parse_record ==\n");
    const uint64_t MB = 1024 * 1024;
    /* runlist：1 簇长、3 字节偏移 => lcn 5，8 簇。 */
    static const uint8_t RUN_LCN5[] = { 0x31, 0x08, 0x05, 0x00, 0x00, 0x00 };

    /* (a) 正常文件：video.mp4，Win32 命名空间，parent 5，4MB，attrs 0x20，
     * first_lcn 5（8 簇 × 4096），bpc 4096 => can_raw_read=1，sort_key=5。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        CHECK_INT(cdc::parse_record_in_place(r.buf, sizeof r.buf, 20, 512, 4096, MB, out), 0, "file rc");
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::File, "file kind");
        CHECK_STREQ(out.name.c_str(), "video.mp4", "file name");
        CHECK_U64(out.frn, (1ULL << 48) | 20, "file frn");
        CHECK_U64(out.parent_frn, (3ULL << 48) | 5, "file parent");
        CHECK_U64(out.size, 4 * MB, "file size");
        CHECK_U64(out.mtime, 1700000000, "file mtime");
        CHECK_U64(out.first_lcn, 5, "file first_lcn");
        CHECK_U64(out.sort_key, 5, "file sort_key");
        CHECK_INT(out.can_raw_read, 1, "file can_raw_read");
    }

    /* (b) 压缩属性 => can_raw_read=0，其余同 (a)。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x800);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 20, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::File, "compressed kind");
        CHECK_INT(out.can_raw_read, 0, "compressed can_raw_read");
    }

    /* (c) 稀疏 data_flags 0x8000 => can_raw_read=0。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0x8000);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 20, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::File, "sparse kind");
        CHECK_INT(out.can_raw_read, 0, "sparse can_raw_read");
    }

    /* (d) size 100 < min_size 1MB => Skip。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "tiny.txt", 1);
        rec_data_nonres(&r, 100, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 21, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::Skip, "tiny skip");
    }

    /* (e) 目录：flags=3，reparse 属性 0x400 => DIR + reparse=1。 */
    {
        Rec r;
        rec_init(&r, 3, 3);
        rec_si(&r, ft_unix(1700000000), 0x400);
        rec_fn(&r, 5, "Users", 1);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 10, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::Dir, "dir kind");
        CHECK_STREQ(out.name.c_str(), "Users", "dir name");
        CHECK_INT(out.seq, 3, "dir seq");
        CHECK_INT(out.reparse, 1, "dir reparse");
        CHECK_U64(out.parent_frn, 5, "dir parent");
    }

    /* (f) $ 开头的文件名 => Skip。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "$foo", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 22, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::Skip, "dollar skip");
    }

    /* (g) 扩展记录（u64@32 != 0，index != 0）=> Skip。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        w64(r.buf + 32, (1ULL << 48) | 20); /* base record */
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 23, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::Skip, "extension skip");
    }

    /* (h) flags&1 == 0（未在使用中）=> Skip。 */
    {
        Rec r;
        rec_init(&r, 1, 0);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 24, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::Skip, "not-in-use skip");
    }

    /* (i) 坏 magic => Skip。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        memcpy(r.buf, "BAD!", 4);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 25, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::Skip, "bad magic skip");
    }

    /* (j) MFT 根（index 0）：runlist {0x31,0x08,0x45,0x23,0x01,0x00}
     * => 1 条 run：vcn 0，lcn 0x12345，len 8。 */
    {
        static const uint8_t RUN_MFT[] = { 0x31, 0x08, 0x45, 0x23, 0x01, 0x00 };
        Rec r;
        rec_init(&r, 1, 1);
        rec_data_nonres(&r, 256 * 1024, RUN_MFT, sizeof RUN_MFT, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 0, 512, 4096, MB, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::MftRoot, "mft root kind");
        CHECK_U64(out.runs.size(), 1, "mft root n_runs");
        if (out.runs.size() == 1) {
            CHECK_U64(out.runs[0].vcn, 0, "mft run vcn");
            CHECK_U64(out.runs[0].lcn, 0x12345, "mft run lcn");
            CHECK_U64(out.runs[0].len, 8, "mft run len");
        }
    }

    /* (k) 命名空间优先级：DOS 先、Win32 后 => Win32 胜；
     * Win32 先、DOS 后 => 仍 Win32；Win32 先、POSIX(0) 后 => POSIX 胜。 */
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "VIDEO~1.MP4", 2);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 26, 512, 4096, MB, out);
        CHECK_STREQ(out.name.c_str(), "video.mp4", "ns DOS-then-Win32");
    }
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_fn(&r, (3ULL << 48) | 5, "VIDEO~1.MP4", 2);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 27, 512, 4096, MB, out);
        CHECK_STREQ(out.name.c_str(), "video.mp4", "ns Win32-then-DOS");
    }
    {
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "video.mp4", 1);
        rec_fn(&r, (3ULL << 48) | 5, "video-posix.mp4", 0);
        rec_data_nonres(&r, 4 * MB, RUN_LCN5, sizeof RUN_LCN5, 0);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 28, 512, 4096, MB, out);
        CHECK_STREQ(out.name.c_str(), "video-posix.mp4", "ns Win32-then-POSIX");
    }

    /* (l) resident $DATA：size=内容长，first_lcn=0，can_raw_read=0，
     * sort_key=index（min_size 用 1，否则 100 字节直接被 Skip）。 */
    {
        static const uint8_t CONTENT[100] = { 0x41 };
        Rec r;
        rec_init(&r, 1, 1);
        rec_si(&r, ft_unix(1700000000), 0x20);
        rec_fn(&r, (3ULL << 48) | 5, "res.txt", 1);
        rec_data_res(&r, CONTENT, 100);
        rec_end(&r);
        cdc::ParsedRecord out;
        cdc::parse_record_in_place(r.buf, sizeof r.buf, 29, 512, 4096, 1, out);
        CHECK_INT((int)out.kind, (int)cdc::RecordKind::File, "resident kind");
        CHECK_U64(out.size, 100, "resident size");
        CHECK_U64(out.first_lcn, 0, "resident first_lcn");
        CHECK_INT(out.can_raw_read, 0, "resident can_raw_read");
        CHECK_U64(out.sort_key, 29, "resident sort_key");
    }

    printf("parse_record done\n");
}

/* ---------------- 2. 快照 roundtrip + USN 判定 ---------------- */

static void test_snapshot() {
    printf("== snapshot ==\n");
    char dir_tmpl[] = "/tmp/cdc-test-snap-XXXXXX";
    if (!mkdtemp(dir_tmpl)) { perror("mkdtemp"); exit(2); }
    char path[PATH_MAX];
    join_path(path, sizeof path, dir_tmpl, "mft-v3-test.bin");

    cdc::Snapshot s;
    s.volume_serial = 0x12345678;
    s.journal_id = 0xABCDEF01;
    s.next_usn = 42000;
    s.min_size = 1ULL << 20;
    s.bytes_per_cluster = 4096;
    s.dirs.push_back(cdc::DirNode{10, 3, 5, "Users", false});
    s.files.push_back(cdc::RawFile{(1ULL << 48) | 20, (3ULL << 48) | 10, "video.mp4", 4194304,
                                   1700000000, 999, 999, true});

    CHECK_INT(cdc::snapshot_save(path, s), 0, "snapshot save");
    cdc::Snapshot back;
    CHECK_INT(cdc::snapshot_load(path, back), 0, "snapshot load");
    CHECK_U64(back.volume_serial, 0x12345678, "snap serial");
    CHECK_U64(back.journal_id, 0xABCDEF01, "snap journal");
    CHECK_INT(back.next_usn, 42000, "snap next_usn");
    CHECK_U64(back.min_size, 1ULL << 20, "snap min_size");
    CHECK_INT(back.bytes_per_cluster, 4096, "snap bpc");
    CHECK_U64(back.dirs.size(), 1, "snap n_dirs");
    if (back.dirs.size() == 1) {
        CHECK_U64(back.dirs[0].index, 10, "snap dir index");
        CHECK_INT(back.dirs[0].seq, 3, "snap dir seq");
        CHECK_U64(back.dirs[0].parent_frn, 5, "snap dir parent");
        CHECK_STREQ(back.dirs[0].name.c_str(), "Users", "snap dir name");
    }
    CHECK_U64(back.files.size(), 1, "snap n_files");
    if (back.files.size() == 1) {
        CHECK_U64(back.files[0].frn, (1ULL << 48) | 20, "snap file frn");
        CHECK_U64(back.files[0].parent_frn, (3ULL << 48) | 10, "snap file parent");
        CHECK_STREQ(back.files[0].name.c_str(), "video.mp4", "snap file name");
        CHECK_U64(back.files[0].size, 4194304, "snap file size");
        CHECK_U64(back.files[0].mtime, 1700000000, "snap file mtime");
        CHECK_U64(back.files[0].first_lcn, 999, "snap file first_lcn");
        CHECK_U64(back.files[0].sort_key, 999, "snap file sort_key");
        CHECK_INT(back.files[0].can_raw_read, 1, "snap file can_raw_read");
    }
    unlink(path);
    rmdir(dir_tmpl);

    /* 不存在的快照文件 => load 失败。 */
    cdc::Snapshot missing;
    CHECK_INT(cdc::snapshot_load("/tmp/cdc-test-snap-definitely-missing.bin", missing) == 0, 0, "missing snapshot fails");

    /* usn_state_usable：游标在 [first, next] 内且 journal 一致才可用。 */
    CHECK_INT(cdc::usn_state_usable(0xABCDEF01, 100, 0xABCDEF01, 50, 200), 1, "usn usable in-range");
    CHECK_INT(cdc::usn_state_usable(0xABCDEF01, 50, 0xABCDEF01, 50, 200), 1, "usn usable at first");
    CHECK_INT(cdc::usn_state_usable(0xABCDEF01, 200, 0xABCDEF01, 50, 200), 1, "usn usable at next");
    CHECK_INT(cdc::usn_state_usable(0xABCDEF02, 100, 0xABCDEF01, 50, 200), 0, "usn journal changed");
    CHECK_INT(cdc::usn_state_usable(0xABCDEF01, 49, 0xABCDEF01, 50, 200), 0, "usn cursor before first");
    CHECK_INT(cdc::usn_state_usable(0xABCDEF01, 201, 0xABCDEF01, 50, 200), 0, "usn cursor after next");
    printf("snapshot done\n");
}

/* ---------------- 3. hashcache ---------------- */

static cdc::DigestSet digest_set1(cdc::Digest d) {
    cdc::DigestSet s{};
    s.has_full = true;
    s.full = d;
    return s;
}

// entry_for 的 optional 结果写进 out（找不到时 out 不动），返回是否命中。
static bool entry_into(const cdc::HashCache *hc, const char *path, uint64_t size, uint64_t mtime,
                       cdc::DigestSet &out) {
    if (!hc) return false;
    std::optional<cdc::DigestSet> d = hc->entry_for(path, size, mtime);
    if (d) out = *d;
    return d.has_value();
}

static void test_hashcache() {
    printf("== hashcache ==\n");
    char dir_tmpl[] = "/tmp/cdc-test-hc-XXXXXX";
    if (!mkdtemp(dir_tmpl)) { perror("mkdtemp"); exit(2); }
    char dbpath[PATH_MAX];
    join_path(dbpath, sizeof dbpath, dir_tmpl, "hash-cache-v3.db");
    uint64_t now = cdc::now_seconds();

    cdc::Digest d1 = blake3_of("hello", 5);
    cdc::Digest d2 = blake3_of("world", 5);
    cdc::Digest d3 = blake3_of("sample", 6);

    /* remember / entry_for roundtrip（内存态 + 落盘）。 */
    std::unique_ptr<cdc::HashCache> hc = cdc::HashCache::open_at(dbpath, false);
    CHECK(hc != nullptr, "hashcache open");
    const char *pA = "/tmp/a.bin", *pB = "/tmp/b.bin", *pC = "/tmp/c.bin";
    cdc::DigestSet s1 = digest_set1(d1);
    hc->remember(pA, 100, 1000, s1, now);
    CHECK(hc->size() > 0, "remember A");
    cdc::DigestSet out;
    CHECK(entry_into(hc.get(), pA, 100, 1000, out), "entry A found");
    CHECK_INT(out.has_full, 1, "entry A has_full");
    CHECK(memcmp(out.full.bytes, d1.bytes, 32) == 0, "entry A digest matches");
    /* (size, mtime) 不符 => 找不到。 */
    CHECK(!entry_into(hc.get(), pA, 101, 1000, out), "entry A size mismatch");
    CHECK(!entry_into(hc.get(), pA, 100, 1001, out), "entry A mtime mismatch");
    CHECK(!entry_into(hc.get(), pB, 100, 1000, out), "entry B absent");

    /* 文件变了（size/mtime）=> 旧摘要清掉，只保留新记的档位。 */
    cdc::DigestSet s2{};
    s2.has_sample = true;
    s2.sample = d3;
    hc->remember(pA, 200, 2000, s2, now);
    CHECK(hc->size() > 0, "remember A changed");
    CHECK(entry_into(hc.get(), pA, 200, 2000, out), "entry A new found");
    CHECK_INT(out.has_full, 0, "entry A full reset");
    CHECK_INT(out.has_sample, 1, "entry A sample kept");
    CHECK(memcmp(out.sample.bytes, d3.bytes, 32) == 0, "entry A sample matches");

    /* COALESCE：先记 head，再记 full，落盘后两档都在。 */
    cdc::DigestSet sh{};
    sh.has_head = true;
    sh.head = d2;
    hc->remember(pB, 50, 500, sh, now);
    CHECK(hc->size() > 0, "remember B head");
    CHECK_INT(hc->save_dirty(), 0, "save_dirty 1");
    cdc::DigestSet sf = digest_set1(d1);
    hc->remember(pB, 50, 500, sf, now);
    CHECK(hc->size() > 0, "remember B full");
    CHECK_INT(hc->save_dirty(), 0, "save_dirty 2");
    hc.reset();

    /* 重开：只加载请求的路径。
     * load_for_paths 走默认库（需 LOCALAPPDATA 指向 tmp）。 */
    setenv("LOCALAPPDATA", dir_tmpl, 1);
    std::string defpath_s = cdc::hashcache_default_path().value_or(std::string());
    const char *defpath = defpath_s.c_str();
    CHECK(!defpath_s.empty(), "default path");
    /* 回归：cdc::cache_dir 只能是 "C盘清理工具/mftcache"（一个"盘"）。
     * 2026-11-17 切片 4 端到端验证发现 mft.c 的旧实现写成了 "C盘盘清理工具"。 */
    {
        std::optional<std::string> cdo = cdc::cache_dir();
        CHECK(cdo.has_value(), "cache_dir non-null");
        if (cdo) {
            const char *cd = cdo->c_str();
            CHECK(strstr(cd, "C盘清理工具") != nullptr, "cache_dir single-pan: %s", cd);
            CHECK(strstr(cd, "C盘盘清理工具") == nullptr, "cache_dir no doubled pan: %s", cd);
            CHECK(strstr(cd, "mftcache") != nullptr, "cache_dir has mftcache: %s", cd);
        }
    }
    CHECK(strcmp(defpath, dbpath) == 0 || strstr(defpath, "hash-cache-v3.db") != nullptr,
          "default path name");
    /* 把 dbpath 的库复制到默认位置，再测 load_for_paths 只加载请求路径。 */
    {
        char cmd[PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof cmd, "cp '%s' '%s'", dbpath, defpath);
        CHECK_INT(system(cmd), 0, "copy db to default path");
    }
    const char *paths[] = { pB };
    std::unique_ptr<cdc::HashCache> hc3 = cdc::HashCache::load_for_paths(std::span<const char *const>(paths, 1));
    CHECK(hc3 != nullptr, "load_for_paths");
    CHECK(entry_into(hc3.get(), pB, 50, 500, out), "load B found");
    CHECK_INT(out.has_head, 1, "load B head kept");
    CHECK_INT(out.has_full, 1, "load B full kept");
    CHECK(!entry_into(hc3.get(), pA, 200, 2000, out), "load A not requested");
    CHECK_INT(hc3->size(), 1, "load only 1 entry");
    hc3.reset();

    /* 30 天剪枝：last_seen = now-31d 的条目在 save_dirty 后消失。
     * open_at 不预加载任何路径，SQL 直查表验证剪枝结果。 */
    std::unique_ptr<cdc::HashCache> hc4 = cdc::HashCache::open_at(dbpath, false);
    cdc::DigestSet sC = digest_set1(d2);
    hc4->remember(pC, 10, 10, sC, now - 31ULL * 86400);
    CHECK(hc4->size() > 0, "remember C old");
    CHECK_INT(hc4->save_dirty(), 0, "save_dirty prune");
    hc4.reset();
    CHECK_INT(db_query_int(dbpath, "SELECT COUNT(*) FROM file_hashes WHERE path='/tmp/c.bin'"), 0, "pruned C gone");
    CHECK_INT(db_query_int(dbpath, "SELECT COUNT(*) FROM file_hashes WHERE path='/tmp/b.bin'"), 1, "B survives prune");

    /* hex 往返。 */
    std::string hex = cdc::digest_to_hex(d1);
    cdc::Digest back;
    CHECK(cdc::hex_to_digest(hex, back), "hex roundtrip ok");
    CHECK(memcmp(back.bytes, d1.bytes, 32) == 0, "hex roundtrip bytes");
    CHECK(!cdc::hex_to_digest("zz", back), "hex bad rejects");
    CHECK(!cdc::hex_to_digest("abcd", back), "hex short rejects");

    /* 旧版 v2 JSON 迁移（仅 CDC_HAVE_CJSON 时）。 */
#ifdef CDC_HAVE_CJSON
    {
        char v2path[PATH_MAX];
        join_path(v2path, sizeof v2path, dir_tmpl, "mftcache");
        mkdir(v2path, 0755);
        join_path(v2path, sizeof v2path, dir_tmpl, "mftcache/hash-cache-v2.json");
        char d1hex[65], d3hex[65];
        cdc::digest_to_hex(d1, d1hex);
        cdc::digest_to_hex(d3, d3hex);
        FILE *f = fopen(v2path, "w");
        fprintf(f,
                "{\"entries\":{\"/tmp/legacy.bin\":{\"size\":777,\"mtime\":888,"
                "\"sample\":\"%s\",\"full\":\"%s\",\"last_seen\":%llu}}}",
                d3hex, d1hex, (unsigned long long)now);
        fclose(f);
        /* cache_dir() = $LOCALAPPDATA/C盘清理工具/mftcache；LOCALAPPDATA 已指
         * 向 dir_tmpl，但迁移读的是 cache_dir()/hash-cache-v2.json——路径
         * 里多了 "C盘清理工具" 一层，上面写的位置不对，移到正确位置。 */
        char real_v2[PATH_MAX];
        std::string cd = cdc::cache_dir().value_or(".");
        snprintf(real_v2, sizeof real_v2, "%s/hash-cache-v2.json", cd.c_str());
        char cmd[PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof cmd, "cp '%s' '%s'", v2path, real_v2);
        CHECK_INT(system(cmd), 0, "copy v2 json to cache dir");
        char db2[PATH_MAX];
        join_path(db2, sizeof db2, dir_tmpl, "v2-migrated.db");
        std::unique_ptr<cdc::HashCache> hcm = cdc::HashCache::open_at(db2, true);
        CHECK(hcm != nullptr, "migration open");
        hcm.reset();
        /* open_at 不预加载，SQL 直查迁移结果。 */
        CHECK_INT(db_query_int(db2, "SELECT COUNT(*) FROM file_hashes WHERE path='/tmp/legacy.bin'"), 1,
                  "migrated entry found");
        CHECK_INT(db_query_int(db2, "SELECT head IS NULL FROM file_hashes WHERE path='/tmp/legacy.bin'"), 1,
                  "migrated head absent (v2 has no head)");
        CHECK(db_blob_equals(db2, "SELECT sample FROM file_hashes WHERE path='/tmp/legacy.bin'", d3.bytes, 32),
              "migrated sample present");
        CHECK(db_blob_equals(db2, "SELECT full FROM file_hashes WHERE path='/tmp/legacy.bin'", d1.bytes, 32),
              "migrated full present");
    }
#endif

    unlink(dbpath);
    rmdir(dir_tmpl);
    printf("hashcache done\n");
}

/* ---------------- 4. fastio（Linux 等价实现） ---------------- */

static int g_ticks = 0;
static void tick_cb() { g_ticks++; }

static void test_fastio() {
    printf("== fastio ==\n");
    char dir_tmpl[] = "/tmp/cdc-test-fio-XXXXXX";
    if (!mkdtemp(dir_tmpl)) { perror("mkdtemp"); exit(2); }
    char pa[PATH_MAX], pb[PATH_MAX], pc[PATH_MAX];
    join_path(pa, sizeof pa, dir_tmpl, "a.bin");
    join_path(pb, sizeof pb, dir_tmpl, "b.bin");
    join_path(pc, sizeof pc, dir_tmpl, "single.bin");

    /* 硬链接对：identity 相同；单文件：无标识。 */
    write_repeat(pa, 8192, 'A');
    if (link(pa, pb) != 0) { perror("link"); exit(2); }
    write_repeat(pc, 100, 'C');
    cdc::FileId idA, idB;
    CHECK_INT(cdc::query_file_identity(pa, idA), 1, "hardlink A has id");
    CHECK_INT(cdc::query_file_identity(pb, idB), 1, "hardlink B has id");
    CHECK_U64(idA.file_index, idB.file_index, "hardlink same index");
    CHECK_INT(cdc::query_file_identity(pc, idA), 0, "single no id");
    CHECK_INT(cdc::query_file_identity("/tmp/cdc-test-no-such-file.bin", idA), 0,
              "missing no id");

    /* iocp_read_heads：摘要 == xxh3(前 4KB) 的 32 字节载体编码，
     * ticks == 请求数（含失败请求）。 */
    static char headA[4096], headC[100];
    memset(headA, 'A', sizeof headA);
    memset(headC, 'C', sizeof headC);
    cdc::Digest expA, expC;
    cdc::xxh3_buf_to_digest32((const uint8_t *)headA, 4096, expA.bytes);
    cdc::xxh3_buf_to_digest32((const uint8_t *)headC, 100, expC.bytes);
    const cdc::IocpRequest reqs[3] = {
        {0, pa, 0, 4096},
        {1, pc, 0, 4096},
        {2, "/tmp/cdc-test-no-such-file.bin", 0, 4096},
    };
    g_ticks = 0;
    std::vector<cdc::IocpResult> res = cdc::iocp_read_heads(reqs, 4, nullptr, tick_cb);
    size_t nres = res.size();
    CHECK_U64(nres, 2, "iocp results (missing skipped)");
    CHECK_INT(g_ticks, 3, "iocp ticks == requests");
    if (nres == 2) {
        CHECK_INT(res[0].idx, 0, "iocp res0 idx");
        CHECK(memcmp(res[0].digest, expA.bytes, 32) == 0, "iocp res0 digest");
        CHECK_INT(res[1].idx, 1, "iocp res1 idx");
        CHECK(memcmp(res[1].digest, expC.bytes, 32) == 0, "iocp res1 digest");
    }

    /* coalesced_raw_volume_heads：Linux 恒返回 0。 */
    const cdc::RawHeadTarget tgt[1] = {{0, 0, 4096}};
    CHECK_U64(cdc::coalesced_raw_volume_heads('C', tgt, true, nullptr, {}).size(), 0,
              "coalesced linux 0");

    /* open_checked + read_at：读到 'A'。 */
    {
        cdc::File f = cdc::File::open_checked(pa, 0, cdc::kFileFlagSequentialScan, nullptr,
                                              nullptr, nullptr, nullptr);
        CHECK(static_cast<bool>(f), "open_checked");
        if (f) {
            uint8_t buf[16];
            size_t got = 0;
            CHECK_INT(f.read_at(0, buf, sizeof buf, got), 0, "read_at rc");
            CHECK_U64(got, 16, "read_at n");
            CHECK(memcmp(buf, "AAAAAAAAAAAAAAAA", 16) == 0, "read_at content");
        }
    }

    unlink(pa);
    unlink(pb);
    unlink(pc);
    rmdir(dir_tmpl);
    printf("fastio done\n");
}

/* ---------------- 5. scan_big（fixtures 边界分档） ---------------- */

static void test_scan_big(const char *fixtures) {
    printf("== scan_big (%s) ==\n", fixtures);
    const char *drives[] = { fixtures };
    cdc::BigFileList out;
    cdc::ScanOpts opts;
    CHECK_INT(cdc::scan_big(drives, opts, out), 0, "scan_big rc");
    /* 期望 5 条，按 size 降序。 */
    static const char *want_names[] = { "setup.exe", "data.bin", "movie2.mkv", "song.mp3", "photo.jpg" };
    static const uint64_t want_sizes[] = { 150ULL * 1024 * 1024, 60ULL * 1024 * 1024,
                                            25ULL * 1024 * 1024, 6ULL * 1024 * 1024,
                                            3ULL * 1024 * 1024 };
    CHECK_U64(out.size(), 5, "scan_big count");
    for (size_t i = 0; i < out.size() && i < 5; i++) {
        CHECK_STREQ(base_name(out[i].path.c_str()), want_names[i], "scan_big name");
        CHECK_U64(out[i].size, want_sizes[i], "scan_big size");
    }
    /* 排除项：setup2.exe 90M（<100M）、data2.bin 40M（<50M）、
     * movie.mp4 3M（<20M）、song2.mp3 4M（<5M）、small.jpg 1M（<2MB 底线）、
     * sub/pagefile.sys 10M（系统名）。 */
    for (size_t i = 0; i < out.size(); i++) {
        const char *b = base_name(out[i].path.c_str());
        CHECK(strcmp(b, "setup2.exe") != 0 && strcmp(b, "data2.bin") != 0 &&
                  strcmp(b, "movie.mp4") != 0 && strcmp(b, "song2.mp3") != 0 &&
                  strcmp(b, "small.jpg") != 0 && strcmp(b, "pagefile.sys") != 0,
              "scan_big excluded %s", b);
    }
    printf("scan_big done\n");
}

/* ---------------- 6. scan_dups 端到端 ---------------- */

static int group_has_files(const cdc::DupGroupList *g, const char *a, const char *b) {
    for (const cdc::DupGroup &grp : *g) {
        if (grp.files.size() == 2) {
            const char *f0 = base_name(grp.files[0].c_str());
            const char *f1 = base_name(grp.files[1].c_str());
            if ((strcmp(f0, a) == 0 && strcmp(f1, b) == 0) ||
                (strcmp(f0, b) == 0 && strcmp(f1, a) == 0))
                return 1;
        }
    }
    return 0;
}

static int group_count_for(const cdc::DupGroupList *g, const char *a) {
    int n = 0;
    for (const cdc::DupGroup &grp : *g)
        for (const std::string &f : grp.files)
            if (strcmp(base_name(f.c_str()), a) == 0) n++;
    return n;
}

static void test_scan_dups() {
    printf("== scan_dups ==\n");
    char dir_tmpl[] = "/tmp/cdc-test-dups-XXXXXX";
    if (!mkdtemp(dir_tmpl)) { perror("mkdtemp"); exit(2); }
    /* 缓存落到临时目录（cache_dir 走 LOCALAPPDATA）。 */
    char cache_tmpl[] = "/tmp/cdc-test-dups-cache-XXXXXX";
    if (!mkdtemp(cache_tmpl)) { perror("mkdtemp"); exit(2); }
    setenv("LOCALAPPDATA", cache_tmpl, 1);

    char p[PATH_MAX];
    /* a 对：相同 8KB => 成组。 */
    join_path(p, sizeof p, dir_tmpl, "a1.bin"); write_repeat(p, 8192, 'A');
    join_path(p, sizeof p, dir_tmpl, "a2.bin"); write_repeat(p, 8192, 'A');
    /* b 对：同大小、同 head 4KB，偏移 5000 处不同 => 不成组。 */
    join_path(p, sizeof p, dir_tmpl, "b1.bin"); write_repeat(p, 8192, 'B');
    join_path(p, sizeof p, dir_tmpl, "b2.bin"); write_repeat(p, 8192, 'B');
    join_path(p, sizeof p, dir_tmpl, "b2.bin"); pwrite_byte(p, 5000, 'X');
    /* c 对：相同 100 字节（<=4KB）=> 成组。 */
    join_path(p, sizeof p, dir_tmpl, "c1.bin"); write_repeat(p, 100, 'C');
    join_path(p, sizeof p, dir_tmpl, "c2.bin"); write_repeat(p, 100, 'C');
    /* d 组：相同 100KB + d1 的硬链接 d1h => 组里只有 2 个（去重）。 */
    char d1[PATH_MAX], d1h[PATH_MAX];
    join_path(d1, sizeof d1, dir_tmpl, "d1.bin"); write_repeat(d1, 100 * 1024, 'D');
    join_path(p, sizeof p, dir_tmpl, "d2.bin"); write_repeat(p, 100 * 1024, 'D');
    join_path(d1h, sizeof d1h, dir_tmpl, "d1h.bin");
    if (link(d1, d1h) != 0) { perror("link"); exit(2); }
    /* e 对：2MB，strong 窗口内相同、512K 处不同 => fast 成组，normal 不成组。 */
    join_path(p, sizeof p, dir_tmpl, "e1.bin"); make_sparse(p, 2ULL * 1024 * 1024);
    join_path(p, sizeof p, dir_tmpl, "e2.bin"); make_sparse(p, 2ULL * 1024 * 1024);
    join_path(p, sizeof p, dir_tmpl, "e2.bin"); pwrite_byte(p, 512 * 1024, 'E');
    /* f 对：20MB 稀疏，head/mid/tail 相同、25%（5MB）处不同 => probe 淘汰。 */
    join_path(p, sizeof p, dir_tmpl, "f1.bin"); make_sparse(p, 20ULL * 1024 * 1024);
    join_path(p, sizeof p, dir_tmpl, "f2.bin"); make_sparse(p, 20ULL * 1024 * 1024);
    join_path(p, sizeof p, dir_tmpl, "f2.bin"); pwrite_byte(p, 5ULL * 1024 * 1024, 'F');

    const char *roots[] = { dir_tmpl };
    cdc::ScanOpts opts;

    /* 正常模式：a、c、d 三组；e、f 不成组。 */
    cdc::DupGroupList g1;
    CHECK_INT(cdc::scan_dups(roots, 1, opts, g1), 0, "scan_dups rc");
    CHECK_U64(g1.size(), 3, "scan_dups normal groups");
    CHECK(group_has_files(&g1, "a1.bin", "a2.bin"), "group a");
    CHECK(group_has_files(&g1, "c1.bin", "c2.bin"), "group c");
    CHECK(group_has_files(&g1, "d1.bin", "d2.bin"), "group d");
    CHECK_INT(group_count_for(&g1, "d1h.bin"), 0, "hardlink deduped");
    CHECK_INT(group_count_for(&g1, "b1.bin"), 0, "b no group");
    CHECK_INT(group_count_for(&g1, "e1.bin"), 0, "e no group (normal)");
    CHECK_INT(group_count_for(&g1, "f1.bin"), 0, "f no group (probe)");

    /* fast 模式：e 成组；f 也成组——fast 下 strong 即终局、不走 probe，
     * 5MB 处的差异落在 strong 窗口之外（与 Rust 版 fast 模式同一取舍）。 */
    cdc::ScanOpts fopts;
    fopts.fast = true;
    cdc::DupGroupList g2;
    CHECK_INT(cdc::scan_dups(roots, 1, fopts, g2), 0, "scan_dups fast rc");
    CHECK(group_has_files(&g2, "e1.bin", "e2.bin"), "group e (fast)");
    CHECK(group_has_files(&g2, "f1.bin", "f2.bin"), "group f (fast, strong misses 5MB diff)");

    /* 第二次跑：结果相同（缓存命中路径），且缓存里有 a1 的 full 摘要。 */
    cdc::DupGroupList g3;
    CHECK_INT(cdc::scan_dups(roots, 1, opts, g3), 0, "scan_dups second rc");
    CHECK_U64(g3.size(), 3, "scan_dups second groups");
    CHECK(group_has_files(&g3, "a1.bin", "a2.bin"), "group a (second)");
    {
        char pa1[PATH_MAX];
        join_path(pa1, sizeof pa1, dir_tmpl, "a1.bin");
        struct stat st;
        stat(pa1, &st);
        const char *paths[] = { pa1 };
        std::unique_ptr<cdc::HashCache> hc = cdc::HashCache::load_for_paths(std::span<const char *const>(paths, 1));
        cdc::DigestSet o;
        CHECK(entry_into(hc.get(), pa1, (uint64_t)st.st_size, (uint64_t)st.st_mtime, o), "cache has a1");
        CHECK_INT(o.has_full, 1, "cache a1 has_full");
        hc.reset();
    }

    /* 改动 a2（内容+mtime）=> a 组消失。 */
    join_path(p, sizeof p, dir_tmpl, "a2.bin");
    write_repeat(p, 8192, 'Z');
    bump_mtime(p, 5); /* 同秒内重写 mtime 不变，缓存仍会命中——必须推后 mtime */
    cdc::DupGroupList g4;
    CHECK_INT(cdc::scan_dups(roots, 1, opts, g4), 0, "scan_dups after touch rc");
    CHECK_INT(group_count_for(&g4, "a1.bin"), 0, "a group gone after touch");
    CHECK_INT(group_count_for(&g4, "a2.bin"), 0, "a2 not grouped");

    /* 清理。 */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", dir_tmpl, cache_tmpl);
    CHECK_INT(system(cmd), 0, "cleanup dups tmp");
    printf("scan_dups done\n");
}

/* ---------------- 7. cdc::scan_hash_groups（切片 4 契约） ---------------- */

static void test_hash_groups() {
    printf("== scan_hash_groups ==\n");
    char dir_tmpl[] = "/tmp/cdc-test-hg-XXXXXX";
    if (!mkdtemp(dir_tmpl)) { perror("mkdtemp"); exit(2); }
    char cache_tmpl[] = "/tmp/cdc-test-hg-cache-XXXXXX";
    if (!mkdtemp(cache_tmpl)) { perror("mkdtemp"); exit(2); }
    setenv("LOCALAPPDATA", cache_tmpl, 1);

    char g1p[PATH_MAX], g2p[PATH_MAX], g3p[PATH_MAX];
    join_path(g1p, sizeof g1p, dir_tmpl, "g1.bin"); write_repeat(g1p, 8192, 'G');
    join_path(g2p, sizeof g2p, dir_tmpl, "g2.bin"); write_repeat(g2p, 8192, 'G');
    join_path(g3p, sizeof g3p, dir_tmpl, "g3.bin"); write_repeat(g3p, 4096, 'H');
    struct stat s1, s2, s3;
    stat(g1p, &s1); stat(g2p, &s2); stat(g3p, &s3);

    cdc::ScanInput inputs[3];
    inputs[0].path = g1p; inputs[0].size = (uint64_t)s1.st_size; inputs[0].mtime = (uint64_t)s1.st_mtime; inputs[0].frn = 0;
    inputs[1].path = g2p; inputs[1].size = (uint64_t)s2.st_size; inputs[1].mtime = (uint64_t)s2.st_mtime; inputs[1].frn = 0;
    inputs[2].path = g3p; inputs[2].size = (uint64_t)s3.st_size; inputs[2].mtime = (uint64_t)s3.st_mtime; inputs[2].frn = 0;

    std::unique_ptr<cdc::HashCache> cache = cdc::HashCache::open(nullptr); /* 纯内存 */
    std::vector<cdc::DigestUpdate> updates;
    CHECK_INT(cdc::scan_hash_groups(inputs, cache.get(), cdc::now_seconds(), updates), 0,
              "hash_groups rc");
    size_t n_updates = updates.size();
    /* g1/g2 一对（同 size 且内容相同）=> 各一条带 full 的 update；g3 单件 => 无。 */
    CHECK_U64(n_updates, 2, "hash_groups updates");
    int saw_g1 = 0, saw_g2 = 0;
    for (size_t i = 0; i < n_updates; i++) {
        CHECK_INT(updates[i].digests.has_full, 1, "update has_full");
        /* volume_serial 填 0 是已知缺口（见 scan.h）：调用方在
         * set_digests_batch 前必须按卷填好。 */
        CHECK_U64(updates[i].volume_serial, 0, "update volume_serial 0 (known gap)");
        if (updates[i].size == 8192 && updates[i].mtime == (uint64_t)s1.st_mtime) saw_g1 = 1;
        if (updates[i].size == 8192 && updates[i].mtime == (uint64_t)s2.st_mtime) saw_g2 = 1;
    }
    CHECK(saw_g1 && saw_g2, "updates cover g1+g2");
    /* 两文件内容相同 => 两条 update 的 full 摘要字节相等。 */
    if (n_updates == 2)
        CHECK(memcmp(updates[0].digests.full.bytes, updates[1].digests.full.bytes, 32) == 0,
              "updates full digests equal");

    /* 管线已把摘要写回缓存（内存态）。 */
    cdc::DigestSet o;
    CHECK(entry_into(cache.get(), g1p, (uint64_t)s1.st_size, (uint64_t)s1.st_mtime, o), "cache has g1");
    CHECK_INT(o.has_full, 1, "cache g1 has_full");
    cache.reset();

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", dir_tmpl, cache_tmpl);
    CHECK_INT(system(cmd), 0, "cleanup hg tmp");
    printf("scan_hash_groups done\n");
}

/* ---------------- 6. sampling（XXH3-64） ---------------- */

static void test_sampling() {
    printf("== sampling ==\n");
    /* XXH3-64 已知向量：空输入（seed 0）= 0x2d06800538d394c2。 */
    CHECK_U64(cdc::xxh3_64(nullptr, 0), 0x2d06800538d394c2ULL, "xxh3 empty vector");
    CHECK_U64(cdc::xxh3_64((const uint8_t *)"", 0), 0x2d06800538d394c2ULL, "xxh3 empty str");
    /* 确定性 + 流式与一次性一致。 */
    static const uint8_t data[] = "the quick brown fox jumps over the lazy dog";
    uint64_t a = cdc::xxh3_64(data, sizeof data - 1);
    uint64_t b = cdc::xxh3_64(data, sizeof data - 1);
    CHECK_U64(a, b, "xxh3 deterministic");
    CHECK(a != 0, "xxh3 nonzero");
    auto st = cdc::Xxh3State::create();
    CHECK(st != nullptr, "xxh3 state new");
    if (st) {
        CHECK_INT(st->update(data, 10), 0, "xxh3 update 1");
        CHECK_INT(st->update(data + 10, sizeof data - 1 - 10), 0,
                  "xxh3 update 2");
        CHECK_U64(st->finalize(), a, "xxh3 streaming == oneshot");
    }
    /* 载体编码往返 + hex。 */
    uint8_t carrier[32];
    cdc::xxh_to_digest32(a, carrier);
    CHECK_U64(cdc::xxh_from_digest32(carrier), a, "xxh carrier roundtrip");
    CHECK(carrier[8] == 0 && carrier[31] == 0, "xxh carrier zero tail");
    uint8_t buf2[32];
    cdc::xxh3_buf_to_digest32(data, sizeof data - 1, buf2);
    CHECK(memcmp(buf2, carrier, 32) == 0, "xxh buf_to_digest32");
    char hex[17];
    cdc::xxh_to_hex(a, hex);
    CHECK(strlen(hex) == 16, "xxh hex len");
    uint64_t back = 0;
    for (int i = 0; i < 16; i++) {
        char ch = hex[i];
        CHECK((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'), "xxh hex lowercase");
        unsigned v = (ch >= '0' && ch <= '9') ? (unsigned)(ch - '0') : (unsigned)(ch - 'a' + 10);
        back = (back << 4) | v;
    }
    CHECK_U64(back, a, "xxh hex roundtrip");
    printf("sampling done\n");
}

/* ---------------- 7. hashcache 与旧版 BLAKE3 full 的兼容 ---------------- */

static void test_hashcache_xxh_compat() {
    printf("== hashcache xxh compat ==\n");
    char dir_tmpl[] = "/tmp/cdc-test-xc-XXXXXX";
    if (!mkdtemp(dir_tmpl)) { perror("mkdtemp"); exit(2); }
    char dbpath[PATH_MAX];
    join_path(dbpath, sizeof dbpath, dir_tmpl, "hash-cache-v3.db");
    uint64_t now = cdc::now_seconds();
    const char *p = "/tmp/compat.bin";

    /* 1. 预置一条"旧版"记录：只有 BLAKE3 full（模拟 Rust 版或旧版 C
     *    写下的库），xxh 三列全 nullptr。 */
    cdc::Digest full = blake3_of("compat-content", 14);
    std::unique_ptr<cdc::HashCache> hc = cdc::HashCache::open_at(dbpath, false);
    CHECK(hc != nullptr, "compat open");
    cdc::DigestSet s{};
    s.has_full = true;
    s.full = full;
    hc->remember(p, 1234, 5678, s, now);
    CHECK(hc->size() > 0, "compat remember full");
    CHECK_INT(hc->save_dirty(), 0, "compat save");
    hc.reset();
    CHECK(db_blob_equals(dbpath, "SELECT full FROM file_hashes WHERE path='/tmp/compat.bin'",
                         full.bytes, 32),
          "compat full stored");
    CHECK_INT(db_query_int(dbpath, "SELECT head_xxh IS NULL FROM file_hashes WHERE path='/tmp/compat.bin'"),
              1, "compat head_xxh null before xxh write");

    /* 2. 重开读回（走默认库路径，与 test_hashcache 同一套路）：
     *    full 命中且值不变，xxh 三档都没有。 */
    setenv("LOCALAPPDATA", dir_tmpl, 1);
    std::string defpath_s = cdc::hashcache_default_path().value_or(std::string());
    const char *defpath = defpath_s.c_str();
    CHECK(!defpath_s.empty(), "compat default path");
    {
        char cmd[PATH_MAX * 2 + 64];
        snprintf(cmd, sizeof cmd, "cp '%s' '%s'", dbpath, defpath);
        CHECK_INT(system(cmd), 0, "compat copy db to default path");
    }
    const char *paths[] = {p};
    std::unique_ptr<cdc::HashCache> hc2 = cdc::HashCache::load_for_paths(std::span<const char *const>(paths, 1));
    CHECK(hc2 != nullptr, "compat load_for_paths");
    cdc::DigestSet out;
    CHECK(entry_into(hc2.get(), p, 1234, 5678, out), "compat entry found");
    CHECK_INT(out.has_full, 1, "compat has_full");
    CHECK(memcmp(out.full.bytes, full.bytes, 32) == 0, "compat full value kept");
    CHECK_INT(out.has_head_xxh, 0, "compat no head_xxh yet");
    CHECK_INT(out.has_sample_xxh, 0, "compat no sample_xxh yet");
    CHECK_INT(out.has_strong_xxh, 0, "compat no strong_xxh yet");
    hc2.reset();

    /* 3. 再记一条 xxh head（模拟新版扫描），落盘后 full 仍是原来
     *    的 BLAKE3 值、不被 xxHash 覆盖；head_xxh 落进新列。 */
    uint64_t xhead = cdc::xxh3_64((const uint8_t *)"head-data", 9);
    std::unique_ptr<cdc::HashCache> hc3 = cdc::HashCache::open_at(dbpath, false);
    cdc::DigestSet sx{};
    sx.has_head_xxh = true;
    sx.head_xxh = xhead;
    /* open_at 不预加载：先把旧 full 记回内存（模拟 load_for_paths 已加载）。 */
    cdc::DigestSet sf{};
    sf.has_full = true;
    sf.full = full;
    hc3->remember(p, 1234, 5678, sf, now);
    CHECK(hc3->size() > 0, "compat reseed full");
    hc3->remember(p, 1234, 5678, sx, now);
    CHECK(hc3->size() > 0, "compat remember xxh");
    CHECK_INT(hc3->save_dirty(), 0, "compat save xxh");
    hc3.reset();
    CHECK(db_blob_equals(dbpath, "SELECT full FROM file_hashes WHERE path='/tmp/compat.bin'",
                         full.bytes, 32),
          "compat full not overwritten by xxh");
    CHECK_INT(db_query_int(dbpath, "SELECT head_xxh FROM file_hashes WHERE path='/tmp/compat.bin'"),
              (int64_t)xhead, "compat head_xxh stored");
    CHECK_INT(db_query_int(dbpath, "SELECT head IS NULL FROM file_hashes WHERE path='/tmp/compat.bin'"),
              1, "compat head blob still null (xxh not in blake3 col)");

    /* 4. 旧库补列：手动建一个只有 7 列的表（模拟 Rust 版建的库），
     *    打开后三列必须被补上。 */
    char old_db[PATH_MAX];
    join_path(old_db, sizeof old_db, dir_tmpl, "old-7col.db");
    sqlite3 *odb = nullptr;
    CHECK_INT(sqlite3_open(old_db, &odb), SQLITE_OK, "old db open");
    char *emsg = nullptr;
    sqlite3_exec(odb,
                 "CREATE TABLE file_hashes (path TEXT PRIMARY KEY, size INTEGER NOT NULL,"
                 " mtime INTEGER NOT NULL, head BLOB, sample BLOB, strong BLOB, full BLOB,"
                 " last_seen INTEGER NOT NULL) WITHOUT ROWID;",
                 nullptr, nullptr, &emsg);
    sqlite3_close(odb);
    std::unique_ptr<cdc::HashCache> hco = cdc::HashCache::open_at(old_db, false);
    CHECK(hco != nullptr, "old db open_at");
    hco.reset();
    CHECK_INT(db_query_int(old_db,
                           "SELECT COUNT(*) FROM pragma_table_info('file_hashes')"
                           " WHERE name IN ('head_xxh','sample_xxh','strong_xxh')"),
              3, "old db xxh cols added");

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir_tmpl);
    CHECK_INT(system(cmd), 0, "compat cleanup");
    printf("hashcache xxh compat done\n");
}

/* 一组内是否恰好包含 names 全部（n 个）文件。 */
static int group_has_all(const cdc::DupGroupList *g, const char **names, size_t n) {
    for (const cdc::DupGroup &grp : *g) {
        if (grp.files.size() != n) continue;
        int ok = 1;
        for (size_t k = 0; k < n && ok; k++) {
            int found = 0;
            for (const std::string &f : grp.files) {
                if (strcmp(base_name(f.c_str()), names[k]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) ok = 0;
        }
        if (ok) return 1;
    }
    return 0;
}

/* ---------------- 8. 并行（cdc::par_for / 共享锁） ---------------- */

static int g_par_marks[1024];
static void par_mark_work(size_t idx) { g_par_marks[idx] = 1; }

static int g_par_locked_total = 0;
static void par_locked_work(size_t) {
    cdc::LockGuard g(cdc::shared_mutex());
    g_par_locked_total++;
}

static void test_par() {
    printf("== par ==\n");
    CHECK_INT(cdc::par_cpu_count() >= 1, 1, "cpu count >= 1");
    CHECK_INT(cdc::par_hash_workers(true) >= 1 && cdc::par_hash_workers(true) <= 32, 1,
              "ssd workers range");
    CHECK_INT(cdc::par_hash_workers(false) >= 1 && cdc::par_hash_workers(false) <= 32, 1,
              "hdd workers range");
    /* 1000 下标、8 线程：每一下标恰好处理一次。 */
    memset(g_par_marks, 0, sizeof g_par_marks);
    CHECK_INT(cdc::par_for(1000, 8, par_mark_work), 0, "par_for rc");
    int all = 1;
    for (int i = 0; i < 1000; i++) all &= (g_par_marks[i] == 1);
    CHECK(all, "par_for each idx once");
    /* 顺序退化：nthreads 0/1、n=0/1。 */
    memset(g_par_marks, 0, sizeof g_par_marks);
    CHECK_INT(cdc::par_for(1000, 1, par_mark_work), 0, "par_for 1 thread");
    all = 1;
    for (int i = 0; i < 1000; i++) all &= (g_par_marks[i] == 1);
    CHECK(all, "par_for 1 thread each idx once");
    CHECK_INT(cdc::par_for(0, 8, par_mark_work), 0, "par_for n=0");
    CHECK_INT(cdc::par_for(1, 8, par_mark_work), 0, "par_for n=1");
    /* 共享锁：8 线程 × 1000 次加锁递增，总数精确（无丢失更新）。 */
    g_par_locked_total = 0;
    CHECK_INT(cdc::par_for(8000, 8, par_locked_work), 0, "par_for locked rc");
    CHECK_INT(g_par_locked_total, 8000, "par_for locked total");
    printf("par done\n");
}

/* ---------------- 9. fixtures 上的 dup 端到端 ---------------- */

static void test_scan_dups_fixtures(const char *fixtures) {
    printf("== scan_dups fixtures (%s) ==\n", fixtures);
    char cache_tmpl[] = "/tmp/cdc-test-dupsfx-cache-XXXXXX";
    if (!mkdtemp(cache_tmpl)) { perror("mkdtemp"); exit(2); }
    setenv("LOCALAPPDATA", cache_tmpl, 1);
    const char *roots[] = { fixtures };
    cdc::ScanOpts opts;
    cdc::DupGroupList g;
    CHECK_INT(cdc::scan_dups(roots, 1, opts, g), 0, "scan_dups fixtures rc");
    /* 只有 movie.mp4 与 photo.jpg 同为 3MB 稀疏零 => 一组；
     * 其余尺寸唯一，pagefile.sys 是系统名被排除。 */
    CHECK_U64(g.size(), 1, "scan_dups fixtures groups");
    CHECK(group_has_files(&g, "movie.mp4", "photo.jpg"), "fixtures group");
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", cache_tmpl);
    CHECK_INT(system(cmd), 0, "cleanup dupsfx cache");
    printf("scan_dups fixtures done\n");
}

/* ---------------- 10. 并行 dup（大文件 + 多文件 + tick 计数） ---------------- */

/* 独立算整文件 blake3（对照 full_hash_by_frn 的结果；不用 scan.c 的函数）。 */
static cdc::Digest blake3_of_file(const char *path) {
    cdc::Digest d{};
    int fd = open(path, O_RDONLY);
    if (fd < 0) return d;
    blake3_hasher h;
    blake3_hasher_init(&h);
    static uint8_t buf[1024 * 1024];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        blake3_hasher_update(&h, buf, (size_t)r);
    close(fd);
    blake3_hasher_finalize(&h, d.bytes, 32);
    return d;
}

static void test_par_dups() {
    printf("== par_dups ==\n");
    char dir_tmpl[] = "/tmp/cdc-test-pardups-XXXXXX";
    if (!mkdtemp(dir_tmpl)) { perror("mkdtemp"); exit(2); }
    char cache_tmpl[] = "/tmp/cdc-test-pardups-cache-XXXXXX";
    if (!mkdtemp(cache_tmpl)) { perror("mkdtemp"); exit(2); }
    setenv("LOCALAPPDATA", cache_tmpl, 1);
    char p[PATH_MAX];
    /* h 对：相同 20MB 稀疏（走 ≥16MB 的 2MB 缓冲全哈希）。 */
    join_path(p, sizeof p, dir_tmpl, "h1.bin");
    make_sparse(p, 20ULL * 1024 * 1024);
    join_path(p, sizeof p, dir_tmpl, "h2.bin");
    make_sparse(p, 20ULL * 1024 * 1024);
    /* 12 个 64KB：6 个 'A' + 6 个 'B' => 两组。 */
    for (int k = 0; k < 6; k++) {
        char name[32];
        snprintf(name, sizeof name, "pa%d.bin", k);
        join_path(p, sizeof p, dir_tmpl, name);
        write_repeat(p, 64 * 1024, 'A');
        snprintf(name, sizeof name, "pb%d.bin", k);
        join_path(p, sizeof p, dir_tmpl, name);
        write_repeat(p, 64 * 1024, 'B');
    }
    const char *roots[] = { dir_tmpl };
    cdc::ScanOpts opts;
    opts.on_tick = tick_cb;
    g_ticks = 0;
    cdc::DupGroupList g;
    CHECK_INT(cdc::scan_dups(roots, 1, opts, g), 0, "par_dups rc");
    CHECK_U64(g.size(), 3, "par_dups groups");
    CHECK(group_has_files(&g, "h1.bin", "h2.bin"), "par group h");
    static const char *want_a[] = { "pa0.bin", "pa1.bin", "pa2.bin",
                                    "pa3.bin", "pa4.bin", "pa5.bin" };
    static const char *want_b[] = { "pb0.bin", "pb1.bin", "pb2.bin",
                                    "pb3.bin", "pb4.bin", "pb5.bin" };
    CHECK(group_has_all(&g, want_a, 6), "par group A");
    CHECK(group_has_all(&g, want_b, 6), "par group B");
    /* h 组的 full 摘要 == 独立算出的整文件 blake3（验证 2MB 缓冲全哈希）。 */
    for (const cdc::DupGroup &grp : g) {
        if (grp.files.size() == 2 &&
            strcmp(base_name(grp.files[0].c_str()), "h1.bin") == 0) {
            char hp[PATH_MAX];
            join_path(hp, sizeof hp, dir_tmpl, "h1.bin");
            cdc::Digest ref = blake3_of_file(hp);
            char hex[65];
            cdc::digest_to_hex(ref, hex);
            CHECK_STREQ(grp.hash_hex.c_str(), hex, "par h full digest");
        }
    }
    /* tick 数与顺序语义一致：阶段 1 的 14 次 + 阶段 2 的 14 次 +
     * 探针 2 次 + 阶段 3 的 14 次 = 44（64KB 组 36 + 20MB 组 8）。 */
    CHECK_INT(g_ticks, 44, "par_dups ticks");
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s'", dir_tmpl, cache_tmpl);
    CHECK_INT(system(cmd), 0, "cleanup par_dups tmp");
    printf("par_dups done\n");
}

/* ---------------- main ---------------- */

/* ---------------- 8. fallback walk 与直接遍历等价 ----------------
 * cdc::fallback_walk_files（新：走 cdc::enum_dir）与"逐项 opendir/readdir +
 * lstat"参照实现在同一棵树上逐项比较：相对路径、大小、mtime 全等。
 * 树含：文件、空文件、子目录、中文名、$ 开头名（跳过）、文件/目录
 * 符号链接（不跟随）、悬空链接（不跟随）、skip_top 顶层目录（整棵跳过，
 * 仅顶层生效——子目录同名不跳过）。 */
using StrList = std::vector<std::string>;  // "relpath|size|mtime"

/* 参照实现：opendir/readdir + lstat，与新 walk 同款过滤。 */
static void ref_fallback_walk(const char *dir, const char *rel, uint64_t min_size,
                              const char *const *skip_top, size_t n_skip,
                              int is_top, StrList *out) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != nullptr) {
        char full[PATH_MAX], rpath[PATH_MAX];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (e->d_name[0] == '$') continue;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (rel[0] == '\0')
            snprintf(rpath, sizeof rpath, "%s", e->d_name);
        else
            snprintf(rpath, sizeof rpath, "%s/%s", rel, e->d_name);
        if (lstat(full, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            int skip = 0;
            if (is_top) {
                for (size_t i = 0; i < n_skip; i++)
                    if (strcasecmp(e->d_name, skip_top[i]) == 0) skip = 1;
            }
            if (!skip)
                ref_fallback_walk(full, rpath, min_size, skip_top, n_skip, 0, out);
            continue;
        }
        if (S_ISREG(st.st_mode) && (uint64_t)st.st_size >= min_size) {
            char line[PATH_MAX + 64];
            snprintf(line, sizeof line, "%s|%llu|%llu", rpath,
                     (unsigned long long)st.st_size,
                     (unsigned long long)st.st_mtime);
            out->push_back(line);
        }
    }
    closedir(d);
}

static void check_walk_matches(const char *root, uint64_t min_size,
                               const char *const *skip_top, size_t n_skip,
                               size_t want_n, const char *what) {
    cdc::FileEntryList got;
    StrList ref, got_s;
    CHECK_INT(cdc::fallback_walk_files(root, min_size, cdc::SkipList(skip_top, n_skip), got), 0, what);
    ref_fallback_walk(root, "", min_size, skip_top, n_skip, 1, &ref);
    for (size_t i = 0; i < got.size(); i++) {
        char line[PATH_MAX + 64];
        const char *rel = got[i].path.c_str() + strlen(root) + 1;
        snprintf(line, sizeof line, "%s|%llu|%llu", rel,
                 (unsigned long long)got[i].size,
                 (unsigned long long)got[i].mtime);
        got_s.push_back(line);
    }
    std::sort(ref.begin(), ref.end());
    std::sort(got_s.begin(), got_s.end());
    CHECK_U64(got_s.size(), want_n, what);
    CHECK_U64(got_s.size(), ref.size(), what);
    for (size_t i = 0; i < got_s.size() && i < ref.size(); i++)
        CHECK_STREQ(got_s[i].c_str(), ref[i].c_str(), what);
}

static void test_fallback_walk() {
    printf("== fallback walk ==\n");
    char root_tmpl[] = "/tmp/cdc-test-walk-XXXXXX";
    char *root = mkdtemp(root_tmpl);
    char p[PATH_MAX], q[PATH_MAX];
    if (!root) {
        perror("mkdtemp");
        exit(2);
    }
    /* 树：文件、空文件、子目录、中文名、$ 名、链接、skip_top（顶层/子层同名）。 */
    join_path(p, sizeof p, root, "top.bin");
    write_repeat(p, 100, 'A');
    join_path(p, sizeof p, root, "empty.bin");
    write_repeat(p, 0, 'A');
    join_path(p, sizeof p, root, "a");
    mkdir(p, 0755);
    join_path(p, sizeof p, root, "a/mid.bin");
    write_repeat(p, 200, 'B');
    join_path(p, sizeof p, root, "a/b");
    mkdir(p, 0755);
    join_path(p, sizeof p, root, "a/b/deep.bin");
    write_repeat(p, 300, 'C');
    join_path(p, sizeof p, root, "$skip.bin");
    write_repeat(p, 400, 'D');
    join_path(p, sizeof p, root, "中文.bin");
    write_repeat(p, 50, 'E');
    join_path(p, sizeof p, root, "link_file");
    { int sr = symlink("top.bin", p); (void)sr; }
    join_path(p, sizeof p, root, "link_dir");
    { int sr = symlink("a", p); (void)sr; }
    join_path(p, sizeof p, root, "link_dangling");
    { int sr = symlink("no-such-target", p); (void)sr; }
    join_path(p, sizeof p, root, "SkipMe");
    mkdir(p, 0755);
    join_path(p, sizeof p, root, "SkipMe/inner.bin");
    write_repeat(p, 500, 'F');
    join_path(p, sizeof p, root, "a/SkipMe");
    mkdir(p, 0755);
    join_path(p, sizeof p, root, "a/SkipMe/nested.bin");
    write_repeat(p, 600, 'G');
    join_path(p, sizeof p, root, "emptydir");
    mkdir(p, 0755);

    /* skip_top 用大写 SKIPME：验证大小写不敏感匹配。 */
    static const char *skip_top[] = { "SKIPME" };
    check_walk_matches(root, 0, skip_top, 1, 6, "walk min_size=0");
    check_walk_matches(root, 250, skip_top, 1, 2, "walk min_size=250");
    /* 无 skip_top：SkipMe/inner.bin 也进来（6+1=7）。 */
    check_walk_matches(root, 0, nullptr, 0, 7, "walk no skip_top");
    /* 缺失目录：打不开静默跳过，返回 0、空集。 */
    {
        cdc::FileEntryList out;
        join_path(q, sizeof q, root, "no-such-dir");
        CHECK_INT(cdc::fallback_walk_files(q, 0, cdc::SkipList(skip_top, 1), out), 0, "walk missing dir");
        CHECK_U64(out.size(), 0, "walk missing dir empty");
    }
    printf("fallback walk done\n");
}

int main(int argc, char **argv) {
    const char *fixtures = argc > 1 ? argv[1] : "tests/fixtures";
    test_parse();
    test_snapshot();
    test_hashcache();
    test_fastio();
    test_scan_big(fixtures);
    test_scan_dups();
    test_scan_dups_fixtures(fixtures);
    test_hash_groups();
    test_fallback_walk();
    test_par();
    test_par_dups();
    test_sampling();
    test_hashcache_xxh_compat();
    printf("test_scan: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
