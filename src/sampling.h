/* sampling.h — 采样层摘要（XXH3-64），对照 scan.c 的 head/sample/probe/strong。
 *
 * 采样层只用 XXH3-64（64 位快速非密码学哈希）；BLAKE3 只留给最终
 * full 确认。32 字节载体编码：前 8 字节为 xxHash64 的小端表示，后 24
 * 字节为 0——这样旧的 digest[32] 槽位（fastio 的 CdcHeadResult /
 * CdcIocpResult、scan.c 各阶段的 digest[32]）不用改形状，调用方按
 * cdc_xxh_from_digest32 解回 uint64 再比较。
 *
 * 注意：载体编码的 32 字节值不是 BLAKE3，绝不能写进 hashcache /
 * fileindex 里原来存 BLAKE3 的 BLOB 列（会污染 Rust 版读到的值）；
 * 落库只写 head_xxh / sample_xxh / strong_xxh 三个 INTEGER 列。
 */
#ifndef CDC_SAMPLING_H
#define CDC_SAMPLING_H

#include <stddef.h>
#include <stdint.h>

/* 一次性 XXH3-64。data 可为 NULL 当且仅当 len == 0。 */
uint64_t cdc_xxh3_64(const uint8_t *data, size_t len);

/* 流式 XXH3-64（大文件分块喂）。new 失败返回 NULL；update 返回 0 成功。 */
typedef struct cdc_xxh3_state cdc_xxh3_state_t;
cdc_xxh3_state_t *cdc_xxh3_state_new(void);
void cdc_xxh3_state_free(cdc_xxh3_state_t *s);
int cdc_xxh3_state_update(cdc_xxh3_state_t *s, const uint8_t *data, size_t len);
uint64_t cdc_xxh3_state_finalize(cdc_xxh3_state_t *s);

/* uint64 <-> 32 字节载体编码。 */
void cdc_xxh_to_digest32(uint64_t xxh, uint8_t out[32]);
uint64_t cdc_xxh_from_digest32(const uint8_t digest[32]);
/* 一次性算 XXH3 并写成载体编码（fastio 头摘要的直接调用点）。 */
void cdc_xxh3_buf_to_digest32(const uint8_t *data, size_t len, uint8_t out[32]);

/* uint64 -> 16 位小写 hex + NUL（out 至少 17 字节；fast 模式组 key 用）。 */
void cdc_xxh_to_hex(uint64_t xxh, char out[17]);

#endif
