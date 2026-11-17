// fastio.cpp — 高速文件读取原语，逐行对照 desktop/src/fastio.rs。
//
// Windows：CreateFileW / OpenFileById / IOCP 均为真实实现。
// Linux：open/pread/read + stat 的等价实现（frn 被忽略，硬链接标识用
// st_nlink/st_ino），保证 scan 流水线在 Linux 下可跑、可测。
#include "fastio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "mft.h"       // kMftSkipFileAttrs
#include "sampling.h"  // 头摘要是 XXH3-64（xxh3_buf_to_digest32）

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cdc {
namespace {

Mutex g_shared_mutex;

// on_tick 串行化：回调可能非线程安全，并行路径下多个 worker 会同时调它。
void call_tick(TickFn on_tick) {
    if (!on_tick) return;
    LockGuard g(g_shared_mutex);
    on_tick();
}

bool is_cancelled(const volatile int *cancel) { return cancel && *cancel; }

#ifdef _WIN32
char upper_letter(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
#endif

}  // namespace

Mutex &shared_mutex() { return g_shared_mutex; }

#ifdef _WIN32

void DriveHintCloser::operator()(void *h) const {
    if (h) CloseHandle(static_cast<HANDLE>(h));
}

DriveHint open_drive_hint(char drive_letter) {
    wchar_t root[4] = {static_cast<wchar_t>(upper_letter(drive_letter)), L':', L'\\', 0};
    HANDLE h = CreateFileW(root, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    return DriveHint(h == INVALID_HANDLE_VALUE ? nullptr : h);
}

namespace {

UniqueHandle open_raw_handle_by_id_or_path(void *hint, uint64_t frn, const char *path,
                                           DWORD flags) {
    if (frn != 0 && hint) {
        FILE_ID_DESCRIPTOR desc{};
        desc.dwSize = static_cast<DWORD>(sizeof desc);
        desc.Type = FileIdType;
        desc.FileId.QuadPart = static_cast<LONGLONG>(frn);
        UniqueHandle h = adopt_handle(OpenFileById(static_cast<HANDLE>(hint), &desc, GENERIC_READ,
                                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                       FILE_SHARE_DELETE,
                                                   nullptr, flags));
        if (h) return h;
    }
    auto w = utf8_to_wide(path);
    if (!w) return nullptr;
    return adopt_handle(CreateFileW(w->c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, flags, nullptr));
}

bool info_to_file_id(const BY_HANDLE_FILE_INFORMATION &info, FileId &out) {
    uint64_t index = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    if (info.nNumberOfLinks > 1 && index != 0) {
        out.volume_serial = info.dwVolumeSerialNumber;
        out.file_index = index;
        return true;
    }
    return false;
}

UniqueHandle open_volume(char drive_letter, DWORD flags) {
    wchar_t name[8];
    std::swprintf(name, 8, L"\\\\.\\%c:", static_cast<wchar_t>(upper_letter(drive_letter)));
    return adopt_handle(CreateFileW(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, flags, nullptr));
}

}  // namespace

File File::open_checked(const char *path, uint64_t frn, uint32_t flags, void *hint,
                        bool *has_file_id, FileId *file_id_out, uint64_t *actual_size_out) {
    File f;
    UniqueHandle h = open_raw_handle_by_id_or_path(hint, frn, path, static_cast<DWORD>(flags));
    if (!h) return f;
    if (has_file_id) *has_file_id = false;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h.get(), &info)) {
        if (info.dwFileAttributes & kMftSkipFileAttrs) return f;
        if (has_file_id && file_id_out) *has_file_id = info_to_file_id(info, *file_id_out);
        if (actual_size_out)
            *actual_size_out = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    } else if (actual_size_out) {
        LARGE_INTEGER sz;
        *actual_size_out = GetFileSizeEx(h.get(), &sz) ? static_cast<uint64_t>(sz.QuadPart) : 0;
    }
    f.h_ = std::move(h);
    return f;
}

int File::read_at(uint64_t offset, uint8_t *buf, size_t len, size_t &out_n) {
    out_n = 0;
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h_.get(), li, nullptr, FILE_BEGIN)) return -1;
    return read_seq(buf, len, out_n);
}

