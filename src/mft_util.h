// mft_util.h — 纯逻辑移植自 Rust 版 desktop/src/mft.rs
//
// 覆盖的函数（逐行对照 Rust 实现，语义相同）：
//   u16_at / u32_at / u64_at, read_uint_le, read_int_le,
//   filetime_to_unix, apply_fixups, parse_runlist,
//   parse_first_extent, utf16le_to_utf8
//
// 这些函数不碰任何 Win32 API，同一份代码在 Linux 和 Windows 下都能编译、
// 都能跑自测；真正的卷直读只在 mft.cpp / mft_raw.cpp 的 _WIN32 段里。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cdc {

// 越界返回 false 且不写 out。
bool mft_u16_at(const uint8_t *b, size_t len, size_t o, uint16_t &out);
bool mft_u32_at(const uint8_t *b, size_t len, size_t o, uint32_t &out);
bool mft_u64_at(const uint8_t *b, size_t len, size_t o, uint64_t &out);

// 小端无符号 / 有符号变长整数（bytes 长度 1..8；read_int_le 在高位为 1 时做符号扩展）。
int64_t mft_read_uint_le(const uint8_t *bytes, size_t n);
int64_t mft_read_int_le(const uint8_t *bytes, size_t n);

// FILETIME(100ns, 自 1601) -> Unix 秒；小于 epoch 的部分 saturating 到 0。
uint64_t mft_filetime_to_unix(uint64_t ft);

// NTFS 记录的 USA fixup：把每个扇区尾 2 字节换回 usa 数组里的值。
void mft_apply_fixups(uint8_t *rec, size_t rec_len, size_t bytes_per_sector);

struct MftRun {
    int64_t vcn = 0;
    int64_t lcn = 0;  // -1 为稀疏
    int64_t len = 0;
};

// 解析 NTFS runlist。Rust 版在非法 header 处直接 break：已解析出的 run 保留。
std::vector<MftRun> mft_parse_runlist(const uint8_t *buf, size_t len);

// 首个 $DATA extent 的 (first_lcn, run_clusters)；稀疏或无效返回 (0,0)。
void mft_parse_first_extent(const uint8_t *buf, size_t len, uint64_t &first_lcn,
                            uint64_t &run_clusters);

// UTF-16LE -> UTF-8（lossy：非法代理对写 U+FFFD，与 Rust from_utf16_lossy 同策略）。
std::string mft_utf16le_to_utf8(const uint8_t *bytes, size_t len);
void mft_utf16le_to_utf8(const uint8_t *bytes, size_t len, std::string &out);  // 复用缓冲

}  // namespace cdc
