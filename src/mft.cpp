// mft.cpp — $MFT 解析 + 快照 + USN Journal 增量，逐行对照 desktop/src/mft.rs。
//
// 并行性说明（与 Rust 版的行为差异，仅影响速度不影响结果）：
//   - MFT 块解析、USN 变更重读、快照后台重建里的并行在本版是顺序执行；
//     解析与读盘也不重叠。
//   - 快照后台落盘仍走独立线程（与 Rust 的 save_snapshot_in_background 一致）。
//
// 资源全部 RAII：卷句柄 UniqueHandle，快照/记录/路径缓存用 std::vector /
// std::string / std::deque。Windows-only 的内部结构（U64Map、内部快照、
// resolve_dir、后台线程）用 #ifdef _WIN32 隔离。
#include "mft.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <utility>

#include "raii.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace cdc {
namespace {

constexpr uint64_t kIndexMask = 0xFFFFFFFFFFFFULL;

// ASCII 小写比较（skip_top 表全是 ASCII，与 Rust 的 to_lowercase 在这批词上等价）。
bool ascii_lower_eq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool name_in_skip_top(std::string_view name, SkipList skip_top) {
    for (const char *s : skip_top)
        if (ascii_lower_eq(name, s)) return true;
    return false;
}

// 小端编解码（快照格式与 Rust 版字节一致）。
void put_le(uint8_t *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint64_t get_le(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace

uint64_t now_seconds() { return static_cast<uint64_t>(std::time(nullptr)); }

// ---------------- parse_record_in_place ----------------

int parse_record_in_place(uint8_t *rec, size_t rec_len, uint64_t index, size_t bytes_per_sector,
                          size_t bytes_per_cluster, uint64_t min_size, ParsedRecord &out) {
    // 重置但保留 name/runs 的容量（解析循环里同一个 out 反复使用）。
    out.kind = RecordKind::Skip;
    out.index = index;
    out.frn = out.parent_frn = out.size = out.mtime = out.first_lcn = out.sort_key = 0;
    out.can_raw_read = false;
    out.seq = 0;
    out.reparse = false;
    out.name.clear();
    out.runs.clear();
    if (rec_len < 48 || std::memcmp(rec, "FILE", 4) != 0) return 0;
    mft_apply_fixups(rec, rec_len, bytes_per_sector);

    uint16_t flags = 0;
    if (!mft_u16_at(rec, rec_len, 22, flags)) return 0;
    if ((flags & 1) == 0) return 0;  // 未在使用中
    if (index != 0) {
        uint64_t base = 0;
        if (mft_u64_at(rec, rec_len, 32, base) && base != 0) return 0;  // 扩展记录
    }

    bool is_dir = (flags & 2) != 0;
    uint16_t sequence = 0;
    if (!mft_u16_at(rec, rec_len, 16, sequence)) return 0;
    uint64_t frn = (static_cast<uint64_t>(sequence) << 48) | index;
    uint16_t first_attr = 0;
    if (!mft_u16_at(rec, rec_len, 20, first_attr)) return 0;

    // 只记名字字节切片，等确认 size >= min_size 后再解码 UTF-16，
    // 避免百万小文件无效堆分配（与 Rust 版同款剪枝）。
    bool have_name_span = false;
    size_t name_off = 0, name_byte_len = 0;
    bool name_is_dos = false;
    uint64_t parent_frn = 0, size = 0, mtime = 0;
    uint32_t attrs = 0;
    uint16_t data_flags = 0;
    uint64_t first_lcn = 0, first_run_clusters = 0;
    std::vector<MftRun> mft_runs;

    size_t pos = first_attr;
    while (pos + 8 <= rec_len) {
        uint32_t attr_type = 0;
        if (!mft_u32_at(rec, rec_len, pos, attr_type)) break;
        if (attr_type == 0xFFFFFFFFu) break;
        uint32_t attr_len = 0;
        if (!mft_u32_at(rec, rec_len, pos + 4, attr_len)) break;
        if (attr_len < 16 || pos + attr_len > rec_len) break;
        uint8_t non_resident = rec[pos + 8];
        uint8_t name_len = rec[pos + 9];

        if (attr_type == 0x10 && non_resident == 0) {
            // $STANDARD_INFORMATION
            uint16_t val_off = 0;
            if (mft_u16_at(rec, rec_len, pos + 20, val_off)) {
                size_t v = pos + val_off;
                if (v + 36 <= pos + attr_len) {
                    uint64_t ft = 0;
                    if (mft_u64_at(rec, rec_len, v + 8, ft)) mtime = mft_filetime_to_unix(ft);
                    uint32_t a = 0;
                    if (mft_u32_at(rec, rec_len, v + 32, a)) {
                        attrs = a;
                        if (!is_dir && (attrs & kMftSkipFileAttrs) != 0) return 0;
                    }
                }
            }
        } else if (attr_type == 0x30 && non_resident == 0) {
            // $FILE_NAME
            uint16_t val_off = 0;
            if (mft_u16_at(rec, rec_len, pos + 20, val_off)) {
                size_t v = pos + val_off;
                if (v + 66 <= pos + attr_len) {
                    uint64_t parent_ref = 0;
                    if (mft_u64_at(rec, rec_len, v, parent_ref)) {
                        size_t nlen = rec[v + 64];
                        uint8_t name_ns = rec[v + 65];
                        size_t noff = v + 66;
                        size_t blen = nlen * 2;
                        if (noff + blen <= pos + attr_len) {
                            bool is_dos = name_ns == 2;
                            if (!have_name_span || (name_is_dos && !is_dos) || name_ns == 0 ||
                                name_ns == 1) {
                                have_name_span = true;
                                name_off = noff;
                                name_byte_len = blen;
                                parent_frn = parent_ref;
                                name_is_dos = is_dos;
                            }
                        }
                    }
                }
            }
        } else if (attr_type == 0x80 && name_len == 0) {
            // $DATA（未命名）
            data_flags = 0;
            uint16_t df = 0;
            if (mft_u16_at(rec, rec_len, pos + 12, df)) data_flags = df;
            if (non_resident != 0) {
                uint64_t lowest_vcn = 0;
                if (pos + 56 <= rec_len && mft_u64_at(rec, rec_len, pos + 16, lowest_vcn) &&
                    lowest_vcn == 0) {
                    uint64_t real_size = 0;
                    if (mft_u64_at(rec, rec_len, pos + 48, real_size)) size = real_size;
                    uint16_t run_off = 0;
                    if (mft_u16_at(rec, rec_len, pos + 32, run_off)) {
                        size_t start = pos + run_off;
                        if (start <= pos + attr_len) {
                            if (index == 0)
                                mft_runs = mft_parse_runlist(rec + start, pos + attr_len - start);
                            else
                                mft_parse_first_extent(rec + start, pos + attr_len - start,
                                                       first_lcn, first_run_clusters);
                        }
                    }
                }
            } else {
                uint32_t vlen = 0;
                if (mft_u32_at(rec, rec_len, pos + 16, vlen)) size = vlen;
            }
        }
        pos += attr_len;
    }

    if (index == 0) {
        if (!mft_runs.empty()) {
            out.kind = RecordKind::MftRoot;
            out.runs = std::move(mft_runs);
        }
        return 0;
    }
    if (!have_name_span) return 0;

    if (is_dir) {
        mft_utf16le_to_utf8(rec + name_off, name_byte_len, out.name);
        out.kind = RecordKind::Dir;
        out.parent_frn = parent_frn;
        out.seq = sequence;
        out.reparse = (attrs & kFileAttrReparsePoint) != 0;
        return 0;
    }
    if (size < min_size || (attrs & kMftSkipFileAttrs) != 0) return 0;
    mft_utf16le_to_utf8(rec + name_off, name_byte_len, out.name);
    if (!out.name.empty() && out.name[0] == '$') {
        out.name.clear();
        return 0;
    }

    uint64_t first_extent_bytes;
    if (first_run_clusters > UINT64_MAX / static_cast<uint64_t>(bytes_per_cluster))
        first_extent_bytes = UINT64_MAX;
    else
        first_extent_bytes = first_run_clusters * static_cast<uint64_t>(bytes_per_cluster);
    uint64_t need = size < 4096 ? size : 4096;
    bool can_raw_read = (attrs & kMftNoRawAttrs) == 0 &&
                        (data_flags & (0x0001u | 0x4000u | 0x8000u)) == 0 && first_lcn > 0 &&
                        first_extent_bytes >= need;

    out.kind = RecordKind::File;
    out.frn = frn;
    out.parent_frn = parent_frn;
    out.size = size;
    out.mtime = mtime;
    out.first_lcn = first_lcn;
    out.sort_key = first_lcn > 0 ? first_lcn : index;
    out.can_raw_read = can_raw_read;
    return 0;
}

// ---------------- 快照编解码（与 Rust 版字节一致） ----------------

namespace {

class SnapWriter {
public:
    explicit SnapWriter(std::FILE *f) : f_(f) {}
    void bytes(const void *p, size_t n) {
        if (!err_ && std::fwrite(p, 1, n, f_) != n) err_ = true;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(uint16_t v) { le(v, 2); }
    void u32(uint32_t v) { le(v, 4); }
    void u64(uint64_t v) { le(v, 8); }
    void str(const std::string &s) {
        u32(static_cast<uint32_t>(s.size()));
        if (!s.empty()) bytes(s.data(), s.size());
    }
    bool failed() const { return err_; }
    void fail() { err_ = true; }

private:
    void le(uint64_t v, int n) {
        uint8_t b[8];
        put_le(b, v, n);
        bytes(b, static_cast<size_t>(n));
    }
    std::FILE *f_;
    bool err_ = false;
};

class SnapReader {
public:
    explicit SnapReader(std::FILE *f) : f_(f) {}
    bool bytes(void *p, size_t n) {
        if (err_) return false;
        if (std::fread(p, 1, n, f_) != n) err_ = true;
        return !err_;
    }
    bool u8(uint8_t &v) { return bytes(&v, 1); }
    template <class T>
    bool le(T &v) {
        uint8_t b[sizeof(T)];
        if (!bytes(b, sizeof(T))) return false;
        v = static_cast<T>(get_le(b, sizeof(T)));
        return true;
    }
    bool str(std::string &s) {
        uint32_t n = 0;
        if (!le(n)) return false;
        if (n > 32 * 1024) {
            err_ = true;
            return false;
        }
        s.resize(n);
        return n == 0 || bytes(s.data(), n);
    }
    bool failed() const { return err_; }

private:
    std::FILE *f_;
    bool err_ = false;
};

}  // namespace

// 注意：沿用 C 版的窄字符 fopen / MoveFileExA（不经 UTF-8 -> UTF-16）。
// 缓存目录含"C盘清理工具"等非 ASCII 字符时，Windows 上的窄 API 按 ANSI
// 代码页解释路径而失败——这是 C 版的既有行为（快照只活在进程内缓存里），
// 本次移植保持不变，见 README "已知问题"。
int snapshot_save(const std::string &path, const Snapshot &snap) {
    std::string tmp = path + ".tmp";
    bool write_failed;
    {
        UniqueFile f(std::fopen(tmp.c_str(), "wb"));
        if (!f) return -1;
        SnapWriter w(f.get());
        w.bytes("MFT3", 4);
        w.u64(snap.volume_serial);
        w.u64(snap.journal_id);
        w.u64(static_cast<uint64_t>(snap.next_usn));
        w.u64(snap.min_size);
        w.u32(snap.bytes_per_cluster);
        w.u32(static_cast<uint32_t>(snap.dirs.size()));
        for (const DirNode &d : snap.dirs) {
            w.u64(d.index);
            w.u16(d.seq);
            w.u8(d.reparse ? 1 : 0);
            w.u64(d.parent_frn);
            w.str(d.name);
        }
        w.u32(static_cast<uint32_t>(snap.files.size()));
        for (const RawFile &rf : snap.files) {
            w.u64(rf.frn & kIndexMask);
            w.u64(rf.frn);
            w.u64(rf.parent_frn);
            w.u64(rf.size);
            w.u64(rf.mtime);
            w.u64(rf.first_lcn);
            w.u64(rf.sort_key);
            w.u8(rf.can_raw_read ? 1 : 0);
            w.str(rf.name);
        }
        if (std::fclose(f.release()) != 0) w.fail();
        write_failed = w.failed();
    }
    if (write_failed) {
        std::remove(tmp.c_str());
        return -1;
    }
#ifdef _WIN32
    bool moved = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    bool moved = std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
    if (!moved) {
        std::remove(tmp.c_str());
        return -1;
    }
    return 0;
}

int snapshot_load(const std::string &path, Snapshot &out) {
    out = Snapshot();
    UniqueFile f(std::fopen(path.c_str(), "rb"));
    if (!f) return -1;
    SnapReader r(f.get());
    char magic[4];
    if (!r.bytes(magic, 4) || std::memcmp(magic, "MFT3", 4) != 0) return -1;
    Snapshot s;
    uint64_t next = 0;
    uint32_t dir_count = 0, file_count = 0;
    if (!r.le(s.volume_serial) || !r.le(s.journal_id) || !r.le(next) || !r.le(s.min_size) ||
        !r.le(s.bytes_per_cluster) || !r.le(dir_count))
        return -1;
    s.next_usn = static_cast<int64_t>(next);
    // 先在局部快照里攒，读完再整体交给 out（读一半失败时 out 保持干净）。
    for (uint32_t i = 0; i < dir_count; i++) {
        DirNode node;
        uint8_t rep = 0;
        if (!r.le(node.index) || !r.le(node.seq) || !r.u8(rep) || !r.le(node.parent_frn) ||
            !r.str(node.name))
            return -1;
        node.reparse = rep != 0;
        s.dirs.push_back(std::move(node));
    }
    if (!r.le(file_count)) return -1;
    for (uint32_t i = 0; i < file_count; i++) {
        RawFile rf;
        uint64_t idx = 0;  // 文件的键就是 frn 的低 48 位；idx 与之一致
        uint8_t raw = 0;
        if (!r.le(idx) || !r.le(rf.frn) || !r.le(rf.parent_frn) || !r.le(rf.size) ||
            !r.le(rf.mtime) || !r.le(rf.first_lcn) || !r.le(rf.sort_key) || !r.u8(raw) ||
            !r.str(rf.name))
            return -1;
        rf.can_raw_read = raw != 0;
        s.files.push_back(std::move(rf));
    }
    if (r.failed()) return -1;
    out = std::move(s);
    return 0;
}

bool usn_state_usable(uint64_t stored_journal_id, int64_t stored_next_usn, uint64_t journal_id,
                      int64_t first_usn, int64_t next_usn) {
    return stored_journal_id == journal_id && stored_next_usn >= first_usn &&
           stored_next_usn <= next_usn;
}

#ifdef _WIN32

// ---------------- Windows 专属：内部结构 ----------------

namespace {

// u64 -> V 开放寻址哈希表（splitmix64，线性探测，无 tombstone 删除）。
template <class V>
class U64Map {
public:
    explicit U64Map(size_t cap_hint = 16) { reset(cap_hint); }

    size_t size() const { return len_; }

    void put(uint64_t key, V val) {
        if ((len_ + 1) * 10 > slots_.size() * 7) grow();
        size_t i = slot_for(key);
        if (!slots_[i].used) {
            slots_[i].used = true;
            slots_[i].key = key;
            len_++;
        }
        slots_[i].val = val;
    }

    const V *get(uint64_t key) const {
        size_t mask = slots_.size() - 1;
        for (size_t i = hash(key) & mask; slots_[i].used; i = (i + 1) & mask)
            if (slots_[i].key == key) return &slots_[i].val;
        return nullptr;
    }

    bool remove(uint64_t key) {
        size_t mask = slots_.size() - 1;
        for (size_t i = hash(key) & mask; slots_[i].used; i = (i + 1) & mask) {
            if (slots_[i].key != key) continue;
            // 无 tombstone 的删除：把簇内后续元素逐个重插。
            slots_[i].used = false;
            len_--;
            for (size_t j = (i + 1) & mask; slots_[j].used; j = (j + 1) & mask) {
                Slot moved = slots_[j];
                slots_[j].used = false;
                len_--;
                put(moved.key, moved.val);
            }
            return true;
        }
        return false;
    }

    template <class F>
    void for_each_key(F &&f) const {
        for (const Slot &s : slots_)
            if (s.used) f(s.key);
    }

private:
    struct Slot {
        uint64_t key = 0;
        V val{};
        bool used = false;
    };

    static uint64_t hash(uint64_t x) {  // splitmix64
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
    size_t slot_for(uint64_t key) const {
        size_t mask = slots_.size() - 1;
        size_t i = hash(key) & mask;
        while (slots_[i].used && slots_[i].key != key) i = (i + 1) & mask;
        return i;
    }
    void reset(size_t cap_hint) {
        size_t cap = 16;
        while (cap < cap_hint * 2 && cap < (1u << 24)) cap *= 2;
        slots_.assign(cap, Slot());
        len_ = 0;
    }
    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, Slot());
        len_ = 0;
        for (const Slot &s : old)
            if (s.used) put(s.key, s.val);
    }

    std::vector<Slot> slots_;
    size_t len_ = 0;
};

// 内部快照：数组 + 记录号索引。
class SnapIndex {
public:
    SnapIndex() = default;
    explicit SnapIndex(Snapshot snap)
        : snap_(std::move(snap)),
          dir_index_(snap_.dirs.empty() ? 16 : snap_.dirs.size()),
          file_index_(snap_.files.empty() ? 16 : snap_.files.size()) {
        for (size_t i = 0; i < snap_.dirs.size(); i++) dir_index_.put(snap_.dirs[i].index, i);
        for (size_t i = 0; i < snap_.files.size(); i++)
            file_index_.put(snap_.files[i].frn & kIndexMask, i);
    }

    Snapshot &snap() { return snap_; }
    const Snapshot &snap() const { return snap_; }

    const DirNode *dir(uint64_t index) const {
        const size_t *pos = dir_index_.get(index);
        return (pos && *pos < snap_.dirs.size()) ? &snap_.dirs[*pos] : nullptr;
    }

    void put_dir(DirNode node) {
        const size_t *pos = dir_index_.get(node.index);
        if (pos && *pos < snap_.dirs.size()) {
            snap_.dirs[*pos] = std::move(node);
            return;
        }
        dir_index_.put(node.index, snap_.dirs.size());
        snap_.dirs.push_back(std::move(node));
    }

    void put_file(RawFile f) {
        uint64_t index = f.frn & kIndexMask;
        const size_t *pos = file_index_.get(index);
        if (pos && *pos < snap_.files.size()) {
            snap_.files[*pos] = std::move(f);
            return;
        }
        file_index_.put(index, snap_.files.size());
        snap_.files.push_back(std::move(f));
    }

    // swap-remove：把最后一个元素挪到被删位置，并修正它的索引。
    void remove_dir(uint64_t index) {
        const size_t *p = dir_index_.get(index);
        if (!p || *p >= snap_.dirs.size()) return;
        size_t pos = *p, last = snap_.dirs.size() - 1;
        if (pos != last) {
            snap_.dirs[pos] = std::move(snap_.dirs[last]);
            dir_index_.put(snap_.dirs[pos].index, pos);
        }
        snap_.dirs.pop_back();
        dir_index_.remove(index);
    }

    void remove_file(uint64_t index) {
        const size_t *p = file_index_.get(index);
        if (!p || *p >= snap_.files.size()) return;
        size_t pos = *p, last = snap_.files.size() - 1;
        if (pos != last) {
            snap_.files[pos] = std::move(snap_.files[last]);
            file_index_.put(snap_.files[pos].frn & kIndexMask, pos);
        }
        snap_.files.pop_back();
        file_index_.remove(index);
    }

    void insert_parsed(ParsedRecord &rec) {
        if (rec.kind == RecordKind::Dir) {
            put_dir(DirNode{rec.index, rec.seq, rec.parent_frn, std::move(rec.name), rec.reparse});
        } else if (rec.kind == RecordKind::File) {
            put_file(RawFile{rec.frn, rec.parent_frn, std::move(rec.name), rec.size, rec.mtime,
                             rec.first_lcn, rec.sort_key, rec.can_raw_read});
        }
    }

private:
    Snapshot snap_;
    U64Map<size_t> dir_index_;
    U64Map<size_t> file_index_;
};

using SnapPtr = std::unique_ptr<SnapIndex>;

// 后台线程：CreateThread 跑一个堆上的可调用对象（mingw-gcc win32 线程模型
// 没有 std::thread）。创建失败时退化为同步执行，保证不丢动作。
template <class F>
void spawn_detached(F &&fn) {
    using Task = std::decay_t<F>;
    auto *task = new Task(std::forward<F>(fn));
    HANDLE h = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            std::unique_ptr<Task> t(static_cast<Task *>(p));
            (*t)();
            return 0;
        },
        task, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        std::unique_ptr<Task> t(task);
        (*t)();
    }
}

// ---------------- 进程内快照缓存 ----------------

constexpr size_t kMemSnapsMax = 8;
constexpr size_t kRebuildMax = 8;

struct MemCache {
    Mutex lock;
    std::vector<std::pair<uint64_t, SnapPtr>> snaps;  // serial -> 快照
    std::vector<uint64_t> rebuild_in_progress;
};

MemCache &mem() {
    static MemCache *c = new MemCache();  // 进程生命期；不析构（后台线程可能仍在用）
    return *c;
}

// 借用：调用方不拥有，缓存条目被替换前有效（与 C 版同一约束：单扫描线程）。
SnapIndex *mem_snapshot_get_borrowed(uint64_t serial) {
    LockGuard g(mem().lock);
    for (auto &e : mem().snaps)
        if (e.first == serial) return e.second.get();
    return nullptr;
}

SnapPtr mem_snapshot_take(uint64_t serial) {
    LockGuard g(mem().lock);
    auto &v = mem().snaps;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].first != serial) continue;
        SnapPtr found = std::move(v[i].second);
        v[i] = std::move(v.back());
        v.pop_back();
        return found;
    }
    return nullptr;
}

