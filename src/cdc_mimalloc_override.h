/* cdc_mimalloc_override.h — mimalloc 全局替换的 -include 入口（性能优化项 6）。
 *
 * 本文件只被构建的 -include / /FI 引用，不直接 #include。
 *
 * 顺序有讲究：必须先包含声明了 malloc 家族的系统头，再包含
 * mimalloc-override.h。glibc 的 <stdlib.h> 在 _DEFAULT_SOURCE 下有
 * 一处 __attr_dealloc (reallocarray, 1)（reallocarray 以自身为释放函数）：
 * 若宏先定义，第一处声明会被重命名为 mi_reallocarray，导致第二处的
 * reallocarray 未声明而编译失败（-std=c11 -Werror 下实测）。
 * 系统头先行可避免该问题；之后再包含的系统头因 include guard 不再
 * 重复处理，宏只作用于本项目自己的代码。
 *
 * 另外：-include 在翻译单元自己的 #define 之前生效，而 features.h
 * 只在首次包含时读取特性宏——若 wrapper 不先定义 _GNU_SOURCE，
 * main.cpp / util.cpp 等在文件顶部才 #define _DEFAULT_SOURCE 的翻译单元
 * 会在 wrapper 里就把 features.h 锁死为严格 ANSI，导致 lstat /
 * O_CLOEXEC 等声明丢失（实测）。所以 wrapper 先定义 _GNU_SOURCE
 *（超集，含 _DEFAULT_SOURCE / _POSIX_C_SOURCE 的声明；与各文件顶部
 * 的同名 #define 值相同，不触发重定义警告）。
 */
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "mimalloc-override.h"

/* TRANSITIONAL（C++ 迁移期）：自家 C++ 翻译单元也 -include 本文件时，
 * 标准库头里的 std::malloc / std::free 等会被宏改写成 std::mi_*；
 * 把 mi_* 引入 std 命名空间让这些调用仍可解析。迁移完成后自家 C++ 代码
 * 不再 -include 本文件（只剩 sqlite3.c / cJSON.c 用），本段随之无用。 */
#ifdef __cplusplus
namespace std {
using ::mi_calloc;
using ::mi_free;
using ::mi_malloc;
using ::mi_realloc;
}  // namespace std
#endif
