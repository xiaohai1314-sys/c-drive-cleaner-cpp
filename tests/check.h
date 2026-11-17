// check.h — 测试共用的断言计数宏（每个测试可执行文件各自一份计数）。
// CHECK(cond, fmt, ...)：计一次断言；失败时打印 "FAIL: " + 说明并计失败。
#pragma once

#include <cstdio>

namespace cdc_test {
inline int checks = 0;
inline int failures = 0;
}  // namespace cdc_test

#define CHECK(cond, ...)                         \
    do {                                         \
        ::cdc_test::checks++;                    \
        if (!(cond)) {                           \
            ::cdc_test::failures++;              \
            std::printf("FAIL: ");               \
            std::printf(__VA_ARGS__);            \
            std::printf("\n");                   \
        }                                        \
    } while (0)