// 放入缓存并返回借用指针。
SnapIndex *mem_snapshot_put(uint64_t serial, SnapPtr snap) {
    SnapIndex *raw = snap.get();
    LockGuard g(mem().lock);
    auto &v = mem().snaps;
    for (auto &e : v) {
        if (e.first == serial) {
            e.second = std::move(snap);
            return raw;
        }
    }
    if (v.size() == kMemSnapsMax) {
        v[0] = std::move(v.back());
        v.pop_back();
    }
    v.emplace_back(serial, std::move(snap));
    return raw;
}

// ---------------- resolve_dir ----------------

// 目录路径缓存：frn -> 路径在 store 里的下标；kNone 表示"已解析为无"。
// 路径存在 std::deque 里，push_back 不搬动已有元素，借用引用长期有效。
class ResolveCache {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    explicit ResolveCache(size_t hint) : map_(hint) {}
    const uint32_t *get(uint64_t frn) const { return map_.get(frn); }
    const std::string &at(uint32_t i) const { return store_[i]; }
    const std::string &put(uint64_t frn, std::string path) {
        store_.push_back(std::move(path));
        map_.put(frn, static_cast<uint32_t>(store_.size() - 1));
        return store_.back();
    }
    void put_none(uint64_t frn) { map_.put(frn, kNone); }

private:
    U64Map<uint32_t> map_;
    std::deque<std::string> store_;
};

