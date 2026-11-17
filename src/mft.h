/* mft.h — $MFT 解析 + 快照 + USN Journal 增量，对照 desktop/src/mft.rs。
 *
 * Windows 专属的卷操作（CreateFileW / FSCTL_*）只在 _WIN32 下编译；
 * Linux 下 cdc_mft_try_scan 返回"不适用"，调用方退回目录遍历。
 * 纯逻辑函数（parse_record、快照编解码、USN gap 判定）全平台可测。
 *
 * 注意：cdc_now_seconds 由本模块提供；cdc_cache_dir 的实现在 util.c，
 * 这里的声明只是复用同一原型（fileindex.c、hashcache.c、scan.c 经 mft.h
 * 拿到它；不要在本模块另写实现——此前这里的版本把"盘"写了两遍）。
 */
#ifndef CDC_MFT_H
#define CDC_MFT_H

#include <stddef.h>
#include <stdint.h>

#include "mft_util.h" /* MftRun */

#define CDC_MFT_SNAPSHOT_FLOOR_MIN_SIZE (1ULL << 20)
#define CDC_MFT_MAX_USN_DELTA_FRNS 25000
#define CDC_MFT_RAW_READ_MIN_AGE_SECS 300

#define CDC_FILE_ATTR_REPARSE_POINT 0x00000400u
#define CDC_FILE_ATTR_COMPRESSED 0x00000800u
#define CDC_FILE_ATTR_SPARSE_FILE 0x00000200u
#define CDC_FILE_ATTR_OFFLINE 0x00001000u
#define CDC_FILE_ATTR_ENCRYPTED 0x00004000u
#define CDC_FILE_ATTR_RECALL_ON_OPEN 0x00040000u
#define CDC_FILE_ATTR_RECALL_ON_DATA_ACCESS 0x00400000u
#define CDC_MFT_SKIP_FILE_ATTRS \
    (CDC_FILE_ATTR_REPARSE_POINT | CDC_FILE_ATTR_OFFLINE | \
     CDC_FILE_ATTR_RECALL_ON_OPEN | CDC_FILE_ATTR_RECALL_ON_DATA_ACCESS)
#define CDC_MFT_NO_RAW_ATTRS \
    (CDC_MFT_SKIP_FILE_ATTRS | CDC_FILE_ATTR_COMPRESSED | \
     CDC_FILE_ATTR_ENCRYPTED | CDC_FILE_ATTR_SPARSE_FILE)

/* 顶层跳过目录（mft.rs TOP_SKIP，比较时转小写后比对）。 */
extern const char *const CDC_TOP_SKIP[];
extern const size_t CDC_TOP_SKIP_N;

typedef struct {
    char *path; /* malloc'd UTF-8，调用方用 cdc_file_entry_list_free 释放 */
    uint64_t size;
    uint64_t mtime; /* unix 秒 */
    uint64_t sort_key;
    uint64_t frn; /* 完整 FRN：seq<<48 | index */
    uint64_t first_lcn;
    uint32_t bytes_per_cluster;
    int can_raw_read;
} CdcFileEntry;

typedef struct {
    CdcFileEntry *items;
    size_t n, cap;
} CdcFileEntryList;

void cdc_file_entry_list_free(CdcFileEntryList *list);

/* keep 回调：name 为文件名（不含路径），返回 0 丢弃、非 0 保留。ctx 可为 NULL。 */
typedef int (*cdc_keep_fn)(const char *name, uint64_t size, void *ctx);

/* 扫描入口：先试 $MFT（Windows），不行退回目录遍历。返回 0 成功。 */
int cdc_scan_files(const char *root, uint64_t min_size,
                   const char *const *skip_top, size_t n_skip,
                   cdc_keep_fn keep, void *keep_ctx, CdcFileEntryList *out);

/* 仅 $MFT 路径；0=成功填入 entries，1=不适用（调用方应退回遍历），<0=出错。 */
int cdc_mft_try_scan(const char *root, uint64_t min_size,
                     const char *const *skip_top, size_t n_skip,
                     cdc_keep_fn keep, void *keep_ctx, CdcFileEntryList *out);

/* 目录遍历退路（全平台）：skip_top 只在根目录那一层生效。 */
int cdc_fallback_walk_files(const char *root, uint64_t min_size,
                            const char *const *skip_top, size_t n_skip,
                            CdcFileEntryList *out);

