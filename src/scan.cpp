/* scan.c — 大文件扫描与重复文件扫描，逐行对照 desktop/src/main.rs 的
 * scan_big / scan_dups / run_dup_pipeline / run_dup_pipelines / hash_index_groups。
 *
 * 并行性说明（与 Rust 版的行为差异，仅影响速度不影响结果）：
 *   - 各哈希阶段（sample/strong/probe/full）与 Linux 的 IOCP 等价路径
 *     用 fastio.c 的 cdc_par_for 按文件并行（生产者/消费者队列）；
 *     每个候选只被一个 worker 处理，候选内部字段无需额外锁；
 *     on_tick 回调与 drive hint 缓存由 cdc_shared_lock() 串行化。
 *     阶段结束后分组前都按 (size, 摘要) 排序，所以最终分组与
 *     顺序版一致；取消语义也一致（worker 入口先查 cancel）。
 *   - 机械盘的 NCQ 排序保留（同盘按 sort_key 排，SSD 在前）；
 *     worker 数按 SSD/HDD 分档（SSD 激进、HDD 保守）。
 *   - 全哈希 ≥16MB 用 2MB 缓冲（与 Rust 的双缓冲分支同一档位），
 *     仍是单缓冲循环；读与哈希的重叠主要靠跨文件并行 +
 *     内核 readahead，单文件内再开读线程的收益很小，暂不做。
 *   - 阶段内顺序与 Rust 的 HashMap 迭代顺序不同，但所有分组最后都按
 *     (size, 摘要) 排序、组内按 path 排序，所以最终分组结果一致。
 *
 * 所有权约定：DupCandidate 的 path 是 owned；各阶段 vec 之间用"移动"
 * （push 后把源 path 置 NULL）传递，丢弃的项当场 cand_free。
 * run_dup_pipeline 消费输入数组（元素或被移走或被释放，数组外壳由它 free）。
 */
#ifndef _WIN32
#endif

#include "scan.h"

#include <algorithm>
#include <string_view>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bigfile.h"
#include "blake3.h" /* 只剩 full 确认用 BLAKE3；采样层见 sampling.h */
#include "fastio.h"
#include "mft.h"
#include "sampling.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/* ---------------- 小工具 ---------------- */

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int is_cancelled(const CdcScanOpts *opts) {
    return opts && opts->cancel && *opts->cancel;
}

/* on_tick 串行化：回调可能非线程安全（如测试的计数器），并行路径下
 * 多个 worker 会同时调它；用 fastio.c 的 cdc_shared_lock() 包住这次调用。 */
static void call_tick(const CdcScanOpts *opts) {
    if (opts && opts->on_tick) {
        cdc_shared_lock();
        opts->on_tick(opts->tick_ctx);
        cdc_shared_unlock();
    }
}

static double now_mono(void) {
#ifdef _WIN32
    return (double)GetTickCount64() / 1000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void blake3_hash_buf(const uint8_t *data, size_t len, uint8_t out[32]) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    if (len) blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, out, 32);
}

static int digest_eq(const uint8_t a[32], const uint8_t b[32]) {
    return memcmp(a, b, 32) == 0;
}

/* ---------------- 盘符 hint（OpenFileById 用；Rust 的 thread_local DRIVE_HINTS） ---------------- */

/* drive hint 缓存：并行阶段多 worker 会同时调这里；g_hints 的读写
 * 用 cdc_shared_lock() 串行化。打开句柄本身在锁外做，拿到后再在
 * 锁内认领（已有缓存时关闭自己新开的，避免句柄泄漏）。 */
static void *drive_hint_for_path(const char *path) {
#ifdef _WIN32
    static void *g_hints[26];
    char ch = path && path[0] ? path[0] : 'C';
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch < 'A' || ch > 'Z') ch = 'C';
    int i = ch - 'A';
    cdc_shared_lock();
    void *h = g_hints[i];
    cdc_shared_unlock();
    if (h) return h;
    void *nh = cdc_open_drive_hint(ch);
    if (!nh) return NULL;
    cdc_shared_lock();
    if (!g_hints[i]) {
        g_hints[i] = nh;
        nh = NULL;
    }
    h = g_hints[i];
    cdc_shared_unlock();
    if (nh) CloseHandle((HANDLE)nh);
    return h;
#else
    (void)path;
    return NULL;
#endif
}

/* frn==0 时不带 hint（与 Rust 的 open_checked_file_with_size 一致）。 */
static CdcFile *open_checked_with_size(const char *path, uint64_t frn, uint32_t flags,
                                       int *has_file_id, CdcFileId *file_id_out,
                                       uint64_t *actual_size_out) {
    void *hint = NULL;
    if (frn != 0) hint = drive_hint_for_path(path);
    return cdc_file_open_checked(path, frn, flags, hint, has_file_id, file_id_out,
                                 actual_size_out);
}

/* ---------------- 摘要原语（对照 main.rs 的 hash_chunk / sample / strong / probe / full） ---------------- */

/* 读 offset 起 length 字节进 XXH3 流式状态（buf 分片）。寻道/读取出错
 * 返回 -1；文件尾短读只摘要读到的部分（与 Rust 的 hash_chunk 一致，
 * 算法换成 XXH3）。 */
static int hash_chunk_xxh(CdcFile *f, uint8_t *buf, size_t buflen, uint64_t offset,
                          uint64_t length, cdc::Xxh3State *st) {
    uint64_t remaining = length;
    uint64_t pos = offset;
    while (remaining > 0) {
        size_t want = remaining < buflen ? (size_t)remaining : buflen;
        size_t got = 0;
        if (cdc_file_read_at(f, pos, buf, want, &got) != 0) return -1;
        if (got == 0) break;
        if (st->update(buf, got) != 0) return -1;
        pos += got;
        remaining -= got;
    }
    return 0;
}

/* 小文件全文采样：整文件顺序读一遍算 XXH3（1MB 缓冲）。
 * sample / strong 的 size <= chunk*3 分支用它，不再走 BLAKE3 全文哈希；
 * 旧版这里返回的是 BLAKE3 全文摘要并被抄给 full，新版 sample 与 full
 * 算法不同，full 留给阶段 3。 */
static int whole_file_xxh_by_frn(const char *path, uint64_t frn, uint64_t size,
                                 uint8_t digest[32], CdcFileId *fid_out,
                                 int *has_fid_out) {
    (void)size;
    int hid = 0;
    CdcFileId id = {0, 0};
    uint64_t actual = 0;
    CdcFile *f = open_checked_with_size(path, frn, CDC_FILE_FLAG_SEQUENTIAL_SCAN,
                                        &hid, &id, &actual);
    if (!f) return -1;
    uint8_t *buf = (uint8_t *)malloc(CDC_SCAN_FULL_BUF_SIZE);
    if (!buf) {
        cdc_file_close(f);
        return -1;
    }
    auto st = cdc::Xxh3State::create();
    if (!st) {
        free(buf);
        cdc_file_close(f);
        return -1;
    }
    int rc = 0;
    for (;;) {
        size_t got = 0;
        if (cdc_file_read_seq(f, buf, CDC_SCAN_FULL_BUF_SIZE, &got) != 0) {
            rc = -1;
            break;
        }
        if (got == 0) break;
        if (st->update(buf, got) != 0) {
            rc = -1;
            break;
        }
    }
    uint64_t xxh = st->finalize();
    free(buf);
    cdc_file_close(f);
    if (rc != 0) return -1;
    cdc::xxh_to_digest32(xxh, digest);
    if (fid_out) *fid_out = id;
    if (has_fid_out) *has_fid_out = hid;
    return 0;
}

/* 整文件顺序哈希（每 16 块检查一次取消，与 Rust 的 full_hash_by_frn
 * 单缓冲分支一致；≥16MB 用 2MB 缓冲，与 Rust 的双缓冲分支同一档位，
 * C 版仍是单缓冲循环，大缓冲减少 read 调用次数）。 */
static int full_hash_by_frn(const char *path, uint64_t frn, uint8_t digest[32],
                            CdcFileId *fid_out, int *has_fid_out,
                            const CdcScanOpts *opts) {
    int hid = 0;
    CdcFileId id = {0, 0};
    uint64_t actual = 0;
    CdcFile *f = open_checked_with_size(path, frn, CDC_FILE_FLAG_SEQUENTIAL_SCAN,
                                        &hid, &id, &actual);
    if (!f) return -1;
    size_t buflen = actual >= CDC_SCAN_FULL_LARGE_THRESHOLD ? CDC_SCAN_FULL_BUF_LARGE
                                                            : CDC_SCAN_FULL_BUF_SIZE;
    uint8_t *buf = (uint8_t *)malloc(buflen);
    if (!buf) {
        cdc_file_close(f);
        return -1;
    }
    blake3_hasher h;
    blake3_hasher_init(&h);
    uint32_t chunks = 0;
    int rc = 0;
    for (;;) {
        if ((chunks & 15) == 0 && is_cancelled(opts)) {
            rc = -1;
            break;
        }
        size_t got = 0;
        if (cdc_file_read_seq(f, buf, buflen, &got) != 0) {
            rc = -1;
            break;
        }
        if (got == 0) break;
        blake3_hasher_update(&h, buf, got);
        chunks++;
    }
    free(buf);
    cdc_file_close(f);
    if (rc != 0) return -1;
    blake3_hasher_finalize(&h, digest, 32);
    if (fid_out) *fid_out = id;
    if (has_fid_out) *has_fid_out = hid;
    return 0;
}

int cdc_full_hash_file(const char *path, uint64_t frn, uint8_t digest[32]) {
    return full_hash_by_frn(path, frn, digest, NULL, NULL, NULL);
}

/* 采样：头、中、尾各取 chunk_len；size <= chunk_len*3 时直接全文 XXH3。
 * digest[32] 是 XXH3-64 载体编码（见 sampling.h），不是 BLAKE3。 */
