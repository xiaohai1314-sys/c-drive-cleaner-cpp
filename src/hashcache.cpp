/* hashcache.c — SQLite 哈希缓存，逐行对照 desktop/src/hashcache.rs。
 *
 * 与 Rust 版一致的点：
 *   - 默认库 cache_dir()/hash-cache-v3.db；表结构与 PRAGMA 逐字一致；
 *   - load 只预加载给定路径（不全量读库）；entry_for 要求 (size, mtime)
 *     精确匹配；remember 的 size/mtime 变化清摘要规则；
 *   - save_dirty 只写脏条目，upsert 的 COALESCE 语义逐字一致，
 *     同事务删 last_seen < now-30 天的行；
 *   - 旧版 hash-cache-v2.json 迁移（仅表为空时；head 恒为 NULL）。
 *
 * 有意差别：
 *   - 内存条目是按 path 排序的数组 + 二分查找（Rust 是 HashMap），
 *     行为一致；dirty 记在条目上，不另建集合。
 *   - 打开失败也返回可用的空缓存（与 Rust load_for_paths_at 一致），
 *     save_dirty 时再重试打开。
 *   - 采样层摘要（XXH3-64）存在 head_xxh / sample_xxh / strong_xxh
 *     三列（INTEGER，按位存 uint64），不进 BLAKE3 的 BLOB 列：
 *     与 Rust 版共用同一个 hash-cache-v3.db，Rust 读自己那几列不
 *     受影响；旧库（Rust 或旧版 C 建的表）打开时 ALTER TABLE 补列。
 *     full 仍是 BLAKE3，值保持兼容。
 */
#ifndef _WIN32
#endif

#include "hashcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "util.h"

#ifdef CDC_HAVE_CJSON
#include "cJSON.h"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#ifdef _WIN32
#define CDC_PATH_SEP "\\"
#else
#define CDC_PATH_SEP "/"
#endif

typedef struct {
    char *path; /* owned */
    uint64_t size, mtime, last_seen;
    cdc_digest_set_t digests;
    int dirty;
} CdcHashEntry;

struct cdc_hashcache {
    char *db_path; /* NULL = 纯内存态 */
    cdc_vec_t entries; /* 元素 CdcHashEntry，按 path 排序 */
};

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---------------- 排序数组 ---------------- */

/* 二分查找：找到返回下标；没找到返回 -1，*insert_pos 为插入位置。 */
static long entry_find(const cdc_hashcache_t *c, const char *path, size_t *insert_pos) {
    size_t lo = 0, hi = c->entries.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(path, CDC_VEC_AT(&c->entries, CdcHashEntry, mid).path);
        if (cmp == 0) return (long)mid;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    if (insert_pos) *insert_pos = lo;
    return -1;
}

/* 中间插入：cdc_vec_t 没有 insert 接口，先 reserve 再 memmove 腾位置，
 * 增长策略仍在 util.c。 */
static int entry_insert_at(cdc_hashcache_t *c, size_t pos, CdcHashEntry *e) {
    if (cdc_vec_reserve(&c->entries, 1) != CDC_OK) return -1;
    CdcHashEntry *arr = (CdcHashEntry *)c->entries.data;
    if (pos < c->entries.len)
        memmove(&arr[pos + 1], &arr[pos],
                (c->entries.len - pos) * sizeof(CdcHashEntry));
    arr[pos] = *e;
    c->entries.len++;
    return 0;
}