int File::read_seq(uint8_t *buf, size_t len, size_t &out_n) {
    out_n = 0;
    size_t total = 0;
    while (total < len) {
        DWORD read = 0;
        if (!ReadFile(h_.get(), buf + total, static_cast<DWORD>(len - total), &read, nullptr))
            return -1;
        if (read == 0) break;
        total += read;
    }
    out_n = total;
    return 0;
}

bool File::identity(FileId &out) const {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h_.get(), &info)) return false;
    return info_to_file_id(info, out);
}

bool query_file_identity(const char *path, FileId &out) {
    auto w = utf8_to_wide(path);
    if (!w) return false;
    UniqueHandle h = adopt_handle(CreateFileW(w->c_str(), FILE_READ_ATTRIBUTES,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                              nullptr));
    if (!h) return false;
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(h.get(), &info) && info_to_file_id(info, out);
}

#else  // !_WIN32

void DriveHintCloser::operator()(void *) const {}

DriveHint open_drive_hint(char) { return nullptr; }

namespace {

bool stat_to_file_id(const struct stat &st, FileId &out) {
    if (st.st_nlink > 1 && st.st_ino != 0) {
        out.volume_serial = static_cast<uint32_t>(st.st_dev);
        out.file_index = static_cast<uint64_t>(st.st_ino);
        return true;
    }
    return false;
}

}  // namespace

File File::open_checked(const char *path, uint64_t, uint32_t flags, void *, bool *has_file_id,
                        FileId *file_id_out, uint64_t *actual_size_out) {
    File f;
    UniqueFd fd = adopt_fd(::open(path, O_RDONLY | O_CLOEXEC));
    if (!fd) return f;
    if (flags & kFileFlagRandomAccess)
        posix_fadvise(fd_of(fd), 0, 0, POSIX_FADV_RANDOM);
    else if (flags & kFileFlagSequentialScan)
        posix_fadvise(fd_of(fd), 0, 0, POSIX_FADV_SEQUENTIAL);
    if (has_file_id) *has_file_id = false;
    struct stat st;
    if (fstat(fd_of(fd), &st) == 0) {
        if (has_file_id && file_id_out) *has_file_id = stat_to_file_id(st, *file_id_out);
        if (actual_size_out) *actual_size_out = static_cast<uint64_t>(st.st_size);
    } else if (actual_size_out) {
        *actual_size_out = 0;
    }
    f.h_ = std::move(fd);
    return f;
}

int File::read_at(uint64_t offset, uint8_t *buf, size_t len, size_t &out_n) {
    out_n = 0;
    size_t total = 0;
    while (total < len) {
        ssize_t r = pread(fd_of(h_), buf + total, len - total, static_cast<off_t>(offset + total));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        total += static_cast<size_t>(r);
    }
    out_n = total;
    return 0;
}

