// hashcache.h — SQLite 哈希缓存，对照 desktop/src/hashcache.rs。
//
// 表 file_hashes（path 主键，(size, mtime) 校验，存 head/sample/strong/full
// 四档 BLAKE3 摘要 + head_xxh/sample_xxh/strong_xxh 三档 XXH3-64），
// 默认库 cache_dir()/hash-cache-v3.db。
//
// 摘要类型（DigestSet 等）的规范出处是 fileindex.h，这里直接用同一定义。
//
// 内存模型：条目按 path 排序的 std::vector + 二分查找（Rust 版是 HashMap；
// 行为一致，查找 O(log n)）。dirty 记在条目上，save_dirty 只写脏条目。
// 析构会先尽力 save_dirty（main 的回填流程依赖这一点：管线把新摘要
// remember 进缓存后直接销毁，不再另调 save）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fileindex.h"  // DigestSet / Digest

namespace cdc {

class HashCache {
public:
    // 按路径预加载默认库（只读已有摘要，不触发哈希计算）+ 迁移旧版 v2 JSON。
    // 打不开库时返回可用的空缓存（调用方按"无缓存"继续）；默认路径取不到时为
    // 纯内存态。不保留 paths（返回后调用方即可释放）。
    static std::unique_ptr<HashCache> load_for_paths(std::span<const char *const> paths);
    static std::unique_ptr<HashCache> load_for_paths(std::span<const std::string> paths);

    // 打开指定库（db_path 为空指针时为纯内存态，对应 Rust HashCache::default；
    // migrate_legacy 为真时在表为空时迁移 hash-cache-v2.json）。不预加载任何路径。
    // 打开失败也返回可用的空缓存，save_dirty 时再重试打开。
    static std::unique_ptr<HashCache> open_at(const char *db_path, bool migrate_legacy);
    static std::unique_ptr<HashCache> open(const char *db_path) { return open_at(db_path, true); }

    ~HashCache();  // 尽力 save_dirty，忽略错误
    HashCache(const HashCache &) = delete;
    HashCache &operator=(const HashCache &) = delete;

    // 取一条 (path, size, mtime) 的已有摘要；(size, mtime) 必须精确匹配。
    std::optional<DigestSet> entry_for(std::string_view path, uint64_t size,
                                       uint64_t mtime) const;

    // 记一条摘要（对应 Rust remember_digests）：只并入 digests 里 has_* 为真的
    // 档位；size/mtime 变了则全部摘要清掉重记；有变化时 last_seen=now 并标脏。
    void remember(std::string_view path, uint64_t size, uint64_t mtime,
                  const DigestSet &digests, uint64_t now);

    // 仅把脏条目写回 SQLite（upsert 的 COALESCE 语义与 Rust 版逐字一致，
    // NULL 不覆盖已有摘要），同事务删 last_seen < now-30 天的行。
    // 无脏条目时直接返回 0；提交成功后清脏标记。返回 0 成功，-1 出错
    // （出错时脏标记保留，下次可重试）。
    int save_dirty();

    size_t size() const { return entries_.size(); }  // 内存条目数（测试用）

private:
    struct Entry {
        std::string path;
        uint64_t size = 0, mtime = 0, last_seen = 0;
        DigestSet digests{};
        bool dirty = false;
    };

    HashCache() = default;
    static std::unique_ptr<HashCache> load_at(const char *db_path,
                                              std::vector<std::string_view> paths,
                                              bool migrate_legacy);
    // 二分查找：找到返回 true 且 pos 为下标；否则 pos 为插入位置。
    bool find(std::string_view path, size_t &pos) const;

    std::optional<std::string> db_path_;  // 无值 = 纯内存态
    std::vector<Entry> entries_;           // 按 path 排序
};

// 默认库路径 cache_dir()/hash-cache-v3.db；cache_dir() 取不到时返回 nullopt。
std::optional<std::string> hashcache_default_path();

// 32 字节摘要 <-> 64 位小写 hex。hex 长度必须正好是 64 且全是 hex 字符，
// 否则 hex_to_digest 返回 false（out 可能已部分写入）。
std::string digest_to_hex(const Digest &d);
void digest_to_hex(const Digest &d, char out[65]);
bool hex_to_digest(std::string_view hex, Digest &out);

}  // namespace cdc