/* ---- parse_record_in_place ---- */

typedef enum {
    CDC_REC_SKIP,
    CDC_REC_DIR,
    CDC_REC_FILE,
    CDC_REC_MFT_ROOT
} CdcRecordKind;

typedef struct {
    CdcRecordKind kind;
    uint64_t index;      /* 记录号（48 位） */
    uint64_t frn;        /* 完整 FRN（文件） */
    uint64_t parent_frn; /* 文件/目录 */
    char *name;          /* malloc'd，文件/目录；Skip/Root 为 NULL */
    uint64_t size;       /* 文件 */
    uint64_t mtime;      /* 文件 */
    uint64_t first_lcn;  /* 文件 */
    uint64_t sort_key;   /* 文件 */
    int can_raw_read;    /* 文件 */
    uint16_t seq;        /* 目录 */
    int reparse;         /* 目录 */
    MftRun *runs;        /* MFT_ROOT：$MFT 自身的 runlist */
    size_t n_runs;
} CdcParsedRecord;

/* rec 会被原地改写（USA fixup）。返回 0 成功。 */
int cdc_parse_record_in_place(uint8_t *rec, size_t rec_len, uint64_t index,
                              size_t bytes_per_sector, size_t bytes_per_cluster,
                              uint64_t min_size, CdcParsedRecord *out);
void cdc_parsed_record_free(CdcParsedRecord *r);

/* ---- 快照（与 Rust 版二进制格式字节一致，见 mft.rs save_snapshot） ---- */

typedef struct {
    uint64_t index;
    uint16_t seq;
    uint64_t parent_frn;
    char *name; /* malloc'd */
    int reparse;
} CdcDirNode;

typedef struct {
    uint64_t frn, parent_frn;
    char *name; /* malloc'd */
    uint64_t size, mtime, first_lcn, sort_key;
    int can_raw_read;
} CdcRawFile;

typedef struct {
    uint64_t volume_serial, journal_id;
    int64_t next_usn;
    uint64_t min_size;
    uint32_t bytes_per_cluster;
    CdcDirNode *dirs;
    size_t n_dirs;
    CdcRawFile *files;
    size_t n_files;
} CdcSnapshot;

int cdc_snapshot_save(const char *path, const CdcSnapshot *snap); /* 经 .tmp 原子改名 */
int cdc_snapshot_load(const char *path, CdcSnapshot *out);        /* 0 成功 */
void cdc_snapshot_free(CdcSnapshot *snap);

/* ---- USN ---- */

int cdc_volume_usn_state(const char *root, uint64_t *volume_serial, uint64_t *journal_id,
                         int64_t *first_usn, int64_t *next_usn); /* 0 成功 */

/* 纯函数：存量游标是否还能增量追赶（与 usn_changes_since 里的判定一致）。 */
int cdc_usn_state_usable(uint64_t stored_journal_id, int64_t stored_next_usn,
                         uint64_t journal_id, int64_t first_usn, int64_t next_usn);

/* 1=可用，0=应全量重建 */

typedef struct {
    uint64_t frn;
    char *path; /* malloc'd */
    uint64_t size, mtime;
} CdcUsnChangedFile;

typedef struct {
    uint64_t journal_id;
    int64_t next_usn;
    uint64_t *deleted_frns;
    size_t n_deleted;
    CdcUsnChangedFile *files;
    size_t n_files;
    char **renamed_old; /* malloc'd，与 renamed_new 一一对应 */
    char **renamed_new;
    size_t n_renamed;
} CdcUsnChangeSet;

/* 0=成功；1=调用方应全量重建；<0=出错。 */
int cdc_usn_changes_since(const char *root, uint64_t stored_journal_id, int64_t stored_next_usn,
                          const char *const *skip_top, size_t n_skip, CdcUsnChangeSet *out);
void cdc_usn_change_set_free(CdcUsnChangeSet *s);

/* ---- 共享小工具（cdc_now_seconds 由本模块提供；cdc_cache_dir 的实现在 util.c） ---- */

/* malloc'd；%LOCALAPPDATA%\C盘清理工具\mftcache（Linux：$LOCALAPPDATA 或 "."）。
 * 目录不存在时创建。实现见 util.c。 */
char *cdc_cache_dir(void);
uint64_t cdc_now_seconds(void);

#endif
