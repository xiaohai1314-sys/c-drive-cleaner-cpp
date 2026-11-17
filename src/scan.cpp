// scan.cpp — 大文件扫描与重复文件扫描，逐行对照 desktop/src/main.rs 的
// scan_big / scan_dups / run_dup_pipeline / run_dup_pipelines / hash_index_groups。
//
// 并行性说明（与 Rust 版的行为差异，仅影响速度不影响结果）：
//   - 各哈希阶段（sample/strong/probe/full）与 Linux 的 IOCP 等价路径用
//     fastio 的 par_for 按文件并行；每个候选只被一个 worker 处理，候选
//     内部字段无需额外锁；on_tick 回调与 drive hint 缓存由 shared_mutex()
//     串行化。阶段结束后分组前都按 (size, 摘要) 排序，最终分组与顺序版一致；
//     取消语义也一致（worker 入口先查 cancel）。
//   - 机械盘的 NCQ 排序保留（同盘按 sort_key 排，SSD 在前）。
//   - 全哈希 ≥16MB 用 2MB 缓冲，仍是单缓冲循环。
//   - 阶段内顺序与 Rust 的 HashMap 迭代顺序不同，但所有分组最后都按
//     (size, 摘要) 排序、组内按 path 排序，所以最终分组结果一致。
//
// 所有权：DupCandidate 持有自己的 path（std::string）；各阶段之间用
// std::vector<DupCandidate> 移动传递，丢弃的项随容器析构。
#include "scan.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string_view>

#include "bigfile.h"
#include "blake3.h"  // 只剩 full 确认用 BLAKE3；采样层见 sampling.h
#include "fastio.h"
#include "mft.h"
#include "sampling.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace cdc {
namespace {

// ---------------- 小工具 ----------------

bool is_cancelled(const ScanOpts &opts) { return opts.cancel && *opts.cancel; }

// on_tick 串行化：回调可能非线程安全，并行路径下多个 worker 会同时调它。
void call_tick(const ScanOpts &opts) {
    if (opts.on_tick) {
        LockGuard g(shared_mutex());
        opts.on_tick();
    }
}

double now_mono() {
#ifdef _WIN32
    return static_cast<double>(GetTickCount64()) / 1000.0;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
#endif
}

void blake3_hash_buf(const uint8_t *data, size_t len, uint8_t out[32]) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    if (len) blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, out, 32);
}

char drive_letter_of(std::string_view path) {
    char ch = path.empty() ? 'C' : path[0];
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (ch < 'A' || ch > 'Z') ch = 'C';
    return ch;
}

// ---------------- 盘符 hint（OpenFileById 用；Rust 的 thread_local DRIVE_HINTS） ----------------

// drive hint 缓存：进程生命期内持有（不关闭）。并行阶段多 worker 会同时调这里；
// g_hints 的读写用 shared_mutex() 串行化。打开句柄本身在锁外做，拿到后再在
// 锁内认领（已有缓存时丢弃自己新开的）。
void *drive_hint_for_path(const char *path) {
#ifdef _WIN32
    static void *g_hints[26];
    int i = drive_letter_of(path ? path : "") - 'A';
    {
        LockGuard g(shared_mutex());
        if (g_hints[i]) return g_hints[i];
    }
    DriveHint nh = open_drive_hint(static_cast<char>('A' + i));
    if (!nh) return nullptr;
    LockGuard g(shared_mutex());
    if (!g_hints[i]) g_hints[i] = nh.release();
    return g_hints[i];  // nh 若未被认领，离开作用域时关闭
#else
    (void)path;
    return nullptr;
#endif
}

// frn==0 时不带 hint（与 Rust 的 open_checked_file_with_size 一致）。
File open_checked_with_size(const char *path, uint64_t frn, uint32_t flags, bool *has_file_id,
                            FileId *file_id_out, uint64_t *actual_size_out) {
    void *hint = frn != 0 ? drive_hint_for_path(path) : nullptr;
    return File::open_checked(path, frn, flags, hint, has_file_id, file_id_out, actual_size_out);
}

// ---------------- 摘要原语 ----------------

// 一次摘要的结果：摘要 + 打开时拿到的硬链接标识。
struct HashOut {
    uint8_t digest[32] = {};
    FileId fid;
    bool has_fid = false;
};

// 读 offset 起 length 字节进 XXH3 流式状态（buf 分片）。寻道/读取出错返回 -1；
// 文件尾短读只摘要读到的部分（与 Rust 的 hash_chunk 一致，算法换成 XXH3）。
int hash_chunk_xxh(File &f, std::vector<uint8_t> &buf, uint64_t offset, uint64_t length,
                   Xxh3State &st) {
    uint64_t remaining = length, pos = offset;
    while (remaining > 0) {
        size_t want = remaining < buf.size() ? static_cast<size_t>(remaining) : buf.size();
        size_t got = 0;
        if (f.read_at(pos, buf.data(), want, got) != 0) return -1;
        if (got == 0) break;
        if (st.update(buf.data(), got) != 0) return -1;
        pos += got;
        remaining -= got;
    }
    return 0;
}

// 小文件全文采样：整文件顺序读一遍算 XXH3（1MB 缓冲）。
int whole_file_xxh_by_frn(const char *path, uint64_t frn, HashOut &out) {
    HashOut r;
    uint64_t actual = 0;
    File f = open_checked_with_size(path, frn, kFileFlagSequentialScan, &r.has_fid, &r.fid, &actual);
    if (!f) return -1;
    std::vector<uint8_t> buf(kScanFullBufSize);
    auto st = Xxh3State::create();
    if (!st) return -1;
    for (;;) {
        size_t got = 0;
        if (f.read_seq(buf.data(), buf.size(), got) != 0) return -1;
        if (got == 0) break;
        if (st->update(buf.data(), got) != 0) return -1;
    }
    xxh_to_digest32(st->finalize(), r.digest);
    out = r;
    return 0;
}