/* 路径指针的 strcmp 比较（qsort 用，见 load_at）。 */
static int cmp_path_ptr(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---------------- hex ---------------- */

void cdc_digest_to_hex(const cdc_digest_t *d, char out[65]) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = HEX[(d->bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[d->bytes[i] & 0x0F];
    }
    out[64] = '\0';
}

int cdc_hex_to_digest(const char *hex, cdc_digest_t *out) {
    if (!hex || strlen(hex) != 64) return 0;
    for (int i = 0; i < 32; i++) {
        unsigned int hi, lo;
        char c0 = hex[i * 2], c1 = hex[i * 2 + 1];
        if (c0 >= '0' && c0 <= '9') hi = (unsigned int)(c0 - '0');
        else if (c0 >= 'a' && c0 <= 'f') hi = (unsigned int)(c0 - 'a' + 10);
        else if (c0 >= 'A' && c0 <= 'F') hi = (unsigned int)(c0 - 'A' + 10);
        else return 0;
        if (c1 >= '0' && c1 <= '9') lo = (unsigned int)(c1 - '0');
        else if (c1 >= 'a' && c1 <= 'f') lo = (unsigned int)(c1 - 'a' + 10);
        else if (c1 >= 'A' && c1 <= 'F') lo = (unsigned int)(c1 - 'A' + 10);
        else return 0;
        out->bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static int blob_to_digest(const void *blob, int nbytes, cdc_digest_t *out) {
    if (!blob || nbytes != 32) return 0;
    memcpy(out->bytes, blob, 32);
    return 1;
}

/* ---------------- 文件小工具 ---------------- */

static int read_file_all(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out = buf;
    if (out_len) *out_len = got;
    return 0;
}

/* 逐级建父目录（与 mft.c 的 cdc_cache_dir 同一做法）。 */
static void ensure_parent_dir(const char *path) {
    char buf[4096];
    size_t len = strlen(path);
    if (len >= sizeof buf) return;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
#ifdef _WIN32
            wchar_t *w = cdc_utf8_to_wide(buf);
            if (w) {
                CreateDirectoryW(w, NULL);
                free(w);
            }
#else
            mkdir(buf, 0755);
#endif
            *p = save;
        }
    }
}

/* ---------------- SQLite ---------------- */

static int open_conn(const char *db_path, sqlite3 **out_db) {
    ensure_parent_dir(db_path);
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    static const char *SCHEMA =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA temp_store = MEMORY;"
        "CREATE TABLE IF NOT EXISTS file_hashes ("
        "    path TEXT PRIMARY KEY,"
        "    size INTEGER NOT NULL,"
        "    mtime INTEGER NOT NULL,"
        "    head BLOB,"
        "    sample BLOB,"
        "    strong BLOB,"
        "    full BLOB,"
        "    head_xxh INTEGER,"
        "    sample_xxh INTEGER,"
        "    strong_xxh INTEGER,"
        "    last_seen INTEGER NOT NULL"
        ") WITHOUT ROWID;"
        "CREATE INDEX IF NOT EXISTS idx_hashes_last_seen ON file_hashes(last_seen);";
    char *emsg = NULL;
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &emsg) != SQLITE_OK) {
        sqlite3_free(emsg);
        sqlite3_close(db);
        return -1;
    }
    /* 旧库补列：表可能是 Rust 版或旧版 C 建的（只有 7 列），
     * CREATE TABLE IF NOT EXISTS 不会补新列，这里按需 ALTER。 */
    static const char *XXH_COLS[3] = {"head_xxh", "sample_xxh", "strong_xxh"};
    for (int i = 0; i < 3; i++) {
        sqlite3_stmt *ck = NULL;
        char sql[128];
        snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM pragma_table_info('file_hashes')"
                                  " WHERE name = '%s'",
                 XXH_COLS[i]);
        int has = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &ck, NULL) == SQLITE_OK) {
            if (sqlite3_step(ck) == SQLITE_ROW)
                has = sqlite3_column_int(ck, 0) > 0;
            sqlite3_finalize(ck);
        }
        if (!has) {
            snprintf(sql, sizeof sql, "ALTER TABLE file_hashes ADD COLUMN %s INTEGER",
                     XXH_COLS[i]);
            sqlite3_exec(db, sql, NULL, NULL, NULL); /* 失败忽略：下次打开再试 */
        }
    }
    *out_db = db;
    return 0;
}

/* 旧版 v2 JSON 迁移（仅表为空时）。无 cJSON 编译时直接跳过。 */
static void maybe_migrate_legacy(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    long count = 1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM file_hashes", -1, &st, NULL) ==
        SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) count = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (count > 0) return;
    char *dir = cdc_cache_dir();
    if (!dir) return;
    char path[4096];
    snprintf(path, sizeof path, "%s%s%s", dir, CDC_PATH_SEP, "hash-cache-v2.json");
    free(dir);
    char *text = NULL;
    if (read_file_all(path, &text, NULL) != 0) return;
