/* privacy.h — 隐私痕迹：浏览器历史 / Cookie / 下载记录、最近文件、
 * 注册表使用痕迹。移植自 Rust 版 desktop/src/privacy.rs。
 * 只统计有把握的项目，密码库和文档库一律不碰；注册表删除前先用
 * reg.exe 导出备份。USB 使用痕迹只统计、不清理（Rust 版同）。 */
#ifndef CDC_PRIVACY_H
#define CDC_PRIVACY_H

#include <stddef.h>
#include <stdint.h>

#include "cdc_hooks.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *id;       /* 如 "hist_chrome"、"risk_file"、"reg_file" */
    char *section;  /* history / download / shop / high / computer /
                       software / media */
    char *name;
    int64_t count;
    int count_available;
    char **files; /* 敏感文件名 / Cookie 文件等 */
    size_t nfiles;
    char *action; /* sql / files / registry / userassist / media / count */
    char *note;
} cdc_privacy_item_t;

void cdc_privacy_item_free(cdc_privacy_item_t *item);
void cdc_privacy_items_free(cdc_privacy_item_t *items, size_t n);

typedef struct {
    int64_t files;
    int64_t sql;
    int64_t registry;
    char **failed;
    size_t nfailed;
} cdc_privacy_report_t;

void cdc_privacy_report_free(cdc_privacy_report_t *r);

/* 注册进度/取消回调（任务 id "privacy"；不注册则不报告、永不取消）。 */
void cdc_privacy_set_hooks(const cdc_hooks_t *hooks);

int cdc_privacy_scan(cdc_privacy_item_t **out, size_t *out_n, cdc_err_t *err);

/* ids 为要清理的 PrivacyItem id；action == "count" 的项会被跳过
 *（Rust 版 clean() 开头即过滤）。 */
int cdc_privacy_clean(const char **ids, size_t nids,
                      cdc_privacy_report_t *out, cdc_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CDC_PRIVACY_H */