// 整文件顺序 BLAKE3（每 16 块检查一次取消；≥16MB 用 2MB 缓冲）。
int full_hash_by_frn(const char *path, uint64_t frn, HashOut &out, const ScanOpts *opts) {
    HashOut r;
    uint64_t actual = 0;
    File f = open_checked_with_size(path, frn, kFileFlagSequentialScan, &r.has_fid, &r.fid, &actual);
    if (!f) return -1;
    std::vector<uint8_t> buf(actual >= kScanFullLargeThreshold ? kScanFullBufLarge : kScanFullBufSize);
    blake3_hasher h;
    blake3_hasher_init(&h);
    for (uint32_t chunks = 0;; chunks++) {
        if ((chunks & 15) == 0 && opts && is_cancelled(*opts)) return -1;
        size_t got = 0;
        if (f.read_seq(buf.data(), buf.size(), got) != 0) return -1;
        if (got == 0) break;
        blake3_hasher_update(&h, buf.data(), got);
    }
    blake3_hasher_finalize(&h, r.digest, 32);
    out = r;
    return 0;
}

// 头、中、尾各取 chunk_len 的 XXH3；size <= chunk_len*3 时直接全文 XXH3。
// sample（16KB）与 strong（256KB）共用。digest 是 XXH3-64 载体编码。
int three_window_hash_by_frn(const char *path, uint64_t frn, uint64_t size, uint64_t chunk,
                             HashOut &out) {
    if (size <= chunk * 3) return whole_file_xxh_by_frn(path, frn, out);
    HashOut r;
    uint64_t actual = 0;
    File f = open_checked_with_size(path, frn, kFileFlagRandomAccess, &r.has_fid, &r.fid, &actual);
    if (!f) return -1;
    std::vector<uint8_t> buf(static_cast<size_t>(chunk));
    auto st = Xxh3State::create();
    if (!st) return -1;
    if (hash_chunk_xxh(f, buf, 0, chunk, *st) != 0 ||
        hash_chunk_xxh(f, buf, size / 2, chunk, *st) != 0 ||
        hash_chunk_xxh(f, buf, size - chunk, chunk, *st) != 0)
        return -1;
    xxh_to_digest32(st->finalize(), r.digest);
    out = r;
    return 0;
}

// 大文件渐进探针：25% 与 75% 处各 64KB。
int probe_hash_by_frn(const char *path, uint64_t frn, uint64_t size, HashOut &out) {
    HashOut r;
    uint64_t actual = 0;
    File f = open_checked_with_size(path, frn, kFileFlagRandomAccess, &r.has_fid, &r.fid, &actual);
    if (!f) return -1;
    std::vector<uint8_t> buf(static_cast<size_t>(kScanProbeChunk));
    auto st = Xxh3State::create();
    if (!st) return -1;
    uint64_t tail = size >= kScanProbeChunk ? size - kScanProbeChunk : 0;
    uint64_t off1 = size / 4 < tail ? size / 4 : tail;
    uint64_t off2 = size * 3 / 4 < tail ? size * 3 / 4 : tail;
    uint64_t len = kScanProbeChunk < size ? kScanProbeChunk : size;
    if (hash_chunk_xxh(f, buf, off1, len, *st) != 0 || hash_chunk_xxh(f, buf, off2, len, *st) != 0)
        return -1;
    xxh_to_digest32(st->finalize(), r.digest);
    out = r;
    return 0;
}

// 机械盘专用（<= 2MB）：一次顺序读完整文件，同时算出采样与全哈希。
int read_hdd_seq_sample_and_full(const char *path, uint64_t frn, uint64_t size, uint64_t chunk_len,
                                 HashOut &sample, uint8_t full[32]) {
    HashOut r;
    uint64_t actual = 0;
    File f = open_checked_with_size(path, frn, kFileFlagSequentialScan, &r.has_fid, &r.fid, &actual);
    if (!f) return -1;
    if (actual != size || actual > kScanHddSeqThreshold) return -1;
    auto s = static_cast<size_t>(actual);
    std::vector<uint8_t> buf(s ? s : 1);
    size_t filled = 0;
    while (filled < s) {
        size_t got = 0;
        if (f.read_seq(buf.data() + filled, s - filled, got) != 0 || got == 0) break;
        filled += got;
    }
    f = File();
    if (filled != s) return -1;
    blake3_hash_buf(buf.data(), s, full);
    auto c = static_cast<size_t>(chunk_len);
    if (s <= c * 3) {
        // 小文件：采样即全文 XXH3（与 whole_file_xxh_by_frn 同值）。
        xxh3_buf_to_digest32(buf.data(), s, r.digest);
    } else {
        auto st = Xxh3State::create();
        if (!st) return -1;
        if (st->update(buf.data(), c) != 0 || st->update(buf.data() + s / 2, c) != 0 ||
            st->update(buf.data() + s - c, c) != 0)
            return -1;
        xxh_to_digest32(st->finalize(), r.digest);
    }
    sample = r;
    return 0;
}

// ---------------- 磁盘键（对照 main.rs 的 DiskKey / disk_key_for_path） ----------------

enum class DiskKind { Ssd, HddPhysical, HddDrive };

struct DiskKey {
    DiskKind kind = DiskKind::Ssd;
    uint32_t dev_type = 0, dev_num = 0;
    char letter = 0;
};

int cmp_disk_key(const DiskKey &a, const DiskKey &b) {
    if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
    if (a.dev_type != b.dev_type) return a.dev_type < b.dev_type ? -1 : 1;
    if (a.dev_num != b.dev_num) return a.dev_num < b.dev_num ? -1 : 1;
    if (a.letter != b.letter) return a.letter < b.letter ? -1 : 1;
    return 0;
}

class DiskCache {
public:
    DiskKey key_for_path(const std::string &path) {
        char ch = drive_letter_of(path);
        int i = ch - 'A';
        if (valid_[i]) return keys_[i];
        DiskKey key;
        key.letter = ch;
        char drive[4] = {ch, ':', '\\', '\0'};
        if (is_ssd(drive) != 0) {
            // 1=SSD；-1=未知，按 SSD 处理（与 Rust 的 unwrap_or(true) 一致）
            key.kind = DiskKind::Ssd;
        } else {
            uint32_t dt = 0, dn = 0;
            if (physical_disk_id(drive, dt, dn) == 0) {
                key.kind = DiskKind::HddPhysical;
                key.dev_type = dt;
                key.dev_num = dn;
            } else {
                key.kind = DiskKind::HddDrive;
            }
        }
        keys_[i] = key;
        valid_[i] = true;
        return key;
    }

private:
    bool valid_[26] = {};
    DiskKey keys_[26];
};