// 目录路径解析：返回缓存内字符串的借用指针（缓存存活期间有效）；未解析出返回空。
const std::string *resolve_dir(uint64_t frn, const SnapIndex &snap, ResolveCache &cache,
                               const char *drive, bool skip_top_at_root, SkipList skip_top) {
    if (const uint32_t *cv = cache.get(frn)) return *cv == ResolveCache::kNone ? nullptr : &cache.at(*cv);
    if ((frn & kIndexMask) == 5) return &cache.put(frn, std::string(drive) + "\\");

    std::vector<uint64_t> chain;
    uint64_t cur_frn = frn;
    const std::string *base = nullptr;
    for (;;) {
        if (const uint32_t *cv = cache.get(cur_frn)) {
            if (*cv != ResolveCache::kNone) base = &cache.at(*cv);
            break;
        }
        uint64_t cur_idx = cur_frn & kIndexMask;
        auto cur_seq = static_cast<uint16_t>(cur_frn >> 48);
        const DirNode *node = snap.dir(cur_idx);
        if (!node) break;
        if (cur_seq != 0 && node->seq != cur_seq) break;
        if (node->reparse || (!node->name.empty() && node->name[0] == '$')) break;
        uint64_t parent_idx = node->parent_frn & kIndexMask;
        if (node->parent_frn == 0 || parent_idx == 5) {
            if (skip_top_at_root && name_in_skip_top(node->name, skip_top)) break;
            base = &cache.put(cur_frn, std::string(drive) + "\\" + node->name);
            break;
        }
        chain.push_back(cur_frn);
        cur_frn = node->parent_frn;
        if (chain.size() > 4096) break;
    }

    if (!base) {
        for (uint64_t c : chain) cache.put_none(c);
        cache.put_none(frn);
        return nullptr;
    }
    if (chain.empty()) return base;

    // 组装：从最上层往下逐段拼接，每段的完整路径都入缓存。
    std::string path = *base;
    const std::string *result = nullptr;
    for (size_t ri = chain.size(); ri-- > 0;) {
        const DirNode *node = snap.dir(chain[ri] & kIndexMask);
        if (!node) {
            cache.put_none(frn);
            return nullptr;
        }
        while (!path.empty() && path.back() == '\\') path.pop_back();
        path.push_back('\\');
        path += node->name;
        result = &cache.put(chain[ri], path);
    }
    return result;
}