static int sample_hash_by_frn(const char *path, uint64_t frn, uint64_t size,
                              uint8_t digest[32], CdcFileId *fid_out, int *has_fid_out) {
    if (size <= CDC_SCAN_SAMPLE_CHUNK * 3)
        return whole_file_xxh_by_frn(path, frn, size, digest, fid_out, has_fid_out);
    int hid = 0;
    CdcFileId id = {0, 0};
    uint64_t actual = 0;
    CdcFile *f = open_checked_with_size(path, frn, CDC_FILE_FLAG_RANDOM_ACCESS,
                                        &hid, &id, &actual);
    if (!f) return -1;
    uint8_t *buf = (uint8_t *)malloc((size_t)CDC_SCAN_SAMPLE_CHUNK);
    if (!buf) {
        cdc_file_close(f);
        return -1;
    }
    auto st = cdc::Xxh3State::create();
    if (!st) {
        free(buf);
        cdc_file_close(f);
        return -1;
    }
    int rc = 0;
    if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_SAMPLE_CHUNK, 0, CDC_SCAN_SAMPLE_CHUNK,
                      st.get()) != 0)
        rc = -1;
    else if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_SAMPLE_CHUNK, size / 2,
                            CDC_SCAN_SAMPLE_CHUNK, st.get()) != 0)
        rc = -1;
    else if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_SAMPLE_CHUNK,
                            size - CDC_SCAN_SAMPLE_CHUNK, CDC_SCAN_SAMPLE_CHUNK,
                            st.get()) != 0)
        rc = -1;
    uint64_t xxh = st->finalize();
    free(buf);
    cdc_file_close(f);
    if (rc != 0) return -1;
    cdc::xxh_to_digest32(xxh, digest);
    if (fid_out) *fid_out = id;
    if (has_fid_out) *has_fid_out = hid;
    return 0;
}

/* 强采样（快速模式）：头、中、尾各取 256KB；size <= 768KB 时直接全文 XXH3。
 * digest[32] 是 XXH3-64 载体编码。 */
static int strong_hash_by_frn(const char *path, uint64_t frn, uint64_t size,
                              uint8_t digest[32], CdcFileId *fid_out, int *has_fid_out) {
    if (size <= CDC_SCAN_STRONG_CHUNK * 3)
        return whole_file_xxh_by_frn(path, frn, size, digest, fid_out, has_fid_out);
    int hid = 0;
    CdcFileId id = {0, 0};
    uint64_t actual = 0;
    CdcFile *f = open_checked_with_size(path, frn, CDC_FILE_FLAG_RANDOM_ACCESS,
                                        &hid, &id, &actual);
    if (!f) return -1;
    uint8_t *buf = (uint8_t *)malloc((size_t)CDC_SCAN_STRONG_CHUNK);
    if (!buf) {
        cdc_file_close(f);
        return -1;
    }
    auto st = cdc::Xxh3State::create();
    if (!st) {
        free(buf);
        cdc_file_close(f);
        return -1;
    }
    int rc = 0;
    if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_STRONG_CHUNK, 0, CDC_SCAN_STRONG_CHUNK,
                      st.get()) != 0)
        rc = -1;
    else if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_STRONG_CHUNK, size / 2,
                            CDC_SCAN_STRONG_CHUNK, st.get()) != 0)
        rc = -1;
    else if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_STRONG_CHUNK,
                            size - CDC_SCAN_STRONG_CHUNK, CDC_SCAN_STRONG_CHUNK,
                            st.get()) != 0)
        rc = -1;
    uint64_t xxh = st->finalize();
    free(buf);
    cdc_file_close(f);
    if (rc != 0) return -1;
    cdc::xxh_to_digest32(xxh, digest);
    if (fid_out) *fid_out = id;
    if (has_fid_out) *has_fid_out = hid;
    return 0;
}

/* 大文件渐进探针：25% 与 75% 处各 64KB。digest[32] 是 XXH3-64 载体编码。 */
static int probe_hash_by_frn(const char *path, uint64_t frn, uint64_t size,
                             uint8_t digest[32], CdcFileId *fid_out, int *has_fid_out) {
    int hid = 0;
    CdcFileId id = {0, 0};
    uint64_t actual = 0;
    CdcFile *f = open_checked_with_size(path, frn, CDC_FILE_FLAG_RANDOM_ACCESS,
                                        &hid, &id, &actual);
    if (!f) return -1;
    uint8_t *buf = (uint8_t *)malloc((size_t)CDC_SCAN_PROBE_CHUNK);
    if (!buf) {
        cdc_file_close(f);
        return -1;
    }
    auto st = cdc::Xxh3State::create();
    if (!st) {
        free(buf);
        cdc_file_close(f);
        return -1;
    }
    uint64_t tail = size >= CDC_SCAN_PROBE_CHUNK ? size - CDC_SCAN_PROBE_CHUNK : 0;
    uint64_t off1 = size / 4 < tail ? size / 4 : tail;
    uint64_t off2 = size * 3 / 4 < tail ? size * 3 / 4 : tail;
    uint64_t len = CDC_SCAN_PROBE_CHUNK < size ? CDC_SCAN_PROBE_CHUNK : size;
    int rc = 0;
    if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_PROBE_CHUNK, off1, len, st.get()) != 0) rc = -1;
    else if (hash_chunk_xxh(f, buf, (size_t)CDC_SCAN_PROBE_CHUNK, off2, len, st.get()) != 0)
        rc = -1;
    uint64_t xxh = st->finalize();
    free(buf);
    cdc_file_close(f);
    if (rc != 0) return -1;
    cdc::xxh_to_digest32(xxh, digest);
    if (fid_out) *fid_out = id;
    if (has_fid_out) *has_fid_out = hid;
    return 0;
}

/* 机械盘专用（<= 2MB）：一次顺序读完整文件，同时算出采样与全哈希。 */
static int read_hdd_seq_sample_and_full(const char *path, uint64_t frn, uint64_t size,
                                        uint64_t chunk_len, uint8_t sample[32],
                                        uint8_t full[32], CdcFileId *fid_out,
                                        int *has_fid_out) {
    int hid = 0;
    CdcFileId id = {0, 0};
    uint64_t actual = 0;
    CdcFile *f = open_checked_with_size(path, frn, CDC_FILE_FLAG_SEQUENTIAL_SCAN,
                                        &hid, &id, &actual);
    if (!f) return -1;
    if (actual != size || actual > CDC_SCAN_HDD_SEQ_THRESHOLD) {
        cdc_file_close(f);
        return -1;
    }
    size_t s = (size_t)actual;
    uint8_t *buf = (uint8_t *)malloc(s ? s : 1);
    if (!buf) {
        cdc_file_close(f);
        return -1;
    }
    size_t filled = 0;
    while (filled < s) {
        size_t got = 0;
        if (cdc_file_read_seq(f, buf + filled, s - filled, &got) != 0 || got == 0) break;
        filled += got;
    }
    cdc_file_close(f);
    if (filled != s) {
        free(buf);
        return -1;
    }
    blake3_hash_buf(buf, s, full);
    size_t c = (size_t)chunk_len;
    if (s <= c * 3) {
        /* 小文件：采样即全文 XXH3（与 whole_file_xxh_by_frn 同值）。 */
        cdc::xxh3_buf_to_digest32(buf, s, sample);
    } else {
        auto st = cdc::Xxh3State::create();
        if (!st) {
            free(buf);
            return -1;
        }
        int xrc = 0;
        if (st->update(buf, c) != 0) xrc = -1;
        else if (st->update(buf + s / 2, c) != 0) xrc = -1;
        else if (st->update(buf + s - c, c) != 0) xrc = -1;
        uint64_t xxh = st->finalize();
        if (xrc != 0) {
            free(buf);
            return -1;
        }
        cdc::xxh_to_digest32(xxh, sample);
    }
    free(buf);
    if (fid_out) *fid_out = id;
    if (has_fid_out) *has_fid_out = hid;
    return 0;
}

/* ---------------- 磁盘键（对照 main.rs 的 DiskKey / disk_key_for_path） ---------------- */

typedef enum { CDC_DISK_SSD, CDC_DISK_HDD_PHYSICAL, CDC_DISK_HDD_DRIVE } CdcDiskKind;

typedef struct {
    CdcDiskKind kind;
    uint32_t dev_type, dev_num;
    char letter;
} CdcDiskKey;

typedef struct {
    int valid[26];
    CdcDiskKey keys[26];
} CdcDiskCache;

static int cmp_disk_key(const CdcDiskKey *a, const CdcDiskKey *b) {
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    if (a->dev_type != b->dev_type) return a->dev_type < b->dev_type ? -1 : 1;
    if (a->dev_num != b->dev_num) return a->dev_num < b->dev_num ? -1 : 1;
    if (a->letter != b->letter) return a->letter < b->letter ? -1 : 1;
    return 0;
}

static CdcDiskKey disk_key_for_path(const char *path, CdcDiskCache *cache) {
    char ch = path && path[0] ? path[0] : 'C';
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch < 'A' || ch > 'Z') ch = 'C';
    int i = ch - 'A';
    if (cache && cache->valid[i]) return cache->keys[i];
    CdcDiskKey key;
    memset(&key, 0, sizeof key);
    key.letter = ch;
    char drive[4] = {ch, ':', '\\', '\0'};
    int ssd = cdc_is_ssd(drive);
    if (ssd != 0) {
        /* 1=SSD；-1=未知，按 SSD 处理（与 Rust 的 unwrap_or(true) 一致） */
        key.kind = CDC_DISK_SSD;
    } else {
        uint32_t dt = 0, dn = 0;
        if (cdc_physical_disk_id(drive, &dt, &dn) == 0) {
            key.kind = CDC_DISK_HDD_PHYSICAL;
            key.dev_type = dt;
            key.dev_num = dn;
        } else {
            key.kind = CDC_DISK_HDD_DRIVE;
        }
    }
    if (cache) {
        cache->keys[i] = key;
        cache->valid[i] = 1;
    }
    return key;
}

/* ---------------- 候选（对照 main.rs 的 DupCandidate） ---------------- */

typedef struct {
    char *path; /* owned */
    uint64_t size, mtime, sort_key, frn, first_lcn;
    uint32_t bytes_per_cluster;
    int can_raw_read;
    int is_ssd;
    CdcDiskKey disk;
    int has_file_id;
    CdcFileId file_id;
    cdc_digest_set_t digests;
    int has_probe;
    uint64_t probe_xxh; /* 探针 XXH3，只放内存，不落库（fileindex/hashcache 无此列） */
    int dirty_head_xxh, dirty_sample_xxh, dirty_strong_xxh, dirty_full;
} DupCandidate;

static void cand_free(DupCandidate *c) {
    if (!c) return;
    free(c->path);
    c->path = NULL;
}

static void cand_vec_free_all(cdc_vec_t *v) {
    if (!v) return;
    for (size_t i = 0; i < v->len; i++)
        cand_free(&CDC_VEC_AT(v, DupCandidate, i));
    cdc_vec_free(v);
}

