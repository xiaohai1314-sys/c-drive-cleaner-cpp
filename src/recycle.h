// recycle.h — 把文件送进回收站，移植自 Rust 版 desktop/src/recycle.rs。
// FO_DELETE + FOF_ALLOWUNDO；失败时不退回到永久删除，调用方按失败处理。
// 每批 40 个（与 Python 版一致）。
#pragma once

#include <span>
#include <string>
#include <vector>

namespace cdc {

// 返回没能送进回收站的路径。目录、不存在的文件、explicit_delete_forbidden
// 的路径直接计入失败，不进 SHFileOperation。Linux 下全部失败（无回收站 API）。
// 空指针元素按 "" 处理（计入失败）。
std::vector<std::string> send_to_recycle(std::span<const char *const> paths);

}  // namespace cdc