// ---------------- 卷 / USN / $MFT ----------------

struct NtfsVolumeData {
    int64_t volume_serial_number;
    int64_t number_sectors;
    int64_t total_clusters;
    int64_t free_clusters;
    int64_t total_reserved;
    uint32_t bytes_per_sector;
    uint32_t bytes_per_cluster;
    uint32_t bytes_per_file_record_segment;
    uint32_t clusters_per_file_record_segment;
    int64_t mft_valid_data_length;
    int64_t mft_start_lcn;
    int64_t mft2_start_lcn;
    int64_t mft_zone_start;
    int64_t mft_zone_end;
};

struct UsnJournalData {
    uint64_t usn_journal_id;
    int64_t first_usn;
    int64_t next_usn;
    int64_t lowest_valid_usn;
    int64_t max_usn;
    uint64_t maximum_size;
    uint64_t allocation_delta;
};

struct ReadUsnJournalDataV0 {
    int64_t start_usn;
    uint32_t reason_mask;
    uint32_t return_only_on_close;
    uint64_t timeout;
    uint64_t bytes_to_wait_for;
    uint64_t usn_journal_id;
};

// root 如 "C:\\" 或 "C:"；成功时 drive_out="C:"。
bool parse_drive_root(const char *root, char drive_out[3]) {
    size_t len = std::strlen(root);
    while (len > 0 && root[len - 1] == '\\') len--;
    if (len < 2 || root[1] != ':') return false;
    char letter = root[0];
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))) return false;
    if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');
    drive_out[0] = letter;
    drive_out[1] = ':';
    drive_out[2] = '\0';
    return true;
}

UniqueHandle open_volume_handle(const std::string &volume_path) {
    auto w = utf8_to_wide(volume_path);
    if (!w) return nullptr;
    return adopt_handle(CreateFileW(w->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr));
}

bool get_volume_data(HANDLE h, NtfsVolumeData &out) {
    DWORD ret = 0;
    std::memset(&out, 0, sizeof out);
    return DeviceIoControl(h, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &out,
                           static_cast<DWORD>(sizeof out), &ret, nullptr) != 0;
}

bool query_usn_journal(HANDLE h, UsnJournalData &out) {
    DWORD ret = 0;
    std::memset(&out, 0, sizeof out);
    return DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &out,
                           static_cast<DWORD>(sizeof out), &ret, nullptr) != 0;
}

size_t read_exact_at(HANDLE h, int64_t offset, uint8_t *buf, size_t len) {
    LARGE_INTEGER li;
    li.QuadPart = offset;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return 0;
    size_t total = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > 8u * 1024 * 1024) chunk = 8u * 1024 * 1024;
        DWORD read = 0;
        if (!ReadFile(h, buf + total, static_cast<DWORD>(chunk), &read, nullptr) || read == 0)
            break;
        total += read;
    }
    return total;
}

