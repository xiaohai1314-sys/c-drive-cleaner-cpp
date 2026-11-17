/* mft_util.c — 见头文件注释：逐行对照 desktop/src/mft.rs */
#include "mft_util.h"

#include <stdlib.h>
#include <string.h>

int mft_u16_at(const uint8_t *b, size_t len, size_t o, uint16_t *out) {
    if (o + 2 > len) return 0;
    *out = (uint16_t)(b[o] | (b[o + 1] << 8));
    return 1;
}

int mft_u32_at(const uint8_t *b, size_t len, size_t o, uint32_t *out) {
    if (o + 4 > len) return 0;
    *out = (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
           ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
    return 1;
}

int mft_u64_at(const uint8_t *b, size_t len, size_t o, uint64_t *out) {
    if (o + 8 > len) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[o + i] << (8 * i);
    *out = v;
    return 1;
}

int64_t mft_read_uint_le(const uint8_t *bytes, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= (uint64_t)bytes[i] << (8 * i);
    return (int64_t)v;
}

int64_t mft_read_int_le(const uint8_t *bytes, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v |= (uint64_t)bytes[i] << (8 * i);
    size_t bits = n * 8;
    if (bits < 64 && (v & (1ULL << (bits - 1))) != 0) v |= ~0ULL << bits;
    return (int64_t)v;
}

uint64_t mft_filetime_to_unix(uint64_t ft) {
    const uint64_t WINDOWS_EPOCH_DIFF_SECS = 11644473600ULL;
    uint64_t secs = ft / 10000000ULL;
    return secs >= WINDOWS_EPOCH_DIFF_SECS ? secs - WINDOWS_EPOCH_DIFF_SECS : 0;
}

void mft_apply_fixups(uint8_t *rec, size_t rec_len, size_t bytes_per_sector) {
    uint16_t usa_off = 0, usa_count = 0;
    if (!mft_u16_at(rec, rec_len, 4, &usa_off)) return;
    if (!mft_u16_at(rec, rec_len, 6, &usa_count)) return;
    if (usa_off == 0 || usa_count < 2 ||
        (size_t)usa_off + (size_t)usa_count * 2 > rec_len)
        return;
    for (size_t i = 1; i < usa_count; i++) {
        size_t pos = i * bytes_per_sector;
        size_t src = (size_t)usa_off + i * 2;
        if (pos < 2 || pos > rec_len || src + 2 > rec_len) break;
        rec[pos - 2] = rec[src];
        rec[pos - 1] = rec[src + 1];
    }
}

MftRun *mft_parse_runlist(const uint8_t *buf, size_t len, size_t *out_n) {
    *out_n = 0;
    size_t cap = 8, n = 0;
    MftRun *runs = (MftRun *)malloc(cap * sizeof(MftRun));
    if (!runs) return NULL;
    int64_t vcn = 0, prev_lcn = 0;
    size_t pos = 0;
    while (pos < len) {
        uint8_t header = buf[pos++];
        if (header == 0) break;
        size_t len_size = header & 0x0F;
        size_t off_size = header >> 4;
        if (len_size == 0 || len_size > 8 || off_size > 8 ||
            pos + len_size + off_size > len)
            break;
        int64_t run_len = mft_read_uint_le(buf + pos, len_size);
        pos += len_size;
        int64_t off = off_size == 0 ? 0 : mft_read_int_le(buf + pos, off_size);
        pos += off_size;
        if (n == cap) {
            cap *= 2;
            MftRun *grown = (MftRun *)realloc(runs, cap * sizeof(MftRun));
            if (!grown) { free(runs); *out_n = 0; return NULL; }
            runs = grown;
        }
        if (off_size > 0) {
            prev_lcn += off;
            runs[n++] = (MftRun){vcn, prev_lcn, run_len};
        } else {
            runs[n++] = (MftRun){vcn, -1, run_len};
        }
        vcn += run_len;
    }
    *out_n = n;
    return runs;
}

void mft_parse_first_extent(const uint8_t *buf, size_t len,
                            uint64_t *first_lcn, uint64_t *run_clusters) {
    *first_lcn = 0;
    *run_clusters = 0;
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
        *first_lcn = (uint64_t)lcn;
        *run_clusters = (uint64_t)run_len;
    }
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

char *mft_utf16le_to_utf8(const uint8_t *bytes, size_t len) {
    size_t cap = len * 3 / 2 + 8, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t i = 0;
    while (i + 1 < len) {
        uint16_t u = (uint16_t)(bytes[i] | (bytes[i + 1] << 8));
        i += 2;
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
            uint16_t lo = (uint16_t)(bytes[i] | (bytes[i + 1] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                i += 2;
                cp = 0x10000 + ((uint32_t)(u - 0xD800) << 10) + (lo - 0xDC00);
            } else {
                cp = 0xFFFD;
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            cp = 0xFFFD;
        } else {
            cp = u;
        }
        char tmp[4];
        size_t w = utf8_encode(cp, tmp);
        if (n + w + 1 > cap) {
            cap *= 2;
            char *grown = (char *)realloc(out, cap);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        memcpy(out + n, tmp, w);
        n += w;
    }
    out[n] = '\0';
    return out;
}