/* 移动：把 src 的内容拷进 dst，src 的 path 置 NULL（所有权转移）。 */
static int cand_push_move(cdc_vec_t *dst, DupCandidate *src) {
    if (cdc_vec_push(dst, src) != CDC_OK) return -1;
    src->path = NULL;
    return 0;
}

/* 把 src 全部移进 dst（失败时剩余的项留在 src 里，调用方负责释放）。 */
static int cand_vec_move_all(cdc_vec_t *dst, cdc_vec_t *src) {
    for (size_t i = 0; i < src->len; i++) {
        if (cand_push_move(dst, &CDC_VEC_AT(src, DupCandidate, i)) != 0) return -1;
    }
    src->len = 0;
    return 0;
}

static int cand_any_digest(const DupCandidate *c) {
    /* 采样层只看 xxh 字段；旧版 BLAKE3 的 has_head/has_sample/has_strong 也算：
     * 从缓存/索引带进来的旧值要能透传进更新（写回时 COALESCE 不覆盖），
     * 即使本轮管线不再用它们分组。 */
    return c->digests.has_head_xxh || c->digests.has_sample_xxh ||
           c->digests.has_strong_xxh || c->digests.has_head ||
           c->digests.has_sample || c->digests.has_strong || c->digests.has_full;
}

/* 调度排序：SSD 在前，同盘按 sort_key，再按 path（确定性顺序）。 */
static int cmp_cand_schedule(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    if (x->is_ssd != y->is_ssd) return x->is_ssd ? -1 : 1;
    int c = cmp_disk_key(&x->disk, &y->disk);
    if (c) return c;
    if (x->sort_key != y->sort_key) return x->sort_key < y->sort_key ? -1 : 1;
    return strcmp(x->path, y->path);
}

static int cmp_cand_by_size(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return strcmp(x->path, y->path);
}

static int cmp_cand_by_head(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->digests.head_xxh != y->digests.head_xxh)
        return x->digests.head_xxh < y->digests.head_xxh ? -1 : 1;
    return strcmp(x->path, y->path);
}

static int cmp_cand_by_sample(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->digests.sample_xxh != y->digests.sample_xxh)
        return x->digests.sample_xxh < y->digests.sample_xxh ? -1 : 1;
    return strcmp(x->path, y->path);
}

static int cmp_cand_by_probe(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->probe_xxh != y->probe_xxh) return x->probe_xxh < y->probe_xxh ? -1 : 1;
    return strcmp(x->path, y->path);
}

static int cmp_cand_by_full(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    int c = memcmp(x->digests.full.bytes, y->digests.full.bytes, 32);
    if (c) return c;
    return strcmp(x->path, y->path);
}

static int cmp_cand_by_strong(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    if (x->digests.strong_xxh != y->digests.strong_xxh)
        return x->digests.strong_xxh < y->digests.strong_xxh ? -1 : 1;
    return strcmp(x->path, y->path);
}

/* 一组内硬链接去重：保留每个 file_id 的第一次出现；file_id 未知的保留。
 * 原地压缩，返回去重后的个数；尾部槽位已失效（path 为 NULL 或已释放），
 * 调用方只按返回的个数使用前半段。 */
static size_t dedup_hardlinks(DupCandidate *items, size_t n) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        int drop = 0;
        if (items[i].has_file_id) {
            for (size_t j = 0; j < w; j++) {
                if (items[j].has_file_id &&
                    items[j].file_id.volume_serial == items[i].file_id.volume_serial &&
                    items[j].file_id.file_index == items[i].file_id.file_index) {
                    drop = 1;
                    break;
                }
            }
        }
        if (drop) {
            cand_free(&items[i]);
            continue;
        }
        if (w != i) {
            items[w] = items[i];
            items[i].path = NULL;
        }
        w++;
    }
    return w;
}

static void flush_candidate_hashes(cdc_hashcache_t *cache, const DupCandidate *items,
                                   size_t n, uint64_t now) {
    if (!cache) return;
    for (size_t i = 0; i < n; i++) {
        const DupCandidate *c = &items[i];
        if (!(c->dirty_head_xxh || c->dirty_sample_xxh || c->dirty_strong_xxh ||
              c->dirty_full))
            continue;
        cdc_digest_set_t sub;
        memset(&sub, 0, sizeof sub);
        /* 只写 xxh 子字段 + full：旧版 BLAKE3 的 head/sample/strong
         * 不再由管线产生，不写（缓存里已有的旧值靠 COALESCE 保留）。 */
        if (c->dirty_head_xxh) {
            sub.has_head_xxh = 1;
            sub.head_xxh = c->digests.head_xxh;
        }
        if (c->dirty_sample_xxh) {
            sub.has_sample_xxh = 1;
            sub.sample_xxh = c->digests.sample_xxh;
        }
        if (c->dirty_strong_xxh) {
            sub.has_strong_xxh = 1;
            sub.strong_xxh = c->digests.strong_xxh;
        }
        if (c->dirty_full) {
            sub.has_full = 1;
            sub.full = c->digests.full;
        }
        cdc_hashcache_remember(cache, c->path, c->size, c->mtime, &sub, now);
    }
}

/* ---------------- 并行阶段（cdc_par_for） ---------------- */
/* 各阶段原来是顺序循环：每项独立（打开自己的文件、算自己的摘要、
 * 只写自己的候选字段），天然可并行。把循环体抽成 work 函数，
 * 用 cdc_par_for 按下标分给 worker；结果与顺序版一致的原因：
 *   - 每个候选只被一个 worker 处理，digests/dirty/file_id 无共享写；
 *   - 阶段结束后分组前都按 (size, 摘要) 排序，与计算顺序无关；
 *   - on_tick 由 call_tick 内部的 cdc_shared_lock() 串行化；
 *   - 取消语义一致：worker 入口先查 cancel（不 tick），循环结束
 *     后调用方照常按 is_cancelled(opts) 分流。
 * worker 数按本阶段候选是否全在 SSD 上分档（cdc_par_hash_workers）。
 * 阶段摘要函数的签名不改（xxHash 采样层在另一分支里动它们内部）。
 */

typedef struct {
    cdc_vec_t *vec;
    const CdcScanOpts *opts;
} StageParCtx;

static int stage_vec_all_ssd(const cdc_vec_t *v) {
    for (size_t i = 0; i < v->len; i++) {
        if (!CDC_VEC_AT(v, DupCandidate, i).is_ssd) return 0;
    }
    return 1;
}

/* 快速模式 strong：循环体与改前逐行一致（break/continue 改 return）。 */
static void stage_strong_work(void *ctx, size_t i) {
    StageParCtx *c = (StageParCtx *)ctx;
    DupCandidate *cand = &CDC_VEC_AT(c->vec, DupCandidate, i);
    const CdcScanOpts *opts = c->opts;
    if (is_cancelled(opts)) return;
    if (cand->digests.has_strong_xxh) return;
    if (!cand->is_ssd && cand->size <= CDC_SCAN_HDD_SEQ_THRESHOLD) {
        uint8_t strong[32], full[32];
        CdcFileId fid = {0, 0};
        int hid = 0;
        if (read_hdd_seq_sample_and_full(cand->path, cand->frn, cand->size,
                                         CDC_SCAN_STRONG_CHUNK, strong, full,
                                         &fid, &hid) == 0) {
            if (!cand->has_file_id && hid) {
                cand->file_id = fid;
                cand->has_file_id = 1;
            }
            cand->digests.strong_xxh = cdc::xxh_from_digest32(strong);
            cand->digests.has_strong_xxh = 1;
            memcpy(cand->digests.full.bytes, full, 32);
            cand->digests.has_full = 1;
            cand->dirty_strong_xxh = 1;
            cand->dirty_full = 1;
        }
    } else {
        uint8_t digest[32];
        CdcFileId fid = {0, 0};
        int hid = 0;
        if (strong_hash_by_frn(cand->path, cand->frn, cand->size, digest, &fid, &hid) ==
            0) {
            if (!cand->has_file_id && hid) {
                cand->file_id = fid;
                cand->has_file_id = 1;
            }
            cand->digests.strong_xxh = cdc::xxh_from_digest32(digest);
            cand->digests.has_strong_xxh = 1;
            cand->dirty_strong_xxh = 1;
            /* 注意：strong 是 XXH3，full 是 BLAKE3，算法不同，
             * 不能再像旧版那样把 strong 抄给 full；小文件的
             * full 在 fast 模式下就缺着，下次常规扫描再补。 */
        }
    }
    call_tick(opts);
}

/* 常规阶段 2 sample：循环体与改前逐行一致。 */
static void stage_sample_work(void *ctx, size_t i) {
    StageParCtx *c = (StageParCtx *)ctx;
    DupCandidate *cand = &CDC_VEC_AT(c->vec, DupCandidate, i);
    const CdcScanOpts *opts = c->opts;
    if (is_cancelled(opts)) return;
    if (cand->digests.has_sample_xxh || cand->digests.has_full) return;
    if (!cand->is_ssd && cand->size <= CDC_SCAN_HDD_SEQ_THRESHOLD) {
        uint8_t sample[32], full[32];
        CdcFileId fid = {0, 0};
        int hid = 0;
        if (read_hdd_seq_sample_and_full(cand->path, cand->frn, cand->size,
                                         CDC_SCAN_SAMPLE_CHUNK, sample, full,
                                         &fid, &hid) == 0) {
            if (!cand->has_file_id && hid) {
                cand->file_id = fid;
                cand->has_file_id = 1;
            }
            cand->digests.sample_xxh = cdc::xxh_from_digest32(sample);
            cand->digests.has_sample_xxh = 1;
            memcpy(cand->digests.full.bytes, full, 32);
            cand->digests.has_full = 1;
            cand->dirty_sample_xxh = 1;
            cand->dirty_full = 1;
        }
    } else {
        uint8_t digest[32];
        CdcFileId fid = {0, 0};
        int hid = 0;
        if (sample_hash_by_frn(cand->path, cand->frn, cand->size, digest, &fid, &hid) ==
            0) {
            if (!cand->has_file_id && hid) {
                cand->file_id = fid;
                cand->has_file_id = 1;
            }
            cand->digests.sample_xxh = cdc::xxh_from_digest32(digest);
            cand->digests.has_sample_xxh = 1;
            cand->dirty_sample_xxh = 1;
            /* 注意：sample 是 XXH3，full 是 BLAKE3，不能再像旧版
             * 那样把 sample 抄给 full；小文件的 full 留给阶段 3。 */
        }
    }
    call_tick(opts);
}

