// im.h — 微信 / QQ 专清，移植自 Rust 版 desktop/src/im.rs。
// 只识别媒体、缓存和日志，聊天数据库与配置一律跳过。
// 另含 system 搬家功能依赖的 roots / migration_roots /
// is_program_root。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cdc_hooks.h"
#include "util.h"

namespace cdc {

struct ImRecord {
    std::string path;
    uint64_t size = 0;
    uint64_t mtime = 0; /* Unix 秒 */
    std::string kind;   /* video / image / audio / file / log / cache / sns */
    std::string sub;    /* kind=="file" 时的细分：doc / video / image /
                           music / archive / other */
    std::string month;  /* "YYYY-MM" */
    std::string name;   /* 文件名 */
    uint64_t seq = 0;   /* 插入序号（仅供 mtime 相同时稳定排序） */
};

/* 探测到的数据根目录（存在的才返回，已排序去重）。
 * kind: "wechat" / "qq"，其它值返回空列表。 */
int im_roots(std::string_view kind, std::vector<std::string> *out, Error *err);

/* 迁移源白名单候选（不要求目录存在：恢复时源目录刚被改名，
 * 正是不存在的状态）。 */
int im_migration_roots(std::string_view kind, std::vector<std::string> *out, Error *err);

/* 位于 AppData 下的根目录是客户端程序数据（只有缓存和日志）。 */
bool im_is_program_root(std::string_view root);

/* 分类：成功返回 true 且填 kind/sub；不属于可清理项返回 false。
 * root 可为空（空串表示无根目录）。 */
bool im_classify(std::string_view path, std::string_view root, std::string *out_kind,
                 std::string *out_sub);

/* 扫描各根目录下的可清理媒体/缓存/日志记录，按 mtime 降序排列。 */
int im_list_media(std::span<const std::string> roots, std::vector<ImRecord> *out,
                  Error *err);

/* 缩略图协议只服务扫描结果里的文件，协议本身不能读任意路径。 */
void im_remember_scanned(std::span<const ImRecord> records);
bool im_is_scanned(std::string_view path);

/* 缩略图：微信 .dat 是 XOR 0x05 加密的图片，这里还原。
 * 找到返回 1（*out_mime 为静态字符串，*out_data 为解密后字节）；
 * 不是图片返回 0；读失败返回 kErr*。 */
int im_thumbnail(std::string_view path, const char **out_mime,
                 std::vector<uint8_t> *out_data, Error *err);

struct ImPreview {
    std::string text;
    bool binary = false;
    bool truncated = false;
    uint64_t size = 0;
};

/* 日志预览：文本直接显示；二进制（.xlog 等）给前 512 字节的十六进制。 */
int im_preview(std::string_view path, ImPreview *out, Error *err);

struct DeleteReport {
    uint64_t freed = 0;
    uint64_t deleted = 0;
    std::vector<std::string> failed;
    std::vector<std::string> reasons;
};

/* 注册进度/取消回调（任务 id "im_clean"；不注册则不报告、永不取消）。
 * hooks.invalidate_dup_cache 若注册，清理成功后会被调用。 */
void im_set_hooks(const cdc_hooks_t *hooks);

/* 清理：kinds 为要清理的 kind 列表；has_paths 为假时扫描全部根目录，
 * 为真时只清理给出的路径（必须在 roots 内、通过 classify 且 kind 在列）。
 * 注意：不检查微信/QQ 进程是否运行——进程检查只在搬家时做。 */
int im_clean(std::string_view kind, std::span<const std::string> kinds,
             std::span<const std::string> paths, bool has_paths, DeleteReport *out,
             Error *err);

}  // namespace cdc
