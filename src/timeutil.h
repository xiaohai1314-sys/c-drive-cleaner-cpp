// timeutil.h — 本地时间换算，移植自 Rust 版 desktop/src/timeutil.rs。
//
// Unix 秒 → 本地 SYSTEMTIME，供日志文件名与聊天月份分组使用。
// Windows 上走 FileTimeToLocalFileTime；Linux 上走 localtime_r
//（同样取系统本地时区）。
#pragma once

#include <cstdint>
#include <string>

namespace cdc {

struct LocalTime {
    uint16_t year = 0;
    uint16_t month = 0;
    uint16_t day = 0;
    uint16_t hour = 0;
    uint16_t minute = 0;
    uint16_t second = 0;
};

/* Unix 秒 → 本地时间；换算失败返回全 0（与 Rust 版一致）。 */
LocalTime local_time(uint64_t seconds);

/* "YYYY-MM"；换算失败（year==0）时返回 "1970-01"（与 Rust 版一致）。 */
std::string month_label(uint64_t seconds);

/* 当前本地时间 "YYYYMMDD_HHMMSS"（日志文件名用）。 */
std::string stamp();

/* "YYYY-MM-DD HH:MM:SS"。 */
std::string human_stamp(uint64_t seconds);

}  // namespace cdc