// ---------------- 候选（对照 main.rs 的 DupCandidate） ----------------

struct DupCandidate {
    std::string path;
    uint64_t size = 0, mtime = 0, sort_key = 0, frn = 0, first_lcn = 0;
    uint32_t bytes_per_cluster = 0;
    bool can_raw_read = false;
    bool is_ssd = false;
    DiskKey disk;
    bool has_file_id = false;
    FileId file_id;
    cdc_digest_set_t digests{};
    bool has_probe = false;
    uint64_t probe_xxh = 0;  // 探针 XXH3，只放内存，不落库
    bool dirty_head_xxh = false, dirty_sample_xxh = false, dirty_strong_xxh = false,
         dirty_full = false;

    void adopt_file_id(const HashOut &h) {
        if (!has_file_id && h.has_fid) {
            file_id = h.fid;
            has_file_id = true;
        }
    }
};

using CandVec = std::vector<DupCandidate>;

void move_all(CandVec &dst, CandVec &src) {
    for (DupCandidate &c : src) dst.push_back(std::move(c));
    src.clear();
}

bool cand_any_digest(const DupCandidate &c) {
    // 采样层只看 xxh 字段；旧版 BLAKE3 的 has_head/has_sample/has_strong 也算：
    // 从缓存/索引带进来的旧值要能透传进更新（写回时 COALESCE 不覆盖）。
    return c.digests.has_head_xxh || c.digests.has_sample_xxh || c.digests.has_strong_xxh ||
           c.digests.has_head || c.digests.has_sample || c.digests.has_strong ||
           c.digests.has_full;
}

// 比较器（全序：最后都按 path 定序，std::sort 的结果与 qsort 一致）。
bool less_schedule(const DupCandidate &x, const DupCandidate &y) {
    if (x.is_ssd != y.is_ssd) return x.is_ssd;  // SSD 在前
    if (int c = cmp_disk_key(x.disk, y.disk)) return c < 0;
    if (x.sort_key != y.sort_key) return x.sort_key < y.sort_key;
    return x.path < y.path;
}

template <class Key>
auto less_by_size_then(Key key) {
    return [key](const DupCandidate &x, const DupCandidate &y) {
        if (x.size != y.size) return x.size < y.size;
        auto kx = key(x), ky = key(y);
        if (kx != ky) return kx < ky;
        return x.path < y.path;
    };
}

const auto less_by_size = less_by_size_then([](const DupCandidate &) { return 0; });
const auto less_by_head = less_by_size_then([](const DupCandidate &c) { return c.digests.head_xxh; });
const auto less_by_sample =
    less_by_size_then([](const DupCandidate &c) { return c.digests.sample_xxh; });
const auto less_by_probe = less_by_size_then([](const DupCandidate &c) { return c.probe_xxh; });
const auto less_by_strong =
    less_by_size_then([](const DupCandidate &c) { return c.digests.strong_xxh; });

bool less_by_full(const DupCandidate &x, const DupCandidate &y) {
    if (x.size != y.size) return x.size < y.size;
    if (int c = std::memcmp(x.digests.full.bytes, y.digests.full.bytes, 32)) return c < 0;
    return x.path < y.path;
}

// 一组内硬链接去重：保留每个 file_id 的第一次出现；file_id 未知的保留。
// [first, last) 原地压缩，返回去重后的个数。
size_t dedup_hardlinks(CandVec::iterator first, CandVec::iterator last) {
    auto w = first;
    for (auto it = first; it != last; ++it) {
        bool drop = false;
        if (it->has_file_id) {
            for (auto k = first; k != w; ++k) {
                if (k->has_file_id && k->file_id == it->file_id) {
                    drop = true;
                    break;
                }
            }
        }
        if (drop) continue;
        if (w != it) *w = std::move(*it);
        ++w;
    }
    return static_cast<size_t>(w - first);
}

void flush_candidate_hashes(cdc_hashcache_t *cache, const CandVec &items, uint64_t now) {
    if (!cache) return;
    for (const DupCandidate &c : items) {
        if (!(c.dirty_head_xxh || c.dirty_sample_xxh || c.dirty_strong_xxh || c.dirty_full))
            continue;
        // 只写 xxh 子字段 + full：旧版 BLAKE3 的 head/sample/strong 不再由管线
        // 产生，不写（缓存里已有的旧值靠 COALESCE 保留）。
        cdc_digest_set_t sub{};
        if (c.dirty_head_xxh) {
            sub.has_head_xxh = 1;
            sub.head_xxh = c.digests.head_xxh;
        }
        if (c.dirty_sample_xxh) {
            sub.has_sample_xxh = 1;
            sub.sample_xxh = c.digests.sample_xxh;
        }
        if (c.dirty_strong_xxh) {
            sub.has_strong_xxh = 1;
            sub.strong_xxh = c.digests.strong_xxh;
        }
        if (c.dirty_full) {
            sub.has_full = 1;
            sub.full = c.digests.full;
        }
        cdc_hashcache_remember(cache, c.path.c_str(), c.size, c.mtime, &sub, now);
    }
}

// head 已覆盖全文：sample/strong 的 XXH3 值与 head 相同（小文件上采样即全文）；
// full 是 BLAKE3，不能从 xxHash 派生，留给阶段 3 再算。
void set_head(DupCandidate &c, const uint8_t digest[32]) {
    c.digests.head_xxh = xxh_from_digest32(digest);
    c.digests.has_head_xxh = 1;
    c.dirty_head_xxh = true;
    if (c.size <= kScanHeadChunk) {
        c.digests.sample_xxh = c.digests.head_xxh;
        c.digests.has_sample_xxh = 1;
        c.digests.strong_xxh = c.digests.head_xxh;
        c.digests.has_strong_xxh = 1;
        c.dirty_sample_xxh = true;
        c.dirty_strong_xxh = true;
    }
}

// ---------------- 并行阶段（par_for） ----------------
// 每项独立（打开自己的文件、算自己的摘要、只写自己的候选字段），用 par_for
// 按下标分给 worker；worker 数按本阶段候选是否全在 SSD 上分档。