int File::read_seq(uint8_t *buf, size_t len, size_t &out_n) {
    out_n = 0;
    size_t total = 0;
    while (total < len) {
        ssize_t r = ::read(fd_of(h_), buf + total, len - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        total += static_cast<size_t>(r);
    }
    out_n = total;
    return 0;
}

bool File::identity(FileId &out) const {
    struct stat st;
    return fstat(fd_of(h_), &st) == 0 && stat_to_file_id(st, out);
}

bool query_file_identity(const char *path, FileId &out) {
    struct stat st;
    return stat(path, &st) == 0 && stat_to_file_id(st, out);
}

#endif  // _WIN32

// ---------------- 卷句柄 LCN 合并直读（仅 Windows） ----------------

std::vector<HeadResult> coalesced_raw_volume_heads(char drive_letter,
                                                   std::span<const RawHeadTarget> targets,
                                                   bool is_ssd, const volatile int *cancel,
                                                   TickFn on_tick) {
    std::vector<HeadResult> out;
#ifdef _WIN32
    if (targets.empty()) return out;
    UniqueHandle vol =
        open_volume(drive_letter, is_ssd ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN);
    if (!vol) return out;
    std::vector<RawHeadTarget> sorted(targets.begin(), targets.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const RawHeadTarget &a, const RawHeadTarget &b) { return a.offset < b.offset; });
    uint64_t max_gap = is_ssd ? 16u * 1024 : 256u * 1024;
    uint64_t max_span = is_ssd ? 256u * 1024 : 512u * 1024;
    std::vector<uint8_t> buf(static_cast<size_t>(max_span));
    size_t n = sorted.size(), i = 0;
    while (i < n) {
        if (is_cancelled(cancel)) break;
        uint64_t start_off = sorted[i].offset;
        uint64_t end_off = start_off + 4096;
        size_t j = i + 1;
        while (j < n) {
            uint64_t next_off = sorted[j].offset;
            uint64_t next_end = next_off + 4096;
            if (next_off <= end_off + max_gap && next_end - start_off <= max_span) {
                if (next_end > end_off) end_off = next_end;
                j++;
            } else {
                break;
            }
        }
        auto span_len = static_cast<size_t>(end_off - start_off);
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(start_off);
        if (SetFilePointerEx(vol.get(), li, nullptr, FILE_BEGIN)) {
            size_t got = 0;
            while (got < span_len) {
                DWORD read = 0;
                if (!ReadFile(vol.get(), buf.data() + got, static_cast<DWORD>(span_len - got), &read,
                              nullptr) ||
                    read == 0)
                    break;
                got += read;
            }
            if (got == span_len) {
                for (size_t k = i; k < j; k++) {
                    auto rel = static_cast<size_t>(sorted[k].offset - start_off);
                    if (rel + sorted[k].want <= span_len) {
                        HeadResult r;
                        r.idx = sorted[k].idx;
                        xxh3_buf_to_digest32(buf.data() + rel, sorted[k].want, r.digest);
                        out.push_back(r);
                        call_tick(on_tick);
                    }
                }
            }
        }
        i = j;
    }
#else
    (void)drive_letter;
    (void)targets;
    (void)is_ssd;
    (void)cancel;
    (void)on_tick;
#endif
    return out;
}

// ---------------- IOCP 批量读头 ----------------

#ifdef _WIN32

namespace {

struct IocpSlot {
    OVERLAPPED ov;
    UniqueHandle handle;
    size_t item_idx = 0;
    size_t want = 0;
    bool active = false;
    uint8_t buf[4096];
};

class IocpBatch {
public:
    IocpBatch(HANDLE port, std::vector<IocpSlot> &slots, std::span<const IocpRequest> reqs,
              const volatile int *cancel, TickFn on_tick, std::vector<IocpResult> &out)
        : port_(port), slots_(slots), reqs_(reqs), cancel_(cancel), on_tick_(on_tick), out_(out) {}

