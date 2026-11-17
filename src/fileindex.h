/* fileindex.h — 常驻索引 SQLite（对应 Rust 版 desktop/src/fileindex.rs）
 *
 * 独立库 file-index-v1.db（与 hashcache 的 file_hashes 分开建库）：
 *   files 表主键 (volume_serial, frn)，path 只是属性；
 *   usn_state 表每卷存 journal_id / next_usn。
 * 覆盖 ≥1MB 文件（CDC_INDEX_MIN_SIZE）；只给可能重复的文件
 * （同 size ≥2 个）回填哈希，单例只存元数据、摘要 NULL。
 *
 * 所有权约定：返回的指针/数组一律由调用方释放（见各函数注释）。
 * 行里的 path 在返回的数组里是 malloc 的字符串，随数组一起 free
 * （cdc_index_hash_rows_free / cdc_index_dup_rows_free /
 * cdc_index_big_rows_free）。
 *
 * 依赖：util.h（cdc_err_t、CDC_OK、错误码 enum；切片 1 提供）。
 * cdc_err_t 的形状：{ int code; char message[256]; }。
 * 错误码用 util.h 的 enum（CDC_ERR_NOMEM / CDC_ERR_IO /
 * CDC_ERR_INVALID_ARG / CDC_ERR_NOT_FOUND …），本模块不再自定。
 *
 * 注意：摘要类型 cdc_digest_t / cdc_digest_set_t 的规范出处在这里；
 * hashcache.c、scan.c 也用同一定义（待集成时移到公共头或各自 include
 * 本头文件，不要各自定义一份）。
 *
 * 游标可用性判定 cdc_usn_state_usable 在 mft.h（与 usn_changes_since
 * 里的判定一致），本模块不再重复定义。
 */
#ifndef CDC_FILEINDEX_H
#define CDC_FILEINDEX_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

/* 索引覆盖的最小文件大小（1MB）。与 mft 快照 floor 及重复扫描默认 min 一致。 */
#define CDC_INDEX_MIN_SIZE ((uint64_t)1 << 20)
/* 大文件查询的下限（最低档阈值 2MB），对应 query_big_rows 的 size >= 2MB。 */
#define CDC_INDEX_BIG_MIN_SIZE ((uint64_t)2 << 20)
/* USN 增量断档阈值（变化 FRN > 25000 即全量重建）见 mft.h 的
 * CDC_MFT_MAX_USN_DELTA_FRNS，判定在 mft.c 的 usn_changes_since 里。 */

#define CDC_INDEX_SCHEMA_VERSION "1"
#define CDC_DIGEST_LEN 32

/* 32 字节摘要（对应 Rust hashcache::Digest = [u8; 32]，blake3）。 */
typedef struct {
    uint8_t bytes[CDC_DIGEST_LEN];
} cdc_digest_t;

/* 一组四种摘要；has_* 为 0 时对应字段无意义（SQL NULL）。
 *
 * 采样层（head / sample / strong）自 xxHash 改造后是 XXH3-64，存在
 * head_xxh / sample_xxh / strong_xxh（uint64_t，SQL INTEGER 列）；
 * head / sample / strong 三个 32 字节字段是旧版 BLAKE3 值——C 管线
 * 不再计算，只做透传（从缓存/索引读到就原样写回去，供 Rust 版读）；
 * full 仍是 BLAKE3 32 字节（最终确认，共享 full 列，兼容）。 */
typedef struct {
    int has_head;
    int has_sample;
    int has_strong;
    int has_full;
    int has_head_xxh;
    int has_sample_xxh;
    int has_strong_xxh;
    cdc_digest_t head;
    cdc_digest_t sample;
    cdc_digest_t strong;
    cdc_digest_t full;
    uint64_t head_xxh;
    uint64_t sample_xxh;
    uint64_t strong_xxh;
} cdc_digest_set_t;

/* 一条文件元数据记录（摘要另由 cdc_digest_update_t 写）。path 为借用指针。 */
typedef struct {
    uint64_t volume_serial;
    uint64_t frn;
    const char *path;
    uint64_t size;
    uint64_t mtime;
    uint64_t last_seen;
} cdc_index_row_t;