bool all_ssd(const CandVec &v) {
    for (const DupCandidate &c : v)
        if (!c.is_ssd) return false;
    return true;
}

template <class Work>
void run_stage(CandVec &v, Work work) {
    if (v.empty()) return;
    par_for(v.size(), par_hash_workers(all_ssd(v)), [&](size_t i) { work(v[i]); });
}

// 快速模式 strong。
void stage_strong_work(DupCandidate &cand, const ScanOpts &opts) {
    if (is_cancelled(opts)) return;
    if (cand.digests.has_strong_xxh) return;
    if (!cand.is_ssd && cand.size <= kScanHddSeqThreshold) {
        HashOut strong;
        uint8_t full[32];
        if (read_hdd_seq_sample_and_full(cand.path.c_str(), cand.frn, cand.size, kScanStrongChunk,
                                         strong, full) == 0) {
            cand.adopt_file_id(strong);
            cand.digests.strong_xxh = xxh_from_digest32(strong.digest);
            cand.digests.has_strong_xxh = 1;
            std::memcpy(cand.digests.full.bytes, full, 32);
            cand.digests.has_full = 1;
            cand.dirty_strong_xxh = true;
            cand.dirty_full = true;
        }
    } else {
        HashOut h;
        if (three_window_hash_by_frn(cand.path.c_str(), cand.frn, cand.size, kScanStrongChunk, h) ==
            0) {
            cand.adopt_file_id(h);
            cand.digests.strong_xxh = xxh_from_digest32(h.digest);
            cand.digests.has_strong_xxh = 1;
            cand.dirty_strong_xxh = true;
            // 注意：strong 是 XXH3，full 是 BLAKE3，不能把 strong 抄给 full；
            // 小文件的 full 在 fast 模式下就缺着，下次常规扫描再补。
        }
    }
    call_tick(opts);
}

// 常规阶段 2 sample。
void stage_sample_work(DupCandidate &cand, const ScanOpts &opts) {
    if (is_cancelled(opts)) return;
    if (cand.digests.has_sample_xxh || cand.digests.has_full) return;
    if (!cand.is_ssd && cand.size <= kScanHddSeqThreshold) {
        HashOut sample;
        uint8_t full[32];
        if (read_hdd_seq_sample_and_full(cand.path.c_str(), cand.frn, cand.size, kScanSampleChunk,
                                         sample, full) == 0) {
            cand.adopt_file_id(sample);
            cand.digests.sample_xxh = xxh_from_digest32(sample.digest);
            cand.digests.has_sample_xxh = 1;
            std::memcpy(cand.digests.full.bytes, full, 32);
            cand.digests.has_full = 1;
            cand.dirty_sample_xxh = true;
            cand.dirty_full = true;
        }
    } else {
        HashOut h;
        if (three_window_hash_by_frn(cand.path.c_str(), cand.frn, cand.size, kScanSampleChunk, h) ==
            0) {
            cand.adopt_file_id(h);
            cand.digests.sample_xxh = xxh_from_digest32(h.digest);
            cand.digests.has_sample_xxh = 1;
            cand.dirty_sample_xxh = true;
            // 注意：sample 是 XXH3，full 是 BLAKE3；小文件的 full 留给阶段 3。
        }
    }
    call_tick(opts);
}

// 探针。
void stage_probe_work(DupCandidate &cand, const ScanOpts &opts) {
    if (is_cancelled(opts)) return;
    HashOut h;
    if (probe_hash_by_frn(cand.path.c_str(), cand.frn, cand.size, h) == 0) {
        cand.adopt_file_id(h);
        cand.probe_xxh = xxh_from_digest32(h.digest);
        cand.has_probe = true;
    }
    call_tick(opts);
}

// 阶段 3 全哈希。
void stage_full_work(DupCandidate &cand, const ScanOpts &opts) {
    if (is_cancelled(opts)) return;
    if (cand.digests.has_full) return;
    HashOut h;
    if (full_hash_by_frn(cand.path.c_str(), cand.frn, h, &opts) == 0) {
        cand.adopt_file_id(h);
        std::memcpy(cand.digests.full.bytes, h.digest, 32);
        cand.digests.has_full = 1;
        cand.dirty_full = true;
    }
    call_tick(opts);
}

// 按相等键把已排序的 [0,n) 切成组，逐组回调 fn(begin, end)。
template <class Eq, class Fn>
void for_each_run(CandVec &v, Eq eq, Fn fn) {
    size_t i = 0;
    while (i < v.size()) {
        size_t j = i + 1;
        while (j < v.size() && eq(v[j], v[i])) j++;
        fn(i, j);
        i = j;
    }
}

// ---------------- 阶段 1：head 4KB ----------------

