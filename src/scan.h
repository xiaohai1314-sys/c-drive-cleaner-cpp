// scan.h — 大文件扫描与重复文件扫描，对照 desktop/src/main.rs 的
// scan_big / scan_dups / run_dup_pipeline / run_dup_pipelines。
//
// 边界（与 Rust 版的有意差别，见 ARCHITECTURE.md）：
//   - 索引优先（fileindex 查询）与 5 分钟 dups-*.json 缓存不在这里，
//     属于调用方/CLI 层；这里只做纯实时扫描。
//   - 并行：各哈希阶段（sample/strong/probe/full）与 Linux 的 IOCP 等价路径
//     用 fastio 的 par_for 按文件并行；SSD/HDD 分档定 worker 数。
//     结果与顺序版一致（分组前都按 (size, 摘要) 排序）。
//   - 全哈希仍是单缓冲循环（≥16MB 用 2MB 缓冲）。
//   - 取消：opts.cancel（调用方随时置 1），各阶段边界检查，取消后返回空结果。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fileindex.h"  // DigestSet / DigestUpdate
#include "hashcache.h"  // HashCache
#include "util.h"       // FunctionRef

namespace cdc {

inline constexpr uint64_t kScanHeadChunk = 4096;
inline constexpr uint64_t kScanSampleChunk = 16ULL * 1024;
inline constexpr uint64_t kScanStrongChunk = 256ULL * 1024;
inline constexpr uint64_t kScanProbeChunk = 64ULL * 1024;
inline constexpr uint64_t kScanProbeThreshold = 16ULL * 1024 * 1024;
inline constexpr uint64_t kScanHddSeqThreshold = 2ULL * 1024 * 1024;
inline constexpr size_t kScanFullBufSize = 1024 * 1024;
// ≥16MB 文件的全哈希改用 2MB 缓冲（与 Rust 的双缓冲分支同一档位）。
inline constexpr size_t kScanFullBufLarge = 2 * 1024 * 1024;
inline constexpr uint64_t kScanFullLargeThreshold = 16ULL * 1024 * 1024;

// ---------------- 大文件 ----------------
struct BigFile {
    std::string path;
    uint64_t size = 0;
    uint64_t mtime = 0;
};
using BigFileList = std::vector<BigFile>;

// ---------------- 重复组 ----------------
struct DupGroup {
    std::string hash_hex;  // 全哈希（fast 模式为 strong）的 hex
    uint64_t size = 0;
    std::vector<std::string> files;  // 与 mtimes 一一对应，按 path 排序
    std::vector<uint64_t> mtimes;
};
using DupGroupList = std::vector<DupGroup>;

// ---------------- 扫描选项 ----------------
struct ScanOpts {
    const volatile int *cancel = nullptr;  // 可为空；调用方置 1 即取消
    FunctionRef<void()> on_tick;  // 每完成一个文件的一次摘要计算回调一次，可为空
    // 进度上报（可为空）：on_stage 在枚举换盘 / 进入比对时回调；on_total 在
    // 候选总数确定（同 size ≥2 的文件数）时回调一次。GUI 用它驱动
    // “正在比对文件 123/456…”——没有它，全盘扫描全程只显示“正在枚举文件…”。
    FunctionRef<void(const char *)> on_stage;
    FunctionRef<void(uint64_t)> on_total;
    bool trace = false;  // 为真时打 Rust 版的 dup: / dup[label]: 行到 stderr
    bool fast = false;   // 为真时 strong 即终局（快速模式），否则 full 为终局
};

// 哈希管线的输入：文件 + 已有摘要（管线内部再用缓存补齐）。path 为借用指针。
struct ScanInput {
    const char *path = nullptr;
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint64_t frn = 0;
    DigestSet digests{};
};

// 哈希管线：把 inputs 按 size 分组，只有同 size ≥2 的组才进分阶段哈希
// （head 4KB → sample 16KB×3 → probe 64KB@25%/75%（>16MB）→ full）；
// 候选摘要先用 inputs 自带的、再用 cache 补齐；新算出的摘要写回 cache。
// 返回 0 时 out_updates 为每个"算完仍有任一摘要"的候选一条；-1 表示管线
// 整体失败。注意：updates 的 volume_serial 填 0——调用方在
// set_digests_batch 之前必须按卷填好。
int scan_hash_groups(std::span<const ScanInput> inputs, HashCache *cache, uint64_t now,
                     std::vector<DigestUpdate> &out_updates);

// 大文件：按最低档 2MB 枚举（keep 过滤用文件名算分档阈值），再排除系统文件名、
// 复核阈值，mtime 为 0 时读文件属性补齐；结果按 size 降序。返回 0 成功。
// drives 为空时扫全部固定盘。
int scan_big(std::span<const char *const> drives, const ScanOpts &opts, BigFileList &out);

// 重复文件：枚举 → 排除系统文件 → 大小分组 → 分阶段哈希 → 按终局摘要分组建组；
// 组内硬链接去重，组按 size×count 降序。min_size 为 0 时取 1MB。
// 返回 0 成功（取消也返回 0 但 out 为空）。roots 为空时扫全部固定盘。
int scan_dups(std::span<const char *const> roots, uint64_t min_size, const ScanOpts &opts,
              DupGroupList &out);

// ---------------- 小工具 ----------------

// 整文件 BLAKE3 全哈希。frn!=0 时优先 OpenFileById。0 成功；-1 失败。
int full_hash_file(const char *path, uint64_t frn, uint8_t digest[32]);
// 文件的 mtime（unix 秒），取不到返回 0。
uint64_t file_mtime(const char *path);
// 固定盘符列表（Windows：GetLogicalDrives + DRIVE_FIXED；空时返回 C:\；Linux：空）。
std::vector<std::string> fixed_drives();

}  // namespace cdc