/* 一批摘要写回。size / mtime 是写前校验：行内容变了就不写。 */
typedef struct {
    uint64_t volume_serial;
    uint64_t frn;
    uint64_t size;
    uint64_t mtime;
    cdc_digest_set_t digests;
} cdc_digest_update_t;

/* 一卷已存摘要的一条：(frn, digests)。all_digests 返回的数组按 frn 升序，
 * 只含至少一种摘要的行；无 owned 指针，调用方直接 free() 数组。 */
typedef struct {
    uint64_t frn;
    cdc_digest_set_t digests;
} cdc_index_digest_entry_t;

/* 哈希补算 / 重复查询的输入行：元数据 + 已有摘要（可能没有）。
 * path 为 malloc 的字符串。 */
typedef struct {
    uint64_t frn;
    uint64_t size;
    uint64_t mtime;
    char *path;
    cdc_digest_set_t digests;
} cdc_index_hash_row_t;

/* 重复查询的行：跨卷分组需要 volume_serial（硬链接去重用）。 */
typedef struct {
    uint64_t volume_serial;
    uint64_t frn;
    uint64_t size;
    uint64_t mtime;
    char *path;
    cdc_digest_set_t digests;
} cdc_index_dup_row_t;

/* 大文件查询的行：元数据即可。 */
typedef struct {
    uint64_t size;
    uint64_t mtime;
    char *path;
} cdc_index_big_row_t;

/* TRANSITIONAL：C++ 名字，fileindex 迁移时成为正式定义。 */
namespace cdc {
using Digest = cdc_digest_t;
using DigestSet = cdc_digest_set_t;
using DigestUpdate = cdc_digest_update_t;
}  // namespace cdc

typedef struct cdc_fileindex cdc_fileindex_t;

/* 打开索引库（建父目录 + 建表 + PRAGMA）。失败返回 NULL，err 可空。
 * 默认路径：%LOCALAPPDATA%\C盘清理工具\mftcache\file-index-v1.db
 * （LOCALAPPDATA 缺失时回落 "."，与 Rust cache_dir() 一致）。 */
cdc_fileindex_t *cdc_fileindex_open(const char *db_path, cdc_err_t *err);
cdc_fileindex_t *cdc_fileindex_open_default(cdc_err_t *err);
void cdc_fileindex_close(cdc_fileindex_t *idx);

/* 批量写入元数据。同一 (volume_serial, frn) 已存在时更新 path/size/mtime；
 * size 或 mtime 变了则摘要全部清 NULL，否则保留已有摘要。
 * 返回实际写入的行数；事务失败返回 0。 */
size_t cdc_fileindex_upsert_files(cdc_fileindex_t *idx,
                                  const cdc_index_row_t *rows, size_t n);

/* 批量写回摘要。COALESCE 只补空缺（不覆盖已有摘要）；
 * size / mtime 对不上的行不写。now_secs 为调用方传入的当前 unix 秒
 * （写 last_seen 用；Rust 版在函数内取 now_seconds()，这里显式传参
 * 是为了可测，行为一致）。返回实际更新的行数。 */
size_t cdc_fileindex_set_digests_batch(cdc_fileindex_t *idx,
                                       const cdc_digest_update_t *updates, size_t n,
                                       uint64_t now_secs);

/* 写 / 读一卷的 USN 游标。get 返回 1=找到，0=没有，-1=出错。 */
int cdc_fileindex_set_usn_state(cdc_fileindex_t *idx, uint64_t volume_serial,
                                uint64_t journal_id, int64_t next_usn);
int cdc_fileindex_get_usn_state(cdc_fileindex_t *idx, uint64_t volume_serial,
                                uint64_t *journal_id, int64_t *next_usn);

/* 删掉一批 FRN 对应的行。返回实际删除的行数；空列表返回 0。 */
size_t cdc_fileindex_delete_rows(cdc_fileindex_t *idx, uint64_t volume_serial,
                                 const uint64_t *frns, size_t n);

