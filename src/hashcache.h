/* hashcache.h — SQLite 哈希缓存，对照 desktop/src/hashcache.rs。
 *
 * 表 file_hashes（path 主键，(size, mtime) 校验，存 head/sample/strong/full
 * 四档 BLAKE3 摘要），默认库 cache_dir()/hash-cache-v3.db。
 *
 * 摘要类型 cdc_digest_t / cdc_digest_set_t / cdc_digest_update_t 的规范出处
 * 是 fileindex.h（切片 4），这里直接用同一定义，不另起一份。
 *
 * 内存模型：条目按 path 排序的数组 + 二分查找（Rust 版是 HashMap；
 * 行为一致，查找 O(log n)）。dirty 记在条目上，save_dirty 只写脏条目。
 * close() 会先尽力 save_dirty 再释放（main.c 的回填流程依赖这一点：
 * 管线把新摘要 remember 进缓存后直接 close，不再另调 save）。
 */
#ifndef CDC_HASHCACHE_H
#define CDC_HASHCACHE_H

#include <stddef.h>
#include <stdint.h>

#include "fileindex.h" /* cdc_digest_t / cdc_digest_set_t / cdc_digest_update_t */
#include "mft.h"       /* cdc_cache_dir / cdc_now_seconds */

typedef struct cdc_hashcache cdc_hashcache_t;

/* ================= slice 4 契约（main.c 的 CROSS-SLICE INTERFACE 块原样） =================
 * 待集成时 main.c 删掉那块、改 include 本头文件，调用点不用动。
 * 约定：
 *  - 返回 int 的函数：0=成功，-1=失败；另有说明的除外。
 *  - 字符串一律 UTF-8；路径在 Windows 下由实现方内部转 UTF-16。
 *  - cache 参数可为 NULL（表示"没有缓存"，管线照常跑）。
 *  - load_for_paths 不保留 paths 指针（调用方在返回后即可释放）。
 */

/* 按路径预加载 file_hashes 缓存（只读已有的摘要，不触发哈希计算）。
 * 默认库 + 迁移旧版 v2 JSON。失败返回 NULL（调用方按"无缓存"处理）。
 * paths 可为 NULL 且 n 为 0（空候选集也返回一个可用的空缓存）。 */
cdc_hashcache_t *cdc_hashcache_load_for_paths(const char **paths, size_t n);
void cdc_hashcache_close(cdc_hashcache_t *cache);
/* 取一条 (path, size, mtime) 的已有摘要：1=找到，0=没有，-1=出错。
 * 找到时 *out 填满（has_* 标明哪些摘要存在）；没找到时 *out 不动。 */
int cdc_hashcache_entry_for(cdc_hashcache_t *cache, const char *path, uint64_t size,
                            uint64_t mtime, cdc_digest_set_t *out);

/* ================= 扩展 API（scan.c 管线与测试用） ================= */

/* 打开指定库（db_path 为 NULL 时为纯内存态，对应 Rust HashCache::default；
 * migrate_legacy 为真时在表为空时迁移 hash-cache-v2.json）。
 * 打开失败也返回一个可用的空缓存（与 Rust load_for_paths_at 一致：
 * 调用方按"无缓存"继续，save_dirty 时再重试打开）。 */
cdc_hashcache_t *cdc_hashcache_open_at(const char *db_path, int migrate_legacy);
cdc_hashcache_t *cdc_hashcache_open(const char *db_path); /* migrate_legacy = 1 */

/* 记一条摘要（对应 Rust remember_digests）：只把 digests 里 has_* 为真的
 * 档位并进去；size/mtime 变了则全部摘要清掉重记；有变化时 last_seen=now
 * 并标脏。返回 0 成功，-1 出错。 */
int cdc_hashcache_remember(cdc_hashcache_t *cache, const char *path, uint64_t size,
                           uint64_t mtime, const cdc_digest_set_t *digests,
                           uint64_t now);

/* 仅把脏条目写回 SQLite（upsert 的 COALESCE 语义与 Rust 版逐字一致，
 * NULL 不覆盖已有摘要），同事务里删掉 last_seen < now-30 天的行。
 * 无脏条目时直接返回 0；提交成功后清脏标记。返回 0 成功，-1 出错
 * （出错时脏标记保留，下次可重试）。 */
int cdc_hashcache_save_dirty(cdc_hashcache_t *cache);

/* 默认库路径 cache_dir()/hash-cache-v3.db 写进 buf（0=成功）。
 * cache_dir() 取不到时返回 -1。 */
int cdc_hashcache_default_path(char *buf, size_t cap);

/* 内存条目数（测试用）。 */
size_t cdc_hashcache_len(cdc_hashcache_t *cache);

/* 32 字节摘要 <-> 64 位小写 hex。hex 长度必须正好是 64 且全是 hex 字符，
 * 否则 cdc_hex_to_digest 返回 0。 */
void cdc_digest_to_hex(const cdc_digest_t *d, char out[65]);
int cdc_hex_to_digest(const char *hex, cdc_digest_t *out);

#endif