    // true=投递了一路在途 I/O，false=请求耗尽或已取消。
    bool submit_next(size_t slot_idx) {
        IocpSlot &slot = slots_[slot_idx];
        while (next_req_ < reqs_.size()) {
            const IocpRequest &req = reqs_[next_req_++];
            if (is_cancelled(cancel_)) return false;
            char drive_ch = upper_letter(req.path && req.path[0] ? req.path[0] : 'C');
            void *hint = nullptr;
            if (req.frn != 0 && drive_ch >= 'A' && drive_ch <= 'Z') {
                int di = drive_ch - 'A';
                if (!hints_tried_[di]) {
                    hints_tried_[di] = true;
                    hints_[di] = open_drive_hint(drive_ch);
                }
                hint = hints_[di].get();
            }
            UniqueHandle h = open_raw_handle_by_id_or_path(hint, req.frn, req.path,
                                                           FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS);
            if (!h) {
                call_tick(on_tick_);
                continue;
            }
            if (req.frn == 0) {
                BY_HANDLE_FILE_INFORMATION info;
                // 提交时算出的 file_id 被丢弃（Rust 的 let _ = file_id）；
                // 返回的 file_id 只取完成时的那次查询。
                if (GetFileInformationByHandle(h.get(), &info) &&
                    (info.dwFileAttributes & kMftSkipFileAttrs)) {
                    call_tick(on_tick_);
                    continue;
                }
            }
            if (!CreateIoCompletionPort(h.get(), port_, static_cast<ULONG_PTR>(slot_idx), 0)) {
                call_tick(on_tick_);
                continue;
            }
            size_t want = req.want < 4096 ? req.want : 4096;
            if (want == 0) {
                IocpResult r;
                r.idx = req.idx;
                xxh3_buf_to_digest32(nullptr, 0, r.digest);
                out_.push_back(r);
                call_tick(on_tick_);
                continue;
            }
            std::memset(&slot.ov, 0, sizeof slot.ov);
            slot.item_idx = req.idx;
            slot.want = want;
            slot.active = true;
            DWORD dummy = 0;
            if (!ReadFile(h.get(), slot.buf, static_cast<DWORD>(want), &dummy, &slot.ov) &&
                GetLastError() != ERROR_IO_PENDING) {
                slot.active = false;
                call_tick(on_tick_);
                continue;
            }
            slot.handle = std::move(h);
            return true;
        }
        return false;
    }

private:
    HANDLE port_;
    std::vector<IocpSlot> &slots_;
    std::span<const IocpRequest> reqs_;
    size_t next_req_ = 0;
    const volatile int *cancel_;
    TickFn on_tick_;
    std::vector<IocpResult> &out_;
    DriveHint hints_[26];
    bool hints_tried_[26] = {};
};

}  // namespace

std::vector<IocpResult> iocp_read_heads(std::span<const IocpRequest> reqs, size_t concurrency,
                                        const volatile int *cancel, TickFn on_tick) {
    std::vector<IocpResult> out;
    if (reqs.empty()) return out;
    UniqueHandle port(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0));
    if (!port) return out;
    size_t pool_size = concurrency < 1 ? 1 : concurrency;
    // 上限 128：NVMe 上 SSD 批放宽到 64 在途仍有余量（见 scan.cpp 调用处）。
    if (pool_size > 128) pool_size = 128;
    if (pool_size > reqs.size()) pool_size = reqs.size();
    std::vector<IocpSlot> slots(pool_size);
    std::vector<size_t> free_slots;
    free_slots.reserve(pool_size);
    for (size_t i = 0; i < pool_size; i++) free_slots.push_back(pool_size - 1 - i);
    IocpBatch batch(port.get(), slots, reqs, cancel, on_tick, out);
    size_t in_flight = 0;
    while (!free_slots.empty()) {
        size_t slot_idx = free_slots.back();
        free_slots.pop_back();
        if (!batch.submit_next(slot_idx)) break;
        in_flight++;
    }
    while (in_flight > 0) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(port.get(), &bytes, &key, &ov, INFINITE);
        if (!ov) break;
        IocpSlot &slot = slots[static_cast<size_t>(key)];
        if (ok && bytes > 0) {
            IocpResult r;
            size_t m = static_cast<size_t>(bytes) < slot.want ? static_cast<size_t>(bytes) : slot.want;
            xxh3_buf_to_digest32(slot.buf, m, r.digest);
            r.idx = slot.item_idx;
            BY_HANDLE_FILE_INFORMATION info;
            if (GetFileInformationByHandle(slot.handle.get(), &info))
                r.has_file_id = info_to_file_id(info, r.file_id);
            out.push_back(r);
        }
        slot.handle.reset();
        slot.active = false;
        in_flight--;
        call_tick(on_tick);
        if (!is_cancelled(cancel) && batch.submit_next(static_cast<size_t>(key))) {
            in_flight++;
        } else if (is_cancelled(cancel)) {
            for (IocpSlot &s : slots)
                if (s.active && s.handle) CancelIoEx(s.handle.get(), nullptr);
        }
    }
    return out;  // slots / 端口 / drive hint 句柄由 RAII 关闭
}