void stage_head(CandVec &items, const ScanOpts &opts) {
    // 已有 head 的先 tick（与 Rust 的第一遍循环一致）。
    for (const DupCandidate &c : items)
        if (c.digests.has_head_xxh) call_tick(opts);

    if (!is_cancelled(opts)) {
        // 卷句柄 LCN 合并直读：按盘符分组，同盘一次调用（内部按偏移合并读）。
        struct RawPair {
            char letter;
            RawHeadTarget target;
        };
        std::vector<RawPair> pairs;
        for (size_t i = 0; i < items.size(); i++) {
            const DupCandidate &c = items[i];
            if (c.digests.has_head_xxh || !c.can_raw_read || c.bytes_per_cluster == 0 ||
                c.first_lcn == 0)
                continue;
            RawHeadTarget t;
            t.idx = i;
            t.offset = c.first_lcn * c.bytes_per_cluster;
            t.want = c.size < kScanHeadChunk ? static_cast<size_t>(c.size)
                                             : static_cast<size_t>(kScanHeadChunk);
            pairs.push_back(RawPair{drive_letter_of(c.path), t});
        }
        if (!pairs.empty()) {
            // 按（盘符，偏移）排，同盘符的连续成组（稳定排序保持原相对顺序，
            // 与 C 版的选择排序在相等键上的结果一致）。
            std::stable_sort(pairs.begin(), pairs.end(), [](const RawPair &a, const RawPair &b) {
                if (a.letter != b.letter) return a.letter < b.letter;
                return a.target.offset < b.target.offset;
            });
            std::vector<RawHeadTarget> targets;
            targets.reserve(pairs.size());
            for (const RawPair &p : pairs) targets.push_back(p.target);
            size_t a = 0;
            while (a < pairs.size()) {
                size_t b = a + 1;
                while (b < pairs.size() && pairs[b].letter == pairs[a].letter) b++;
                // [a, b) 同盘符：is_ssd 取第一项（同盘一致）。
                bool ssd = items[targets[a].idx].is_ssd;
                std::vector<HeadResult> got = coalesced_raw_volume_heads(
                    pairs[a].letter, std::span<const RawHeadTarget>(&targets[a], b - a), ssd,
                    opts.cancel, opts.on_tick);
                for (const HeadResult &r : got) set_head(items[r.idx], r.digest);
                a = b;
            }
        }
    }

    // IOCP 回退：SSD 一批（并发 64），机械盘按盘分组各一批（并发 8）。
    if (is_cancelled(opts)) return;
    std::vector<IocpRequest> ssd_reqs, hdd_reqs;
    for (size_t i = 0; i < items.size(); i++) {
        const DupCandidate &c = items[i];
        if (c.digests.has_head_xxh) continue;
        IocpRequest r;
        r.idx = i;
        r.path = c.path.c_str();
        r.frn = c.frn;
        r.want = c.size < kScanHeadChunk ? static_cast<size_t>(c.size)
                                         : static_cast<size_t>(kScanHeadChunk);
        (c.is_ssd ? ssd_reqs : hdd_reqs).push_back(r);
    }
    std::vector<IocpResult> res;
    if (!ssd_reqs.empty()) {
        // SSD 批 64 在途（NVMe 放宽；fastio 的槽上限已同步到 128）。
        res = iocp_read_heads(ssd_reqs, 64, opts.cancel, opts.on_tick);
    }
    // 机械盘段已按 (disk, sort_key, path) 排好（输入是调度序），直接按 disk 分组。
    size_t hpos = 0;
    while (hpos < hdd_reqs.size()) {
        size_t hend = hpos + 1;
        while (hend < hdd_reqs.size() &&
               cmp_disk_key(items[hdd_reqs[hpos].idx].disk, items[hdd_reqs[hend].idx].disk) == 0)
            hend++;
        std::vector<IocpResult> part = iocp_read_heads(
            std::span<const IocpRequest>(&hdd_reqs[hpos], hend - hpos), 8, opts.cancel, opts.on_tick);
        res.insert(res.end(), part.begin(), part.end());
        hpos = hend;
    }
    for (const IocpResult &r : res) {
        DupCandidate &c = items[r.idx];
        if (!c.has_file_id && r.has_file_id) {
            c.file_id = r.file_id;
            c.has_file_id = true;
        }
        set_head(c, r.digest);
    }
}

// ---------------- 管线（对照 main.rs 的 run_dup_pipeline） ----------------

// 消费 items（元素或被移进 out_final/out_elim，或被丢弃）。
void run_dup_pipeline(CandVec items, const char *label, const ScanOpts &opts, CandVec &out_final,
                      CandVec &out_elim) {
    if (items.empty()) return;
    // 调度顺序：SSD 在前，同盘按 sort_key。
    std::sort(items.begin(), items.end(), less_schedule);

    stage_head(items, opts);

    if (is_cancelled(opts)) {
        // 取消：final 为空，eliminated = 全部 stage1（摘要照常回写缓存）。
        move_all(out_elim, items);
        return;
    }

    // 按 (size, head) 分组：有 head 的才参与；组内硬链接去重；≥2 进阶段 2，
    // 单例进 eliminated；没 head 的直接丢弃（与 Rust 一致，不进 eliminated）。
    CandVec stage2;
    std::sort(items.begin(), items.end(), less_by_head);
    {
        size_t i = 0;
        while (i < items.size()) {
            if (!items[i].digests.has_head_xxh) {
                i++;
                continue;
            }
            size_t j = i + 1;
            while (j < items.size() && items[j].size == items[i].size &&
                   items[j].digests.has_head_xxh &&
                   items[j].digests.head_xxh == items[i].digests.head_xxh)
                j++;
            size_t kept = dedup_hardlinks(items.begin() + static_cast<ptrdiff_t>(i),
                                          items.begin() + static_cast<ptrdiff_t>(j));
            CandVec &dst = kept >= 2 ? stage2 : out_elim;
            for (size_t k = i; k < i + kept; k++) dst.push_back(std::move(items[k]));
            i = j;
        }
        items.clear();
    }

    if (opts.trace) std::fprintf(stderr, "dup[%s]: 4KB 初筛后剩余 %zu 个文件\n", label, stage2.size());
    if (stage2.empty()) return;

    if (opts.fast) {
        // ---------- 快速模式：strong 即终局 ----------
        std::sort(stage2.begin(), stage2.end(), less_schedule);
        run_stage(stage2, [&](DupCandidate &c) { stage_strong_work(c, opts); });
        move_all(is_cancelled(opts) ? out_elim : out_final, stage2);
        return;
    }

    // ---------- 阶段 2（常规）：sample 16KB×3 ----------
    std::sort(stage2.begin(), stage2.end(), less_schedule);
    run_stage(stage2, [&](DupCandidate &c) { stage_sample_work(c, opts); });
    if (is_cancelled(opts)) {
        move_all(out_elim, stage2);
        return;
    }

    // 按 (size, sample) 分组：有 sample 的分组；sample 缺但 full 有的直进阶段 3；
    // 两者皆无的丢弃。
    CandVec stage3, with_sample;
    for (DupCandidate &c : stage2) {
        if (c.digests.has_sample_xxh)
            with_sample.push_back(std::move(c));
        else if (c.digests.has_full)
            stage3.push_back(std::move(c));
    }
    stage2.clear();
    std::sort(with_sample.begin(), with_sample.end(), less_by_sample);
    for_each_run(
        with_sample,
        [](const DupCandidate &a, const DupCandidate &b) {
            return a.size == b.size && a.digests.sample_xxh == b.digests.sample_xxh;
        },
        [&](size_t i, size_t j) {
            CandVec &dst = (j - i) >= 2 ? stage3 : out_elim;
            for (size_t k = i; k < j; k++) dst.push_back(std::move(with_sample[k]));
        });

    if (opts.trace) std::fprintf(stderr, "dup[%s]: 采样后仍需全校验 %zu 个文件\n", label, stage3.size());
    if (stage3.empty() || std::getenv("CDC_DUP_SAMPLE_ONLY")) {
        move_all(out_elim, stage3);
        return;
    }

    // ---------- 阶段 3 前置：>16MB 探针 ----------
    CandVec direct, need_probe, probed;
    for (DupCandidate &c : stage3) {
        if (c.size > kScanProbeThreshold && c.digests.has_sample_xxh)
            need_probe.push_back(std::move(c));
        else
            direct.push_back(std::move(c));
    }
    stage3.clear();
    // need_probe 按 (size, sample) 分组：组 ≥2 且全员无 full 才探针，其余直进阶段 3。
    std::sort(need_probe.begin(), need_probe.end(), less_by_sample);
    for_each_run(
        need_probe,
        [](const DupCandidate &a, const DupCandidate &b) {
            return a.size == b.size && a.digests.sample_xxh == b.digests.sample_xxh;
        },
        [&](size_t i, size_t j) {
            bool all_no_full = true;
            for (size_t k = i; k < j; k++)
                if (need_probe[k].digests.has_full) all_no_full = false;
            CandVec &dst = ((j - i) >= 2 && all_no_full) ? probed : direct;
            for (size_t k = i; k < j; k++) dst.push_back(std::move(need_probe[k]));
        });
    need_probe.clear();
    if (!probed.empty() && !is_cancelled(opts)) {
        std::sort(probed.begin(), probed.end(), less_schedule);
        run_stage(probed, [&](DupCandidate &c) { stage_probe_work(c, opts); });
        if (is_cancelled(opts)) {
            move_all(out_elim, direct);
            move_all(out_elim, probed);
            return;
        }
        // 按 (size, probe) 分组：无 probe 的直进；组 ≥2 直进；单例淘汰。
        std::sort(probed.begin(), probed.end(), less_by_probe);
        size_t p = 0;
        while (p < probed.size()) {
            if (!probed[p].has_probe) {
                direct.push_back(std::move(probed[p]));
                p++;
                continue;
            }
            size_t q = p + 1;
            while (q < probed.size() && probed[q].has_probe && probed[q].size == probed[p].size &&
                   probed[q].probe_xxh == probed[p].probe_xxh)
                q++;
            CandVec &dst = (q - p) >= 2 ? direct : out_elim;
            for (size_t k = p; k < q; k++) dst.push_back(std::move(probed[k]));
            p = q;
        }
        probed.clear();
    }
    // 探针前已取消：待探的项按 Rust 版直接丢弃（既不在 direct 也不在 eliminated）。

    if (direct.empty()) return;

    // ---------- 阶段 3：全文件哈希 ----------
    std::sort(direct.begin(), direct.end(), less_schedule);
    run_stage(direct, [&](DupCandidate &c) { stage_full_work(c, opts); });
    move_all(is_cancelled(opts) ? out_elim : out_final, direct);
}