/* 探针：循环体与改前逐行一致。 */
static void stage_probe_work(void *ctx, size_t i) {
    StageParCtx *c = (StageParCtx *)ctx;
    DupCandidate *cand = &CDC_VEC_AT(c->vec, DupCandidate, i);
    const CdcScanOpts *opts = c->opts;
    if (is_cancelled(opts)) return;
    uint8_t digest[32];
    CdcFileId fid = {0, 0};
    int hid = 0;
    if (probe_hash_by_frn(cand->path, cand->frn, cand->size, digest, &fid, &hid) == 0) {
        if (!cand->has_file_id && hid) {
            cand->file_id = fid;
            cand->has_file_id = 1;
        }
        cand->probe_xxh = cdc::xxh_from_digest32(digest);
        cand->has_probe = 1;
    }
    call_tick(opts);
}

/* 阶段 3 全哈希：循环体与改前逐行一致。 */
static void stage_full_work(void *ctx, size_t i) {
    StageParCtx *c = (StageParCtx *)ctx;
    DupCandidate *cand = &CDC_VEC_AT(c->vec, DupCandidate, i);
    const CdcScanOpts *opts = c->opts;
    if (is_cancelled(opts)) return;
    if (cand->digests.has_full) return;
    uint8_t digest[32];
    CdcFileId fid = {0, 0};
    int hid = 0;
    if (full_hash_by_frn(cand->path, cand->frn, digest, &fid, &hid, opts) == 0) {
        if (!cand->has_file_id && hid) {
            cand->file_id = fid;
            cand->has_file_id = 1;
        }
        memcpy(cand->digests.full.bytes, digest, 32);
        cand->digests.has_full = 1;
        cand->dirty_full = 1;
    }
    call_tick(opts);
}

/* ---------------- 管线（对照 main.rs 的 run_dup_pipeline） ---------------- */

/* 消费 items[0..n)（元素或被移进 out_final/out_elim，或被释放；数组外壳
 * 由本函数 free）。返回 0 成功，-1 内存失败（输出 vec 可能含部分结果，
 * 调用方按失败处理并释放）。 */
