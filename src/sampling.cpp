/* sampling.c — 采样层摘要（XXH3-64）的薄封装。实现在
 * third_party/xxhash/xxhash.h（本文件只取声明，不定义
 * XXH_IMPLEMENTATION——定义在 third_party/xxhash/xxhash.c）。
 */
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

#include "sampling.h"

#include <stdlib.h>
#include <string.h>

uint64_t cdc_xxh3_64(const uint8_t *data, size_t len) {
    static const uint8_t empty[1] = {0};
    if (!data) data = empty; /* len == 0 时上游也接受 NULL，这里再兜一层 */
    return XXH3_64bits(data, len);
}

struct cdc_xxh3_state {
    XXH3_state_t *st;
};

cdc_xxh3_state_t *cdc_xxh3_state_new(void) {
    cdc_xxh3_state_t *s = (cdc_xxh3_state_t *)malloc(sizeof *s);
    if (!s) return NULL;
    s->st = XXH3_createState();
    if (!s->st) {
        free(s);
        return NULL;
    }
    if (XXH3_64bits_reset(s->st) != XXH_OK) {
        XXH3_freeState(s->st);
        free(s);
        return NULL;
    }
    return s;
}

void cdc_xxh3_state_free(cdc_xxh3_state_t *s) {
    if (!s) return;
    XXH3_freeState(s->st);
    free(s);
}

int cdc_xxh3_state_update(cdc_xxh3_state_t *s, const uint8_t *data, size_t len) {
    if (!s) return -1;
    if (len == 0) return 0;
    if (!data) return -1;
    return XXH3_64bits_update(s->st, data, len) == XXH_OK ? 0 : -1;
}

uint64_t cdc_xxh3_state_finalize(cdc_xxh3_state_t *s) {
    if (!s) return 0;
    return XXH3_64bits_digest(s->st);
}

void cdc_xxh_to_digest32(uint64_t xxh, uint8_t out[32]) {
    memset(out, 0, 32);
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(xxh >> (8 * i));
}

uint64_t cdc_xxh_from_digest32(const uint8_t digest[32]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)digest[i] << (8 * i);
    return v;
}

void cdc_xxh3_buf_to_digest32(const uint8_t *data, size_t len, uint8_t out[32]) {
    cdc_xxh_to_digest32(cdc_xxh3_64(data, len), out);
}

void cdc_xxh_to_hex(uint64_t xxh, char out[17]) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = hexd[(xxh >> (4 * (15 - i))) & 0xf];
    out[16] = '\0';
}
