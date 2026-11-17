// bigfile.h — 大文件分档阈值，逐条对照 Rust 版 desktop/src/main.rs
// big_file_threshold_bytes（毒霸 kbigfile.dat，v2024.07.12.1）。
// 扩展名取最后一个 '.' 之后、转小写；无扩展名走兜底 50MB。
// gif 按前端归类算图片 2MB（Rust 注释同款说明）。
#pragma once

#include <cstdint>
#include <string_view>

namespace cdc {

inline constexpr uint64_t kMB = 1024ULL * 1024ULL;
// 2MB 是 scan_big 的枚举预过滤下限（Rust: mft::scan_files_where(..., 2MB)）。
inline constexpr uint64_t kScanMinBytes = 2ULL * kMB;

uint64_t big_file_threshold_bytes(std::string_view path);
// Rust is_system_file_name：这几个系统文件不进大文件列表。
bool is_system_file_name(std::string_view path);

}  // namespace cdc
