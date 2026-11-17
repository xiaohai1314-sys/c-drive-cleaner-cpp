/* timeutil.c — 移植自 Rust 版 desktop/src/timeutil.rs。 */
#include "timeutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

cdc_local_time_t cdc_local_time(uint64_t seconds) {
    cdc_local_time_t out;
    memset(&out, 0, sizeof(out));
#ifdef _WIN32
    /* FILETIME 从 1601-01-01 起算，单位 100ns（与 Rust 版逐行一致，
     * saturating 语义：溢出钳到 u64 最大值）。 */
    uint64_t ticks;
    if (seconds > (UINT64_MAX / 10000000ULL - 11644473600ULL)) {
        ticks = UINT64_MAX;
    } else {
        ticks = (seconds + 11644473600ULL) * 10000000ULL;
    }
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)(ticks >> 32);
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) ||
        !FileTimeToSystemTime(&local, &st))
        return out;
    out.year = st.wYear;
    out.month = st.wMonth;
    out.day = st.wDay;
    out.hour = st.wHour;
    out.minute = st.wMinute;
    out.second = st.wSecond;
    return out;
#else
    time_t t = (time_t)seconds;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return out;
    out.year = (uint16_t)(tmv.tm_year + 1900);
    out.month = (uint16_t)(tmv.tm_mon + 1);
    out.day = (uint16_t)tmv.tm_mday;
    out.hour = (uint16_t)tmv.tm_hour;
    out.minute = (uint16_t)tmv.tm_min;
    out.second = (uint16_t)tmv.tm_sec;
    return out;
#endif
}

char *cdc_month_label(uint64_t seconds) {
    cdc_local_time_t t = cdc_local_time(seconds);
    char *out = (char *)malloc(8);
    if (!out) return NULL;
    if (t.year == 0) {
        memcpy(out, "1970-01", 8);
        return out;
    }
    snprintf(out, 8, "%04u-%02u", t.year, t.month);
    return out;
}

char *cdc_stamp(void) {
    uint64_t seconds;
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks =
        ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* 100ns → 秒，再减 1601→1970 的偏移（与 mft_filetime_to_unix 同算法）。 */
    seconds = ticks / 10000000ULL;
    seconds = seconds >= 11644473600ULL ? seconds - 11644473600ULL : 0;
#else
    seconds = (uint64_t)time(NULL);
#endif
    cdc_local_time_t t = cdc_local_time(seconds);
    char *out = (char *)malloc(16);
    if (!out) return NULL;
    snprintf(out, 16, "%04u%02u%02u_%02u%02u%02u", t.year, t.month, t.day,
             t.hour, t.minute, t.second);
    return out;
}

char *cdc_human_stamp(uint64_t seconds) {
    cdc_local_time_t t = cdc_local_time(seconds);
    char *out = (char *)malloc(20);
    if (!out) return NULL;
    snprintf(out, 20, "%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day,
             t.hour, t.minute, t.second);
    return out;
}
