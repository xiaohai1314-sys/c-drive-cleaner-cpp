// fastio.h — 高速文件读取原语，对照 desktop/src/fastio.rs。
//
// Windows：卷句柄 LCN 合并直读 + OpenFileById + IOCP 异步重叠 I/O。
// Linux：等价的顺序实现（pread/read + stat），保证流水线全平台可跑、可测。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "raii.h"
#include "util.h"

namespace cdc {

// 与 fastio.rs 对应的打开标志（Windows FILE_FLAG_*；Linux 下映射为 posix_fadvise）。
inline constexpr uint32_t kFileFlagRandomAccess = 0x10000000u;
inline constexpr uint32_t kFileFlagSequentialScan = 0x08000000u;
inline constexpr uint32_t kFileFlagOverlapped = 0x40000000u;
inline constexpr uint32_t kFileFlagBackupSemantics = 0x02000000u;

struct FileId {
    uint32_t volume_serial = 0;
    uint64_t file_index = 0;
    bool operator==(const FileId &o) const {
        return volume_serial == o.volume_serial && file_index == o.file_index;
    }
};

// OpenFileById 用的盘符根句柄（Windows）；Linux 恒为空。
struct DriveHintCloser {
    void operator()(void *h) const;
};
using DriveHint = std::unique_ptr<void, DriveHintCloser>;
DriveHint open_drive_hint(char drive_letter);

// 已打开的只读文件（Windows HANDLE / Linux fd），move-only。
class File {
public:
    File() = default;
    File(File &&) noexcept = default;
    File &operator=(File &&) noexcept = default;
    explicit operator bool() const { return static_cast<bool>(h_); }

    // 打开并校验：SKIP 属性返回空 File；frn!=0 且 hint 非空时优先 OpenFileById，
    // 失败回退按路径打开。has_file_id：仅 links>1 且 index!=0 时为 true；
    // actual_size_out 为句柄看到的大小（失败时不写）。三个出参均可为空。
    static File open_checked(const char *path, uint64_t frn, uint32_t flags, void *hint,
                             bool *has_file_id, FileId *file_id_out, uint64_t *actual_size_out);

    // 带偏移读。返回 0 成功，out_n 为实际读到的字节（文件尾可短于 len）；-1 为错误。
    int read_at(uint64_t offset, uint8_t *buf, size_t len, size_t &out_n);
    // 顺序读。返回 0 成功（EOF 时 out_n < len）；-1 为读取错误。
    int read_seq(uint8_t *buf, size_t len, size_t &out_n);
    // 硬链接标识：links>1 且 index!=0 时返回 true 并填 out。
    bool identity(FileId &out) const;

private:
#ifdef _WIN32
    UniqueHandle h_;
#else
    UniqueFd h_;
#endif
};

// 仅查元数据标识：不读内容。返回 true=有硬链接标识（links>1）。
bool query_file_identity(const char *path, FileId &out);

struct RawHeadTarget {
    size_t idx = 0;
    uint64_t offset = 0;
    size_t want = 0;
};

struct HeadResult {
    size_t idx = 0;
    uint8_t digest[32] = {};  // XXH3-64 载体编码（见 sampling.h），不是 BLAKE3
};

using TickFn = FunctionRef<void()>;

// 卷句柄 LCN 合并直读。返回成功算出摘要的结果。Linux：恒返回空。
std::vector<HeadResult> coalesced_raw_volume_heads(char drive_letter,
                                                   std::span<const RawHeadTarget> targets,
                                                   bool is_ssd, const volatile int *cancel,
                                                   TickFn on_tick);

struct IocpRequest {
    size_t idx = 0;
    const char *path = nullptr;  // 借用（调用方拥有）
    uint64_t frn = 0;
    size_t want = 0;
};

struct IocpResult {
    size_t idx = 0;
    uint8_t digest[32] = {};  // XXH3-64 载体编码（见 sampling.h），不是 BLAKE3
    bool has_file_id = false;
    FileId file_id;
};

// OpenFileById + IOCP 批量读头。Windows：真 IOCP；Linux：并行等价实现。
// 每个请求恰好回调一次 on_tick（成功、失败、跳过都算；取消后未处理的不回调）。
std::vector<IocpResult> iocp_read_heads(std::span<const IocpRequest> reqs, size_t concurrency,
                                        const volatile int *cancel, TickFn on_tick);

// 磁盘类型探测（frag.rs 的 is_ssd / physical_disk_id 的最小移植）。
// is_ssd：1=SSD，0=HDD，-1=未知（调用方按 SSD 处理，与 Rust 的 unwrap_or(true) 一致）。
int is_ssd(const char *drive);
// 0=成功并填出参。
int physical_disk_id(const char *drive, uint32_t &dev_type, uint32_t &dev_num);

// ---------------- 并行（线程池式 parallel-for） ----------------

// 把下标 [0,n) 分给 worker，每个下标恰好被处理一次；调用线程也参与干活。
// nthreads<=1 或 n<=1 时顺序执行；建线程失败时退化为更少线程（不报错、不丢下标）。
// fn 内共享的回调与共享小状态必须用 shared_mutex() 串行化。
// 返回 0 成功；-1 仅表示 fn 为空。
int par_for(size_t n, size_t nthreads, FunctionRef<void(size_t)> fn);
size_t par_cpu_count();  // 在线逻辑 CPU 数（>=1）
// 哈希阶段的 worker 数策略：SSD 激进（min(ncpu, 32)）、HDD 保守（min(ncpu, 4)）。
size_t par_hash_workers(bool is_ssd);

// 共享小状态锁：只包极短临界区，不跨越 I/O。
Mutex &shared_mutex();

}  // namespace cdc