static int run_dup_pipeline(DupCandidate *items, size_t n, const char *label,
                            const CdcScanOpts *opts, cdc_vec_t *out_final,
                            cdc_vec_t *out_elim) {
    CdcScanOpts def_opts;
    if (!opts) {
        memset(&def_opts, 0, sizeof def_opts);
        opts = &def_opts;
    }
    int rc = -1;
    cdc_vec_t stage2, stage3, direct, need_probe, probed;
    memset(&stage2, 0, sizeof stage2);
    memset(&stage3, 0, sizeof stage3);
    memset(&direct, 0, sizeof direct);
    memset(&need_probe, 0, sizeof need_probe);
    memset(&probed, 0, sizeof probed);
    if (cdc_vec_init(&stage2, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&stage3, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&direct, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&need_probe, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&probed, sizeof(DupCandidate)) != CDC_OK) {
        goto done;
    }
    if (n == 0) {
        rc = 0;
        goto done;
    }
    /* 调度顺序：SSD 在前，同盘按 sort_key。 */
    qsort(items, n, sizeof(DupCandidate), cmp_cand_schedule);

    /* ---------- 阶段 1：head 4KB ---------- */
    {
        /* 已有 head 的先 tick（与 Rust 的第一遍循环一致）。 */
        for (size_t i = 0; i < n; i++) {
            if (items[i].digests.has_head_xxh) call_tick(opts);
        }
        if (!is_cancelled(opts)) {
            /* 卷句柄 LCN 合并直读：按盘符分组，同盘一次调用（内部按偏移合并读）。 */
            size_t n_raw = 0;
            for (size_t i = 0; i < n; i++) {
                if (!items[i].digests.has_head_xxh && items[i].can_raw_read &&
                    items[i].bytes_per_cluster > 0 && items[i].first_lcn > 0)
                    n_raw++;
            }
            if (n_raw > 0) {
                typedef struct {
                    char letter;
                    CdcRawHeadTarget target;
                } RawPair;
                RawPair *pairs = (RawPair *)malloc(n_raw * sizeof(RawPair));
                CdcRawHeadTarget *targets = (CdcRawHeadTarget *)malloc(n_raw * sizeof(CdcRawHeadTarget));
                CdcHeadResult *results = (CdcHeadResult *)malloc(n_raw * sizeof(CdcHeadResult));
                if (pairs && targets && results) {
                    size_t t = 0;
                    for (size_t i = 0; i < n; i++) {
                        if (!items[i].digests.has_head_xxh && items[i].can_raw_read &&
                            items[i].bytes_per_cluster > 0 && items[i].first_lcn > 0) {
                            char ch = items[i].path && items[i].path[0] ? items[i].path[0] : 'C';
                            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                            if (ch < 'A' || ch > 'Z') ch = 'C';
                            pairs[t].letter = ch;
                            pairs[t].target.idx = i;
                            pairs[t].target.offset =
                                items[i].first_lcn * items[i].bytes_per_cluster;
                            pairs[t].target.want = items[i].size < CDC_SCAN_HEAD_CHUNK
                                                      ? (size_t)items[i].size
                                                      : (size_t)CDC_SCAN_HEAD_CHUNK;
                            t++;
                        }
                    }
                    /* 按（盘符，偏移）排，同盘符的连续成组（选择排序即可，
                     * 直读目标数通常不大）。 */
                    for (size_t a = 0; a < t; a++) {
                        for (size_t b = a + 1; b < t; b++) {
                            int swap = 0;
                            if (pairs[b].letter < pairs[a].letter) swap = 1;
                            else if (pairs[b].letter == pairs[a].letter &&
                                     pairs[b].target.offset < pairs[a].target.offset)
                                swap = 1;
                            if (swap) {
                                RawPair tmp = pairs[a];
                                pairs[a] = pairs[b];
                                pairs[b] = tmp;
                            }
                        }
                    }
                    for (size_t k = 0; k < t; k++) targets[k] = pairs[k].target;
                    size_t a = 0;
                    while (a < t) {
                        size_t b = a + 1;
                        while (b < t && pairs[b].letter == pairs[a].letter) b++;
                        /* [a, b) 同盘符：is_ssd 取第一项（同盘一致）。 */
                        int ssd = items[targets[a].idx].is_ssd;
                        size_t got = cdc_coalesced_raw_volume_heads(
                            pairs[a].letter, &targets[a], b - a, ssd,
                            opts ? opts->cancel : NULL, opts ? opts->on_tick : NULL,
                            opts ? opts->tick_ctx : NULL, results);
                        for (size_t k = 0; k < got; k++) {
                            DupCandidate *c = &items[results[k].idx];
                            c->digests.head_xxh =
                                cdc::xxh_from_digest32(results[k].digest);
                            c->digests.has_head_xxh = 1;
                            c->dirty_head_xxh = 1;
                            if (c->size <= CDC_SCAN_HEAD_CHUNK) {
                                /* head 已覆盖全文：sample/strong 的 XXH3
                                 * 值与 head 相同（小文件上采样即全文）；
                                 * full 是 BLAKE3，不能从 xxHash 派生，
                                 * 留给阶段 3 再算。 */
                                c->digests.sample_xxh = c->digests.head_xxh;
                                c->digests.has_sample_xxh = 1;
                                c->digests.strong_xxh = c->digests.head_xxh;
                                c->digests.has_strong_xxh = 1;
                                c->dirty_sample_xxh = 1;
                                c->dirty_strong_xxh = 1;
                            }
                        }
                        a = b;
                    }
                }
                free(pairs);
                free(targets);
                free(results);
            }
        }
        /* IOCP 回退：SSD 一批（并发 32），机械盘按盘分组排序后各一批（并发 8）。 */
        if (!is_cancelled(opts)) {
            size_t n_ssd = 0, n_hdd = 0;
            for (size_t i = 0; i < n; i++) {
                if (!items[i].digests.has_head_xxh) {
                    if (items[i].is_ssd) n_ssd++;
                    else n_hdd++;
                }
            }
            if (n_ssd > 0 || n_hdd > 0) {
                CdcIocpRequest *reqs = (CdcIocpRequest *)malloc((n_ssd + n_hdd) * sizeof(CdcIocpRequest));
                CdcIocpResult *res = (CdcIocpResult *)malloc((n_ssd + n_hdd) * sizeof(CdcIocpResult));
                if (reqs && res) {
                    size_t s = 0, h = 0;
                    /* SSD 在前（调度序已排好：SSD 段在前）。 */
                    for (size_t i = 0; i < n; i++) {
                        if (items[i].digests.has_head_xxh) continue;
                        CdcIocpRequest *r = &reqs[items[i].is_ssd ? s++ : n_ssd + (h++)];
                        r->idx = i;
                        r->path = items[i].path;
                        r->frn = items[i].frn;
                        r->want = items[i].size < CDC_SCAN_HEAD_CHUNK
                                      ? (size_t)items[i].size
                                      : (size_t)CDC_SCAN_HEAD_CHUNK;
                    }
                    /* 机械盘段已按 (disk, sort_key, path) 排好（输入是调度序），
                     * 下面直接按 disk 分组调用，不用再排。 */
                    size_t n_res = 0;
                    if (n_ssd > 0) {
                        /* SSD 批 64 在途（NVMe 放宽；fastio.c 的槽上限已同步到 128）。 */
                        n_res += cdc_iocp_read_heads(
                            reqs, n_ssd, 64, opts ? opts->cancel : NULL,
                            opts ? opts->on_tick : NULL, opts ? opts->tick_ctx : NULL, res);
                    }
                    /* 机械盘按 disk 分组调用。 */
                    size_t hpos = 0;
                    while (hpos < n_hdd) {
                        size_t hend = hpos + 1;
                        while (hend < n_hdd) {
                            DupCandidate *ca = &items[reqs[n_ssd + hpos].idx];
                            DupCandidate *cb = &items[reqs[n_ssd + hend].idx];
                            if (cmp_disk_key(&ca->disk, &cb->disk) != 0) break;
                            hend++;
                        }
                        n_res += cdc_iocp_read_heads(
                            &reqs[n_ssd + hpos], hend - hpos, 8,
                            opts ? opts->cancel : NULL, opts ? opts->on_tick : NULL,
                            opts ? opts->tick_ctx : NULL, &res[n_res]);
                        hpos = hend;
                    }
                    for (size_t k = 0; k < n_res; k++) {
                        DupCandidate *c = &items[res[k].idx];
                        if (!c->has_file_id && res[k].has_file_id) {
                            c->file_id = res[k].file_id;
                            c->has_file_id = 1;
                        }
                        c->digests.head_xxh = cdc::xxh_from_digest32(res[k].digest);
                        c->digests.has_head_xxh = 1;
                        c->dirty_head_xxh = 1;
                        if (c->size <= CDC_SCAN_HEAD_CHUNK) {
                            /* 同上：sample/strong 与 head 同值，full 留给阶段 3。 */
                            c->digests.sample_xxh = c->digests.head_xxh;
                            c->digests.has_sample_xxh = 1;
                            c->digests.strong_xxh = c->digests.head_xxh;
                            c->digests.has_strong_xxh = 1;
                            c->dirty_sample_xxh = 1;
                            c->dirty_strong_xxh = 1;
                        }
                    }
                }
                free(reqs);
                free(res);
            }
        }
    }

    if (is_cancelled(opts)) {
        /* 取消：final 为空，eliminated = 全部 stage1（摘要照常回写缓存）。 */
        for (size_t i = 0; i < n; i++) {
            if (cand_push_move(out_elim, &items[i]) != 0) goto done;
        }
        rc = 0;
        goto done;
    }

    /* 按 (size, head) 分组：有 head 的才参与；组内硬链接去重；≥2 进阶段 2，
     * 单例进 eliminated；没 head 的直接丢弃（与 Rust 一致，不进 eliminated）。 */
    {
        qsort(items, n, sizeof(DupCandidate), cmp_cand_by_head);
        size_t i = 0;
        while (i < n) {
            if (!items[i].digests.has_head_xxh) {
                cand_free(&items[i]);
                i++;
                continue;
            }
            size_t j = i + 1;
            while (j < n && items[j].size == items[i].size &&
                   items[j].digests.has_head_xxh &&
                   items[j].digests.head_xxh == items[i].digests.head_xxh)
                j++;
            size_t kept = dedup_hardlinks(&items[i], j - i);
            cdc_vec_t *dst = kept >= 2 ? &stage2 : out_elim;
            for (size_t k = i; k < i + kept; k++) {
                if (cand_push_move(dst, &items[k]) != 0) goto done;
            }
            /* 组内被去重的已释放；剩余尾部槽位已失效，不管。 */
            i = j;
        }
    }

    if (opts && opts->trace)
        fprintf(stderr, "dup[%s]: 4KB 初筛后剩余 %zu 个文件\n", label, stage2.len);
    if (stage2.len == 0) {
        rc = 0;
        goto done;
    }

    if (opts->fast) {
        /* ---------- 快速模式：strong 即终局 ---------- */
        qsort(stage2.data, stage2.len, sizeof(DupCandidate), cmp_cand_schedule);
        if (stage2.len > 0) {
            StageParCtx pctx;
            pctx.vec = &stage2;
            pctx.opts = opts;
            cdc_par_for(stage2.len,
                        cdc_par_hash_workers(stage_vec_all_ssd(&stage2)),
                        stage_strong_work, &pctx);
        }
        if (is_cancelled(opts)) {
            if (cand_vec_move_all(out_elim, &stage2) != 0) goto done;
            rc = 0;
            goto done;
        }
        if (cand_vec_move_all(out_final, &stage2) != 0) goto done;
        rc = 0;
        goto done;
    }

    /* ---------- 阶段 2（常规）：sample 16KB×3 ---------- */
    qsort(stage2.data, stage2.len, sizeof(DupCandidate), cmp_cand_schedule);
    if (stage2.len > 0) {
        StageParCtx pctx;
        pctx.vec = &stage2;
        pctx.opts = opts;
        cdc_par_for(stage2.len, cdc_par_hash_workers(stage_vec_all_ssd(&stage2)),
                    stage_sample_work, &pctx);
    }
    if (is_cancelled(opts)) {
        if (cand_vec_move_all(out_elim, &stage2) != 0) goto done;
        rc = 0;
        goto done;
    }

    /* 按 (size, sample) 分组：有 sample 的分组；sample 缺但 full 有的直进阶段 3；
     * 两者皆无的丢弃。 */
    {
        for (size_t i = 0; i < stage2.len; i++) {
            DupCandidate *c = &CDC_VEC_AT(&stage2, DupCandidate, i);
            if (!c->digests.has_sample_xxh && c->digests.has_full) {
                if (cand_push_move(&stage3, c) != 0) goto done;
            } else if (!c->digests.has_sample_xxh) {
                cand_free(c);
            }
            /* 有 sample 的留在 stage2 里，下面排序分组。 */
        }
        /* 把已移走/丢弃的槽位压缩掉：stage2 里只剩有 sample 的项。 */
        size_t w = 0;
        for (size_t i = 0; i < stage2.len; i++) {
            DupCandidate *c = &CDC_VEC_AT(&stage2, DupCandidate, i);
            if (c->path) {
                if (w != i) {
                    CDC_VEC_AT(&stage2, DupCandidate, w) = *c;
                    c->path = NULL;
                }
                w++;
            }
        }
        stage2.len = w;
        if (stage2.len > 0)
            qsort(stage2.data, stage2.len, sizeof(DupCandidate), cmp_cand_by_sample);
        size_t i = 0;
        while (i < stage2.len) {
            size_t j = i + 1;
            while (j < stage2.len &&
                   CDC_VEC_AT(&stage2, DupCandidate, j).size ==
                       CDC_VEC_AT(&stage2, DupCandidate, i).size &&
                   CDC_VEC_AT(&stage2, DupCandidate, j).digests.sample_xxh ==
                       CDC_VEC_AT(&stage2, DupCandidate, i).digests.sample_xxh)
                j++;
            cdc_vec_t *dst = (j - i) >= 2 ? &stage3 : out_elim;
            for (size_t k = i; k < j; k++) {
                if (cand_push_move(dst, &CDC_VEC_AT(&stage2, DupCandidate, k)) != 0)
                    goto done;
            }
            i = j;
        }
    }

    if (opts && opts->trace)
        fprintf(stderr, "dup[%s]: 采样后仍需全校验 %zu 个文件\n", label, stage3.len);
    if (stage3.len == 0 || getenv("CDC_DUP_SAMPLE_ONLY")) {
        if (cand_vec_move_all(out_elim, &stage3) != 0) goto done;
        rc = 0;
        goto done;
    }

    /* ---------- 阶段 3 前置：>16MB 探针 ---------- */
    {
        for (size_t i = 0; i < stage3.len; i++) {
            DupCandidate *c = &CDC_VEC_AT(&stage3, DupCandidate, i);
            if (c->size > CDC_SCAN_PROBE_THRESHOLD && c->digests.has_sample_xxh) {
                if (cand_push_move(&need_probe, c) != 0) goto done;
            } else {
                if (cand_push_move(&direct, c) != 0) goto done;
            }
        }
        stage3.len = 0;
        /* need_probe 按 (size, sample) 分组：组 ≥2 且全员无 full 才探针，
         * 其余直进阶段 3。 */
        if (need_probe.len > 0)
            qsort(need_probe.data, need_probe.len, sizeof(DupCandidate),
                  cmp_cand_by_sample);
        size_t i = 0;
        while (i < need_probe.len) {
            size_t j = i + 1;
            while (j < need_probe.len &&
                   CDC_VEC_AT(&need_probe, DupCandidate, j).size ==
                       CDC_VEC_AT(&need_probe, DupCandidate, i).size &&
                   CDC_VEC_AT(&need_probe, DupCandidate, j).digests.sample_xxh ==
                       CDC_VEC_AT(&need_probe, DupCandidate, i).digests.sample_xxh)
                j++;
            int all_no_full = 1;
            for (size_t k = i; k < j; k++) {
                if (CDC_VEC_AT(&need_probe, DupCandidate, k).digests.has_full) {
                    all_no_full = 0;
                    break;
                }
            }
            cdc_vec_t *dst = ((j - i) >= 2 && all_no_full) ? &probed : &direct;
            for (size_t k = i; k < j; k++) {
                if (cand_push_move(dst, &CDC_VEC_AT(&need_probe, DupCandidate, k)) != 0)
                    goto done;
            }
            i = j;
        }
        need_probe.len = 0;
        /* 注意：probed 在这里先装"待探针"的项，探完后才按 probe 分组；
         * 下面复用 probed 装探完的结果（direct 里的项不动）。 */
        if (probed.len > 0 && !is_cancelled(opts)) {
            qsort(probed.data, probed.len, sizeof(DupCandidate), cmp_cand_schedule);
            {
                StageParCtx pctx;
                pctx.vec = &probed;
                pctx.opts = opts;
                cdc_par_for(probed.len,
                            cdc_par_hash_workers(stage_vec_all_ssd(&probed)),
                            stage_probe_work, &pctx);
            }
            if (is_cancelled(opts)) {
                if (cand_vec_move_all(out_elim, &direct) != 0) goto done;
                if (cand_vec_move_all(out_elim, &probed) != 0) goto done;
                rc = 0;
                goto done;
            }
            /* 按 (size, probe) 分组：无 probe 的直进；组 ≥2 直进；单例淘汰。 */
            qsort(probed.data, probed.len, sizeof(DupCandidate), cmp_cand_by_probe);
            size_t p = 0;
            while (p < probed.len) {
                DupCandidate *c = &CDC_VEC_AT(&probed, DupCandidate, p);
                if (!c->has_probe) {
                    if (cand_push_move(&direct, c) != 0) goto done;
                    p++;
                    continue;
                }
                size_t q = p + 1;
                while (q < probed.len &&
                       CDC_VEC_AT(&probed, DupCandidate, q).has_probe &&
                       CDC_VEC_AT(&probed, DupCandidate, q).size == c->size &&
                       CDC_VEC_AT(&probed, DupCandidate, q).probe_xxh == c->probe_xxh)
                    q++;
                cdc_vec_t *dst = (q - p) >= 2 ? &direct : out_elim;
                for (size_t k = p; k < q; k++) {
                    if (cand_push_move(dst, &CDC_VEC_AT(&probed, DupCandidate, k)) != 0)
                        goto done;
                }
                p = q;
            }
            probed.len = 0;
        }
        /* 探针前已取消：need_probe 里待探的项按 Rust 版直接丢弃
         * （既不在 direct 也不在 eliminated）。 */
    }

    if (direct.len == 0) {
        rc = 0;
        goto done;
    }

    /* ---------- 阶段 3：全文件哈希 ---------- */
    qsort(direct.data, direct.len, sizeof(DupCandidate), cmp_cand_schedule);
    if (direct.len > 0) {
        StageParCtx pctx;
        pctx.vec = &direct;
        pctx.opts = opts;
        cdc_par_for(direct.len, cdc_par_hash_workers(stage_vec_all_ssd(&direct)),
                    stage_full_work, &pctx);
    }
    if (is_cancelled(opts)) {
        if (cand_vec_move_all(out_elim, &direct) != 0) goto done;
        rc = 0;
        goto done;
    }
    if (cand_vec_move_all(out_final, &direct) != 0) goto done;
    rc = 0;

done:
    /* 收尾：各阶段 vec 里残留的项（取消/失败路径）全部释放；
     * 已移进 out_final/out_elim 的项不再属于这里。 */
    cand_vec_free_all(&stage2);
    cand_vec_free_all(&stage3);
    cand_vec_free_all(&direct);
    cand_vec_free_all(&need_probe);
    cand_vec_free_all(&probed);
    /* 输入数组外壳：元素或已移走（path 为 NULL）或已释放，这里只 free 外壳。 */
    for (size_t i = 0; i < n; i++) cand_free(&items[i]);
    free(items);
    return rc;
}

