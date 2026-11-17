/* scan.h — 大文件扫描与重复文件扫描，对照 desktop/src/main.rs 的
 * scan_big / scan_dups / run_dup_pipeline / run_dup_pipelines。
 *
 * 边界（与 Rust 版的有意差别，见 ARCHITECTURE.md）：
 *   - 索引优先（fileindex 查询）与 5 分钟 dups-*.json 缓存不在这里，
 *     属于调用方/CLI 层（切片 4）；这里只做纯实时扫描。
 *   - 并行：各哈希阶段（sample/strong/probe/full）与 Linux 的
 *     IOCP 等价路径用 fastio.c 的 cdc_par_for 按文件并行
 *     （生产者/消费者：主线程产下标、worker 消费；读与哈希
 *     在不同 worker 上重叠）；SSD/HDD 分档定 worker 数。
 *     结果与顺序版一致（分组前都按 (size, 摘要) 排序）。
 *   - 全哈希仍是单缓冲循环（≥16MB 用 2MB 缓冲）；单文件内
 *     再开读线程的收益很小，暂不做。
 *   - 取消：Rust 用全局 DUP_CANCEL + scan 取消标志；C 版用 opts->cancel
 *    （调用方随时置 1），各阶段边界检查，取消后返回空结果。
 */
#ifndef CDC_SCAN_H
#define CDC_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "fileindex.h" /* cdc_digest_set_t / cdc_digest_update_t */
#include "hashcache.h" /* cdc_hashcache_t */
#include "mft.h"       /* cdc_cache_dir */

#define CDC_SCAN_HEAD_CHUNK ((uint64_t)4096)
#define CDC_SCAN_SAMPLE_CHUNK ((uint64_t)16 * 1024)
#define CDC_SCAN_STRONG_CHUNK ((uint64_t)256 * 1024)
#define CDC_SCAN_PROBE_CHUNK ((uint64_t)64 * 1024)
#define CDC_SCAN_PROBE_THRESHOLD ((uint64_t)16 * 1024 * 1024)
#define CDC_SCAN_HDD_SEQ_THRESHOLD ((uint64_t)2 * 1024 * 1024)
#define CDC_SCAN_FULL_BUF_SIZE ((size_t)1024 * 1024)
/* ≥16MB 文件的全哈希改用 2MB 缓冲（与 Rust 的双缓冲分支同一档位；
 * C 版仍是单缓冲循环，大缓冲减少 read 调用次数）。 */
#define CDC_SCAN_FULL_BUF_LARGE ((size_t)2 * 1024 * 1024)
#define CDC_SCAN_FULL_LARGE_THRESHOLD ((uint64_t)16 * 1024 * 1024)

/* ---------------- 大文件 ---------------- */

typedef struct {
    char *path; /* malloc'd */
    uint64_t size;
    uint64_t mtime;
} CdcBigFile;

typedef struct {
    CdcBigFile *items;
    size_t n, cap;
} CdcBigFileList;

void cdc_big_file_list_free(CdcBigFileList *list);

/* ---------------- 重复组 ---------------- */

typedef struct {
    char hash_hex[65]; /* 全哈希（fast 模式为 strong）的 hex */
    uint64_t size;
    char **files; /* malloc'd，与 mtimes 一一对应，按 path 排序 */
    uint64_t *mtimes;
    size_t n_files;
} CdcDupGroup;

typedef struct {
    CdcDupGroup *items;
    size_t n, cap;
} CdcDupGroupList;

void cdc_dup_group_list_free(CdcDupGroupList *list);

/* ---------------- 扫描选项 ---------------- */

typedef struct {
    volatile int *cancel; /* 可为 NULL；调用方置 1 即取消 */
    void (*on_tick)(void *ctx); /* 每完成一个文件的一次摘要计算回调一次，可为 NULL */
    void *tick_ctx;
    int trace; /* 为真时打 Rust 版的 dup: / dup[label]: 行到 stderr */
    int fast;  /* 为真时 strong 即终局（快速模式），否则 full 为终局 */
} CdcScanOpts;

