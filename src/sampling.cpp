// sampling.cpp — 采样层摘要（XXH3-64）的薄封装。实现在 third_party/xxhash
//（C 库，xxhash.h 自带 extern "C"；XXH_IMPLEMENTATION 定义在 xxhash.c）。
#define XXH_STATIC_LINKING_ONLY
#include "sampling.h"

#include <cstring>

#include "xxhash.h"

namespace cdc {

uint64_t xxh3_64(const uint8_t *data, size_t len) {
    static const uint8_t empty[1] = {0};
    if (!data) data = empty;  // len == 0 时上游也接受空指针，这里再兜一层
    return XXH3_64bits(data, len);
}

std::unique_ptr<Xxh3State> Xxh3State::create() {
    std::unique_ptr<Xxh3State> s(new Xxh3State());
    XXH3_state_t *st = XXH3_createState();
    if (!st) return nullptr;
    s->st_ = st;
    if (XXH3_64bits_reset(st) != XXH_OK) return nullptr;
    return s;
}

Xxh3State::~Xxh3State() {
    if (st_) XXH3_freeState(static_cast<XXH3_state_t *>(st_));
}

int Xxh3State::update(const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    if (!data) return -1;
    return XXH3_64bits_update(static_cast<XXH3_state_t *>(st_), data, len) == XXH_OK ? 0 : -1;
}

uint64_t Xxh3State::finalize() const {
    return XXH3_64bits_digest(static_cast<const XXH3_state_t *>(st_));
}

void xxh_to_digest32(uint64_t xxh, uint8_t out[32]) {
    std::memset(out, 0, 32);
    for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(xxh >> (8 * i));
}

uint64_t xxh_from_digest32(const uint8_t digest[32]) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(digest[i]) << (8 * i);
    return v;
}

void xxh3_buf_to_digest32(const uint8_t *data, size_t len, uint8_t out[32]) {
    xxh_to_digest32(xxh3_64(data, len), out);
}

void xxh_to_hex(uint64_t xxh, char out[17]) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) out[i] = hexd[(xxh >> (4 * (15 - i))) & 0xf];
    out[16] = '\0';
}

}  // namespace cdc