/* ---------------- 分流与双路（对照 main.rs 的 split_dup_candidates / run_dup_pipelines） ---------------- */

/* 公共结构体的裸数组扩容：经 cdc_vec_reserve 扩容（策略在 util.c）。 */
static int list_grow_raw(void **data, size_t *cap, size_t elem_size, size_t need_len) {
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

static int big_file_list_push(CdcBigFileList *list, CdcBigFile e) {
    if (list->n == list->cap) {
        void *data = list->items;
        if (list_grow_raw(&data, &list->cap, sizeof(CdcBigFile), list->n) != 0)
            return -1;
        list->items = (CdcBigFile *)data;
    }
    list->items[list->n++] = e;
    return 0;
}

static int dup_group_list_push(CdcDupGroupList *list, CdcDupGroup g) {
    if (list->n == list->cap) {
        void *data = list->items;
        if (list_grow_raw(&data, &list->cap, sizeof(CdcDupGroup), list->n) != 0)
            return -1;
        list->items = (CdcDupGroup *)data;
    }
    list->items[list->n++] = g;
    return 0;
}

/* 输入 cands[0..n) 按 size 排好；本函数消费它（元素或被移进三路 vec，
 * 或被释放；数组外壳由调用方 free）。返回 0 成功，-1 内存失败。 */
static int split_dup_candidates(DupCandidate *cands, size_t n, int fast,
                                cdc_vec_t *ready, cdc_vec_t *ssd_stage1,
                                cdc_vec_t *mixed_stage1) {
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && cands[j].size == cands[i].size) j++;
        if (j - i < 2) {
            for (size_t k = i; k < j; k++) cand_free(&cands[k]);
            i = j;
            continue;
        }
        int all_ready = 1, all_ssd = 1;
        for (size_t k = i; k < j; k++) {
            int has_final =
                fast ? cands[k].digests.has_strong_xxh : cands[k].digests.has_full;
            if (!has_final) all_ready = 0;
            if (!cands[k].is_ssd) all_ssd = 0;
        }
        cdc_vec_t *dst = all_ready ? ready : (all_ssd ? ssd_stage1 : mixed_stage1);
        for (size_t k = i; k < j; k++) {
            if (cand_push_move(dst, &cands[k]) != 0) {
                for (size_t m = k; m < n; m++) cand_free(&cands[m]);
                return -1;
            }
        }
        i = j;
    }
    return 0;
}

/* SSD 与混合两路顺序跑完，摘要回写缓存，返回（最终组候选，淘汰候选）。
 * ssd_stage1 / mixed_stage1 是调用方所有权转移进来的 vec（本函数消费，
 * 之后不再有效）；out_final / out_elim 由调用方初始化好的空 vec。 */
static int run_dup_pipelines(cdc_vec_t *ssd_stage1, cdc_vec_t *mixed_stage1,
                             cdc_hashcache_t *cache, uint64_t now,
                             const CdcScanOpts *opts, const char *ssd_label,
                             const char *mixed_label, cdc_vec_t *out_final,
                             cdc_vec_t *out_elim) {
    if (ssd_stage1->len == 0 && mixed_stage1->len == 0) return 0;
    cdc_vec_t ssd_final, ssd_elim, mixed_final, mixed_elim;
    memset(&ssd_final, 0, sizeof ssd_final);
    memset(&ssd_elim, 0, sizeof ssd_elim);
    memset(&mixed_final, 0, sizeof mixed_final);
    memset(&mixed_elim, 0, sizeof mixed_elim);
    if (cdc_vec_init(&ssd_final, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&ssd_elim, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&mixed_final, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&mixed_elim, sizeof(DupCandidate)) != CDC_OK) {
        cand_vec_free_all(&ssd_final);
        cand_vec_free_all(&ssd_elim);
        cand_vec_free_all(&mixed_final);
        cand_vec_free_all(&mixed_elim);
        cand_vec_free_all(ssd_stage1);
        cand_vec_free_all(mixed_stage1);
        return -1;
    }
    DupCandidate *sarr = (DupCandidate *)ssd_stage1->data;
    size_t sn = ssd_stage1->len;
    ssd_stage1->data = NULL;
    ssd_stage1->len = ssd_stage1->cap = 0;
    DupCandidate *marr = (DupCandidate *)mixed_stage1->data;
    size_t mn = mixed_stage1->len;
    mixed_stage1->data = NULL;
    mixed_stage1->len = mixed_stage1->cap = 0;
    int rc = 0;
    if (run_dup_pipeline(sarr, sn, ssd_label, opts, &ssd_final, &ssd_elim) != 0) rc = -1;
    if (run_dup_pipeline(marr, mn, mixed_label, opts, &mixed_final, &mixed_elim) != 0)
        rc = -1;
    if (rc == 0) {
        flush_candidate_hashes(cache, (const DupCandidate *)ssd_elim.data, ssd_elim.len, now);
        flush_candidate_hashes(cache, (const DupCandidate *)ssd_final.data, ssd_final.len, now);
        flush_candidate_hashes(cache, (const DupCandidate *)mixed_elim.data, mixed_elim.len, now);
        flush_candidate_hashes(cache, (const DupCandidate *)mixed_final.data, mixed_final.len, now);
        if (cache) cdc_hashcache_save_dirty(cache);
        if (cand_vec_move_all(out_final, &ssd_final) != 0) rc = -1;
        else if (cand_vec_move_all(out_final, &mixed_final) != 0) rc = -1;
        else if (cand_vec_move_all(out_elim, &ssd_elim) != 0) rc = -1;
        else if (cand_vec_move_all(out_elim, &mixed_elim) != 0) rc = -1;
    }
    cand_vec_free_all(&ssd_final);
    cand_vec_free_all(&ssd_elim);
    cand_vec_free_all(&mixed_final);
    cand_vec_free_all(&mixed_elim);
    return rc;
}

/* ---------------- 建组（对照 main.rs 的 build_dup_groups / sort_dup_groups） ---------------- */

/* 终局摘要相等比较：fast 模式比 strong_xxh（uint64），常规比 full（32 字节）。 */
static int final_digest_eq(const DupCandidate *a, const DupCandidate *b, int fast) {
    if (fast) return a->digests.strong_xxh == b->digests.strong_xxh;
    return digest_eq(a->digests.full.bytes, b->digests.full.bytes);
}

/* 终局摘要写组 key：fast 模式为 strong_xxh 的 16 位 hex，
 * 常规为 full 的 64 位 hex；hash_hex[65] 都装得下。 */
static void final_digest_hex(const DupCandidate *c, int fast, char out[65]) {
    if (fast) {
        char tmp[17];
        cdc::xxh_to_hex(c->digests.strong_xxh, tmp);
        strcpy(out, tmp);
    } else {
        cdc_digest_to_hex(&c->digests.full, out);
    }
}

static int cmp_cand_by_path(const void *a, const void *b) {
    const DupCandidate *x = (const DupCandidate *)a, *y = (const DupCandidate *)b;
    return strcmp(x->path, y->path);
}

static int cmp_dup_group(const void *a, const void *b) {
    const CdcDupGroup *x = (const CdcDupGroup *)a, *y = (const CdcDupGroup *)b;
    uint64_t left = x->size * (uint64_t)x->n_files;
    uint64_t right = y->size * (uint64_t)y->n_files;
    if (left != right) return left < right ? 1 : -1; /* size×count 降序 */
    if (x->size != y->size) return x->size < y->size ? 1 : -1;
    return strcmp(x->files[0], y->files[0]);
}

static void sort_dup_groups(CdcDupGroupList *groups) {
    if (groups->n > 1)
        qsort(groups->items, groups->n, sizeof(CdcDupGroup), cmp_dup_group);
}

/* 消费 items[0..n)（元素与数组外壳都由本函数释放或移交）。
 * 只留有终局摘要的项，按 (size, 终局摘要) 分组；组内补查 file_id、
 * 硬链接去重、按 path 排序后建成 CdcDupGroup。返回 0 成功，-1 内存失败。 */