/* ================= slice 4 契约（main.c 的 CROSS-SLICE INTERFACE 块原样） =================
 * 待集成时 main.c 删掉那块、改 include 本头文件，调用点不用动。
 * cdc_digest_t / cdc_digest_set_t / cdc_digest_update_t 见 fileindex.h。
 */

/* 哈希管线的输入：文件 + 已有摘要（行里有的或缓存里有的都可先填；
 * 管线内部再用缓存补齐）。path 为借用指针。 */
typedef struct {
    const char *path;
    uint64_t size;
    uint64_t mtime;
    uint64_t frn;
    cdc_digest_set_t digests;
} cdc_scan_input_t;

/* 哈希管线：把 inputs 按 size 分组，只有同 size ≥2 的组才进分阶段哈希
 * （head 4KB → sample 16KB×3 → probe 64KB@25%/75%（>16MB）→ full；
 * fast=false）；候选摘要先用 inputs 自带的、再用 cache 补齐；新算出的
 * 摘要写回 cache（close 时落盘）。
 * 返回 0 时 *out_updates 为 malloc 的数组（每个"算完仍有任一摘要"的候选
 * 一条：已就绪的 + 跑完管线的 final/eliminated；可能为 NULL 且 *out_n=0），
 * 调用方 free；返回 -1 表示管线整体失败，调用方按"无新摘要"继续。
 * 注意：updates 的 volume_serial 填 0——inputs 里没有卷号，调用方
 * （main.c 的回填/增量）在 set_digests_batch 之前必须按卷填好，
 * 否则 WHERE volume_serial=?1 匹配不到行。 */
int cdc_scan_hash_groups(const cdc_scan_input_t *inputs, size_t n,
                         cdc_hashcache_t *cache, uint64_t now,
                         cdc_digest_update_t **out_updates, size_t *out_n);

/* ================= 实时扫描 ================= */

/* 大文件：按最低档 2MB 枚举（keep 过滤用文件名算分档阈值），再排除
 * 系统文件名、复核阈值（按路径算，与按名算等价，因阈值只看扩展名），
 * mtime 为 0 时读文件属性补齐；结果按 size 降序。返回 0 成功。 */
int cdc_scan_big(const char **drives, size_t n_drives, const CdcScanOpts *opts,
                 CdcBigFileList *out);

/* 重复文件：枚举（MFT 优先，失败退回遍历）→ 排除系统文件 → 大小分组
 * （同 size ≥2 才进管线）→ 分阶段哈希 → 按终局摘要分组建组；
 * 组内硬链接去重（file_id 相同的只留一个），组按 size×count 降序。
 * min_size 为 0 时取 1MB。返回 0 成功（取消也返回 0 但 out 为空）。 */
int cdc_scan_dups(const char **roots, size_t n_roots, uint64_t min_size,
                  const CdcScanOpts *opts, CdcDupGroupList *out);

/* ---------------- 小工具（供删除校验等复用） ---------------- */

/* 整文件 BLAKE3 全哈希（1MB 顺序缓冲；≥16MB 的 Rust 双缓冲在 C 版是
 * 单缓冲循环，结果一致）。frn!=0 时优先 OpenFileById。
 * 返回 0 成功并填 digest；-1 失败（打不开/读错/取消）。 */
int cdc_full_hash_file(const char *path, uint64_t frn, uint8_t digest[32]);

/* 文件的 mtime（unix 秒），取不到返回 0。 */
uint64_t cdc_file_mtime(const char *path);

/* 固定盘符列表（Windows：GetLogicalDrives + GetDriveTypeW==3；
 * 空时返回 C:\；Linux：返回 0 条）。drives 为调用方提供的
 * [cap][4] 缓冲（"C:\\" 形）。返回 0 成功。 */
int cdc_fixed_drives_buf(char (*drives)[4], size_t cap, size_t *out_n);

#endif
