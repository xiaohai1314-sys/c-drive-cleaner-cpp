// fileindex.h — 常驻索引 SQLite（对应 Rust 版 desktop/src/fileindex.rs）
//
// 独立库 file-index-v1.db（与 hashcache 的 file_hashes 分开建库）：
//   files 表主键 (volume_serial, frn)，path 只是属性；
//   usn_state 表每卷存 journal_id / next_usn。
// 覆盖 ≥1MB 文件（kIndexMinSize）；只给可能重复的文件（同 size ≥2 个）
// 回填哈希，单例只存元数据、摘要 NULL。
//
// 所有权：查询结果以 std::vector 返回，行内 path 为 std::string（自有）；
// 写入接口的行用 std::string_view 借用路径。
//
// 摘要类型 Digest / DigestSet / DigestUpdate 的规范出处在这里，
// hashcache、scan、main、gui 共用。
//
// 游标可用性判定 usn_state_usable 在 mft.h（与 usn_changes_since 里的判定
// 一致），本模块不再重复定义。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "raii.h"
#include "util.h"

namespace cdc {

// 索引覆盖的最小文件大小（1MB）。与 mft 快照 floor 及重复扫描默认 min 一致。
inline constexpr uint64_t kIndexMinSize = 1ULL << 20;
// 大文件查询的下限（最低档阈值 2MB），对应 query_big_rows 的 size >= 2MB。
inline constexpr uint64_t kIndexBigMinSize = 2ULL << 20;
// USN 增量断档阈值见 mft.h 的 kMftMaxUsnDeltaFrns。
inline constexpr const char *kIndexSchemaVersion = "1";
inline constexpr size_t kDigestLen = 32;

// 32 字节摘要（对应 Rust hashcache::Digest = [u8; 32]，blake3）。
struct Digest {
    uint8_t bytes[kDigestLen] = {};
};

// 一组摘要；has_* 为 false 时对应字段无意义（SQL NULL）。
//
// 采样层（head / sample / strong）自 xxHash 改造后是 XXH3-64，存在
// head_xxh / sample_xxh / strong_xxh（SQL INTEGER 列）；head / sample /
// strong 三个 32 字节字段是旧版 BLAKE3 值——C++ 管线不再计算，只做透传
// （从缓存/索引读到就原样写回去，供 Rust 版读）；full 仍是 BLAKE3 32 字节。
struct DigestSet {
    bool has_head = false;
    bool has_sample = false;
    bool has_strong = false;
    bool has_full = false;
    bool has_head_xxh = false;
    bool has_sample_xxh = false;
    bool has_strong_xxh = false;
    Digest head;
    Digest sample;
    Digest strong;
    Digest full;
    uint64_t head_xxh = 0;
    uint64_t sample_xxh = 0;
    uint64_t strong_xxh = 0;
};

// 一条文件元数据记录（摘要另由 DigestUpdate 写）。path 借用。
struct IndexRow {
    uint64_t volume_serial = 0;
    uint64_t frn = 0;
    std::string_view path;
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint64_t last_seen = 0;
};

// 一批摘要写回。size / mtime 是写前校验：行内容变了就不写。
struct DigestUpdate {
    uint64_t volume_serial = 0;
    uint64_t frn = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;
    DigestSet digests;
};

// 一卷已存摘要的一条：(frn, digests)。all_digests 按 frn 升序，只含至少
// 一种 BLAKE3 摘要的行。
struct IndexDigestEntry {
    uint64_t frn = 0;
    DigestSet digests;
};

// 哈希补算的输入行：元数据 + 已有摘要（可能没有）。
struct IndexHashRow {
    uint64_t frn = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;
    std::string path;
    DigestSet digests;
};

// 重复查询的行：跨卷分组需要 volume_serial（硬链接去重用）。
struct IndexDupRow {
    uint64_t volume_serial = 0;
    uint64_t frn = 0;
    uint64_t size = 0;
    uint64_t mtime = 0;
    std::string path;
    DigestSet digests;
};

// 大文件查询的行：元数据即可。
struct IndexBigRow {
    uint64_t size = 0;
    uint64_t mtime = 0;
    std::string path;
};

struct UsnState {
    uint64_t journal_id = 0;
    int64_t next_usn = 0;
};

struct IndexCounts {
    uint64_t total = 0;
    uint64_t with_full = 0;
};

class FileIndex {
public:
    // 打开索引库（建父目录 + 建表 + PRAGMA）。失败返回空，err 可空。
    // 默认路径：%LOCALAPPDATA%\C盘清理工具\mftcache\file-index-v1.db
    // （LOCALAPPDATA 缺失时回落 "."，与 Rust cache_dir() 一致）。
    static std::unique_ptr<FileIndex> open(const char *db_path, Error *err);
    static std::unique_ptr<FileIndex> open_default(Error *err);

