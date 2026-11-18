// logs.h — 清理日志，移植自 Rust 版 desktop/src/logs.rs。
//
// 每次清理写一份 YYYYMMDD_HHMMSS.log；历史页读取。
// 日志目录与 Rust 版一致：%LOCALAPPDATA%\C盘清理工具\logs
//（LOCALAPPDATA 未设置时退回可执行文件所在目录）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "util.h"

namespace cdc {

/* 应用数据目录 %LOCALAPPDATA%\C盘清理工具（不存在则创建）。 */
std::optional<std::string> app_data_dir();

/* 日志目录 %LOCALAPPDATA%\C盘清理工具\logs（不存在则创建）。 */
std::optional<std::string> logs_dir();

/* 写一份日志：每行去尾空白；文件名 stamp.log，重名时 stamp_2.log…
 * 写完轮转旧日志（只留最新的 100 个 .log）。成功返回 kOk，
 * out_path 非空时填完整路径。 */
int write_log(std::span<const std::string> lines, std::string *out_path, Error *err);

/* 往 logs/<name> 追加一行。name 非法（空、含 '\\' '/' 或 ".."）时
 * 静默丢弃（与 Rust 版一致）。 */
int append_line(std::string_view name, std::string_view line);

struct LogEntry {
    std::string name;
    uint64_t size = 0;
    uint64_t mtime = 0; /* Unix 秒 */
};

int logs_list(std::vector<LogEntry> *out, Error *err);

/* 只允许读取日志目录里的普通文件，防止路径穿越。
 * name 非法返回 kErrInvalidArg；不存在返回 kErrNotFound。 */
int log_read(std::string_view name, std::string *out_text, Error *err);

}  // namespace cdc