static int build_dup_groups(DupCandidate *items, size_t n, int fast,
                            CdcDupGroupList *out) {
    int rc = 0;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        int has_final =
            fast ? items[i].digests.has_strong_xxh : items[i].digests.has_full;
        if (!has_final) {
            cand_free(&items[i]);
            continue;
        }
        if (w != i) {
            items[w] = items[i];
            items[i].path = NULL;
        }
        w++;
    }
    n = w;
    if (n > 0)
        qsort(items, n, sizeof(DupCandidate),
              fast ? cmp_cand_by_strong : cmp_cand_by_full);
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && items[j].size == items[i].size &&
               final_digest_eq(&items[j], &items[i], fast))
            j++;
        if (j - i >= 2) {
            for (size_t k = i; k < j; k++) {
                if (!items[k].has_file_id) {
                    CdcFileId fid = {0, 0};
                    if (cdc_query_file_identity(items[k].path, &fid)) {
                        items[k].file_id = fid;
                        items[k].has_file_id = 1;
                    }
                }
            }
            size_t kept = dedup_hardlinks(&items[i], j - i);
            if (kept >= 2) {
                qsort(&items[i], kept, sizeof(DupCandidate), cmp_cand_by_path);
                CdcDupGroup g;
                memset(&g, 0, sizeof g);
                final_digest_hex(&items[i], fast, g.hash_hex);
                g.size = items[i].size;
                g.files = (char **)malloc(kept * sizeof(char *));
                g.mtimes = (uint64_t *)malloc(kept * sizeof(uint64_t));
                if (!g.files || !g.mtimes) {
                    free(g.files);
                    free(g.mtimes);
                    rc = -1;
                    break;
                }
                for (size_t k = 0; k < kept; k++) {
                    g.files[k] = dup_str(items[i + k].path);
                    g.mtimes[k] = items[i + k].mtime;
                    if (!g.files[k]) {
                        for (size_t m = 0; m < k; m++) free(g.files[m]);
                        free(g.files);
                        free(g.mtimes);
                        rc = -1;
                        break;
                    }
                }
                if (rc != 0) break;
                g.n_files = kept;
                if (dup_group_list_push(out, g) != 0) {
                    for (size_t k = 0; k < kept; k++) free(g.files[k]);
                    free(g.files);
                    free(g.mtimes);
                    rc = -1;
                    break;
                }
            }
        }
        i = j;
    }
    for (size_t k = 0; k < n; k++) cand_free(&items[k]);
    free(items);
    return rc;
}

/* ---------------- 哈希管线入口（对照 main.rs 的 hash_index_groups） ---------------- */

int cdc_scan_hash_groups(const cdc_scan_input_t *inputs, size_t n,
                         cdc_hashcache_t *cache, uint64_t now,
                         cdc_digest_update_t **out_updates, size_t *out_n) {
    if (out_updates) *out_updates = NULL;
    if (out_n) *out_n = 0;
    if (!inputs || n == 0) return 0;
    CdcScanOpts opts;
    memset(&opts, 0, sizeof opts);
    opts.fast = 0;
    opts.trace = 0;
    DupCandidate *cands = (DupCandidate *)malloc(n * sizeof(DupCandidate));
    if (!cands) return -1;
    size_t nc = 0;
    CdcDiskCache dcache;
    memset(&dcache, 0, sizeof dcache);
    for (size_t i = 0; i < n; i++) {
        DupCandidate c;
        memset(&c, 0, sizeof c);
        c.path = dup_str(inputs[i].path);
        if (!c.path) continue;
        c.size = inputs[i].size;
        c.mtime = inputs[i].mtime;
        c.sort_key = 0;
        c.frn = inputs[i].frn;
        c.first_lcn = 0;
        c.bytes_per_cluster = 0;
        c.can_raw_read = 0; /* 索引行没有布局信息，不走 raw 快车道 */
        c.disk = disk_key_for_path(inputs[i].path, &dcache);
        c.is_ssd = (c.disk.kind == CDC_DISK_SSD);
        c.digests = inputs[i].digests;
        if (cache) {
            cdc_digest_set_t ce;
            memset(&ce, 0, sizeof ce);
            if (cdc_hashcache_entry_for(cache, inputs[i].path, inputs[i].size,
                                        inputs[i].mtime, &ce) == 1) {
                if (!c.digests.has_head && ce.has_head) {
                    c.digests.has_head = 1;
                    c.digests.head = ce.head;
                }
                if (!c.digests.has_sample && ce.has_sample) {
                    c.digests.has_sample = 1;
                    c.digests.sample = ce.sample;
                }
                if (!c.digests.has_strong && ce.has_strong) {
                    c.digests.has_strong = 1;
                    c.digests.strong = ce.strong;
                }
                /* 采样层只看 xxh 字段（旧版 BLAKE3 的三档只透传、不用于跳过）。 */
                if (!c.digests.has_head_xxh && ce.has_head_xxh) {
                    c.digests.has_head_xxh = 1;
                    c.digests.head_xxh = ce.head_xxh;
                }
                if (!c.digests.has_sample_xxh && ce.has_sample_xxh) {
                    c.digests.has_sample_xxh = 1;
                    c.digests.sample_xxh = ce.sample_xxh;
                }
                if (!c.digests.has_strong_xxh && ce.has_strong_xxh) {
                    c.digests.has_strong_xxh = 1;
                    c.digests.strong_xxh = ce.strong_xxh;
                }
                if (!c.digests.has_full && ce.has_full) {
                    c.digests.has_full = 1;
                    c.digests.full = ce.full;
                }
            }
        }
        cands[nc++] = c;
    }
    if (nc == 0) {
        free(cands);
        return 0;
    }
    qsort(cands, nc, sizeof(DupCandidate), cmp_cand_by_size);
    cdc_vec_t ready, ssd_stage1, mixed_stage1;
    memset(&ready, 0, sizeof ready);
    memset(&ssd_stage1, 0, sizeof ssd_stage1);
    memset(&mixed_stage1, 0, sizeof mixed_stage1);
    if (cdc_vec_init(&ready, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&ssd_stage1, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&mixed_stage1, sizeof(DupCandidate)) != CDC_OK) {
        free(cands);
        cand_vec_free_all(&ready);
        cand_vec_free_all(&ssd_stage1);
        cand_vec_free_all(&mixed_stage1);
        return -1;
    }
    if (split_dup_candidates(cands, nc, 0, &ready, &ssd_stage1, &mixed_stage1) != 0) {
        free(cands);
        cand_vec_free_all(&ready);
        cand_vec_free_all(&ssd_stage1);
        cand_vec_free_all(&mixed_stage1);
        return -1;
    }
    free(cands);
    cdc_vec_t final_items, eliminated;
    memset(&final_items, 0, sizeof final_items);
    memset(&eliminated, 0, sizeof eliminated);
    if (cdc_vec_init(&final_items, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&eliminated, sizeof(DupCandidate)) != CDC_OK) {
        cand_vec_free_all(&ready);
        cand_vec_free_all(&ssd_stage1);
        cand_vec_free_all(&mixed_stage1);
        cand_vec_free_all(&final_items);
        cand_vec_free_all(&eliminated);
        return -1;
    }
    int rc = run_dup_pipelines(&ssd_stage1, &mixed_stage1, cache, now, &opts,
                               "index-ssd", "index-hdd", &final_items, &eliminated);
    if (rc != 0) {
        cand_vec_free_all(&ready);
        cand_vec_free_all(&final_items);
        cand_vec_free_all(&eliminated);
        return -1;
    }
    /* updates：ready + final + eliminated 中仍有任一摘要的。 */
    cdc_vec_t updates;
    memset(&updates, 0, sizeof updates);
    if (cdc_vec_init(&updates, sizeof(cdc_digest_update_t)) != CDC_OK) {
        cand_vec_free_all(&ready);
        cand_vec_free_all(&final_items);
        cand_vec_free_all(&eliminated);
        return -1;
    }
    int ok = 1;
    cdc_vec_t *lanes[3] = {&ready, &final_items, &eliminated};
    for (int li = 0; li < 3 && ok; li++) {
        for (size_t k = 0; k < lanes[li]->len && ok; k++) {
            DupCandidate *c = &CDC_VEC_AT(lanes[li], DupCandidate, k);
            if (!cand_any_digest(c)) continue;
            cdc_digest_update_t u;
            memset(&u, 0, sizeof u);
            u.volume_serial = 0; /* 调用方按卷填（见 scan.h 注释） */
            u.frn = c->frn;
            u.size = c->size;
            u.mtime = c->mtime;
            u.digests = c->digests;
            if (cdc_vec_push(&updates, &u) != CDC_OK) ok = 0;
        }
    }
    cand_vec_free_all(&ready);
    cand_vec_free_all(&final_items);
    cand_vec_free_all(&eliminated);
    if (!ok) {
        cdc_vec_free(&updates);
        return -1;
    }
    size_t n_upd = updates.len;
    if (out_updates) {
        *out_updates = (cdc_digest_update_t *)updates.data;
        updates.data = NULL;
        updates.len = 0;
    }
    if (out_n) *out_n = out_updates ? n_upd : 0;
    cdc_vec_free(&updates);
    return 0;
}

/* ---------------- 实时扫描入口 ---------------- */

static bool entry_less_by_size(const cdc::FileEntry &x, const cdc::FileEntry &y) {
    if (x.size != y.size) return x.size < y.size;
    return strcmp(x.path.c_str(), y.path.c_str()) < 0;
}

static int cmp_big_file(const void *a, const void *b) {
    const CdcBigFile *x = (const CdcBigFile *)a, *y = (const CdcBigFile *)b;
    if (x->size != y->size) return x->size < y->size ? 1 : -1; /* size 降序 */
    return strcmp(x->path, y->path);
}

static int candidate_from_entry(const cdc::FileEntry *e, uint64_t mtime,
                                cdc_hashcache_t *cache, CdcDiskCache *dcache,
                                DupCandidate *out) {
    memset(out, 0, sizeof *out);
    out->path = dup_str(e->path.c_str());
    if (!out->path) return -1;
    out->size = e->size;
    out->mtime = mtime;
    out->sort_key = e->sort_key;
    out->frn = e->frn;
    out->first_lcn = e->first_lcn;
    out->bytes_per_cluster = e->bytes_per_cluster;
    out->can_raw_read = e->can_raw_read;
    out->disk = disk_key_for_path(e->path.c_str(), dcache);
    out->is_ssd = (out->disk.kind == CDC_DISK_SSD);
    if (cache) {
        cdc_digest_set_t ce;
        memset(&ce, 0, sizeof ce);
        if (cdc_hashcache_entry_for(cache, e->path.c_str(), e->size, mtime, &ce) == 1)
            out->digests = ce;
    }
    return 0;
}

void cdc_big_file_list_free(CdcBigFileList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->n; i++) free(list->items[i].path);
    free(list->items);
    list->items = NULL;
    list->n = list->cap = 0;
}

void cdc_dup_group_list_free(CdcDupGroupList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->n; i++) {
        CdcDupGroup *g = &list->items[i];
        for (size_t k = 0; k < g->n_files; k++) free(g->files[k]);
        free(g->files);
        free(g->mtimes);
    }
    free(list->items);
    list->items = NULL;
    list->n = list->cap = 0;
}