// ---------------- 分流与双路（对照 main.rs 的 split_dup_candidates / run_dup_pipelines） ----------------

// 输入 cands 按 size 排好；本函数消费它。
void split_dup_candidates(CandVec &cands, bool fast, CandVec &ready, CandVec &ssd_stage1,
                          CandVec &mixed_stage1) {
    for_each_run(
        cands, [](const DupCandidate &a, const DupCandidate &b) { return a.size == b.size; },
        [&](size_t i, size_t j) {
            if (j - i < 2) return;
            bool all_ready = true, all_ssd_disk = true;
            for (size_t k = i; k < j; k++) {
                bool has_final = fast ? cands[k].digests.has_strong_xxh : cands[k].digests.has_full;
                if (!has_final) all_ready = false;
                if (!cands[k].is_ssd) all_ssd_disk = false;
            }
            CandVec &dst = all_ready ? ready : (all_ssd_disk ? ssd_stage1 : mixed_stage1);
            for (size_t k = i; k < j; k++) dst.push_back(std::move(cands[k]));
        });
    cands.clear();
}

// SSD 与混合两路顺序跑完，摘要回写缓存，返回（最终组候选，淘汰候选）。
void run_dup_pipelines(CandVec ssd_stage1, CandVec mixed_stage1, cdc_hashcache_t *cache,
                       uint64_t now, const ScanOpts &opts, const char *ssd_label,
                       const char *mixed_label, CandVec &out_final, CandVec &out_elim) {
    if (ssd_stage1.empty() && mixed_stage1.empty()) return;
    CandVec ssd_final, ssd_elim, mixed_final, mixed_elim;
    run_dup_pipeline(std::move(ssd_stage1), ssd_label, opts, ssd_final, ssd_elim);
    run_dup_pipeline(std::move(mixed_stage1), mixed_label, opts, mixed_final, mixed_elim);
    flush_candidate_hashes(cache, ssd_elim, now);
    flush_candidate_hashes(cache, ssd_final, now);
    flush_candidate_hashes(cache, mixed_elim, now);
    flush_candidate_hashes(cache, mixed_final, now);
    if (cache) cdc_hashcache_save_dirty(cache);
    move_all(out_final, ssd_final);
    move_all(out_final, mixed_final);
    move_all(out_elim, ssd_elim);
    move_all(out_elim, mixed_elim);
}

// ---------------- 建组（对照 main.rs 的 build_dup_groups / sort_dup_groups） ----------------

// 终局摘要写组 key：fast 模式为 strong_xxh 的 16 位 hex，常规为 full 的 64 位 hex。
std::string final_digest_hex(const DupCandidate &c, bool fast) {
    char buf[65];
    if (fast)
        xxh_to_hex(c.digests.strong_xxh, buf);
    else
        cdc_digest_to_hex(&c.digests.full, buf);
    return buf;
}

void sort_dup_groups(DupGroupList &groups) {
    std::sort(groups.begin(), groups.end(), [](const DupGroup &x, const DupGroup &y) {
        uint64_t left = x.size * static_cast<uint64_t>(x.files.size());
        uint64_t right = y.size * static_cast<uint64_t>(y.files.size());
        if (left != right) return left > right;  // size×count 降序
        if (x.size != y.size) return x.size > y.size;
        return x.files[0] < y.files[0];
    });
}

