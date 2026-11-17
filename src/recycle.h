/* recycle.h — 把文件送进回收站，移植自 Rust 版 desktop/src/recycle.rs。
 * FO_DELETE + FOF_ALLOWUNDO；失败时不退回到永久删除，调用方按失败处理。
 * 每批 40 个（与 Python 版一致）。 */
#ifndef CDC_RECYCLE_H
#define CDC_RECYCLE_H

#include <stddef.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 返回没能送进回收站的路径（malloc 的字符串数组，cdc_strvec_free 释放）。
 * 目录、不存在的文件、explicit_delete_forbidden 的路径直接计入失败，
 * 不进 SHFileOperation。Linux 下全部失败（无回收站 API）。 */
int cdc_send_to_recycle(const char **paths, size_t npaths, char ***out_failed,
                        size_t *out_nfailed, cdc_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CDC_RECYCLE_H */
