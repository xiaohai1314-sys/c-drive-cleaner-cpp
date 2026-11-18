// shred.h — 文件粉碎，移植自 Rust 版 desktop/src/shred.rs。
//
// 用系统安全随机数覆盖写入后删除，不走回收站。
// 机械盘上覆盖后原内容基本无法恢复；固态盘有磨损均衡和预留空间，
// 覆盖不一定落到原物理块，只能视为「尽力清除」。
// 系统目录、受保护路径和重解析点一律拒绝；目录逐个文件粉碎后清空。
// passes 钳制在 1..=3（kShredMaxPasses）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cdc_hooks.h"
#include "util.h"

namespace cdc {

inline constexpr uint32_t kShredMaxPasses = 3;

struct ShredReport {
    uint64_t freed = 0;
    uint64_t files_ok = 0;
    uint64_t files_failed = 0;
    uint64_t finished = 0; /* 成功清空的目录数 */
    std::vector<std::string> errors;
};

/* 注册进度/取消回调（任务 id "shred"；不注册则不报告、永不取消）。 */
void shred_set_hooks(const cdc_hooks_t *hooks);

int shred_paths(std::span<const std::string> paths, uint32_t passes, ShredReport *out,
                Error *err);

}  // namespace cdc
