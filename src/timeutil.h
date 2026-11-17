/* timeutil.h — 本地时间换算，移植自 Rust 版 desktop/src/timeutil.rs。
 * Unix 秒 → 本地 SYSTEMTIME，供日志文件名与聊天月份分组使用。
 * Windows 上走 FileTimeToLocalFileTime；Linux 上走 localtime_r
 *（同样取系统本地时区）。 */
#ifndef CDC_TIMEUTIL_H
#define CDC_TIMEUTIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;
    uint16_t month;
    uint16_t day;
    uint16_t hour;
    uint16_t minute;
    uint16_t second;
} cdc_local_time_t;

/* Unix 秒 → 本地时间；换算失败返回全 0（与 Rust 版一致）。 */
cdc_local_time_t cdc_local_time(uint64_t seconds);

/* "YYYY-MM"；year==0 时返回 "1970-01"（与 Rust 版一致）。调用方 free()。 */
char *cdc_month_label(uint64_t seconds);

/* 当前本地时间 "YYYYMMDD_HHMMSS"（日志文件名用）。调用方 free()。 */
char *cdc_stamp(void);

/* "YYYY-MM-DD HH:MM:SS"。调用方 free()。 */
char *cdc_human_stamp(uint64_t seconds);

#ifdef __cplusplus
}
#endif

#endif /* CDC_TIMEUTIL_H */