int cdc_scan_big(const char **drives, size_t n_drives, const CdcScanOpts *opts,
                 CdcBigFileList *out) {
    memset(out, 0, sizeof *out);
    char fixed[26][4];
    const char *fixed_ptrs[26];
    const char **use = drives;
    size_t n_use = n_drives;
    if (n_drives == 0) {
        size_t nf = 0;
        cdc_fixed_drives_buf(fixed, 26, &nf);
        for (size_t i = 0; i < nf; i++) fixed_ptrs[i] = fixed[i];
        use = fixed_ptrs;
        n_use = nf;
    }
    for (size_t d = 0; d < n_use; d++) {
        if (is_cancelled(opts)) break;
        cdc::FileEntryList list;
        auto big_keep = [](std::string_view name, uint64_t size) {
            return size >= cdc::big_file_threshold_bytes(name);
        };
        if (cdc::scan_files(use[d], cdc::kScanMinBytes, cdc::kTopSkip, big_keep, list) != 0)
            continue;
        for (cdc::FileEntry &e : list) {
            if (cdc::is_system_file_name(e.path)) continue;
            if (e.size < cdc::big_file_threshold_bytes(e.path)) continue;
            CdcBigFile bf;
            bf.path = dup_str(e.path.c_str());
            bf.size = e.size;
            bf.mtime = e.mtime ? e.mtime : cdc_file_mtime(bf.path);
            if (!bf.path || big_file_list_push(out, bf) != 0) free(bf.path);
        }
    }
    if (out->n > 1)
        qsort(out->items, out->n, sizeof(CdcBigFile), cmp_big_file);
    return 0;
}

int cdc_scan_dups(const char **roots, size_t n_roots, uint64_t min_size,
                  const CdcScanOpts *opts, CdcDupGroupList *out) {
    memset(out, 0, sizeof *out);
    CdcScanOpts def_opts;
    if (!opts) {
        memset(&def_opts, 0, sizeof def_opts);
        opts = &def_opts;
    }
    double started = now_mono();
    uint64_t min = min_size ? min_size : (1ULL << 20);
    char fixed[26][4];
    const char *fixed_ptrs[26];
    const char **use_roots = roots;
    size_t n_use = n_roots;
    if (n_roots == 0) {
        size_t nf = 0;
        cdc_fixed_drives_buf(fixed, 26, &nf);
        for (size_t i = 0; i < nf; i++) fixed_ptrs[i] = fixed[i];
        use_roots = fixed_ptrs;
        n_use = nf;
    }
    if (n_use == 0) return 0;
    /* 枚举（MFT 优先，失败退回遍历；系统文件在这里先滤掉）。 */
    cdc::FileEntryList all;
    for (size_t d = 0; d < n_use; d++) {
        if (is_cancelled(opts)) break;
        cdc::FileEntryList list;
        if (cdc::scan_files(use_roots[d], min, cdc::kTopSkip, nullptr, list) != 0) continue;
        for (cdc::FileEntry &e : list) {
            if (cdc::is_system_file_name(e.path)) continue;
            all.push_back(std::move(e));
        }
    }
    if (is_cancelled(opts)) return 0;
    if (all.size() > 1) std::sort(all.begin(), all.end(), entry_less_by_size);
    size_t candidates_count = 0;
    size_t i = 0;
    while (i < all.size()) {
        size_t j = i + 1;
        while (j < all.size() && all[j].size == all[i].size) j++;
        if (j - i >= 2) candidates_count += (j - i);
        i = j;
    }
    if (opts->trace)
        fprintf(stderr, "dup: 参与比对 %zu 个文件, 枚举用时 %.1fs\n", candidates_count,
                now_mono() - started);
    if (candidates_count == 0) return 0;
    /* 缓存预加载（只加载候选路径）。 */
    const char **paths = (const char **)malloc(candidates_count * sizeof(char *));
    cdc_hashcache_t *cache = NULL;
    if (paths) {
        size_t p = 0;
        size_t k = 0;
        while (k < all.size()) {
            size_t j = k + 1;
            while (j < all.size() && all[j].size == all[k].size) j++;
            if (j - k >= 2) {
                for (size_t m = k; m < j; m++) paths[p++] = all[m].path.c_str();
            }
            k = j;
        }
        cache = cdc_hashcache_load_for_paths(paths, candidates_count);
        free(paths);
    }
    uint64_t stamp_now = cdc::now_seconds();
    DupCandidate *cands = (DupCandidate *)malloc(candidates_count * sizeof(DupCandidate));
    if (!cands) {
        if (cache) cdc_hashcache_close(cache);
        return -1;
    }
    size_t nc = 0;
    CdcDiskCache dcache;
    memset(&dcache, 0, sizeof dcache);
    i = 0;
    while (i < all.size()) {
        size_t j = i + 1;
        while (j < all.size() && all[j].size == all[i].size) j++;
        if (j - i >= 2) {
            for (size_t m = i; m < j; m++) {
                cdc::FileEntry *e = &all[m];
                uint64_t mtime = e->mtime ? e->mtime : cdc_file_mtime(e->path.c_str());
                DupCandidate c;
                if (candidate_from_entry(e, mtime, cache, &dcache, &c) == 0)
                    cands[nc++] = c;
            }
        }
        i = j;
    }
    all = cdc::FileEntryList();
    /* cands 已按 size 排好（all 排过序，桶顺序保留）。 */
    cdc_vec_t ready, ssd_stage1, mixed_stage1;
    memset(&ready, 0, sizeof ready);
    memset(&ssd_stage1, 0, sizeof ssd_stage1);
    memset(&mixed_stage1, 0, sizeof mixed_stage1);
    if (cdc_vec_init(&ready, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&ssd_stage1, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&mixed_stage1, sizeof(DupCandidate)) != CDC_OK) {
        free(cands);
        cand_vec_free_all(&ready);
        cand_vec_free_all(&ssd_stage1);
        cand_vec_free_all(&mixed_stage1);
        if (cache) cdc_hashcache_close(cache);
        return -1;
    }
    if (split_dup_candidates(cands, nc, opts->fast, &ready, &ssd_stage1,
                             &mixed_stage1) != 0) {
        free(cands);
        cand_vec_free_all(&ready);
        cand_vec_free_all(&ssd_stage1);
        cand_vec_free_all(&mixed_stage1);
        if (cache) cdc_hashcache_close(cache);
        return -1;
    }
    free(cands);
    if (ssd_stage1.len == 0 && mixed_stage1.len == 0) {
        DupCandidate *rarr = (DupCandidate *)ready.data;
        size_t rn = ready.len;
        ready.data = NULL;
        ready.len = ready.cap = 0;
        int brc = build_dup_groups(rarr, rn, opts->fast, out);
        sort_dup_groups(out);
        if (opts->trace)
            fprintf(stderr, "dup: 找到 %zu 组, 总用时 %.1fs\n", out->n,
                    now_mono() - started);
        if (cache) cdc_hashcache_close(cache);
        return brc;
    }
    cdc_vec_t final_items, eliminated;
    memset(&final_items, 0, sizeof final_items);
    memset(&eliminated, 0, sizeof eliminated);
    if (cdc_vec_init(&final_items, sizeof(DupCandidate)) != CDC_OK ||
        cdc_vec_init(&eliminated, sizeof(DupCandidate)) != CDC_OK) {
        cand_vec_free_all(&ready);
        cand_vec_free_all(&ssd_stage1);
        cand_vec_free_all(&mixed_stage1);
        cand_vec_free_all(&final_items);
        cand_vec_free_all(&eliminated);
        if (cache) cdc_hashcache_close(cache);
        return -1;
    }
    int rc = run_dup_pipelines(&ssd_stage1, &mixed_stage1, cache, stamp_now, opts,
                               "ssd", "hdd+mixed", &final_items, &eliminated);
    cand_vec_free_all(&eliminated);
    if (rc != 0) {
        cand_vec_free_all(&ready);
        cand_vec_free_all(&final_items);
        if (cache) cdc_hashcache_close(cache);
        return -1;
    }
    if (is_cancelled(opts)) {
        cand_vec_free_all(&ready);
        cand_vec_free_all(&final_items);
        if (cache) cdc_hashcache_close(cache);
        return 0;
    }
    for (size_t k = 0; k < final_items.len; k++) {
        DupCandidate *c = &CDC_VEC_AT(&final_items, DupCandidate, k);
        int has_final = opts->fast ? c->digests.has_strong_xxh : c->digests.has_full;
        if (has_final) {
            if (cand_push_move(&ready, c) != 0) {
                cand_vec_free_all(&ready);
                cand_vec_free_all(&final_items);
                if (cache) cdc_hashcache_close(cache);
                return -1;
            }
        }
    }
    cand_vec_free_all(&final_items);
    {
        DupCandidate *rarr = (DupCandidate *)ready.data;
        size_t rn = ready.len;
        ready.data = NULL;
        ready.len = ready.cap = 0;
        rc = build_dup_groups(rarr, rn, opts->fast, out);
    }
    sort_dup_groups(out);
    if (opts->trace)
        fprintf(stderr, "dup: 找到 %zu 组, 总用时 %.1fs\n", out->n,
                now_mono() - started);
    if (cache) cdc_hashcache_close(cache);
    return rc;
}

/* ---------------- 小工具 ---------------- */

uint64_t cdc_file_mtime(const char *path) {
    if (!path) return 0;
#ifdef _WIN32
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return 0;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    uint64_t out = 0;
    if (GetFileAttributesExW(w, GetFileExInfoStandard, &attr)) {
        ULARGE_INTEGER ft;
        ft.LowPart = attr.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = attr.ftLastWriteTime.dwHighDateTime;
        out = cdc::mft_filetime_to_unix(ft.QuadPart);
    }
    free(w);
    return out;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime < 0 ? 0 : (uint64_t)st.st_mtime;
#endif
}

int cdc_fixed_drives_buf(char (*drives)[4], size_t cap, size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!drives || cap == 0) return 0;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    size_t n = 0;
    for (int i = 0; i < 26 && n < cap; i++) {
        if ((mask & (1u << i)) == 0) continue;
        wchar_t wroot[4] = {(wchar_t)('A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(wroot) == DRIVE_FIXED) {
            drives[n][0] = (char)('A' + i);
            drives[n][1] = ':';
            drives[n][2] = '\\';
            drives[n][3] = '\0';
            n++;
        }
    }
    if (n == 0) {
        memcpy(drives[0], "C:\\", 4);
        n = 1;
    }
    if (out_n) *out_n = n;
    return 0;
#else
    (void)drives;
    (void)cap;
    return 0;
#endif
}
