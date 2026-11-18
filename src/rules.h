// rules.h — 规则解析/校验/远程更新，移植自 Rust 版 desktop/src/rules.rs
// 与 desktop/src/main.rs 里的 Rule / all_rules（规则引擎属于规则模块，
// 故 Rule 结构与 all_rules 一并放在这里，scan 片直接用）。
//
// 规则 JSON（rules.json，编译期嵌入，与 Rust include_str! 一致）：
// 字段与 Rust Rule 结构逐项对应；未知字段忽略。注意一个已验证的怪象：
// rules.json 里 977 条规则用的是 "desc" 而不是 "describe"，Rust 的
// serde 同样忽略 "desc"，所以这些规则的 describe 在 Rust 版里就是空
// 串——这里保持一致，只读 "describe"。
//
// 远程更新只接受 https，1MB 上限、200 条上限、可选 blake3 pin；
// kind 必须为 "files"，tier ∈ {conservative, standard, deep}，
// 带命令的规则拒绝；version 不降级；原子 temp+rename + 上一版备份；
// rollback 再校验。下载走 WinHTTP（仅 Windows）。
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

inline constexpr uint32_t kRulesMaxBytes = 1000000;
inline constexpr uint32_t kRulesMaxCount = 200;

struct Rule {
    std::string key;
    std::string name;
    std::string tier;
    std::string kind;
    std::string describe;
    std::string warning;
    std::string note;
    std::vector<std::string> paths;
    std::vector<std::string> glob_dirs;
    std::vector<std::string> patterns;
    std::vector<std::string> exclude;
    uint64_t min_age_minutes = 0;
    bool requires_confirm = false;
    bool prune_dirs = false;
};

/* 解析一段规则 JSON 文本（{"version": N, "categories": [...]}）。
 * 字段类型错（serde 严格解析）时整个文件失败，返回错误。
 * 成功时 *out_version 为 version（缺失为 0）。 */
int parse_rules_text(std::string_view text, std::vector<Rule> *out_rules,
                     uint64_t *out_version, Error *err);

/* 内置规则（编译期嵌入的 rules.json）+ 远程规则文件合并：
 * 内置优先，同 key 的远程规则跳过（与 Rust all_rules 一致）。
 * 远程文件读不到/解析失败时只返回内置规则，不报错。 */
int load_all_rules(std::vector<Rule> *out, Error *err);

/* %环境变量% 展开（与 main.rs expand_env 逐行一致：找不到的变量保留
 * 原样 "%NAME%"）。 */
std::string expand_env(std::string_view raw);

/* 远程规则文件路径（%LOCALAPPDATA%\C盘清理工具\rules_remote.json，
 * 目录不存在则创建；LOCALAPPDATA 缺失时退回 "."）。 */
std::optional<std::string> remote_rules_path();
std::optional<std::string> remote_rules_backup_path();

struct RulesConfig {
    std::string url;
    std::optional<std::string> blake3;
};

/* 规则更新配置（exe 旁 rules_update.json，或 exe 下 resources/ 内）。
 * 配置文件不存在/损坏时 url 为 ""。 */
RulesConfig rules_config();

/* key 合法性：非空、≤80 字节、只含 ASCII 字母数字 / '_' / '-'。 */
bool rule_key_ok(std::string_view key);

/* 远程规则路径白名单（与 Rust path_allowed 逐项一致）。失败时 *out_err
 * 为中文原因（可 NULL）。 */
int rule_path_allowed(std::string_view raw, std::string *out_err);

/* 校验一条规则（与 Rust rules::validate 逐项一致）。失败时 err 填中文原因。 */
int validate_rule(std::string_view key, std::string_view kind, std::string_view tier,
                  bool has_command, bool command_non_empty,
                  std::span<const std::string> paths,
                  std::span<const std::string> glob_dirs, Error *err);

/* 校验一整份远程规则文件文本：顶层须有 categories 数组（≤200 条），
 * 每条规则过 validate_rule。供自测与 update/rollback 共用。 */
int validate_rules_text(std::string_view text, Error *err);

struct RulesUpdateResult {
    size_t accepted = 0;
    size_t total = 0;
    std::string message;
};

/* 下载并保存远程规则（仅 Windows：WinHTTP，20 秒超时）。
 * 成功时填 accepted（新增条数）、total（all_rules 总条数）、message。 */
int update_rules(RulesUpdateResult *out, Error *err);

/* 从备份回滚（再校验）。成功时 *out_message 为中文说明。 */
int rollback_rules(std::string *out_message, Error *err);

}  // namespace cdc