bool read_single_mft_record(HANDLE h, uint64_t index, size_t record_size,
                            std::vector<uint8_t> &out_buf, uint64_t &actual_frn) {
    int64_t in = static_cast<int64_t>(index);  // NTFS_FILE_RECORD_INPUT_BUFFER
    DWORD ret = 0;
    if (!DeviceIoControl(h, FSCTL_GET_NTFS_FILE_RECORD, &in, static_cast<DWORD>(sizeof in),
                         out_buf.data(), static_cast<DWORD>(16 + record_size), &ret, nullptr))
        return false;
    if (ret < 12 + record_size) return false;
    actual_frn = get_le(out_buf.data(), 8);
    uint32_t rec_len = static_cast<uint32_t>(get_le(out_buf.data() + 8, 4));
    return rec_len >= record_size;
}

// ---- USN 变化收集 ----

enum class DeltaKind { Changed, TooMany, Failed };

struct UsnDelta {
    DeltaKind kind = DeltaKind::Failed;
    std::vector<uint64_t> frns;
};

UsnDelta collect_usn_changed_indices(HANDLE h, const UsnJournalData &journal, int64_t start_usn) {
    UsnDelta d;
    ReadUsnJournalDataV0 input{};
    input.start_usn = start_usn;
    input.reason_mask = 0xFFFFFFFFu;
    input.usn_journal_id = journal.usn_journal_id;
    std::vector<uint8_t> buf(64 * 1024);
    U64Map<uint8_t> seen(1024);
    for (;;) {
        DWORD returned = 0;
        if (!DeviceIoControl(h, FSCTL_READ_USN_JOURNAL, &input, static_cast<DWORD>(sizeof input),
                             buf.data(), static_cast<DWORD>(buf.size()), &returned, nullptr) ||
            returned < 8)
            return d;  // Failed
        size_t len = returned;
        auto next_cursor = static_cast<int64_t>(get_le(buf.data(), 8));
        if (len == 8) break;
        size_t off = 8;
        while (off + 60 <= len) {
            auto rec_len = static_cast<uint32_t>(get_le(buf.data() + off, 4));
            if (rec_len < 60 || off + rec_len > len) break;
            auto major = static_cast<uint16_t>(get_le(buf.data() + off + 4, 2));
            if (major == 2 || major == 3) {
                uint64_t frn = get_le(buf.data() + off + 8, 8) & kIndexMask;
                if (!seen.get(frn)) {
                    seen.put(frn, 1);
                    if (seen.size() > kMftMaxUsnDeltaFrns) {
                        d.kind = DeltaKind::TooMany;
                        return d;
                    }
                }
            }
            off += rec_len;
        }
        if (next_cursor <= input.start_usn || next_cursor >= journal.next_usn) break;
        input.start_usn = next_cursor;
    }
    d.kind = DeltaKind::Changed;
    d.frns.reserve(seen.size());
    seen.for_each_key([&](uint64_t k) { d.frns.push_back(k); });
    return d;
}

// ---- USN 变更记录重读（顺序版；Rust 版 >=64 条时 8 线程并行） ----

struct ParsedDeltaItem {
    uint64_t idx;
    ParsedRecord rec;
};

void apply_usn_delta(HANDLE h, const std::vector<uint64_t> &changed, size_t record_size,
                     size_t bytes_per_sector, size_t bytes_per_cluster, uint64_t min_size,
                     std::vector<ParsedDeltaItem> &out) {
    std::vector<uint8_t> buf(16 + record_size);
    for (uint64_t idx : changed) {
        uint64_t actual = 0;
        if (!read_single_mft_record(h, idx, record_size, buf, actual)) continue;
        if ((actual & kIndexMask) != idx) continue;
        ParsedDeltaItem item{idx, ParsedRecord()};
        if (parse_record_in_place(buf.data() + 12, record_size, idx, bytes_per_sector,
                                  bytes_per_cluster, min_size, item.rec) != 0)
            continue;
        out.push_back(std::move(item));
    }
}

// ---- $MFT 全量读取（顺序版；Rust 版用 Rayon 并行解析 + 读盘重叠） ----

void parse_chunk(const uint8_t *data, size_t len, uint64_t base_idx, size_t record_size,
                 size_t bytes_per_sector, size_t bytes_per_cluster, uint64_t min_size,
                 SnapIndex &out, std::vector<uint8_t> &scratch, ParsedRecord &rec) {
    for (size_t off = 0; off + record_size <= len; off += record_size) {
        if (std::memcmp(data + off, "FILE", 4) != 0) continue;
        std::memcpy(scratch.data(), data + off, record_size);
        if (parse_record_in_place(scratch.data(), record_size, base_idx + off / record_size,
                                  bytes_per_sector, bytes_per_cluster, min_size, rec) != 0)
            continue;
        out.insert_parsed(rec);
    }
}

// 0 成功；1 读到空表。
int full_mft_read(HANDLE h, const NtfsVolumeData &vol, const std::vector<MftRun> &runs,
                  uint64_t store_min_size, SnapIndex &out) {
    size_t record_size = vol.bytes_per_file_record_segment;
    size_t bytes_per_cluster = vol.bytes_per_cluster;
    size_t bytes_per_sector = vol.bytes_per_sector;
    uint64_t max_mft_bytes = vol.mft_valid_data_length > 0
                                 ? static_cast<uint64_t>(vol.mft_valid_data_length)
                                 : UINT64_MAX;
    constexpr size_t kChunk = 8 * 1024 * 1024;
    std::vector<uint8_t> read_buf(kChunk), parse_buf(kChunk), scratch(record_size),
        carry(record_size);
    ParsedRecord rec;
    size_t carry_len = 0, pending_len = 0;
    uint64_t pending_base_idx = 0, total_offset = 0;
    auto flush_pending = [&] {
        if (pending_len > 0) {
            parse_chunk(parse_buf.data(), pending_len, pending_base_idx, record_size,
                        bytes_per_sector, bytes_per_cluster, store_min_size, out, scratch, rec);
            pending_len = 0;
        }
    };

    for (const MftRun &run : runs) {
        if (total_offset >= max_mft_bytes) break;
        if (run.len <= 0) continue;
        uint64_t run_bytes = static_cast<uint64_t>(run.len) * static_cast<uint64_t>(bytes_per_cluster);
        if (run.lcn < 0) {
            flush_pending();
            total_offset = run_bytes > UINT64_MAX - total_offset ? UINT64_MAX : total_offset + run_bytes;
            carry_len = 0;
            continue;
        }
        int64_t disk_offset = run.lcn * static_cast<int64_t>(bytes_per_cluster);
        uint64_t remain = max_mft_bytes - total_offset;
        uint64_t to_process = run_bytes < remain ? run_bytes : remain;
        uint64_t done_in_run = 0;
        while (done_in_run < to_process) {
            uint64_t left = to_process - done_in_run;
            size_t want = left < kChunk ? static_cast<size_t>(left) : kChunk;
            flush_pending();
            size_t got = read_exact_at(h, disk_offset + static_cast<int64_t>(done_in_run),
                                       read_buf.data(), want);
            if (got == 0) break;
            size_t cursor = 0;
            if (carry_len > 0) {
                size_t need = record_size - carry_len;
                if (got >= need) {
                    uint64_t rec_idx = (total_offset - carry_len) / record_size;
                    std::memcpy(scratch.data(), carry.data(), carry_len);
                    std::memcpy(scratch.data() + carry_len, read_buf.data(), need);
                    if (parse_record_in_place(scratch.data(), record_size, rec_idx,
                                              bytes_per_sector, bytes_per_cluster, store_min_size,
                                              rec) == 0)
                        out.insert_parsed(rec);
                    carry_len = 0;
                    cursor = need;
                } else {
                    std::memcpy(carry.data() + carry_len, read_buf.data(), got);
                    carry_len += got;
                    done_in_run += got;
                    total_offset += got;
                    continue;
                }
            }
            size_t aligned_len = ((got - cursor) / record_size) * record_size;
            if (aligned_len > 0) {
                std::memcpy(parse_buf.data(), read_buf.data() + cursor, aligned_len);
                pending_len = aligned_len;
                pending_base_idx = (total_offset + cursor) / record_size;
                cursor += aligned_len;
            }
            if (cursor < got) {
                std::memcpy(carry.data(), read_buf.data() + cursor, got - cursor);
                carry_len = got - cursor;
            }
            done_in_run += got;
            total_offset += got;
        }
    }
    flush_pending();
    if (out.snap().dirs.empty() && out.snap().files.empty()) return 1;
    return 0;
}