/* 目录改名/移动后，把该目录下所有索引行的 path 前缀批量改写。
 * old_prefix / new_prefix 不带尾斜杠；函数内部补 '\'，只匹配"目录下"
 * （C:\旧名\...），不误伤 C:\旧名2\...。长度用 SQLite 的 LENGTH() 算
 * （字符数），中文目录名安全。返回改写的行数。 */
size_t cdc_fileindex_update_path_prefix(cdc_fileindex_t *idx, uint64_t volume_serial,
                                        const char *old_prefix, const char *new_prefix);

/* 哈希补算的输入：同 size ≥ 2 且组内至少一个缺 full 的组的所有行
 * （含已有 full 的成员）。按 size、frn 排序。返回 malloc 的数组
 * （可能为 NULL 且 *out_n=0），调用方用 cdc_index_hash_rows_free 释放。 */
cdc_index_hash_row_t *cdc_fileindex_rows_needing_full(cdc_fileindex_t *idx,
                                                      uint64_t volume_serial,
                                                      size_t *out_n);
void cdc_index_hash_rows_free(cdc_index_hash_row_t *rows, size_t n);

/* 一卷已存的全部摘要（按 frn 升序）。回填播种用：upsert 之后读回，
 * 索引里保留的摘要可直接复用——FRN 是主键，文件改名/移动后仍在；
 * hashcache 的 file_hashes 按 path 主键、且 30 天未见即剪枝，
 * 改名或久未跑 live 扫描后缓存里已没有，索引里还有。
 * upsert 已保证：size/mtime 变了的行摘要已清 NULL，所以读回的摘要
 * 与当前文件一致，与缓存播种是同一信任级别。
 * 返回 malloc 的数组（可能为 NULL 且 *out_n=0），调用方 free()。 */
cdc_index_digest_entry_t *cdc_fileindex_all_digests(cdc_fileindex_t *idx,
                                                    uint64_t volume_serial,
                                                    size_t *out_n);

/* 清掉一卷的全部索引行和 USN 游标（--rebuild 用）。 */
void cdc_fileindex_delete_volume(cdc_fileindex_t *idx, uint64_t volume_serial);

/* （总行数，含 full 摘要的行数）：全库 / 一卷。 */
void cdc_fileindex_counts(cdc_fileindex_t *idx, uint64_t *total, uint64_t *with_full);
void cdc_fileindex_volume_counts(cdc_fileindex_t *idx, uint64_t volume_serial,
                                 uint64_t *total, uint64_t *with_full);

/* 一卷 USN 游标的更新时间（unix 秒）。返回 1=有，0=没有，-1=出错。 */
int cdc_fileindex_usn_state_updated_at(cdc_fileindex_t *idx, uint64_t volume_serial,
                                       uint64_t *updated_at);

/* 重复查询：给定卷上"同 size ≥ 2"的组的所有行（含缺摘要的成员——
 * 调用方据此判断完整性）。子查询按 size 跨卷分组，跨盘的重复对也能查到。
 * serials 为空返回 NULL 且 *out_n=0。按 size 倒序。 */
cdc_index_dup_row_t *cdc_fileindex_query_dup_rows(cdc_fileindex_t *idx,
                                                  const uint64_t *serials, size_t n_serials,
                                                  uint64_t min_size, size_t *out_n);
void cdc_index_dup_rows_free(cdc_index_dup_row_t *rows, size_t n);

/* 大文件查询：给定卷上 ≥2MB 的所有行，按 size 倒序。 */
cdc_index_big_row_t *cdc_fileindex_query_big_rows(cdc_fileindex_t *idx,
                                                  const uint64_t *serials, size_t n_serials,
                                                  size_t *out_n);
void cdc_index_big_rows_free(cdc_index_big_row_t *rows, size_t n);

/* 自测：与 Rust fileindex.rs 的 #[cfg(test)] 逐项对应
 * （usn_state_usable、回填/摘要规则、删行+中文前缀改写、分组、
 * 清卷、dup/big 查询、计数）。返回失败断言数，0=全过。 */
int cdc_fileindex_selftest(void);

#endif