    FileIndex(const FileIndex &) = delete;
    FileIndex &operator=(const FileIndex &) = delete;

    // 批量写入元数据。同一 (volume_serial, frn) 已存在时更新 path/size/mtime；
    // size 或 mtime 变了则摘要全部清 NULL，否则保留已有摘要。
    // 返回实际写入的行数；事务失败返回 0。
    size_t upsert_files(std::span<const IndexRow> rows);

    // 批量写回摘要。COALESCE 只补空缺（不覆盖已有摘要）；size / mtime 对不上
    // 的行不写。now_secs 写 last_seen（显式传参以便测试）。返回实际更新的行数。
    size_t set_digests_batch(std::span<const DigestUpdate> updates, uint64_t now_secs);

    // 写 / 读一卷的 USN 游标。set 返回 0 成功、-1 失败；get 没有或出错返回 nullopt。
    int set_usn_state(uint64_t volume_serial, uint64_t journal_id, int64_t next_usn);
    std::optional<UsnState> get_usn_state(uint64_t volume_serial);

    // 删掉一批 FRN 对应的行。返回实际删除的行数；空列表返回 0。
    size_t delete_rows(uint64_t volume_serial, std::span<const uint64_t> frns);

    // 目录改名/移动后，把该目录下所有索引行的 path 前缀批量改写。
    // old_prefix / new_prefix 不带尾斜杠；内部补 '\'，只匹配"目录下"
    // （C:\旧名\...），不误伤 C:\旧名2\...。长度用 SQLite 的 LENGTH() 算
    // （字符数），中文目录名安全。返回改写的行数。
    size_t update_path_prefix(uint64_t volume_serial, std::string_view old_prefix,
                              std::string_view new_prefix);

    // 哈希补算的输入：同 size ≥ 2 且组内至少一个缺 full 的组的所有行
    // （含已有 full 的成员）。按 size、frn 排序。
    std::vector<IndexHashRow> rows_needing_full(uint64_t volume_serial);

    // 一卷已存的全部摘要（按 frn 升序）。回填播种用：FRN 是主键，改名/移动后
    // 仍在；upsert 已保证 size/mtime 变了的行摘要已清 NULL。
    std::vector<IndexDigestEntry> all_digests(uint64_t volume_serial);

    // 清掉一卷的全部索引行和 USN 游标（--rebuild 用）。
    void delete_volume(uint64_t volume_serial);

    // （总行数，含 full 摘要的行数）：全库 / 一卷。出错时为 0。
    IndexCounts counts();
    IndexCounts volume_counts(uint64_t volume_serial);

    // 一卷 USN 游标的更新时间（unix 秒）。没有或出错返回 nullopt。
    std::optional<uint64_t> usn_state_updated_at(uint64_t volume_serial);

    // 重复查询：给定卷上"同 size ≥ 2"的组的所有行（含缺摘要的成员——调用方
    // 据此判断完整性）。子查询按 size 跨卷分组。serials 为空返回空。按 size 倒序。
    std::vector<IndexDupRow> query_dup_rows(std::span<const uint64_t> serials,
                                            uint64_t min_size);

    // 大文件查询：给定卷上 ≥2MB 的所有行，按 size 倒序。
    std::vector<IndexBigRow> query_big_rows(std::span<const uint64_t> serials);

private:
    explicit FileIndex(UniqueSqlite db) : db_(std::move(db)) {}
    UniqueSqlite db_;
};

// 自测：与 Rust fileindex.rs 的 #[cfg(test)] 逐项对应。返回失败断言数。
int fileindex_selftest();

}  // namespace cdc
