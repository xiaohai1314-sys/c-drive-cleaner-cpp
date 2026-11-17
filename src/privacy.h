// privacy.h — 隐私痕迹：浏览器历史 / Cookie / 下载记录、最近文件、
// 注册表使用痕迹。移植自 Rust 版 desktop/src/privacy.rs。
// 只统计有把握的项目，密码库和文档库一律不碰；注册表删除前先用 reg.exe
// 导出备份。USB 使用痕迹只统计、不清理（Rust 版同）。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cdc_hooks.h"

namespace cdc {

struct PrivacyItem {
    std::string id;       // 如 "hist_chrome"、"risk_file"、"reg_file"
    std::string section;  // history / download / shop / high / computer / software / media
    std::string name;
    int64_t count = 0;
    bool count_available = false;
    std::vector<std::string> files;  // 敏感文件名 / Cookie 文件等
    std::string action;              // sql / files / registry / userassist / media / count
    std::string note;
};

struct PrivacyReport {
    int64_t files = 0;
    int64_t sql = 0;
    int64_t registry = 0;
    std::vector<std::string> failed;
};

// 进度 / 取消回调（hooks 为空指针时清除）。
void privacy_set_hooks(const cdc_hooks_t *hooks);

// 扫描全部隐私项（顺序与 Rust 版一致：high、history、download、shop、
// computer、software、media）。Linux 下注册表 / 浏览器目录为空。
std::vector<PrivacyItem> privacy_scan();

// 清理选中的项（action == "count" 的跳过）；取消时 failed 含 "已取消"。
PrivacyReport privacy_clean(std::span<const char *const> ids);

}  // namespace cdc