#ifdef CDC_HAVE_CJSON
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return;
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (cJSON_IsObject(entries)) {
        sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        sqlite3_stmt *ins = NULL;
        if (sqlite3_prepare_v2(db,
                               "INSERT OR IGNORE INTO file_hashes"
                               " (path, size, mtime, head, sample, strong, full, last_seen)"
                               " VALUES (?1, ?2, ?3, NULL, ?4, ?5, ?6, ?7)",
                               -1, &ins, NULL) == SQLITE_OK) {
            for (cJSON *child = entries->child; child; child = child->next) {
                const char *p = child->string;
                if (!p) continue;
                cJSON *jsize = cJSON_GetObjectItemCaseSensitive(child, "size");
                cJSON *jmtime = cJSON_GetObjectItemCaseSensitive(child, "mtime");
                cJSON *jsample = cJSON_GetObjectItemCaseSensitive(child, "sample");
                cJSON *jstrong = cJSON_GetObjectItemCaseSensitive(child, "strong");
                cJSON *jfull = cJSON_GetObjectItemCaseSensitive(child, "full");
                cJSON *jlast = cJSON_GetObjectItemCaseSensitive(child, "last_seen");
                cdc_digest_t d_sample, d_strong, d_full;
                int has_sample = cJSON_IsString(jsample) && jsample->valuestring &&
                                 cdc_hex_to_digest(jsample->valuestring, &d_sample);
                int has_strong = cJSON_IsString(jstrong) && jstrong->valuestring &&
                                 cdc_hex_to_digest(jstrong->valuestring, &d_strong);
                int has_full = cJSON_IsString(jfull) && jfull->valuestring &&
                               cdc_hex_to_digest(jfull->valuestring, &d_full);
                if (!has_sample && !has_strong && !has_full) continue;
                sqlite3_bind_text(ins, 1, p, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(ins, 2,
                                   (sqlite3_int64)(cJSON_IsNumber(jsize)
                                                       ? (uint64_t)jsize->valuedouble
                                                       : 0));
                sqlite3_bind_int64(ins, 3,
                                   (sqlite3_int64)(cJSON_IsNumber(jmtime)
                                                       ? (uint64_t)jmtime->valuedouble
                                                       : 0));
                if (has_sample)
                    sqlite3_bind_blob(ins, 4, d_sample.bytes, 32, SQLITE_TRANSIENT);
                else
                    sqlite3_bind_null(ins, 4);
                if (has_strong)
                    sqlite3_bind_blob(ins, 5, d_strong.bytes, 32, SQLITE_TRANSIENT);
                else
                    sqlite3_bind_null(ins, 5);
                if (has_full)
                    sqlite3_bind_blob(ins, 6, d_full.bytes, 32, SQLITE_TRANSIENT);
                else
                    sqlite3_bind_null(ins, 6);
                sqlite3_bind_int64(ins, 7,
                                   (sqlite3_int64)(cJSON_IsNumber(jlast)
                                                       ? (uint64_t)jlast->valuedouble
                                                       : 0));
                sqlite3_step(ins);
                sqlite3_reset(ins);
                sqlite3_clear_bindings(ins);
            }
            sqlite3_finalize(ins);
        }
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    }
    cJSON_Delete(root);
#else
    free(text);
#endif
}

/* ---------------- 打开 / 加载 ---------------- */