#else  // !_WIN32

// Linux 等价路径：每个请求独立（打开、读 ≤4KB、哈希），用 par_for 按下标分给
// worker；结果先写 slot[i]（下标互斥），再按请求顺序压缩，输出顺序与顺序版一致。
std::vector<IocpResult> iocp_read_heads(std::span<const IocpRequest> reqs, size_t concurrency,
                                        const volatile int *cancel, TickFn on_tick) {
    std::vector<IocpResult> slots(reqs.size());
    std::vector<uint8_t> valid(reqs.size(), 0);
    // 并发数沿用调用方的 SSD/HDD 分档；为 0 时退回 CPU 数。
    size_t workers = concurrency ? concurrency : par_cpu_count();
    par_for(reqs.size(), workers, [&](size_t i) {
        if (is_cancelled(cancel)) return;  // 未 tick，与顺序版 break 一致
        const IocpRequest &req = reqs[i];
        bool has_id = false;
        FileId fid;
        uint64_t actual = 0;
        File f = File::open_checked(req.path, req.frn, kFileFlagOverlapped | kFileFlagRandomAccess,
                                    nullptr, &has_id, &fid, &actual);
        if (!f) {
            call_tick(on_tick);
            return;
        }
        size_t want = req.want < 4096 ? req.want : 4096;
        IocpResult &r = slots[i];
        if (want == 0) {
            xxh3_buf_to_digest32(nullptr, 0, r.digest);
            r.idx = req.idx;
            valid[i] = 1;
        } else {
            uint8_t buf[4096];
            size_t got = 0;
            if (f.read_seq(buf, want, got) == 0 && got > 0) {
                xxh3_buf_to_digest32(buf, got, r.digest);
                r.idx = req.idx;
                r.has_file_id = has_id;
                r.file_id = fid;
                valid[i] = 1;
            }
        }
        call_tick(on_tick);
    });
    std::vector<IocpResult> out;
    for (size_t i = 0; i < slots.size(); i++)
        if (valid[i]) out.push_back(slots[i]);
    return out;
}

#endif  // _WIN32

// ---------------- 磁盘类型探测 ----------------

#ifdef _WIN32

namespace {

UniqueHandle open_volume_noaccess(const char *drive) {
    if (!drive || !drive[0]) return nullptr;
    char letter = upper_letter(drive[0]);
    if (letter < 'A' || letter > 'Z') return nullptr;
    wchar_t name[8];
    std::swprintf(name, 8, L"\\\\.\\%c:", static_cast<wchar_t>(letter));
    return adopt_handle(CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, 0, nullptr));
}

}  // namespace

int is_ssd(const char *drive) {
    UniqueHandle h = open_volume_noaccess(drive);
    if (!h) return -1;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR desc{};
    DWORD ret = 0;
    if (!DeviceIoControl(h.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query,
                         static_cast<DWORD>(sizeof query), &desc, static_cast<DWORD>(sizeof desc),
                         &ret, nullptr))
        return -1;
    return desc.IncursSeekPenalty == 0 ? 1 : 0;
}

int physical_disk_id(const char *drive, uint32_t &dev_type, uint32_t &dev_num) {
    UniqueHandle h = open_volume_noaccess(drive);
    if (!h) return -1;
    STORAGE_DEVICE_NUMBER info{};
    DWORD ret = 0;
    if (!DeviceIoControl(h.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &info,
                         static_cast<DWORD>(sizeof info), &ret, nullptr))
        return -1;
    dev_type = info.DeviceType;
    dev_num = info.DeviceNumber;
    return 0;
}