// 读 $MFT 第 0 号记录拿到 runlist，再全量读取。成功返回新快照，否则空。
SnapPtr build_snapshot(HANDLE h, const NtfsVolumeData &volume, uint64_t serial, bool have_journal,
                       const UsnJournalData &journal, uint64_t store_min_size) {
    size_t record_size = volume.bytes_per_file_record_segment;
    std::vector<uint8_t> first(record_size);
    if (read_exact_at(h, volume.mft_start_lcn * static_cast<int64_t>(volume.bytes_per_cluster),
                      first.data(), record_size) < record_size)
        return nullptr;
    ParsedRecord rec;
    if (parse_record_in_place(first.data(), record_size, 0, volume.bytes_per_sector,
                              volume.bytes_per_cluster, store_min_size, rec) != 0 ||
        rec.kind != RecordKind::MftRoot)
        return nullptr;
    Snapshot s;
    s.volume_serial = serial;
    s.journal_id = have_journal ? journal.usn_journal_id : 0;
    s.next_usn = have_journal ? journal.next_usn : 0;
    s.min_size = store_min_size;
    s.bytes_per_cluster = volume.bytes_per_cluster;
    auto si = std::make_unique<SnapIndex>(std::move(s));
    if (full_mft_read(h, volume, rec.runs, store_min_size, *si) != 0) return nullptr;
    return si;
}

// ---- 快照路径与后台落盘 / 后台重建 ----

std::string snapshot_path(uint64_t serial) {
    auto dir = cache_dir();
    if (!dir) return {};
    return *dir + format("\\mft-v3-%016llx.bin", static_cast<unsigned long long>(serial));
}

Mutex &save_lock() {
    static Mutex *m = new Mutex();
    return *m;
}

void save_snapshot_in_background(const std::string &path, const SnapIndex &snap) {
    // 深拷贝快照交给线程（快照本体留在缓存里继续用）。
    auto copy = std::make_unique<Snapshot>(snap.snap());
    spawn_detached([path, copy = std::move(copy)] {
        LockGuard g(save_lock());
        snapshot_save(path, *copy);
    });
}

void finish_rebuild(uint64_t serial) {
    LockGuard g(mem().lock);
    auto &v = mem().rebuild_in_progress;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == serial) {
            v[i] = v.back();
            v.pop_back();
            break;
        }
    }
}

void spawn_background_rebuild(const std::string &volume_path, uint64_t serial,
                              const NtfsVolumeData &volume, uint64_t store_min_size,
                              const std::string &snap_file) {
    {
        LockGuard g(mem().lock);
        auto &v = mem().rebuild_in_progress;
        for (uint64_t s : v)
            if (s == serial) return;
        if (v.size() == kRebuildMax) return;
        v.push_back(serial);
    }
    spawn_detached([volume_path, serial, volume, store_min_size, snap_file] {
        SnapPtr rebuilt;
        if (UniqueHandle h = open_volume_handle(volume_path)) {
            // 先取 USN 游标再读 MFT：读取期间的变更在下次增量里补上。
            UsnJournalData journal;
            bool have_journal = query_usn_journal(h.get(), journal);
            rebuilt = build_snapshot(h.get(), volume, serial, have_journal, journal, store_min_size);
        }
        if (rebuilt) {
            // 先落盘再进缓存：落盘用的是本线程拥有的指针，无竞态。
            snapshot_save(snap_file, rebuilt->snap());
            mem_snapshot_put(serial, std::move(rebuilt));
        }
        finish_rebuild(serial);
    });
}

// 逐字符小写前缀比较：full 的前 prefix.size() 个字节与小写的 prefix 相等。
bool lower_is_prefix(std::string_view full, std::string_view prefix) {
    if (full.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        auto a = static_cast<unsigned char>(full[i]);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
        if (a != static_cast<unsigned char>(prefix[i])) return false;
    }
    return true;
}

// rest（已去掉 root 前缀）的第一段与 skip_top 比较（两侧 ASCII 小写）。
bool lower_has_skip_segment(std::string_view rest, SkipList skip_top) {
    size_t slash = rest.find('\\');
    return name_in_skip_top(slash == std::string_view::npos ? rest : rest.substr(0, slash),
                            skip_top);
}

std::string join_dir(const std::string &dir_path, const std::string &name) {
    std::string full = dir_path;
    if (full.empty() || full.back() != '\\') full.push_back('\\');
    full += name;
    return full;
}

// 打开卷、读卷参数并校验记录大小；失败返回空句柄。
UniqueHandle open_ntfs_volume(const char *root, char drive[3], std::string &volume_path,
                              NtfsVolumeData &volume) {
    if (!parse_drive_root(root, drive)) return nullptr;
    volume_path = std::string("\\\\.\\") + drive;
    UniqueHandle h = open_volume_handle(volume_path);
    if (!h || !get_volume_data(h.get(), volume)) return nullptr;
    size_t record_size = volume.bytes_per_file_record_segment;
    if ((record_size != 1024 && record_size != 4096) || volume.bytes_per_cluster == 0 ||
        volume.bytes_per_sector == 0)
        return nullptr;
    return h;
}

}  // namespace

// ---------------- 公共入口（Windows） ----------------

int volume_usn_state(const char *root, uint64_t &volume_serial, uint64_t &journal_id,
                     int64_t &first_usn, int64_t &next_usn) {
    char drive[3];
    if (!parse_drive_root(root, drive)) return -1;
    UniqueHandle h = open_volume_handle(std::string("\\\\.\\") + drive);
    if (!h) return -1;
    NtfsVolumeData volume;
    UsnJournalData journal;
    if (!get_volume_data(h.get(), volume) || !query_usn_journal(h.get(), journal)) return -1;
    volume_serial = static_cast<uint64_t>(volume.volume_serial_number);
    journal_id = journal.usn_journal_id;
    first_usn = journal.first_usn;
    next_usn = journal.next_usn;
    return 0;
}