static cdc_hashcache_t *load_at(const char *db_path, const char **paths, size_t n,
                                int migrate_legacy) {
    cdc_hashcache_t *c = (cdc_hashcache_t *)calloc(1, sizeof *c);
    if (!c) return NULL;
    if (cdc_vec_init(&c->entries, sizeof(CdcHashEntry)) != CDC_OK) {
        free(c);
        return NULL;
    }
    if (db_path) {
        c->db_path = dup_str(db_path);
        if (!c->db_path) {
            cdc_vec_free(&c->entries);
            free(c);
            return NULL;
        }
    }
    if (!db_path) return c; /* 纯内存态 */
    sqlite3 *db = NULL;
    if (open_conn(db_path, &db) != 0) return c; /* 打不开也返回空缓存 */
    if (migrate_legacy) maybe_migrate_legacy(db);
    /* 先把输入路径按 strcmp 排好序再逐条加载：entries 是按 path 排序的
     * 数组，未排序时每次命中都要 entry_insert_at 中间插入、平均移动
     * 半个数组，N 条命中是 O(n²)（实测：50k 命中约 5.9s，其中 SQL
     * 本身只占约 0.2s）；排好序后每次插入都在末尾追加，无 memmove。
     * 去重检查（entry_find）仍有效：重复路径排好序后相邻，第二次查到。
     * 排序只动指针数组的副本，不改调用方传入的顺序。
     * malloc 失败时退回原顺序：结果正确，只是慢。 */
    const char **sorted = NULL;
    if (n > 1 && paths) {
        sorted = (const char **)malloc(n * sizeof(const char *));
        if (sorted) {
            memcpy(sorted, paths, n * sizeof(const char *));
            qsort(sorted, n, sizeof(const char *), cmp_path_ptr);
            paths = sorted;
        }
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT size, mtime, head, sample, strong, full,"
                           " head_xxh, sample_xxh, strong_xxh, last_seen"
                           " FROM file_hashes WHERE path = ?1",
                           -1, &stmt, NULL) == SQLITE_OK) {
        for (size_t i = 0; i < n; i++) {
            const char *path = paths ? paths[i] : NULL;
            if (!path) continue;
            if (entry_find(c, path, NULL) >= 0) continue; /* 去重 */
            sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                CdcHashEntry e;
                memset(&e, 0, sizeof e);
                e.path = dup_str(path);
                if (!e.path) {
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                    continue;
                }
                sqlite3_int64 sz = sqlite3_column_int64(stmt, 0);
                sqlite3_int64 mt = sqlite3_column_int64(stmt, 1);
                sqlite3_int64 ls = sqlite3_column_int64(stmt, 9);
                e.size = sz > 0 ? (uint64_t)sz : 0;
                e.mtime = mt > 0 ? (uint64_t)mt : 0;
                e.last_seen = ls > 0 ? (uint64_t)ls : 0;
                e.digests.has_head =
                    blob_to_digest(sqlite3_column_blob(stmt, 2),
                                   sqlite3_column_bytes(stmt, 2), &e.digests.head);
                e.digests.has_sample =
                    blob_to_digest(sqlite3_column_blob(stmt, 3),
                                   sqlite3_column_bytes(stmt, 3), &e.digests.sample);
                e.digests.has_strong =
                    blob_to_digest(sqlite3_column_blob(stmt, 4),
                                   sqlite3_column_bytes(stmt, 4), &e.digests.strong);
                e.digests.has_full =
                    blob_to_digest(sqlite3_column_blob(stmt, 5),
                                   sqlite3_column_bytes(stmt, 5), &e.digests.full);
                if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
                    e.digests.has_head_xxh = 1;
                    e.digests.head_xxh = (uint64_t)sqlite3_column_int64(stmt, 6);
                }
                if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
                    e.digests.has_sample_xxh = 1;
                    e.digests.sample_xxh = (uint64_t)sqlite3_column_int64(stmt, 7);
                }
                if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
                    e.digests.has_strong_xxh = 1;
                    e.digests.strong_xxh = (uint64_t)sqlite3_column_int64(stmt, 8);
                }
                size_t pos = 0;
                entry_find(c, path, &pos);
                if (entry_insert_at(c, pos, &e) != 0) free(e.path);
            }
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
        sqlite3_finalize(stmt);
    }
    free(sorted);
    sqlite3_close(db);
    return c;
}

int cdc_hashcache_default_path(char *buf, size_t cap) {
    char *dir = cdc_cache_dir();
    if (!dir) return -1;
    int n = snprintf(buf, cap, "%s%s%s", dir, CDC_PATH_SEP, "hash-cache-v3.db");
    free(dir);
    return (n >= 0 && (size_t)n < cap) ? 0 : -1;
}

cdc_hashcache_t *cdc_hashcache_load_for_paths(const char **paths, size_t n) {
    char db[4096];
    if (cdc_hashcache_default_path(db, sizeof db) != 0)
        return load_at(NULL, paths, n, 0); /* 默认路径都取不到：纯内存态 */
    return load_at(db, paths, n, 1);
}

cdc_hashcache_t *cdc_hashcache_open_at(const char *db_path, int migrate_legacy) {
    return load_at(db_path, NULL, 0, migrate_legacy);
}

cdc_hashcache_t *cdc_hashcache_open(const char *db_path) {
    return load_at(db_path, NULL, 0, 1);
}

