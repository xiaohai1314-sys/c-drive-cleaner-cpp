/* logs.h — 清理日志，移植自 Rust 版 desktop/src/logs.rs。
 * 每次清理写一份 YYYYMMDD_HHMMSS.log；历史页读取。
 * 日志目录与 Rust 版一致：%LOCALAPPDATA%\C盘清理工具\logs
 *（LOCALAPPDATA 未设置时退回可执行文件所在目录）。 */
#ifndef CDC_LOGS_H
#define CDC_LOGS_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 应用数据目录 %LOCALAPPDATA%\C盘清理工具（不存在则创建）。
 * 调用方 free()。 */
char *cdc_app_data_dir(void);

/* 缓存目录 %LOCALAPPDATA%\C盘清理工具\mftcache（不存在则创建）。
 * 调用方 free()。实现在 util.cpp（不要在这里另写实现）。 */

/* 日志目录 %LOCALAPPDATA%\C盘清理工具\logs（不存在则创建）。
 * 调用方 free()。 */
char *cdc_logs_dir(void);

/* 写一份日志：每行去尾空白；文件名 stamp.log，重名时 stamp_2.log…
 * 写完轮转旧日志（只留最新的 100 个 .log）。成功返回 CDC_OK，
 * *out_path 为 malloc 的完整路径（调用方 free()，可 NULL）。 */
int cdc_write_log(const char **lines, size_t nlines, char **out_path,
                  cdc_err_t *err);

/* 往 logs/<name> 追加一行。name 非法（空、含 '\\' '/' 或 ".."）时
 * 静默丢弃（与 Rust 版一致）。 */
int cdc_append_line(const char *name, const char *line);

typedef struct {
    char *name;
    uint64_t size;
    uint64_t mtime; /* Unix 秒 */
} cdc_log_entry_t;

int cdc_logs_list(cdc_log_entry_t **out, size_t *out_n, cdc_err_t *err);
void cdc_logs_list_free(cdc_log_entry_t *entries, size_t n);

/* 只允许读取日志目录里的普通文件，防止路径穿越。
 * name 非法返回 CDC_ERR_INVALID_ARG；不存在返回 CDC_ERR_NOT_FOUND。 */
int cdc_log_read(const char *name, char **out_text, cdc_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CDC_LOGS_H */
