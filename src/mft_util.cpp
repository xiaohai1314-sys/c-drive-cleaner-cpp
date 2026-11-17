// mft_util.cpp — 见头文件注释：逐行对照 desktop/src/mft.rs
#include "mft_util.h"

#include "util.h"

namespace cdc {

bool mft_u16_at(const uint8_t *b, size_t len, size_t o, uint16_t &out) {
    if (o + 2 > len) return false;
    out = static_cast<uint16_t>(b[o] | (b[o + 1] << 8));
    return true;
}

bool mft_u32_at(const uint8_t *b, size_t len, size_t o, uint32_t &out) {
    if (o + 4 > len) return false;
    out = static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
          (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
    return true;
}

bool mft_u64_at(const uint8_t *b, size_t len, size_t o, uint64_t &out) {
    if (o + 8 > len) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(b[o + i]) << (8 * i);
    out = v;
    return true;
}

int64_t mft_read_uint_le(const uint8_t *bytes, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return static_cast<int64_t>(v);
}

int64_t mft_read_int_le(const uint8_t *bytes, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    size_t bits = n * 8;
    if (bits < 64 && (v & (1ULL << (bits - 1))) != 0) v |= ~0ULL << bits;
    return static_cast<int64_t>(v);
}

uint64_t mft_filetime_to_unix(uint64_t ft) {
    constexpr uint64_t kWindowsEpochDiffSecs = 11644473600ULL;
    uint64_t secs = ft / 10000000ULL;
    return secs >= kWindowsEpochDiffSecs ? secs - kWindowsEpochDiffSecs : 0;
}

void mft_apply_fixups(uint8_t *rec, size_t rec_len, size_t bytes_per_sector) {
    uint16_t usa_off = 0, usa_count = 0;
    if (!mft_u16_at(rec, rec_len, 4, usa_off)) return;
    if (!mft_u16_at(rec, rec_len, 6, usa_count)) return;
    if (usa_off == 0 || usa_count < 2 ||
        static_cast<size_t>(usa_off) + static_cast<size_t>(usa_count) * 2 > rec_len)
        return;
    for (size_t i = 1; i < usa_count; i++) {
        size_t pos = i * bytes_per_sector;
        size_t src = static_cast<size_t>(usa_off) + i * 2;
        if (pos < 2 || pos > rec_len || src + 2 > rec_len) break;
        rec[pos - 2] = rec[src];
        rec[pos - 1] = rec[src + 1];
    }
}

std::vector<MftRun> mft_parse_runlist(const uint8_t *buf, size_t len) {
    std::vector<MftRun> runs;
    runs.reserve(8);
    int64_t vcn = 0, prev_lcn = 0;
    size_t pos = 0;
    while (pos < len) {
        uint8_t header = buf[pos++];
        if (header == 0) break;
        size_t len_size = header & 0x0F;
        size_t off_size = header >> 4;
        if (len_size == 0 || len_size > 8 || off_size > 8 || pos + len_size + off_size > len)
            break;
        int64_t run_len = mft_read_uint_le(buf + pos, len_size);
        pos += len_size;
        int64_t off = off_size == 0 ? 0 : mft_read_int_le(buf + pos, off_size);
        pos += off_size;
        if (off_size > 0) {
            prev_lcn += off;
            runs.push_back(MftRun{vcn, prev_lcn, run_len});
        } else {
            runs.push_back(MftRun{vcn, -1, run_len});
        }
        vcn += run_len;
    }
    return runs;
}

void mft_parse_first_extent(const uint8_t *buf, size_t len, uint64_t &first_lcn,
                            uint64_t &run_clusters) {
    first_lcn = 0;
    run_clusters = 0;
    if (len == 0) return;
    uint8_t header = buf[0];
    if (header == 0) return;
    size_t len_size = header & 0x0F;
    size_t off_size = header >> 4;
    if (len_size == 0 || len_size > 8 || off_size == 0 || off_size > 8 ||
        1 + len_size + off_size > len)
        return;
    int64_t run_len = mft_read_uint_le(buf + 1, len_size);
    int64_t lcn = mft_read_int_le(buf + 1 + len_size, off_size);
    if (lcn > 0 && run_len > 0) {
        first_lcn = static_cast<uint64_t>(lcn);
        run_clusters = static_cast<uint64_t>(run_len);
    }
}

void mft_utf16le_to_utf8(const uint8_t *bytes, size_t len, std::string &out) {
    out.clear();
    out.reserve(len * 3 / 2 + 8);
    size_t i = 0;
    while (i + 1 < len) {
        auto u = static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
        i += 2;
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
            auto lo = static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                i += 2;
                cp = 0x10000 + (static_cast<uint32_t>(u - 0xD800) << 10) + (lo - 0xDC00);
            } else {
                cp = 0xFFFD;
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            cp = 0xFFFD;
        } else {
            cp = u;
        }
        append_utf8(out, cp);
    }
}

std::string mft_utf16le_to_utf8(const uint8_t *bytes, size_t len) {
    std::string out;
    mft_utf16le_to_utf8(bytes, len, out);
    return out;
}

}  // namespace cdc