int mft_try_scan(const char *root, uint64_t min_size, SkipList skip_top, KeepFn keep,
                 FileEntryList &out) {
    char drive[3];
    std::string volume_path;
    NtfsVolumeData volume;
    UniqueHandle h = open_ntfs_volume(root, drive, volume_path, volume);
    if (!h) return 1;
    size_t trimmed_len = std::strlen(root);
    while (trimmed_len > 0 && root[trimmed_len - 1] == '\\') trimmed_len--;
    bool is_drive_root = trimmed_len == 2;
    size_t record_size = volume.bytes_per_file_record_segment;
    size_t bytes_per_cluster = volume.bytes_per_cluster;
    size_t bytes_per_sector = volume.bytes_per_sector;
    auto serial = static_cast<uint64_t>(volume.volume_serial_number);
    UsnJournalData journal;
    bool have_journal = query_usn_journal(h.get(), journal);
    std::string snap_file = snapshot_path(serial);
    uint64_t store_min_size =
        min_size >= kMftSnapshotFloorMinSize ? kMftSnapshotFloorMinSize : min_size;

    SnapIndex *snap = nullptr;  // 本次扫描用的快照（缓存内借用，或 snap_owned）
    SnapPtr snap_owned;         // 未进缓存时由本函数持有

    if (have_journal && !snap_file.empty()) {
        SnapIndex *cached = mem_snapshot_get_borrowed(serial);
        SnapPtr disk;
        if (!cached) {
            Snapshot loaded;
            if (snapshot_load(snap_file, loaded) == 0)
                disk = std::make_unique<SnapIndex>(std::move(loaded));
        }
        SnapIndex *cand = cached ? cached : disk.get();
        if (cand && cand->snap().volume_serial == serial &&
            cand->snap().journal_id == journal.usn_journal_id &&
            cand->snap().bytes_per_cluster == static_cast<uint32_t>(bytes_per_cluster) &&
            cand->snap().min_size <= min_size) {
            if (cand->snap().next_usn == journal.next_usn) {
                snap = cached ? cached : mem_snapshot_put(serial, std::move(disk));
            } else if (cand->snap().next_usn >= journal.first_usn &&
                       cand->snap().next_usn < journal.next_usn) {
                UsnDelta delta = collect_usn_changed_indices(h.get(), journal, cand->snap().next_usn);
                if (delta.kind == DeltaKind::Changed) {
                    SnapPtr owned = cached ? mem_snapshot_take(serial) : std::move(disk);
                    if (owned) {
                        std::vector<ParsedDeltaItem> parsed;
                        apply_usn_delta(h.get(), delta.frns, record_size, bytes_per_sector,
                                        bytes_per_cluster, owned->snap().min_size, parsed);
                        for (ParsedDeltaItem &item : parsed) {
                            owned->remove_dir(item.idx);
                            owned->remove_file(item.idx);
                            owned->insert_parsed(item.rec);
                        }
                        owned->snap().next_usn = journal.next_usn;
                        snap = mem_snapshot_put(serial, std::move(owned));
                        save_snapshot_in_background(snap_file, *snap);
                    }
                } else if (delta.kind == DeltaKind::TooMany) {
                    // 变更太多：本次先用旧快照，后台重建，下一次扫描再换新。
                    snap = cached ? cached : mem_snapshot_put(serial, std::move(disk));
                    spawn_background_rebuild(volume_path, serial, volume, store_min_size, snap_file);
                }
                // Failed：落到下面的同步全量重建。
            }
        }
    }

    if (!snap) {
        SnapPtr si = build_snapshot(h.get(), volume, serial, have_journal, journal, store_min_size);
        if (!si) return 1;
        if (have_journal && store_min_size >= kMftSnapshotFloorMinSize) {
            snap = mem_snapshot_put(serial, std::move(si));
            save_snapshot_in_background(snap_file, *snap);
        } else {
            snap_owned = std::move(si);
            snap = snap_owned.get();
        }
    }

    // 拼路径、过滤、拼 FileEntry。
    std::string root_lower = root;
    while (!root_lower.empty() && root_lower.back() == '\\') root_lower.pop_back();
    for (char &c : root_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t root_len = root_lower.size() + 1;  // root_lower + "\\"

    size_t n_dirs = snap->snap().dirs.size();
    ResolveCache dir_cache(n_dirs == 0 ? 16 : (n_dirs < 65536 ? n_dirs : 65536));
    uint64_t now = now_seconds();
    const std::string drive_root = std::string(drive) + "\\";
    for (const RawFile &raw : snap->snap().files) {
        if (raw.size < min_size) continue;
        if (keep && !keep(raw.name, raw.size)) continue;
        const std::string *dir_path;
        if ((raw.parent_frn & kIndexMask) == 5) {
            dir_path = &drive_root;
        } else {
            dir_path = resolve_dir(raw.parent_frn, *snap, dir_cache, drive, is_drive_root, skip_top);
            if (!dir_path) continue;
        }
        std::string full = join_dir(*dir_path, raw.name);
        if (!is_drive_root) {
            if (!lower_is_prefix(full, root_lower) || full.size() <= root_lower.size() ||
                full[root_lower.size()] != '\\')
                continue;
            if (full.size() > root_len &&
                lower_has_skip_segment(std::string_view(full).substr(root_len), skip_top))
                continue;
        }
        uint64_t age = now >= raw.mtime ? now - raw.mtime : 0;
        bool recent = raw.mtime == 0 || age < kMftRawReadMinAgeSecs;
        FileEntry e;
        e.path = std::move(full);
        e.size = raw.size;
        e.mtime = raw.mtime;
        e.sort_key = raw.sort_key;
        e.frn = raw.frn;
        e.first_lcn = raw.first_lcn;
        e.bytes_per_cluster = snap->snap().bytes_per_cluster;  // Rust：取自卷，非记录
        e.can_raw_read = raw.can_raw_read && !recent;
        out.push_back(std::move(e));
    }
    return 0;
}

int usn_changes_since(const char *root, uint64_t stored_journal_id, int64_t stored_next_usn,
                      SkipList skip_top, UsnChangeSet &out) {
    out = UsnChangeSet();
    char drive[3];
    std::string volume_path;
    NtfsVolumeData volume;
    UniqueHandle h = open_ntfs_volume(root, drive, volume_path, volume);
    if (!h) return 1;
    size_t record_size = volume.bytes_per_file_record_segment;
    auto serial = static_cast<uint64_t>(volume.volume_serial_number);
    UsnJournalData journal;
    if (!query_usn_journal(h.get(), journal)) return 1;
    if (!usn_state_usable(stored_journal_id, stored_next_usn, journal.usn_journal_id,
                          journal.first_usn, journal.next_usn))
        return 1;
    UsnDelta changed = collect_usn_changed_indices(h.get(), journal, stored_next_usn);
    if (changed.kind != DeltaKind::Changed) return 1;
    out.journal_id = journal.usn_journal_id;
    out.next_usn = journal.next_usn;
    if (changed.frns.empty()) return 0;

    // 路径解析需要快照的目录表（没有快照就全量重建）。只取目录表。
    Snapshot dirs_only;
    bool have_dirs = false;
    if (SnapIndex *borrowed = mem_snapshot_get_borrowed(serial)) {
        const Snapshot &b = borrowed->snap();
        dirs_only.volume_serial = b.volume_serial;
        dirs_only.journal_id = b.journal_id;
        dirs_only.next_usn = b.next_usn;
        dirs_only.min_size = b.min_size;
        dirs_only.bytes_per_cluster = b.bytes_per_cluster;
        dirs_only.dirs = b.dirs;
        have_dirs = true;
    } else {
        std::string snap_file = snapshot_path(serial);
        if (!snap_file.empty() && snapshot_load(snap_file, dirs_only) == 0) {
            dirs_only.files.clear();
            dirs_only.files.shrink_to_fit();
            have_dirs = true;
        }
    }
    if (!have_dirs) {
        out = UsnChangeSet();
        return 1;
    }
    // 工作目录表 = 旧表的一份深拷贝。
    SnapIndex old_snap(dirs_only);
    SnapIndex dirs(std::move(dirs_only));

    // 第 1 遍：重读变化 FRN 的 MFT 记录（1MB 下限；小于下限的按消失处理）。
    std::vector<ParsedDeltaItem> parsed;
    apply_usn_delta(h.get(), changed.frns, record_size, volume.bytes_per_sector,
                    volume.bytes_per_cluster, kMftSnapshotFloorMinSize, parsed);
    std::vector<RawFile> file_recs;
    std::vector<uint64_t> dir_frn_list;
    U64Map<uint8_t> seen(parsed.empty() ? 16 : parsed.size());
    for (ParsedDeltaItem &item : parsed) {
        seen.put(item.idx, 1);
        ParsedRecord &rec = item.rec;
        if (rec.kind == RecordKind::Dir) {
            dirs.put_dir(DirNode{rec.index, rec.seq, rec.parent_frn, std::move(rec.name), rec.reparse});
            dir_frn_list.push_back(item.idx);
        } else if (rec.kind == RecordKind::File) {
            file_recs.push_back(RawFile{rec.frn, rec.parent_frn, std::move(rec.name), rec.size,
                                        rec.mtime, rec.first_lcn, rec.sort_key, rec.can_raw_read});
        } else {
            out.deleted_frns.push_back(item.idx);
        }
    }
    for (uint64_t frn : changed.frns)
        if (!seen.get(frn)) out.deleted_frns.push_back(frn);

    // 第 2 遍：用旧/新两份目录表解析本次变化目录的改名。
    {
        ResolveCache old_cache(64), new_cache(64);
        for (uint64_t frn : dir_frn_list) {
            const std::string *o = resolve_dir(frn, old_snap, old_cache, drive, true, skip_top);
            const std::string *n = resolve_dir(frn, dirs, new_cache, drive, true, skip_top);
            if (o && n && *o != *n) {
                out.renamed_old.push_back(*o);
                out.renamed_new.push_back(*n);
            }
        }
    }

    // 第 3 遍：用更新后的目录表解析变化文件的完整路径。
    ResolveCache dir_cache(64);
    const std::string drive_root = std::string(drive) + "\\";
    for (const RawFile &raw : file_recs) {
        const std::string *dir_path =
            (raw.parent_frn & kIndexMask) == 5
                ? &drive_root
                : resolve_dir(raw.parent_frn, dirs, dir_cache, drive, true, skip_top);
        if (!dir_path) {
            out.deleted_frns.push_back(raw.frn & kIndexMask);
            continue;
        }
        out.files.push_back(UsnChangedFile{raw.frn, join_dir(*dir_path, raw.name), raw.size, raw.mtime});
    }
    return 0;
}

#else  // !_WIN32

int volume_usn_state(const char *, uint64_t &, uint64_t &, int64_t &, int64_t &) { return -1; }

int mft_try_scan(const char *, uint64_t, SkipList, KeepFn, FileEntryList &) {
    return 1;  // 不适用：调用方退回目录遍历
}

int usn_changes_since(const char *, uint64_t, int64_t, SkipList, UsnChangeSet &out) {
    out = UsnChangeSet();
    return 1;  // 无 USN：调用方全量重建
}

#endif  // _WIN32

// ---------------- 目录遍历退路 ----------------

namespace {

std::string join_path(const std::string &dir, std::string_view name) {
    std::string p = dir;
    if (p.empty() || (p.back() != '/' && p.back() != '\\')) p.push_back('/');
    p += name;
    return p;
}

}  // namespace

// 单一实现走 enum_dir（util.cpp）：Windows 用 NtQueryDirectoryFile 批量取，
// POSIX 用 opendir/readdir。打不开的目录静默跳过。过滤语义：跳过 "$" 开头名、
// 不跟随链接（POSIX 符号链接 / Windows 重解析点）、Windows 再跳过离线/
// 按需取回属性；顶层目录才比 skip_top。
int fallback_walk_files(const char *root, uint64_t min_size, SkipList skip_top,
                        FileEntryList &out) {
    struct WalkDir {
        std::string path;
        bool is_top;
    };
    std::vector<WalkDir> stack;
    stack.push_back(WalkDir{root, true});
    while (!stack.empty()) {
        WalkDir cur = std::move(stack.back());
        stack.pop_back();
        int r = enum_dir(cur.path, [&](const DirEntry &e) {
            if (!e.name.empty() && e.name[0] == '$') return 0;
            if (e.is_symlink) return 0;  // 不跟随链接
#ifdef _WIN32
            if ((e.attrs & kMftSkipFileAttrs) != 0) return 0;  // 离线/按需取回
#endif
            if (e.is_dir) {
                if (cur.is_top && name_in_skip_top(e.name, skip_top)) return 0;
                stack.push_back(WalkDir{join_path(cur.path, e.name), false});
                return 0;
            }
            if (e.size >= min_size) {
                FileEntry en;
                en.path = join_path(cur.path, e.name);
                en.size = e.size;
                en.mtime = e.mtime;
                out.push_back(std::move(en));
            }
            return 0;
        });
        // 打开失败（缺失/不可读）的目录直接跳过；其它返回为致命错误。
        if (r != kOk && r != kErrNotFound && r != kErrIo) return -1;
    }
    return 0;
}

int scan_files(const char *root, uint64_t min_size, SkipList skip_top, KeepFn keep,
               FileEntryList &out) {
    int r = mft_try_scan(root, min_size, skip_top, keep, out);
    if (r == 0) return 0;
    if (r < 0) return r;
    // r == 1：退回遍历，keep 在名字层再过滤一次（Rust 的 scan_files_where 同款）。
    FileEntryList walked;
    if (fallback_walk_files(root, min_size, skip_top, walked) != 0) return -1;
    for (FileEntry &e : walked) {
        std::string_view path = e.path;
        size_t slash = path.find_last_of("/\\");
        std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
        if (keep && !keep(base, e.size)) continue;
        out.push_back(std::move(e));
    }
    return 0;
}

}  // namespace cdc