void cdc_hashcache_close(cdc_hashcache_t *cache) {
    if (!cache) return;
    cdc_hashcache_save_dirty(cache); /* 尽力写回，忽略错误 */
    for (size_t i = 0; i < cache->entries.len; i++)
        free(CDC_VEC_AT(&cache->entries, CdcHashEntry, i).path);
    cdc_vec_free(&cache->entries);
    free(cache->db_path);
    free(cache);
}

size_t cdc_hashcache_len(cdc_hashcache_t *cache) {
    return cache ? cache->entries.len : 0;
}

int cdc_hashcache_entry_for(cdc_hashcache_t *cache, const char *path, uint64_t size,
                            uint64_t mtime, cdc_digest_set_t *out) {
    if (!cache || !path || !out) return -1;
    long idx = entry_find(cache, path, NULL);
    if (idx < 0) return 0;
    const CdcHashEntry *e = &CDC_VEC_AT(&cache->entries, CdcHashEntry, (size_t)idx);
    if (e->size != size || e->mtime != mtime) return 0;
    *out = e->digests;
    return 1;
}

int cdc_hashcache_remember(cdc_hashcache_t *cache, const char *path, uint64_t size,
                           uint64_t mtime, const cdc_digest_set_t *digests,
                           uint64_t now) {
    if (!cache || !path || !digests) return -1;
    size_t pos = 0;
    long idx = entry_find(cache, path, &pos);
    int changed = 0;
    CdcHashEntry *e = NULL;
    if (idx < 0) {
        CdcHashEntry ne;
        memset(&ne, 0, sizeof ne);
        ne.path = dup_str(path);
        if (!ne.path) return -1;
        ne.size = size;
        ne.mtime = mtime;
        ne.last_seen = now;
        if (entry_insert_at(cache, pos, &ne) != 0) {
            free(ne.path);
            return -1;
        }
        e = &CDC_VEC_AT(&cache->entries, CdcHashEntry, pos);
        changed = 1;
    } else {
        e = &CDC_VEC_AT(&cache->entries, CdcHashEntry, (size_t)idx);
        if (e->size != size || e->mtime != mtime) {
            e->size = size;
            e->mtime = mtime;
            memset(&e->digests, 0, sizeof e->digests);
            changed = 1;
        }
    }
    if (digests->has_head &&
        (!e->digests.has_head ||
         memcmp(&e->digests.head, &digests->head, 32) != 0)) {
        e->digests.has_head = 1;
        e->digests.head = digests->head;
        changed = 1;
    }
    if (digests->has_sample &&
        (!e->digests.has_sample ||
         memcmp(&e->digests.sample, &digests->sample, 32) != 0)) {
        e->digests.has_sample = 1;
        e->digests.sample = digests->sample;
        changed = 1;
    }
    if (digests->has_strong &&
        (!e->digests.has_strong ||
         memcmp(&e->digests.strong, &digests->strong, 32) != 0)) {
        e->digests.has_strong = 1;
        e->digests.strong = digests->strong;
        changed = 1;
    }
    if (digests->has_full &&
        (!e->digests.has_full ||
         memcmp(&e->digests.full, &digests->full, 32) != 0)) {
        e->digests.has_full = 1;
        e->digests.full = digests->full;
        changed = 1;
    }
    if (digests->has_head_xxh &&
        (!e->digests.has_head_xxh || e->digests.head_xxh != digests->head_xxh)) {
        e->digests.has_head_xxh = 1;
        e->digests.head_xxh = digests->head_xxh;
        changed = 1;
    }
    if (digests->has_sample_xxh &&
        (!e->digests.has_sample_xxh || e->digests.sample_xxh != digests->sample_xxh)) {
        e->digests.has_sample_xxh = 1;
        e->digests.sample_xxh = digests->sample_xxh;
        changed = 1;
    }
    if (digests->has_strong_xxh &&
        (!e->digests.has_strong_xxh || e->digests.strong_xxh != digests->strong_xxh)) {
        e->digests.has_strong_xxh = 1;
        e->digests.strong_xxh = digests->strong_xxh;
        changed = 1;
    }
    if (changed) {
        e->last_seen = now;
        e->dirty = 1;
    }
    return 0;
}

