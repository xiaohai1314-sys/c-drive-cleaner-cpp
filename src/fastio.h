/* fastio.h — 高速 Windows 文件读取原语，对照 desktop/src/fastio.rs。
 *
 * Windows：卷句柄 LCN 合并直读 + OpenFileById + IOCP 异步重叠 I/O。
 * Linux：等价的顺序实现（pread/read + stat），保证流水线全平台可跑、可测。
 */
#ifndef CDC_FASTIO_H
#define CDC_FASTIO_H

#include <stddef.h>
#include <stdint.h>

/* 与 fastio.rs 对应的打开标志（Windows FILE_FLAG_*；Linux 下映射为 posix_fadvise）。 */
#define CDC_FILE_FLAG_RANDOM_ACCESS 0x10000000u
#define CDC_FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define CDC_FILE_FLAG_OVERLAPPED 0x40000000u
#define CDC_FILE_FLAG_BACKUP_SEMANTICS 0x02000000u

typedef struct {
    uint32_t volume_serial;
    uint64_t file_index;
} CdcFileId;

typedef struct CdcFile CdcFile; /* 不透明：Windows HANDLE / Linux fd */

/* 打开并校验：SKIP 属性返回 NULL；frn!=0 且 hint 非空时优先 OpenFileById，
 * 失败回退按路径打开。hint 为 cdc_open_drive_hint 的返回值（可为 NULL）。
 * has_file_id：仅 n_number_of_links>1 且 index!=0 时为 1；actual_size_out
 * 为句柄看到的大小（失败时不写）。 */
CdcFile *cdc_file_open_checked(const char *path, uint64_t frn, uint32_t flags, void *hint,
                               int *has_file_id, CdcFileId *file_id_out,
                               uint64_t *actual_size_out);
void cdc_file_close(CdcFile *f);

/* 带偏移读（不改顺序读位置）。返回 0 成功，*out_n 为实际读到的字节
 * （文件尾可短于 len）；-1 为寻道/读取错误。 */
int cdc_file_read_at(CdcFile *f, uint64_t offset, uint8_t *buf, size_t len, size_t *out_n);

/* 顺序读。返回 0 成功（EOF 时 *out_n < len）；-1 为读取错误。 */
int cdc_file_read_seq(CdcFile *f, uint8_t *buf, size_t len, size_t *out_n);

/* 查已打开文件的硬链接标识：links>1 且 index!=0 时返回 1 并填 out。 */
int cdc_file_identity(CdcFile *f, CdcFileId *out);

/* 仅查元数据标识：不读内容。返回 1=有硬链接标识（links>1），0=无/打不开。 */
int cdc_query_file_identity(const char *path, CdcFileId *out);

/* 仅 Windows：打开盘符根句柄（OpenFileById 的 hint）；用完 CloseHandle。
 * Linux 返回 NULL。 */
void *cdc_open_drive_hint(char drive_letter);

typedef struct {
    size_t idx;
    uint64_t offset;
    size_t want;
} CdcRawHeadTarget;

typedef struct {
    size_t idx;
    uint8_t digest[32]; /* XXH3-64 载体编码（见 sampling.h），不是 BLAKE3 */
} CdcHeadResult;

/* 卷句柄 LCN 合并直读。返回成功算出摘要的条数（写入 out，容量 n）。
 * Linux：恒返回 0（由调用方走 IOCP 等价路径）。 */
size_t cdc_coalesced_raw_volume_heads(char drive_letter, const CdcRawHeadTarget *targets,
                                      size_t n, int is_ssd, volatile int *cancel,
                                      void (*on_tick)(void *), void *tick_ctx,
                                      CdcHeadResult *out);

typedef struct {
    size_t idx;
    char *path; /* 调用方拥有 */
    uint64_t frn;
    size_t want;
} CdcIocpRequest;

typedef struct {
    size_t idx;
    uint8_t digest[32]; /* XXH3-64 载体编码（见 sampling.h），不是 BLAKE3 */
    int has_file_id;
    CdcFileId file_id;
} CdcIocpResult;

/* OpenFileById + IOCP 批量读头。Windows：真 IOCP；Linux：顺序等价实现。
 * 每个请求恰好回调一次 on_tick（成功、失败、跳过都算）。返回结果条数。 */
size_t cdc_iocp_read_heads(const CdcIocpRequest *reqs, size_t n, size_t concurrency,
                           volatile int *cancel, void (*on_tick)(void *), void *tick_ctx,
                           CdcIocpResult *out);

/* 磁盘类型探测（frag.rs 的 is_ssd / physical_disk_id 的最小移植）。
 * cdc_is_ssd：1=SSD，0=HDD，-1=未知（调用方按 SSD 处理，与 Rust 的 unwrap_or(true) 一致）。
 * cdc_physical_disk_id：0=成功并填 out。 */
int cdc_is_ssd(const char *drive);
int cdc_physical_disk_id(const char *drive, uint32_t *dev_type, uint32_t *dev_num);

/* ---------------- 并行（线程池式 parallel-for） ---------------- */
/* 跨平台线程池：POSIX pthread / Windows CreateThread（复用 mft.c 的
 * 跨平台线程模式：调用线程也参与干活）。
 * cdc_par_for(n, nthreads, fn, ctx)：把下标 [0,n) 分给 worker，
 * 每个下标恰好被一个 worker 处理一次；nthreads<=1 或 n<=1 时顺序执行；
 * 建线程失败时退化为更少线程（剩余工作由已建线程 + 调用线程跑完，
 * 不报错、不丢下标）。fn 不得假设调用顺序。
 * fn 内共享的回调（如 on_tick）与共享小状态（如 drive hint 缓存）
 * 必须用 cdc_shared_lock() 串行化。
 * 返回 0 成功；-1 仅表示 fn 为 NULL。 */
typedef void (*cdc_par_work_fn)(void *ctx, size_t idx);
int cdc_par_for(size_t n, size_t nthreads, cdc_par_work_fn fn, void *ctx);
/* 在线逻辑 CPU 数（>=1）。 */
size_t cdc_par_cpu_count(void);
/* 哈希阶段的 worker 数策略：SSD 激进（min(ncpu, 32)）、HDD 保守
 *（min(ncpu, 4)）；至少 1。NVMe 上放宽并发上限即指 SSD 这档。 */
size_t cdc_par_hash_workers(int is_ssd);
/* 共享小状态锁：并行 worker 回调 on_tick、读写 drive hint 缓存等
 * 共享小状态时持有。只包极短临界区，不跨越 I/O。 */
void cdc_shared_lock(void);
void cdc_shared_unlock(void);

#endif
