/* rules.h — 规则解析/校验/远程更新，移植自 Rust 版 desktop/src/rules.rs
 * 与 desktop/src/main.rs 里的 Rule / all_rules（规则引擎属于规则模块，
 * 故 Rule 结构与 all_rules 一并放在这里，scan 片直接用）。
 *
 * 规则 JSON（rules.json，编译期嵌入，与 Rust include_str! 一致）：
 * 字段与 Rust Rule 结构逐项对应；未知字段忽略。注意一个已验证的怪象：
 * rules.json 里 977 条规则用的是 "desc" 而不是 "describe"，Rust 的
 * serde 同样忽略 "desc"，所以这些规则的 describe 在 Rust 版里就是空
 * 串——这里保持一致，只读 "describe"。
 *
 * 远程更新只接受 https，1MB 上限、200 条上限、可选 blake3 pin；
 * kind 必须为 "files"，tier ∈ {conservative, standard, deep}，
 * 带命令的规则拒绝；version 不降级；原子 temp+rename + 上一版备份；
 * rollback 再校验。下载走 WinHTTP（仅 Windows）。
 */
#ifndef CDC_RULES_H
#define CDC_RULES_H

#include <stddef.h>
#include <stdint.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDC_RULES_MAX_BYTES 1000000u
#define CDC_RULES_MAX_COUNT 200u

typedef struct {
    char *key;
    char *name;
    char *tier;
    char *kind;
    char *describe;
    char *warning;
    char *note;
    char **paths;
    size_t npaths;
    char **glob_dirs;
    size_t nglob_dirs;
    char **patterns;
    size_t npatterns;
    char **exclude;
    size_t nexclude;
    uint64_t min_age_minutes;
    int requires_confirm;
    int prune_dirs;
} cdc_rule_t;

void cdc_rule_free_members(cdc_rule_t *r);

/* 解析一段规则 JSON 文本（{"version": N, "categories": [...]}）。
 * 字段类型错（serde 严格解析）时整个文件失败，返回错误。
 * 成功时 *out_version 为 version（缺失为 0）。 */
int cdc_rules_parse_text(const char *text, cdc_rule_t **out_rules,
                         size_t *out_n, uint64_t *out_version,
                         cdc_err_t *err);
void cdc_rules_free(cdc_rule_t *rules, size_t n);

/* 内置规则（编译期嵌入的 rules.json）+ 远程规则文件合并：
 * 内置优先，同 key 的远程规则跳过（与 Rust all_rules 一致）。
 * 远程文件读不到/解析失败时只返回内置规则，不报错。 */
int cdc_rules_load_all(cdc_rule_t **out_rules, size_t *out_n, cdc_err_t *err);

/* %环境变量% 展开（与 main.rs expand_env 逐行一致：找不到的变量保留
 * 原样 "%NAME%"）。调用方 free()。 */
char *cdc_expand_env(const char *raw);

/* 远程规则文件路径（%LOCALAPPDATA%\C盘清理工具\rules_remote.json，
 * 目录不存在则创建；LOCALAPPDATA 缺失时退回 "."）。调用方 free()。 */
char *cdc_remote_rules_path(void);
char *cdc_remote_rules_backup_path(void);

/* 规则更新配置（exe 旁 rules_update.json，或 exe 下 resources/ 内）：
 * 返回 url（可能为空串）与 blake3 pin（64 位小写 hex，无则 NULL）。
 * 配置文件不存在/损坏时 url 为 ""。 */
int cdc_rules_config(char **out_url, char **out_blake3, cdc_err_t *err);

/* 校验一条规则（与 Rust rules::validate 逐项一致）。失败时 err 填中文原因。 */
int cdc_rule_validate(const char *key, const char *kind, const char *tier,
                      int has_command, int command_non_empty,
                      const char **paths, size_t npaths,
                      const char **glob_dirs, size_t nglob_dirs,
                      cdc_err_t *err);

/* 校验一整份远程规则文件文本：顶层须有 categories 数组（≤200 条），
 * 每条规则过 cdc_rule_validate。供自测与 update/rollback 共用。 */
int cdc_rules_validate_text(const char *text, cdc_err_t *err);

/* key 合法性：非空、≤80 字节、只含 ASCII 字母数字 / '_' / '-'。 */
int cdc_rule_key_ok(const char *key);

/* 远程规则路径白名单（与 Rust path_allowed 逐项一致）。失败时 *out_err
 * 为 malloc 的中文原因（调用方 free()，可 NULL）。 */
int cdc_rule_path_allowed(const char *raw, char **out_err);

typedef struct {
    size_t accepted;
    size_t total;
    char *message; /* 调用方 free() */
} cdc_rules_update_result_t;

/* 下载并保存远程规则（仅 Windows：WinHTTP，20 秒超时）。
 * 成功时填 accepted（新增条数）、total（all_rules 总条数）、message。 */
int cdc_rules_update(cdc_rules_update_result_t *out, cdc_err_t *err);

/* 从备份回滚（再校验）。成功时 *out_message 为 malloc 的中文说明。 */
int cdc_rules_rollback(char **out_message, cdc_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CDC_RULES_H */
