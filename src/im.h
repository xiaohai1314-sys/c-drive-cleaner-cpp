/* im.h — 微信 / QQ 专清，移植自 Rust 版 desktop/src/im.rs。
 * 只识别媒体、缓存和日志，聊天数据库与配置一律跳过。
 * 另含 system.c 搬家功能依赖的 roots / migration_roots /
 * is_program_root。 */
#ifndef CDC_IM_H
#define CDC_IM_H

#include <stddef.h>
#include <stdint.h>

#include "cdc_hooks.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *path;
    uint64_t size;
    uint64_t mtime; /* Unix 秒 */
    char *kind;     /* video / image / audio / file / log / cache / sns */
    char *sub;      /* kind=="file" 时的细分：doc / video / image /
                       music / archive / other */
    char *month;    /* "YYYY-MM" */
    char *name;     /* 文件名 */
    uint64_t seq;   /* 插入序号（仅供 mtime 相同时稳定排序） */
} cdc_im_record_t;

void cdc_im_record_free(cdc_im_record_t *r);
void cdc_im_records_free(cdc_im_record_t *records, size_t n);

/* 探测到的数据根目录（存在的才返回，已排序去重）。
 * kind: "wechat" / "qq"，其它值返回空列表。 */
int cdc_im_roots(const char *kind, char ***out, size_t *out_n, cdc_err_t *err);

/* 迁移源白名单候选（不要求目录存在：恢复时源目录刚被改名，
 * 正是不存在的状态）。 */
int cdc_im_migration_roots(const char *kind, char ***out, size_t *out_n,
                           cdc_err_t *err);

/* 位于 AppData 下的根目录是客户端程序数据（只有缓存和日志）。 */
int cdc_im_is_program_root(const char *root);

/* 分类：成功返回 1 且 *out_kind 与 *out_sub 为 malloc 字符串；
 * 不属于可清理项返回 0（out 不填）。root 可为 NULL。 */
int cdc_im_classify(const char *path, const char *root, char **out_kind,
                    char **out_sub);

/* 扫描各根目录下的可清理媒体/缓存/日志记录，按 mtime 降序排列。 */
int cdc_im_list_media(const char **roots, size_t nroots,
                      cdc_im_record_t **out, size_t *out_n, cdc_err_t *err);

/* 缩略图协议只服务扫描结果里的文件，协议本身不能读任意路径。 */
void cdc_im_remember_scanned(const cdc_im_record_t *records, size_t n);
int cdc_im_is_scanned(const char *path);

/* 缩略图：微信 .dat 是 XOR 0x05 加密的图片，这里还原。
 * 找到返回 1（*out_mime 为静态字符串，*out_data 调用方 free()）；
 * 不是图片返回 0；读失败返回 CDC_ERR_*。 */
int cdc_im_thumbnail(const char *path, const char **out_mime,
                     uint8_t **out_data, size_t *out_len, cdc_err_t *err);

typedef struct {
    char *text;
    int binary;
    int truncated;
    uint64_t size;
} cdc_im_preview_t;

void cdc_im_preview_free(cdc_im_preview_t *p);

/* 日志预览：文本直接显示；二进制（.xlog 等）给前 512 字节的十六进制。 */
int cdc_im_preview(const char *path, cdc_im_preview_t *out, cdc_err_t *err);

typedef struct {
    uint64_t freed;
    uint64_t deleted;
    char **failed;
    size_t nfailed;
    char **reasons;
    size_t nreasons;
} cdc_delete_report_t;

void cdc_delete_report_free(cdc_delete_report_t *r);

/* 注册进度/取消回调（任务 id "im_clean"；不注册则不报告、永不取消）。
 * hooks.invalidate_dup_cache 若注册，清理成功后会被调用。 */
void cdc_im_set_hooks(const cdc_hooks_t *hooks);

/* 清理：kinds 为要清理的 kind 列表；paths==NULL 时扫描全部根目录，
 * 否则只清理给出的路径（必须在 roots 内、通过 classify 且 kind 在列）。
 * 注意：不检查微信/QQ 进程是否运行——进程检查只在搬家时做。 */
int cdc_im_clean(const char *kind, const char **kinds, size_t nkinds,
                 const char **paths, size_t npaths, int has_paths,
                 cdc_delete_report_t *out, cdc_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* CDC_IM_H */