int cdc_hashcache_save_dirty(cdc_hashcache_t *cache) {
    if (!cache) return -1;
    int any = 0;
    for (size_t i = 0; i < cache->entries.len; i++) {
        if (CDC_VEC_AT(&cache->entries, CdcHashEntry, i).dirty) {
            any = 1;
            break;
        }
    }
    if (!any) return 0;
    if (!cache->db_path) {
        for (size_t i = 0; i < cache->entries.len; i++)
            CDC_VEC_AT(&cache->entries, CdcHashEntry, i).dirty = 0;
        return 0;
    }
    sqlite3 *db = NULL;
    if (open_conn(cache->db_path, &db) != 0) return -1;
    uint64_t now = cdc::now_seconds();
    uint64_t cutoff = now >= (uint64_t)30 * 86400 ? now - (uint64_t)30 * 86400 : 0;
    int rc = -1;
    char *emsg = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &emsg) != SQLITE_OK) {
        sqlite3_free(emsg);
        sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    static const char *UPSERT =
        "INSERT INTO file_hashes"
        " (path, size, mtime, head, sample, strong, full,"
        "  head_xxh, sample_xxh, strong_xxh, last_seen)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)"
        " ON CONFLICT(path) DO UPDATE SET"
        "     size = excluded.size,"
        "     mtime = excluded.mtime,"
        "     head = COALESCE(excluded.head, file_hashes.head),"
        "     sample = COALESCE(excluded.sample, file_hashes.sample),"
        "     strong = COALESCE(excluded.strong, file_hashes.strong),"
        "     full = COALESCE(excluded.full, file_hashes.full),"
        "     head_xxh = COALESCE(excluded.head_xxh, file_hashes.head_xxh),"
        "     sample_xxh = COALESCE(excluded.sample_xxh, file_hashes.sample_xxh),"
        "     strong_xxh = COALESCE(excluded.strong_xxh, file_hashes.strong_xxh),"
        "     last_seen = excluded.last_seen";
    if (sqlite3_prepare_v2(db, UPSERT, -1, &stmt, NULL) == SQLITE_OK) {
        int ok = 1;
        for (size_t i = 0; i < cache->entries.len; i++) {
            const CdcHashEntry *e = &CDC_VEC_AT(&cache->entries, CdcHashEntry, i);
            if (!e->dirty) continue;
            sqlite3_bind_text(stmt, 1, e->path, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, (sqlite3_int64)e->size);
            sqlite3_bind_int64(stmt, 3, (sqlite3_int64)e->mtime);
            if (e->digests.has_head)
                sqlite3_bind_blob(stmt, 4, e->digests.head.bytes, 32, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(stmt, 4);
            if (e->digests.has_sample)
                sqlite3_bind_blob(stmt, 5, e->digests.sample.bytes, 32, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(stmt, 5);
            if (e->digests.has_strong)
                sqlite3_bind_blob(stmt, 6, e->digests.strong.bytes, 32, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(stmt, 6);
            if (e->digests.has_full)
                sqlite3_bind_blob(stmt, 7, e->digests.full.bytes, 32, SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(stmt, 7);
            if (e->digests.has_head_xxh)
                sqlite3_bind_int64(stmt, 8, (sqlite3_int64)e->digests.head_xxh);
            else
                sqlite3_bind_null(stmt, 8);
            if (e->digests.has_sample_xxh)
                sqlite3_bind_int64(stmt, 9, (sqlite3_int64)e->digests.sample_xxh);
            else
                sqlite3_bind_null(stmt, 9);
            if (e->digests.has_strong_xxh)
                sqlite3_bind_int64(stmt, 10, (sqlite3_int64)e->digests.strong_xxh);
            else
                sqlite3_bind_null(stmt, 10);
            sqlite3_bind_int64(stmt, 11, (sqlite3_int64)e->last_seen);
            if (sqlite3_step(stmt) != SQLITE_DONE) ok = 0;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        }
        sqlite3_finalize(stmt);
        if (ok) {
            sqlite3_stmt *del = NULL;
            if (sqlite3_prepare_v2(db, "DELETE FROM file_hashes WHERE last_seen < ?1",
                                   -1, &del, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, (sqlite3_int64)cutoff);
                sqlite3_step(del);
                sqlite3_finalize(del);
            }
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, &emsg) == SQLITE_OK) rc = 0;
            else sqlite3_free(emsg);
        }
    }
    if (rc != 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    } else {
        for (size_t i = 0; i < cache->entries.len; i++)
            CDC_VEC_AT(&cache->entries, CdcHashEntry, i).dirty = 0;
    }
    sqlite3_close(db);
    return rc;
}