// 只留有终局摘要的项，按 (size, 终局摘要) 分组；组内补查 file_id、硬链接去重、
// 按 path 排序后建成 DupGroup。
void build_dup_groups(CandVec items, bool fast, DupGroupList &out) {
    std::erase_if(items, [fast](const DupCandidate &c) {
        return !(fast ? c.digests.has_strong_xxh : c.digests.has_full);
    });
    if (fast)
        std::sort(items.begin(), items.end(), less_by_strong);
    else
        std::sort(items.begin(), items.end(), less_by_full);
    auto final_eq = [fast](const DupCandidate &a, const DupCandidate &b) {
        if (a.size != b.size) return false;
        if (fast) return a.digests.strong_xxh == b.digests.strong_xxh;
        return std::memcmp(a.digests.full.bytes, b.digests.full.bytes, 32) == 0;
    };
    for_each_run(items, final_eq, [&](size_t i, size_t j) {
        if (j - i < 2) return;
        for (size_t k = i; k < j; k++) {
            if (!items[k].has_file_id) {
                FileId fid;
                if (query_file_identity(items[k].path.c_str(), fid)) {
                    items[k].file_id = fid;
                    items[k].has_file_id = true;
                }
            }
        }
        auto first = items.begin() + static_cast<ptrdiff_t>(i);
        size_t kept = dedup_hardlinks(first, items.begin() + static_cast<ptrdiff_t>(j));
        if (kept < 2) return;
        std::sort(first, first + static_cast<ptrdiff_t>(kept),
                  [](const DupCandidate &x, const DupCandidate &y) { return x.path < y.path; });
        DupGroup g;
        g.hash_hex = final_digest_hex(items[i], fast);
        g.size = items[i].size;
        for (size_t k = 0; k < kept; k++) {
            g.files.push_back(items[i + k].path);
            g.mtimes.push_back(items[i + k].mtime);
        }
        out.push_back(std::move(g));
    });
}

DupCandidate candidate_from_entry(const FileEntry &e, uint64_t mtime, cdc_hashcache_t *cache,
                                  DiskCache &dcache) {
    DupCandidate c;
    c.path = e.path;
    c.size = e.size;
    c.mtime = mtime;
    c.sort_key = e.sort_key;
    c.frn = e.frn;
    c.first_lcn = e.first_lcn;
    c.bytes_per_cluster = e.bytes_per_cluster;
    c.can_raw_read = e.can_raw_read;
    c.disk = dcache.key_for_path(e.path);
    c.is_ssd = c.disk.kind == DiskKind::Ssd;
    if (cache) {
        cdc_digest_set_t ce{};
        if (cdc_hashcache_entry_for(cache, e.path.c_str(), e.size, mtime, &ce) == 1) c.digests = ce;
    }
    return c;
}

// 盘符列表为空时取全部固定盘。
std::vector<std::string> roots_or_fixed(std::span<const char *const> roots) {
    if (!roots.empty()) return std::vector<std::string>(roots.begin(), roots.end());
    return fixed_drives();
}

}  // namespace

int full_hash_file(const char *path, uint64_t frn, uint8_t digest[32]) {
    HashOut h;
    if (full_hash_by_frn(path, frn, h, nullptr) != 0) return -1;
    std::memcpy(digest, h.digest, 32);
    return 0;
}

// ---------------- 哈希管线入口（对照 main.rs 的 hash_index_groups） ----------------

int scan_hash_groups(std::span<const ScanInput> inputs, cdc_hashcache_t *cache, uint64_t now,
                     std::vector<cdc_digest_update_t> &out_updates) {
    out_updates.clear();
    if (inputs.empty()) return 0;
    ScanOpts opts;  // fast = false, trace = false
    CandVec cands;
    cands.reserve(inputs.size());
    DiskCache dcache;
    for (const ScanInput &in : inputs) {
        if (!in.path) continue;
        DupCandidate c;
        c.path = in.path;
        c.size = in.size;
        c.mtime = in.mtime;
        c.frn = in.frn;
        c.can_raw_read = false;  // 索引行没有布局信息，不走 raw 快车道
        c.disk = dcache.key_for_path(c.path);
        c.is_ssd = c.disk.kind == DiskKind::Ssd;
        c.digests = in.digests;
        if (cache) {
            cdc_digest_set_t ce{};
            if (cdc_hashcache_entry_for(cache, in.path, in.size, in.mtime, &ce) == 1) {
                cdc_digest_set_t &d = c.digests;
                if (!d.has_head && ce.has_head) d.has_head = 1, d.head = ce.head;
                if (!d.has_sample && ce.has_sample) d.has_sample = 1, d.sample = ce.sample;
                if (!d.has_strong && ce.has_strong) d.has_strong = 1, d.strong = ce.strong;
                // 采样层只看 xxh 字段（旧版 BLAKE3 的三档只透传、不用于跳过）。
                if (!d.has_head_xxh && ce.has_head_xxh) d.has_head_xxh = 1, d.head_xxh = ce.head_xxh;
                if (!d.has_sample_xxh && ce.has_sample_xxh)
                    d.has_sample_xxh = 1, d.sample_xxh = ce.sample_xxh;
                if (!d.has_strong_xxh && ce.has_strong_xxh)
                    d.has_strong_xxh = 1, d.strong_xxh = ce.strong_xxh;
                if (!d.has_full && ce.has_full) d.has_full = 1, d.full = ce.full;
            }
        }
        cands.push_back(std::move(c));
    }
    if (cands.empty()) return 0;
    std::sort(cands.begin(), cands.end(), less_by_size);
    CandVec ready, ssd_stage1, mixed_stage1, final_items, eliminated;
    split_dup_candidates(cands, false, ready, ssd_stage1, mixed_stage1);
    run_dup_pipelines(std::move(ssd_stage1), std::move(mixed_stage1), cache, now, opts, "index-ssd",
                      "index-hdd", final_items, eliminated);
    // updates：ready + final + eliminated 中仍有任一摘要的。
    for (const CandVec *lane : {&ready, &final_items, &eliminated}) {
        for (const DupCandidate &c : *lane) {
            if (!cand_any_digest(c)) continue;
            cdc_digest_update_t u{};
            u.volume_serial = 0;  // 调用方按卷填（见 scan.h 注释）
            u.frn = c.frn;
            u.size = c.size;
            u.mtime = c.mtime;
            u.digests = c.digests;
            out_updates.push_back(u);
        }
    }
    return 0;
}

