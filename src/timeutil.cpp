/* timeutil.cpp — 移植自 Rust 版 desktop/src/timeutil.rs。 */
#include "timeutil.h"

#include <ctime>

#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace cdc {
namespace {

void write_two_digits(char *out, unsigned value) {
    if (value > 99) value = 99;
    out[0] = (char)('0' + value / 10);
    out[1] = (char)('0' + value % 10);
}

void write_four_digits(char *out, unsigned value) {
    if (value > 9999) value = 9999;
    out[0] = (char)('0' + (value / 1000) % 10);
    out[1] = (char)('0' + (value / 100) % 10);
    out[2] = (char)('0' + (value / 10) % 10);
    out[3] = (char)('0' + value % 10);
}

}  // namespace

LocalTime local_time(uint64_t seconds) {
    LocalTime out{};
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

std::string month_label(uint64_t seconds) {
    LocalTime t = local_time(seconds);
    if (t.year == 0) return "1970-01";
    char out[8];
    write_four_digits(out, t.year);
    out[4] = '-';
    write_two_digits(out + 5, t.month);
    out[7] = '\0';
    return std::string(out, 7);
}

std::string stamp() {
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
    LocalTime t = local_time(seconds);
    char out[16];
    write_four_digits(out, t.year);
    write_two_digits(out + 4, t.month);
    write_two_digits(out + 6, t.day);
    out[8] = '_';
    write_two_digits(out + 9, t.hour);
    write_two_digits(out + 11, t.minute);
    write_two_digits(out + 13, t.second);
    out[15] = '\0';
    return std::string(out, 15);
}

std::string human_stamp(uint64_t seconds) {
    LocalTime t = local_time(seconds);
    char out[20];
    write_four_digits(out, t.year);
    out[4] = '-';
    write_two_digits(out + 5, t.month);
    out[7] = '-';
    write_two_digits(out + 8, t.day);
    out[10] = ' ';
    write_two_digits(out + 11, t.hour);
    out[13] = ':';
    write_two_digits(out + 14, t.minute);
    out[16] = ':';
    write_two_digits(out + 17, t.second);
    out[19] = '\0';
    return std::string(out, 19);
}

}  // namespace cdc
