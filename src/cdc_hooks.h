/* cdc_hooks.h — 长任务进度/阶段/取消回调（本切片内各模块共用）。
 *
 * Rust 版的 task.rs（任务坞：set_progress/set_stage/set_unit/is_cancelled）
 * 由另一片（index+CLI / bridge）移植；本切片的 shred/privacy/system/im
 * 只通过这组回调向外报告进度、查询取消，不直接依赖 task 实现。
 * 调用方（bridge）在启动任务前用各模块的 cdc_*_set_hooks() 注册一次；
 * 不注册时默认：不报告进度、永不取消。
 * task 参数是 Rust 版的任务 id（"shred"、"privacy"、"system"、"im_move"、
 * "im_clean" 等），供 bridge 按 id 路由到任务坞。
 */
#ifndef CDC_HOOKS_H
#define CDC_HOOKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cdc_progress_fn)(void *ctx, const char *task, uint64_t done,
                                uint64_t total);
typedef void (*cdc_stage_fn)(void *ctx, const char *task, const char *stage);
typedef void (*cdc_unit_fn)(void *ctx, const char *task, const char *unit);
typedef int (*cdc_cancel_fn)(void *ctx, const char *task);
/* im_clean 完成后要让重复文件缓存失效（Rust 版调
 * crate::invalidate_dup_cache()）；bridge 在此注册回调，不注册则不做。 */
typedef void (*cdc_invalidate_fn)(void *ctx);

typedef struct {
    cdc_progress_fn progress; /* 可 NULL */
    cdc_stage_fn stage;       /* 可 NULL */
    cdc_unit_fn unit;         /* 可 NULL */
    cdc_cancel_fn cancel;     /* 可 NULL（不注册 = 永不取消） */
    cdc_invalidate_fn invalidate_dup_cache; /* 可 NULL */
    void *ctx;
} cdc_hooks_t;

#ifdef __cplusplus
}
#endif

#endif /* CDC_HOOKS_H */