// ---------------- 实时扫描入口 ----------------

int scan_big(std::span<const char *const> drives, const ScanOpts &opts, BigFileList &out) {
    out.clear();
    for (const std::string &drive : roots_or_fixed(drives)) {
        if (is_cancelled(opts)) break;
        FileEntryList list;
        auto big_keep = [](std::string_view name, uint64_t size) {
            return size >= big_file_threshold_bytes(name);
        };
        if (scan_files(drive.c_str(), kScanMinBytes, kTopSkip, big_keep, list) != 0) continue;
        for (FileEntry &e : list) {
            if (is_system_file_name(e.path)) continue;
            if (e.size < big_file_threshold_bytes(e.path)) continue;
            BigFile bf;
            bf.mtime = e.mtime ? e.mtime : file_mtime(e.path.c_str());
            bf.path = std::move(e.path);
            bf.size = e.size;
            out.push_back(std::move(bf));
        }
    }
    std::sort(out.begin(), out.end(), [](const BigFile &x, const BigFile &y) {
        if (x.size != y.size) return x.size > y.size;  // size 降序
        return x.path < y.path;
    });
    return 0;
}

int scan_dups(std::span<const char *const> roots, uint64_t min_size, const ScanOpts &opts,
              DupGroupList &out) {
    out.clear();
    double started = now_mono();
    uint64_t min = min_size ? min_size : (1ULL << 20);
    std::vector<std::string> use_roots = roots_or_fixed(roots);
    if (use_roots.empty()) return 0;
    // 枚举（MFT 优先，失败退回遍历；系统文件在这里先滤掉）。
    FileEntryList all;
    for (const std::string &root : use_roots) {
        if (is_cancelled(opts)) break;
        FileEntryList list;
        if (scan_files(root.c_str(), min, kTopSkip, nullptr, list) != 0) continue;
        for (FileEntry &e : list)
            if (!is_system_file_name(e.path)) all.push_back(std::move(e));
    }
    if (is_cancelled(opts)) return 0;
    std::sort(all.begin(), all.end(), [](const FileEntry &x, const FileEntry &y) {
        if (x.size != y.size) return x.size < y.size;
        return x.path < y.path;
    });
    // 只有同 size ≥2 的才是候选。
    std::vector<std::pair<size_t, size_t>> runs;
    size_t candidates_count = 0;
    for (size_t i = 0; i < all.size();) {
        size_t j = i + 1;
        while (j < all.size() && all[j].size == all[i].size) j++;
        if (j - i >= 2) {
            runs.emplace_back(i, j);
            candidates_count += j - i;
        }
        i = j;
    }
    if (opts.trace)
        std::fprintf(stderr, "dup: 参与比对 %zu 个文件, 枚举用时 %.1fs\n", candidates_count,
                     now_mono() - started);
    if (candidates_count == 0) return 0;
    // 缓存预加载（只加载候选路径）。
    std::vector<const char *> paths;
    paths.reserve(candidates_count);
    for (auto [i, j] : runs)
        for (size_t m = i; m < j; m++) paths.push_back(all[m].path.c_str());
    cdc_hashcache_t *cache = cdc_hashcache_load_for_paths(paths.data(), paths.size());
    uint64_t stamp_now = now_seconds();
    CandVec cands;
    cands.reserve(candidates_count);
    DiskCache dcache;
    for (auto [i, j] : runs) {
        for (size_t m = i; m < j; m++) {
            const FileEntry &e = all[m];
            uint64_t mtime = e.mtime ? e.mtime : file_mtime(e.path.c_str());
            cands.push_back(candidate_from_entry(e, mtime, cache, dcache));
        }
    }
    all = FileEntryList();
    // cands 已按 size 排好（all 排过序，桶顺序保留）。
    CandVec ready, ssd_stage1, mixed_stage1;
    split_dup_candidates(cands, opts.fast, ready, ssd_stage1, mixed_stage1);
    if (!ssd_stage1.empty() || !mixed_stage1.empty()) {
        CandVec final_items, eliminated;
        run_dup_pipelines(std::move(ssd_stage1), std::move(mixed_stage1), cache, stamp_now, opts,
                          "ssd", "hdd+mixed", final_items, eliminated);
        if (is_cancelled(opts)) {
            if (cache) cdc_hashcache_close(cache);
            return 0;
        }
        for (DupCandidate &c : final_items)
            if (opts.fast ? c.digests.has_strong_xxh : c.digests.has_full)
                ready.push_back(std::move(c));
    }
    build_dup_groups(std::move(ready), opts.fast, out);
    sort_dup_groups(out);
    if (opts.trace)
        std::fprintf(stderr, "dup: 找到 %zu 组, 总用时 %.1fs\n", out.size(), now_mono() - started);
    if (cache) cdc_hashcache_close(cache);
    return 0;
}

// ---------------- 小工具 ----------------

uint64_t file_mtime(const char *path) {
    if (!path) return 0;
#ifdef _WIN32
    auto w = utf8_to_wide(path);
    if (!w) return 0;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(w->c_str(), GetFileExInfoStandard, &attr)) return 0;
    ULARGE_INTEGER ft;
    ft.LowPart = attr.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = attr.ftLastWriteTime.dwHighDateTime;
    return mft_filetime_to_unix(ft.QuadPart);
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime < 0 ? 0 : static_cast<uint64_t>(st.st_mtime);
#endif
}

std::vector<std::string> fixed_drives() {
    std::vector<std::string> drives;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if ((mask & (1u << i)) == 0) continue;
        wchar_t wroot[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        if (GetDriveTypeW(wroot) == DRIVE_FIXED) {
            char root[4] = {static_cast<char>('A' + i), ':', '\\', '\0'};
            drives.emplace_back(root);
        }
    }
    if (drives.empty()) drives.emplace_back("C:\\");
#endif
    return drives;
}

}  // namespace cdc
