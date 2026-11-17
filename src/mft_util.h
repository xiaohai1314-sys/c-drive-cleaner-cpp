/* mft_util.h — 纯逻辑移植自 Rust 版 desktop/src/mft.rs
 *
 * 覆盖的函数（逐行对照 Rust 实现，语义相同）：
 *   u16_at / u32_at / u64_at, read_uint_le, read_int_le,
 *   filetime_to_unix, apply_fixups, parse_runlist,
 *   parse_first_extent, utf16le_to_utf8
 *
 * 这些函数不碰任何 Win32 API，所以同一份代码在 Linux 和 Windows
 * 下都能编译、都能跑自测；真正的卷直读（CreateFileW /
 * FSCTL_GET_NTFS_VOLUME_DATA 等）只在 mft_raw.c 里，_WIN32 才编译。
 */
#ifndef CDC_MFT_UTIL_H
#define CDC_MFT_UTIL_H

#include <stddef.h>
#include <stdint.h>

int     mft_u16_at(const uint8_t *b, size_t len, size_t o, uint16_t *out);
int     mft_u32_at(const uint8_t *b, size_t len, size_t o, uint32_t *out);
int     mft_u64_at(const uint8_t *b, size_t len, size_t o, uint64_t *out);

/* 小端无符号 / 有符号变长整数，对应 Rust 的 read_uint_le / read_int_le。
 * bytes 长度 1..8；read_int_le 在高位为 1 时做符号扩展。 */
int64_t mft_read_uint_le(const uint8_t *bytes, size_t n);
int64_t mft_read_int_le(const uint8_t *bytes, size_t n);

/* FILETIME(100ns, 自 1601) -> Unix 秒；小于 epoch 的部分 saturating 到 0。*/
uint64_t mft_filetime_to_unix(uint64_t ft);

/* NTFS 记录的 USA fixup：把每个扇区尾 2 字节换回 usa 数组里的值。
 * 与 Rust 版逐行一致，包括同样的边界提前返回。*/
void mft_apply_fixups(uint8_t *rec, size_t rec_len, size_t bytes_per_sector);

typedef struct { int64_t vcn; int64_t lcn; int64_t len; } MftRun; /* lcn==-1 为稀疏 */

/* 解析 NTFS runlist。返回 malloc 的数组（调用方 free），*out_n 为条数；
 * 出错/空输入返回 NULL 且 *out_n=0。Rust 版在非法 header 处直接 break，
 * 这里同样：已解析出的 run 保留，之后的丢弃。 */
MftRun *mft_parse_runlist(const uint8_t *buf, size_t len, size_t *out_n);

/* 首个 $DATA extent 的 (first_lcn, run_clusters)；稀疏或无效返回 (0,0)。*/
void mft_parse_first_extent(const uint8_t *buf, size_t len,
                            uint64_t *first_lcn, uint64_t *run_clusters);

/* UTF-16LE -> UTF-8（lossy：非法代理对写 U+FFFD，与 Rust from_utf16_lossy
 * 同策略）。返回 malloc 的 NUL 结尾字符串，调用方 free。 */
char *mft_utf16le_to_utf8(const uint8_t *bytes, size_t len);

#endif