#else

int is_ssd(const char *) { return -1; }
int physical_disk_id(const char *, uint32_t &, uint32_t &) { return -1; }

#endif

// ---------------- 并行（线程池式 parallel-for） ----------------
// 读与哈希的重叠主要靠跨文件并行：worker A 在读盘时 worker B 在哈希，
// 另有内核 readahead（SEQUENTIAL_SCAN 的 posix_fadvise）做单文件内的预读。
// 线程用 CreateThread / pthread（不用 std::thread：mingw-gcc 的 win32 线程
// 模型没有它）；调用线程也参与干活，建线程失败时退化为更少线程。

namespace {

constexpr size_t kParMaxThreads = 32;
constexpr size_t kParSsdMaxWorkers = 32;
constexpr size_t kParHddMaxWorkers = 4;

struct ParState {
    FunctionRef<void(size_t)> fn;
    size_t n = 0;
    size_t next = 0;  // 下一个待取下标（lock 保护）
    Mutex lock;

    // 取下标、干活，直到取完；调用线程与 worker 线程都跑这个循环。
    void run() {
        for (;;) {
            size_t idx;
            {
                LockGuard g(lock);
                if (next >= n) return;
                idx = next++;
            }
            fn(idx);
        }
    }
};

}  // namespace

size_t par_cpu_count() {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? static_cast<size_t>(si.dwNumberOfProcessors) : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<size_t>(n) : 1;
#endif
}

size_t par_hash_workers(bool is_ssd) {
    size_t ncpu = par_cpu_count();
    size_t cap = is_ssd ? kParSsdMaxWorkers : kParHddMaxWorkers;
    size_t w = ncpu < cap ? ncpu : cap;
    return w < 1 ? 1 : w;
}

int par_for(size_t n, size_t nthreads, FunctionRef<void(size_t)> fn) {
    if (!fn) return -1;
    if (n == 0) return 0;
    if (nthreads > n) nthreads = n;
    if (nthreads > kParMaxThreads) nthreads = kParMaxThreads;
    if (nthreads <= 1) {
        for (size_t i = 0; i < n; i++) fn(i);
        return 0;
    }
    ParState st;
    st.fn = fn;
    st.n = n;
    size_t nworkers = nthreads - 1;  // 其余一个由调用线程承担
#ifdef _WIN32
    HANDLE hs[kParMaxThreads - 1];
    size_t nspawn = 0;
    for (size_t i = 0; i < nworkers; i++) {
        HANDLE h = CreateThread(
            nullptr, 0,
            [](LPVOID p) -> DWORD {
                static_cast<ParState *>(p)->run();
                return 0;
            },
            &st, 0, nullptr);
        if (!h) break;  // 建失败：用已建好的线程 + 调用线程跑完
        hs[nspawn++] = h;
    }
    st.run();
    if (nspawn > 0) {
        WaitForMultipleObjects(static_cast<DWORD>(nspawn), hs, TRUE, INFINITE);
        for (size_t i = 0; i < nspawn; i++) CloseHandle(hs[i]);
    }
#else
    pthread_t th[kParMaxThreads - 1];
    size_t nspawn = 0;
    for (size_t i = 0; i < nworkers; i++) {
        if (pthread_create(
                &th[nspawn], nullptr,
                [](void *p) -> void * {
                    static_cast<ParState *>(p)->run();
                    return nullptr;
                },
                &st) != 0)
            break;  // 建失败：用已建好的线程 + 调用线程跑完
        nspawn++;
    }
    st.run();
    for (size_t i = 0; i < nspawn; i++) pthread_join(th[i], nullptr);
#endif
    return 0;
}

}  // namespace cdc
