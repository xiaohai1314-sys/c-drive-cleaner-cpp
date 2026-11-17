// sampling.h — 采样层摘要（XXH3-64），对照 scan.cpp 的 head/sample/probe/strong。
//
// 采样层只用 XXH3-64；BLAKE3 只留给最终 full 确认。32 字节载体编码：
// 前 8 字节为 xxHash64 的小端表示，后 24 字节为 0——旧的 digest[32] 槽位
//（fastio 的 HeadResult / IocpResult、scan.cpp 各阶段的 digest）不用改形状，
// 调用方按 xxh_from_digest32 解回 uint64 再比较。
//
// 注意：载体编码的 32 字节值不是 BLAKE3，绝不能写进 hashcache / fileindex
// 里原来存 BLAKE3 的 BLOB 列；落库只写 head_xxh / sample_xxh / strong_xxh。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cdc {

// 一次性 XXH3-64。data 可为空当且仅当 len == 0。
uint64_t xxh3_64(const uint8_t *data, size_t len);

// 流式 XXH3-64（大文件分块喂）。create() 失败返回空指针。
class Xxh3State {
public:
    static std::unique_ptr<Xxh3State> create();
    ~Xxh3State();
    Xxh3State(const Xxh3State &) = delete;
    Xxh3State &operator=(const Xxh3State &) = delete;
    // 返回 0 成功；data 为空且 len > 0 返回 -1。
    int update(const uint8_t *data, size_t len);
    uint64_t finalize() const;

private:
    Xxh3State() = default;
    void *st_ = nullptr;  // XXH3_state_t*
};

// uint64 <-> 32 字节载体编码。
void xxh_to_digest32(uint64_t xxh, uint8_t out[32]);
uint64_t xxh_from_digest32(const uint8_t digest[32]);
// 一次性算 XXH3 并写成载体编码（fastio 头摘要的直接调用点）。
void xxh3_buf_to_digest32(const uint8_t *data, size_t len, uint8_t out[32]);
// uint64 -> 16 位小写 hex + NUL（out 至少 17 字节；fast 模式组 key 用）。
void xxh_to_hex(uint64_t xxh, char out[17]);

}  // namespace cdc
