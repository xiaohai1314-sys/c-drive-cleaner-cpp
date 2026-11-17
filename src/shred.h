/* shred.h — 文件粉碎，移植自 Rust 版 desktop/src/shred.rs。
 * 用系统安全随机数覆盖写入后删除，不走回收站。
 * 机械盘上覆盖后原内容基本无法恢复；固态盘有磨损均衡和预留空间，
 * 覆盖不一定落到原物理块，只能视为「尽力清除」。
 * 系统目录、受保护路径和重解析点一律拒绝；目录逐个文件粉碎后清空。
 * passes 钳制在 1..=3（MAX_PASSES）。 */
#ifndef CDC_SHRED_H
#define CDC_SHRED_H

#include <stddef.h>
#include <stdint.h>

#include "cdc_hooks.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDC_SHRED_MAX_PASSES 3u

typedef struct {
    uint64_t freed;
    uint64_t files_ok;
    uint64_t files_failed;
    uint64_t finished; /* 成功清空的目录数 */
    char **errors;
    size_t nerrors;
} cdc_shred_report_t;

void cdc_shred_report_free(cdc_shred_report_t *r);

/* 注册进度/取消回调（任务 id "shred"；不注册则不报告、永不取消）。 */
void cdc_shred_set_hooks(const cdc_hooks_t *hooks);

int cdc_shred_paths(const char **paths, size_t npaths, uint32_t passes,
                    cdc_shred_report_t *out, cdc_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CDC_SHRED_H */
