// mft.h — $MFT 解析 + 快照 + USN Journal 增量，对照 desktop/src/mft.rs。
//
// Windows 专属的卷操作（CreateFileW / FSCTL_*）只在 _WIN32 下编译；
// Linux 下 mft_try_scan 返回"不适用"，调用方退回目录遍历。
// 纯逻辑函数（parse_record、快照编解码、USN gap 判定）全平台可测。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mft_util.h"  // MftRun
#include "util.h"      // FunctionRef

namespace cdc {

inline constexpr uint64_t kMftSnapshotFloorMinSize = 1ULL << 20;
inline constexpr size_t kMftMaxUsnDeltaFrns = 25000;
inline constexpr uint64_t kMftRawReadMinAgeSecs = 300;

inline constexpr uint32_t kFileAttrReparsePoint = 0x00000400u;
inline constexpr uint32_t kFileAttrCompressed = 0x00000800u;
inline constexpr uint32_t kFileAttrSparseFile = 0x00000200u;
inline constexpr uint32_t kFileAttrOffline = 0x00001000u;
inline constexpr uint32_t kFileAttrEncrypted = 0x00004000u;
inline constexpr uint32_t kFileAttrRecallOnOpen = 0x00040000u;
inline constexpr uint32_t kFileAttrRecallOnDataAccess = 0x00400000u;
inline constexpr uint32_t kMftSkipFileAttrs = kFileAttrReparsePoint | kFileAttrOffline |
                                              kFileAttrRecallOnOpen | kFileAttrRecallOnDataAccess;
inline constexpr uint32_t kMftNoRawAttrs =
    kMftSkipFileAttrs | kFileAttrCompressed | kFileAttrEncrypted | kFileAttrSparseFile;

// 顶层跳过目录列表（比较时 ASCII 转小写后比对）。
using SkipList = std::span<const char *const>;

// mft.rs TOP_SKIP（与 Rust main.rs 的 TOP_SKIP 逐字一致）。
inline constexpr const char *kTopSkipNames[] = {
    "$recycle.bin", "system volume information", "windows", "program files",
    "program files (x86)", "programdata", "recovery", "perflogs", "msocache",
    "config.msi", "$windows.~bt", "$windows.~ws", "windows.old",
};
inline constexpr SkipList kTopSkip{kTopSkipNames};

struct FileEntry {
    std::string path;  // UTF-8
    uint64_t size = 0;
    uint64_t mtime = 0;  // unix 秒
    uint64_t sort_key = 0;
    uint64_t frn = 0;  // 完整 FRN：seq<<48 | index
    uint64_t first_lcn = 0;
    uint32_t bytes_per_cluster = 0;
    bool can_raw_read = false;
};
using FileEntryList = std::vector<FileEntry>;

// keep 回调：name 为文件名（不含路径），返回 false 丢弃。可为空（全保留）。
using KeepFn = FunctionRef<bool(std::string_view name, uint64_t size)>;

// 扫描入口：先试 $MFT（Windows），不行退回目录遍历。结果追加到 out。返回 0 成功。
int scan_files(const char *root, uint64_t min_size, SkipList skip_top, KeepFn keep,
               FileEntryList &out);
// 仅 $MFT 路径；0=成功追加 entries，1=不适用（调用方应退回遍历），<0=出错。
int mft_try_scan(const char *root, uint64_t min_size, SkipList skip_top, KeepFn keep,
                 FileEntryList &out);
// 目录遍历退路（全平台）：skip_top 只在根目录那一层生效。
int fallback_walk_files(const char *root, uint64_t min_size, SkipList skip_top,
                        FileEntryList &out);

// ---- parse_record_in_place ----

enum class RecordKind { Skip, Dir, File, MftRoot };

struct ParsedRecord {
    RecordKind kind = RecordKind::Skip;
    uint64_t index = 0;       // 记录号（48 位）
    uint64_t frn = 0;         // 完整 FRN（文件）
    uint64_t parent_frn = 0;  // 文件/目录
    std::string name;         // 文件/目录；Skip/Root 为空
    uint64_t size = 0;        // 文件
    uint64_t mtime = 0;       // 文件
    uint64_t first_lcn = 0;   // 文件
    uint64_t sort_key = 0;    // 文件
    bool can_raw_read = false;
    uint16_t seq = 0;          // 目录
    bool reparse = false;      // 目录
    std::vector<MftRun> runs;  // MftRoot：$MFT 自身的 runlist
};

// rec 会被原地改写（USA fixup）。out 先被重置（保留容量）。返回 0 成功。
int parse_record_in_place(uint8_t *rec, size_t rec_len, uint64_t index, size_t bytes_per_sector,
                          size_t bytes_per_cluster, uint64_t min_size, ParsedRecord &out);

// ---- 快照（与 Rust 版二进制格式字节一致，见 mft.rs save_snapshot） ----

struct DirNode {
    uint64_t index = 0;
    uint16_t seq = 0;
    uint64_t parent_frn = 0;
    std::string name;
    bool reparse = false;
};

struct RawFile {
    uint64_t frn = 0, parent_frn = 0;
    std::string name;
    uint64_t size = 0, mtime = 0, first_lcn = 0, sort_key = 0;
    bool can_raw_read = false;
};

struct Snapshot {
    uint64_t volume_serial = 0, journal_id = 0;
    int64_t next_usn = 0;
    uint64_t min_size = 0;
    uint32_t bytes_per_cluster = 0;
    std::vector<DirNode> dirs;
    std::vector<RawFile> files;
};

int snapshot_save(const std::string &path, const Snapshot &snap);  // 经 .tmp 原子改名；0 成功
int snapshot_load(const std::string &path, Snapshot &out);         // 0 成功；失败时 out 为空快照

// ---- USN ----

int volume_usn_state(const char *root, uint64_t &volume_serial, uint64_t &journal_id,
                     int64_t &first_usn, int64_t &next_usn);  // 0 成功

// 纯函数：存量游标是否还能增量追赶（与 usn_changes_since 里的判定一致）。
bool usn_state_usable(uint64_t stored_journal_id, int64_t stored_next_usn, uint64_t journal_id,
                      int64_t first_usn, int64_t next_usn);

struct UsnChangedFile {
    uint64_t frn = 0;
    std::string path;
    uint64_t size = 0, mtime = 0;
};

struct UsnChangeSet {
    uint64_t journal_id = 0;
    int64_t next_usn = 0;
    std::vector<uint64_t> deleted_frns;
    std::vector<UsnChangedFile> files;
    std::vector<std::string> renamed_old;  // 与 renamed_new 一一对应
    std::vector<std::string> renamed_new;
};

// 0=成功；1=调用方应全量重建；<0=出错。
int usn_changes_since(const char *root, uint64_t stored_journal_id, int64_t stored_next_usn,
                      SkipList skip_top, UsnChangeSet &out);

// ---- 共享小工具 ----
uint64_t now_seconds();

// mft_raw.cpp（仅 Windows）：打开 \\.\X: 打印卷参数，--probe 命令用。
#ifdef _WIN32
int mft_probe_volume(const wchar_t *drive_letter);
#endif

}  // namespace cdc
