/* fileindex.c — 常驻索引 SQLite（对应 Rust 版 desktop/src/fileindex.rs）
 *
 * SQL 语句与 Rust 版逐字一致（upsert 的 ON CONFLICT、set_digests_batch 的
 * COALESCE + (size, mtime) 校验、update_path_prefix 的 LENGTH() 前缀改写、
 * rows_needing_full / query_dup_rows / query_big_rows 的子查询）。
 * 与 Rust 版的两处有意差别（见头文件注释）：
 *   - set_digests_batch 的 last_seen 由调用方传 now_secs（Rust 在函数内取）；
 *   - 错误不静默吞掉：open 失败填 cdc_err_t，其余函数按返回值表达
 *     （计数类返回 0 即失败/空，与 Rust 的 unwrap_or(0) 同义）。
 */
#ifndef _WIN32
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sqlite3.h"

#include "fileindex.h"
#include "mft.h" /* cdc_cache_dir、cdc_usn_state_usable */

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

struct cdc_fileindex {
    sqlite3 *db;
};

static const char *SCHEMA_SQL =
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous = NORMAL;"
    "PRAGMA temp_store = MEMORY;"
    "CREATE TABLE IF NOT EXISTS index_meta ("
    "    key TEXT PRIMARY KEY,"
    "    value TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS files ("
    "    volume_serial INTEGER NOT NULL,"
    "    frn INTEGER NOT NULL,"
    "    path TEXT NOT NULL,"
    "    size INTEGER NOT NULL,"
    "    mtime INTEGER NOT NULL,"
    "    head BLOB,"
    "    sample BLOB,"
    "    strong BLOB,"
    "    full BLOB,"
    "    head_xxh INTEGER,"
    "    sample_xxh INTEGER,"
    "    strong_xxh INTEGER,"
    "    last_seen INTEGER NOT NULL,"
    "    PRIMARY KEY (volume_serial, frn)"
    ") WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);"
    "CREATE INDEX IF NOT EXISTS idx_files_size_full ON files(size, full);"
    "CREATE TABLE IF NOT EXISTS usn_state ("
    "    volume_serial INTEGER PRIMARY KEY,"
    "    journal_id INTEGER NOT NULL,"
    "    next_usn INTEGER NOT NULL,"
    "    updated_at INTEGER NOT NULL"
    ") WITHOUT ROWID;";

/* ---------- 小工具 ---------- */

/* u64 -> sqlite3_int64（与 Rust 的 `as i64` 同样 wrap）；读回时 clamp 到 0。 */
static sqlite3_int64 to_i64(uint64_t v) { return (sqlite3_int64)v; }
static uint64_t from_i64(sqlite3_int64 v) { return v < 0 ? 0 : (uint64_t)v; }

/* 递归建目录（对应 Rust create_dir_all）。path 为 UTF-8。 */
static int mkdirs(const char *path) {
    if (!path || !*path) return -1;
#ifdef _WIN32
    /* 逐级建：用户名里可能有中文，走宽字符 API（util.h 的转换函数）。 */
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return -1;
    for (int i = 1; w[i] != L'\0'; i++) {
        if (w[i] == L'\\' || w[i] == L'/') {
            wchar_t save = w[i];
            w[i] = L'\0';
            CreateDirectoryW(w, NULL);
            w[i] = save;
        }
    }
    BOOL ok = CreateDirectoryW(w, NULL);
    DWORD e = GetLastError();
    free(w);
    return (ok || e == ERROR_ALREADY_EXISTS) ? 0 : -1;
#else
    char *tmp = strdup(path);
    if (!tmp) return -1;
    int rc = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0) {
                /* 已存在不算错；其它错误继续尝试（末级再判）。 */
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0) {
        struct stat st;
        rc = (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
    }
    free(tmp);
    return rc;
#endif
}

/* 建父目录：去掉最后一个分隔符后的部分。 */
static void ensure_parent_dir(const char *db_path) {
    char *tmp = strdup(db_path);
    if (!tmp) return;
    char *slash = NULL;
    for (char *p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (slash) {
        *slash = '\0';
        mkdirs(tmp);
    }
    free(tmp);
}

cdc_fileindex_t *cdc_fileindex_open(const char *db_path, cdc_err_t *err) {
    if (!db_path || !*db_path) {
        cdc_err_set(err, CDC_ERR_INVALID_ARG, "索引库路径为空");
        return NULL;
    }
    ensure_parent_dir(db_path);
    cdc_fileindex_t *idx = (cdc_fileindex_t *)calloc(1, sizeof(*idx));
    if (!idx) {
        cdc_err_set(err, CDC_ERR_NOMEM, "分配索引句柄失败");
        return NULL;
    }
    int rc = sqlite3_open(db_path, &idx->db);
    if (rc != SQLITE_OK) {
        cdc_err_set(err, CDC_ERR_IO,
                      idx->db ? sqlite3_errmsg(idx->db) : "打开索引库失败");
        if (idx->db) sqlite3_close(idx->db);
        free(idx);
        return NULL;
    }
    char *emsg = NULL;
    rc = sqlite3_exec(idx->db, SCHEMA_SQL, NULL, NULL, &emsg);
    if (rc != SQLITE_OK) {
        cdc_err_set(err, CDC_ERR_IO, emsg ? emsg : "建索引表失败");
        if (emsg) sqlite3_free(emsg);
        sqlite3_close(idx->db);
        free(idx);
        return NULL;
    }
    /* 旧库补列：files 表可能是 Rust 版或旧版 C 建的（无 xxh 三列），
     * CREATE TABLE IF NOT EXISTS 不会补新列，这里按需 ALTER。 */
    static const char *XXH_COLS[3] = {"head_xxh", "sample_xxh", "strong_xxh"};
    for (int i = 0; i < 3; i++) {
        sqlite3_stmt *ck = NULL;
        char sql[128];
        snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM pragma_table_info('files')"
                                  " WHERE name = '%s'",
                 XXH_COLS[i]);
        int has = 0;
        if (sqlite3_prepare_v2(idx->db, sql, -1, &ck, NULL) == SQLITE_OK) {
            if (sqlite3_step(ck) == SQLITE_ROW)
                has = sqlite3_column_int(ck, 0) > 0;
            sqlite3_finalize(ck);
        }
        if (!has) {
            snprintf(sql, sizeof sql, "ALTER TABLE files ADD COLUMN %s INTEGER",
                     XXH_COLS[i]);
            sqlite3_exec(idx->db, sql, NULL, NULL, NULL); /* 失败忽略：下次打开再试 */
        }
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db,
                           "INSERT OR IGNORE INTO index_meta (key, value) "
                           "VALUES ('schema_version', ?1)",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, CDC_INDEX_SCHEMA_VERSION, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (err) cdc_err_init(err);
    return idx;
}

cdc_fileindex_t *cdc_fileindex_open_default(cdc_err_t *err) {
    /* 默认路径：cdc_cache_dir()（%LOCALAPPDATA%\C盘清理工具\mftcache，
     * 缺失时回落 "."，与 Rust cache_dir() 一致）。 */
    char *dir = cdc_cache_dir();
    if (!dir) {
        cdc_err_set(err, CDC_ERR_IO, "取索引缓存目录失败");
        return NULL;
    }
    size_t need = strlen(dir) + strlen("/file-index-v1.db") + 1;
    char *path = (char *)malloc(need);
    if (!path) {
        free(dir);
        cdc_err_set(err, CDC_ERR_NOMEM, "分配索引库路径失败");
        return NULL;
    }
    snprintf(path, need, "%s/file-index-v1.db", dir);
    free(dir);
    cdc_fileindex_t *idx = cdc_fileindex_open(path, err);
    free(path);
    return idx;
}

void cdc_fileindex_close(cdc_fileindex_t *idx) {
    if (!idx) return;
    if (idx->db) sqlite3_close(idx->db);
    free(idx);
}

/* ---------- 写操作 ---------- */

size_t cdc_fileindex_upsert_files(cdc_fileindex_t *idx,
                                  const cdc_index_row_t *rows, size_t n) {
    if (!idx || n == 0) return 0;
    static const char *SQL =
        "INSERT INTO files"
        " (volume_serial, frn, path, size, mtime, head, sample, strong, full, head_xxh, sample_xxh, strong_xxh, last_seen)"
        " VALUES (?1, ?2, ?3, ?4, ?5, NULL, NULL, NULL, NULL, NULL, NULL, NULL, ?6)"
        " ON CONFLICT(volume_serial, frn) DO UPDATE SET"
        "     path = excluded.path,"
        "     size = excluded.size,"
        "     mtime = excluded.mtime,"
        "     head = CASE WHEN files.size <> excluded.size OR files.mtime <> excluded.mtime THEN NULL ELSE files.head END,"
        "     sample = CASE WHEN files.size <> excluded.size OR files.mtime <> excluded.mtime THEN NULL ELSE files.sample END,"
        "     strong = CASE WHEN files.size <> excluded.size OR files.mtime <> excluded.mtime THEN NULL ELSE files.strong END,"
        "     full = CASE WHEN files.size <> excluded.size OR files.mtime <> excluded.mtime THEN NULL ELSE files.full END,"
        "     head_xxh = CASE WHEN files.size <> excluded.size OR files.mtime <> excluded.mtime THEN NULL ELSE files.head_xxh END,"
        "     sample_xxh = CASE WHEN files.size <> excluded.size OR files.mtime <> excluded.mtime THEN NULL ELSE files.sample_xxh END,"
        "     strong_xxh = CASE WHEN files.size <> excluded.size OR files.mtime <> excluded.mtime THEN NULL ELSE files.strong_xxh END,"
        "     last_seen = excluded.last_seen";
    char *emsg = NULL;
    if (sqlite3_exec(idx->db, "BEGIN IMMEDIATE", NULL, NULL, &emsg) != SQLITE_OK) {
        if (emsg) sqlite3_free(emsg);
        return 0;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, SQL, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_exec(idx->db, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, to_i64(rows[i].volume_serial));
        sqlite3_bind_int64(st, 2, to_i64(rows[i].frn));
        sqlite3_bind_text(st, 3, rows[i].path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, to_i64(rows[i].size));
        sqlite3_bind_int64(st, 5, to_i64(rows[i].mtime));
        sqlite3_bind_int64(st, 6, to_i64(rows[i].last_seen));
        if (sqlite3_step(st) == SQLITE_DONE) written++;
    }
    sqlite3_finalize(st);
    if (sqlite3_exec(idx->db, "COMMIT", NULL, NULL, &emsg) != SQLITE_OK) {
        if (emsg) sqlite3_free(emsg);
        return 0;
    }
    return written;
}

static void bind_digest_or_null(sqlite3_stmt *st, int col, int has,
                                const cdc_digest_t *d) {
    if (has) {
        sqlite3_bind_blob(st, col, d->bytes, CDC_DIGEST_LEN, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, col);
    }
}

/* XXH3-64 按位存 INTEGER；没有时绑 NULL（不覆盖已有值）。 */
static void bind_xxh_or_null(sqlite3_stmt *st, int col, int has, uint64_t v) {
    if (has) {
        sqlite3_bind_int64(st, col, (sqlite3_int64)v);
    } else {
        sqlite3_bind_null(st, col);
    }
}

size_t cdc_fileindex_set_digests_batch(cdc_fileindex_t *idx,
                                       const cdc_digest_update_t *updates, size_t n,
                                       uint64_t now_secs) {
    if (!idx || n == 0) return 0;
    static const char *SQL =
        "UPDATE files SET"
        "     head = COALESCE(?5, head),"
        "     sample = COALESCE(?6, sample),"
        "     strong = COALESCE(?7, strong),"
        "     full = COALESCE(?8, full),"
        "     head_xxh = COALESCE(?9, head_xxh),"
        "     sample_xxh = COALESCE(?10, sample_xxh),"
        "     strong_xxh = COALESCE(?11, strong_xxh),"
        "     last_seen = ?12"
        " WHERE volume_serial = ?1 AND frn = ?2 AND size = ?3 AND mtime = ?4";
    char *emsg = NULL;
    if (sqlite3_exec(idx->db, "BEGIN IMMEDIATE", NULL, NULL, &emsg) != SQLITE_OK) {
        if (emsg) sqlite3_free(emsg);
        return 0;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, SQL, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_exec(idx->db, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }
    size_t updated = 0;
    for (size_t i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, to_i64(updates[i].volume_serial));
        sqlite3_bind_int64(st, 2, to_i64(updates[i].frn));
        sqlite3_bind_int64(st, 3, to_i64(updates[i].size));
        sqlite3_bind_int64(st, 4, to_i64(updates[i].mtime));
        bind_digest_or_null(st, 5, updates[i].digests.has_head, &updates[i].digests.head);
        bind_digest_or_null(st, 6, updates[i].digests.has_sample, &updates[i].digests.sample);
        bind_digest_or_null(st, 7, updates[i].digests.has_strong, &updates[i].digests.strong);
        bind_digest_or_null(st, 8, updates[i].digests.has_full, &updates[i].digests.full);
        bind_xxh_or_null(st, 9, updates[i].digests.has_head_xxh,
                         updates[i].digests.head_xxh);
        bind_xxh_or_null(st, 10, updates[i].digests.has_sample_xxh,
                         updates[i].digests.sample_xxh);
        bind_xxh_or_null(st, 11, updates[i].digests.has_strong_xxh,
                         updates[i].digests.strong_xxh);
        sqlite3_bind_int64(st, 12, to_i64(now_secs));
        if (sqlite3_step(st) == SQLITE_DONE) updated += (size_t)sqlite3_changes(idx->db);
    }
    sqlite3_finalize(st);
    if (sqlite3_exec(idx->db, "COMMIT", NULL, NULL, &emsg) != SQLITE_OK) {
        if (emsg) sqlite3_free(emsg);
        return 0;
    }
    return updated;
}

int cdc_fileindex_set_usn_state(cdc_fileindex_t *idx, uint64_t volume_serial,
                                uint64_t journal_id, int64_t next_usn) {
    if (!idx) return -1;
    static const char *SQL =
        "INSERT INTO usn_state (volume_serial, journal_id, next_usn, updated_at)"
        " VALUES (?1, ?2, ?3, ?4)"
        " ON CONFLICT(volume_serial) DO UPDATE SET"
        "     journal_id = excluded.journal_id,"
        "     next_usn = excluded.next_usn,"
        "     updated_at = excluded.updated_at";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, SQL, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, to_i64(volume_serial));
    sqlite3_bind_int64(st, 2, to_i64(journal_id));
    sqlite3_bind_int64(st, 3, next_usn);
    sqlite3_bind_int64(st, 4, to_i64((uint64_t)time(NULL)));
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int cdc_fileindex_get_usn_state(cdc_fileindex_t *idx, uint64_t volume_serial,
                                uint64_t *journal_id, int64_t *next_usn) {
    if (!idx) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db,
                           "SELECT journal_id, next_usn FROM usn_state "
                           "WHERE volume_serial = ?1",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, to_i64(volume_serial));
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return 0;
    }
    if (journal_id) *journal_id = from_i64(sqlite3_column_int64(st, 0));
    if (next_usn) *next_usn = sqlite3_column_int64(st, 1);
    sqlite3_finalize(st);
    return 1;
}

size_t cdc_fileindex_delete_rows(cdc_fileindex_t *idx, uint64_t volume_serial,
                                 const uint64_t *frns, size_t n) {
    if (!idx || n == 0) return 0;
    char *emsg = NULL;
    if (sqlite3_exec(idx->db, "BEGIN IMMEDIATE", NULL, NULL, &emsg) != SQLITE_OK) {
        if (emsg) sqlite3_free(emsg);
        return 0;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db,
                           "DELETE FROM files WHERE volume_serial = ?1 AND frn = ?2",
                           -1, &st, NULL) != SQLITE_OK) {
        sqlite3_exec(idx->db, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }
    size_t deleted = 0;
    for (size_t i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, to_i64(volume_serial));
        sqlite3_bind_int64(st, 2, to_i64(frns[i]));
        if (sqlite3_step(st) == SQLITE_DONE) deleted += (size_t)sqlite3_changes(idx->db);
    }
    sqlite3_finalize(st);
    if (sqlite3_exec(idx->db, "COMMIT", NULL, NULL, &emsg) != SQLITE_OK) {
        if (emsg) sqlite3_free(emsg);
        return 0;
    }
    return deleted;
}

size_t cdc_fileindex_update_path_prefix(cdc_fileindex_t *idx, uint64_t volume_serial,
                                        const char *old_prefix, const char *new_prefix) {
    if (!idx || !old_prefix || !new_prefix) return 0;
    /* 去掉尾部 '\' 再补一个（与 Rust trim_end_matches('\\') + format!("{}\\", …) 一致）。 */
    char *oldp = strdup(old_prefix);
    char *newp = strdup(new_prefix);
    if (!oldp || !newp) {
        free(oldp);
        free(newp);
        return 0;
    }
    size_t olen = strlen(oldp);
    while (olen > 0 && oldp[olen - 1] == '\\') oldp[--olen] = '\0';
    size_t nlen = strlen(newp);
    while (nlen > 0 && newp[nlen - 1] == '\\') newp[--nlen] = '\0';
    char *old_full = (char *)malloc(olen + 2);
    char *new_full = (char *)malloc(nlen + 2);
    if (!old_full || !new_full) {
        free(oldp);
        free(newp);
        free(old_full);
        free(new_full);
        return 0;
    }
    memcpy(old_full, oldp, olen);
    old_full[olen] = '\\';
    old_full[olen + 1] = '\0';
    memcpy(new_full, newp, nlen);
    new_full[nlen] = '\\';
    new_full[nlen + 1] = '\0';
    free(oldp);
    free(newp);

    static const char *SQL =
        "UPDATE files"
        " SET path = ?2 || SUBSTR(path, LENGTH(?3) + 1)"
        " WHERE volume_serial = ?1 AND SUBSTR(path, 1, LENGTH(?3)) = ?3";
    sqlite3_stmt *st = NULL;
    size_t changed = 0;
    if (sqlite3_prepare_v2(idx->db, SQL, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, to_i64(volume_serial));
        sqlite3_bind_text(st, 2, new_full, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, old_full, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_DONE) changed = (size_t)sqlite3_changes(idx->db);
        sqlite3_finalize(st);
    }
    free(old_full);
    free(new_full);
    return changed;
}

void cdc_fileindex_delete_volume(cdc_fileindex_t *idx, uint64_t volume_serial) {
    if (!idx) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, "DELETE FROM files WHERE volume_serial = ?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, to_i64(volume_serial));
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    st = NULL;
    if (sqlite3_prepare_v2(idx->db, "DELETE FROM usn_state WHERE volume_serial = ?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, to_i64(volume_serial));
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

/* ---------- 读操作 ---------- */

/* 从一列读摘要：BLOB 长度必须正好 32 字节，否则视为没有（与 Rust
 * digest_col 的 try_into().ok() 一致）。 */
static void read_digest(sqlite3_stmt *st, int col, int *has, cdc_digest_t *d) {
    *has = 0;
    if (sqlite3_column_type(st, col) != SQLITE_BLOB) return;
    const void *blob = sqlite3_column_blob(st, col);
    int n = sqlite3_column_bytes(st, col);
    if (blob && n == CDC_DIGEST_LEN) {
        memcpy(d->bytes, blob, CDC_DIGEST_LEN);
        *has = 1;
    }
}

/* 从一列读 XXH3-64：INTEGER 列按位还原 uint64；NULL 视为没有。 */
static void read_xxh(sqlite3_stmt *st, int col, int *has, uint64_t *v) {
    *has = 0;
    if (sqlite3_column_type(st, col) != SQLITE_INTEGER) return;
    *v = (uint64_t)sqlite3_column_int64(st, col);
    *has = 1;
}

static char *dup_text(sqlite3_stmt *st, int col) {
    const unsigned char *t = sqlite3_column_text(st, col);
    return strdup(t ? (const char *)t : "");
}

static uint64_t col_u64(sqlite3_stmt *st, int col) {
    return from_i64(sqlite3_column_int64(st, col));
}

cdc_index_hash_row_t *cdc_fileindex_rows_needing_full(cdc_fileindex_t *idx,
                                                      uint64_t volume_serial,
                                                      size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!idx) return NULL;
    static const char *SQL =
        "SELECT frn, path, size, mtime, head, sample, strong, full,"
        " head_xxh, sample_xxh, strong_xxh"
        " FROM files"
        " WHERE volume_serial = ?1 AND size IN ("
        "     SELECT size FROM files WHERE volume_serial = ?1"
        "     GROUP BY size HAVING COUNT(*) >= 2 AND COUNT(full) < COUNT(*)"
        " )"
        " ORDER BY size, frn";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, SQL, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(st, 1, to_i64(volume_serial));
    cdc_vec_t v;
    if (cdc_vec_init(&v, sizeof(cdc_index_hash_row_t)) != CDC_OK) {
        sqlite3_finalize(st);
        return NULL;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        cdc_index_hash_row_t r;
        memset(&r, 0, sizeof(r));
        r.frn = col_u64(st, 0);
        r.path = dup_text(st, 1);
        r.size = col_u64(st, 2);
        r.mtime = col_u64(st, 3);
        read_digest(st, 4, &r.digests.has_head, &r.digests.head);
        read_digest(st, 5, &r.digests.has_sample, &r.digests.sample);
        read_digest(st, 6, &r.digests.has_strong, &r.digests.strong);
        read_digest(st, 7, &r.digests.has_full, &r.digests.full);
        read_xxh(st, 8, &r.digests.has_head_xxh, &r.digests.head_xxh);
        read_xxh(st, 9, &r.digests.has_sample_xxh, &r.digests.sample_xxh);
        read_xxh(st, 10, &r.digests.has_strong_xxh, &r.digests.strong_xxh);
        if (!r.path || cdc_vec_push(&v, &r) != CDC_OK) {
            free(r.path);
            break;
        }
    }
    sqlite3_finalize(st);
    if (out_n) *out_n = v.len;
    cdc_index_hash_row_t *arr = (cdc_index_hash_row_t *)v.data;
    v.data = NULL; /* 交出所有权：调用方用 cdc_index_hash_rows_free 释放 */
    cdc_vec_free(&v);
    return arr;
}

void cdc_index_hash_rows_free(cdc_index_hash_row_t *rows, size_t n) {
    if (!rows) return;
    for (size_t i = 0; i < n; i++) free(rows[i].path);
    free(rows);
}

cdc_index_digest_entry_t *cdc_fileindex_all_digests(cdc_fileindex_t *idx,
                                                    uint64_t volume_serial,
                                                    size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!idx) return NULL;
    static const char *SQL =
        "SELECT frn, head, sample, strong, full"
        " FROM files"
        " WHERE volume_serial = ?1"
        "   AND (head IS NOT NULL OR sample IS NOT NULL"
        "        OR strong IS NOT NULL OR full IS NOT NULL)"
        " ORDER BY frn";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, SQL, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(st, 1, to_i64(volume_serial));
    cdc_vec_t v;
    if (cdc_vec_init(&v, sizeof(cdc_index_digest_entry_t)) != CDC_OK) {
        sqlite3_finalize(st);
        return NULL;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        cdc_index_digest_entry_t e;
        memset(&e, 0, sizeof(e));
        e.frn = col_u64(st, 0);
        read_digest(st, 1, &e.digests.has_head, &e.digests.head);
        read_digest(st, 2, &e.digests.has_sample, &e.digests.sample);
        read_digest(st, 3, &e.digests.has_strong, &e.digests.strong);
        read_digest(st, 4, &e.digests.has_full, &e.digests.full);
        if (cdc_vec_push(&v, &e) != CDC_OK) break;
    }
    sqlite3_finalize(st);
    if (out_n) *out_n = v.len;
    cdc_index_digest_entry_t *arr = (cdc_index_digest_entry_t *)v.data;
    v.data = NULL; /* 交出所有权：无 owned 指针，调用方直接 free() */
    cdc_vec_free(&v);
    return arr;
}

static int count_query(cdc_fileindex_t *idx, const char *sql, uint64_t serial,
                       int use_serial, uint64_t *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (use_serial) sqlite3_bind_int64(st, 1, to_i64(serial));
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) *out = col_u64(st, 0);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

void cdc_fileindex_counts(cdc_fileindex_t *idx, uint64_t *total, uint64_t *with_full) {
    uint64_t t = 0, f = 0;
    if (idx) {
        count_query(idx, "SELECT COUNT(*) FROM files", 0, 0, &t);
        count_query(idx, "SELECT COUNT(*) FROM files WHERE full IS NOT NULL", 0, 0, &f);
    }
    if (total) *total = t;
    if (with_full) *with_full = f;
}

void cdc_fileindex_volume_counts(cdc_fileindex_t *idx, uint64_t volume_serial,
                                 uint64_t *total, uint64_t *with_full) {
    uint64_t t = 0, f = 0;
    if (idx) {
        count_query(idx, "SELECT COUNT(*) FROM files WHERE volume_serial = ?1",
                    volume_serial, 1, &t);
        count_query(idx,
                    "SELECT COUNT(*) FROM files WHERE volume_serial = ?1 AND full IS NOT NULL",
                    volume_serial, 1, &f);
    }
    if (total) *total = t;
    if (with_full) *with_full = f;
}

int cdc_fileindex_usn_state_updated_at(cdc_fileindex_t *idx, uint64_t volume_serial,
                                       uint64_t *updated_at) {
    if (!idx) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db,
                           "SELECT updated_at FROM usn_state WHERE volume_serial = ?1",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, to_i64(volume_serial));
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return 0;
    }
    if (updated_at) *updated_at = col_u64(st, 0);
    sqlite3_finalize(st);
    return 1;
}

/* 拼 "?,?,?" 占位串：n 个时返回 malloc 的字符串，调用方 free。 */
static char *placeholders(size_t n) {
    if (n == 0) return strdup("");
    char *s = (char *)malloc(n * 2); /* "?" + ","，末尾省一个逗号再加 NUL */
    if (!s) return NULL;
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        s[pos++] = '?';
        if (i + 1 < n) s[pos++] = ',';
    }
    s[pos] = '\0';
    return s;
}

cdc_index_dup_row_t *cdc_fileindex_query_dup_rows(cdc_fileindex_t *idx,
                                                  const uint64_t *serials, size_t n_serials,
                                                  uint64_t min_size, size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!idx || n_serials == 0) return NULL;
    char *ph = placeholders(n_serials);
    if (!ph) return NULL;
    /* SQL 模板与 Rust 版一致（IN 列表出现两次：外层与子查询）。 */
    const char *tmpl =
        "SELECT volume_serial, frn, path, size, mtime, head, sample, strong, full,"
        " head_xxh, sample_xxh, strong_xxh"
        " FROM files"
        " WHERE volume_serial IN (%s) AND size >= ? AND size IN ("
        "     SELECT size FROM files WHERE volume_serial IN (%s)"
        "     GROUP BY size HAVING COUNT(*) >= 2"
        " )"
        " ORDER BY size DESC";
    int need = snprintf(NULL, 0, tmpl, ph, ph);
    char *sql = (char *)malloc((size_t)need + 1);
    if (!sql) {
        free(ph);
        return NULL;
    }
    snprintf(sql, (size_t)need + 1, tmpl, ph, ph);
    free(ph);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, sql, -1, &st, NULL) != SQLITE_OK) {
        free(sql);
        return NULL;
    }
    free(sql);
    int col = 1;
    for (size_t i = 0; i < n_serials; i++)
        sqlite3_bind_int64(st, col++, to_i64(serials[i]));
    sqlite3_bind_int64(st, col++, to_i64(min_size));
    for (size_t i = 0; i < n_serials; i++)
        sqlite3_bind_int64(st, col++, to_i64(serials[i]));
    cdc_vec_t v;
    if (cdc_vec_init(&v, sizeof(cdc_index_dup_row_t)) != CDC_OK) {
        sqlite3_finalize(st);
        return NULL;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        cdc_index_dup_row_t r;
        memset(&r, 0, sizeof(r));
        r.volume_serial = col_u64(st, 0);
        r.frn = col_u64(st, 1);
        r.path = dup_text(st, 2);
        r.size = col_u64(st, 3);
        r.mtime = col_u64(st, 4);
        read_digest(st, 5, &r.digests.has_head, &r.digests.head);
        read_digest(st, 6, &r.digests.has_sample, &r.digests.sample);
        read_digest(st, 7, &r.digests.has_strong, &r.digests.strong);
        read_digest(st, 8, &r.digests.has_full, &r.digests.full);
        read_xxh(st, 9, &r.digests.has_head_xxh, &r.digests.head_xxh);
        read_xxh(st, 10, &r.digests.has_sample_xxh, &r.digests.sample_xxh);
        read_xxh(st, 11, &r.digests.has_strong_xxh, &r.digests.strong_xxh);
        if (!r.path || cdc_vec_push(&v, &r) != CDC_OK) {
            free(r.path);
            break;
        }
    }
    sqlite3_finalize(st);
    if (out_n) *out_n = v.len;
    cdc_index_dup_row_t *arr = (cdc_index_dup_row_t *)v.data;
    v.data = NULL; /* 交出所有权：调用方用 cdc_index_dup_rows_free 释放 */
    cdc_vec_free(&v);
    return arr;
}

void cdc_index_dup_rows_free(cdc_index_dup_row_t *rows, size_t n) {
    if (!rows) return;
    for (size_t i = 0; i < n; i++) free(rows[i].path);
    free(rows);
}

cdc_index_big_row_t *cdc_fileindex_query_big_rows(cdc_fileindex_t *idx,
                                                  const uint64_t *serials, size_t n_serials,
                                                  size_t *out_n) {
    if (out_n) *out_n = 0;
    if (!idx || n_serials == 0) return NULL;
    char *ph = placeholders(n_serials);
    if (!ph) return NULL;
    const char *tmpl =
        "SELECT path, size, mtime FROM files"
        " WHERE volume_serial IN (%s) AND size >= ?"
        " ORDER BY size DESC";
    int need = snprintf(NULL, 0, tmpl, ph);
    char *sql = (char *)malloc((size_t)need + 1);
    if (!sql) {
        free(ph);
        return NULL;
    }
    snprintf(sql, (size_t)need + 1, tmpl, ph);
    free(ph);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idx->db, sql, -1, &st, NULL) != SQLITE_OK) {
        free(sql);
        return NULL;
    }
    free(sql);
    int col = 1;
    for (size_t i = 0; i < n_serials; i++)
        sqlite3_bind_int64(st, col++, to_i64(serials[i]));
    sqlite3_bind_int64(st, col++, to_i64(CDC_INDEX_BIG_MIN_SIZE));
    cdc_vec_t v;
    if (cdc_vec_init(&v, sizeof(cdc_index_big_row_t)) != CDC_OK) {
        sqlite3_finalize(st);
        return NULL;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        cdc_index_big_row_t r;
        memset(&r, 0, sizeof(r));
        r.path = dup_text(st, 0);
        r.size = col_u64(st, 1);
        r.mtime = col_u64(st, 2);
        if (!r.path || cdc_vec_push(&v, &r) != CDC_OK) {
            free(r.path);
            break;
        }
    }
    sqlite3_finalize(st);
    if (out_n) *out_n = v.len;
    cdc_index_big_row_t *arr = (cdc_index_big_row_t *)v.data;
    v.data = NULL; /* 交出所有权：调用方用 cdc_index_big_rows_free 释放 */
    cdc_vec_free(&v);
    return arr;
}

void cdc_index_big_rows_free(cdc_index_big_row_t *rows, size_t n) {
    if (!rows) return;
    for (size_t i = 0; i < n; i++) free(rows[i].path);
    free(rows);
}

/* ---------- 自测（与 Rust fileindex.rs #[cfg(test)] 逐项对应） ---------- */

static int fi_failures = 0;
static int fi_checks = 0;

#define FI_CHECK(cond, ...) do { \
    fi_checks++; \
    if (!(cond)) { fi_failures++; printf("FAIL fileindex: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static cdc_digest_t dig(uint8_t fill) {
    cdc_digest_t d;
    memset(d.bytes, fill, CDC_DIGEST_LEN);
    return d;
}

static cdc_digest_update_t upd(uint64_t serial, uint64_t frn, uint64_t size, uint64_t mtime,
                               int has_full, uint8_t fill) {
    cdc_digest_update_t u;
    memset(&u, 0, sizeof(u));
    u.volume_serial = serial;
    u.frn = frn;
    u.size = size;
    u.mtime = mtime;
    if (has_full) {
        u.digests.has_full = 1;
        u.digests.full = dig(fill);
    }
    return u;
}

static cdc_index_row_t irow(uint64_t serial, uint64_t frn, const char *path,
                            uint64_t size, uint64_t mtime) {
    cdc_index_row_t r;
    r.volume_serial = serial;
    r.frn = frn;
    r.path = path;
    r.size = size;
    r.mtime = mtime;
    r.last_seen = 1000;
    return r;
}

/* 临时库路径：Linux 用 mkdtemp，Windows 用 %TEMP% + 进程号子目录。 */
static int fi_tmp_db(char *buf, size_t cap) {
#ifdef _WIN32
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    if (snprintf(buf, cap, "%scdc-fi-test-%lu\\test.db", tmp,
                 (unsigned long)GetCurrentProcessId()) >= (int)cap)
        return -1;
    /* 建父目录。 */
    char *slash = NULL;
    for (char *p = buf; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (slash) {
        *slash = '\0';
        mkdirs(buf);
        *slash = '\\';
    }
    return 0;
#else
    char dir[] = "/tmp/cdc-fi-test-XXXXXX";
    if (!mkdtemp(dir)) return -1;
    if (snprintf(buf, cap, "%s/test.db", dir) >= (int)cap) return -1;
    return 0;
#endif
}

static void test_usn_usable(void) {
    FI_CHECK(cdc_usn_state_usable(7, 500, 7, 100, 500), "usable exact");
    FI_CHECK(cdc_usn_state_usable(7, 500, 7, 100, 900), "usable behind");
    FI_CHECK(cdc_usn_state_usable(7, 100, 7, 100, 900), "usable at first_usn");
    FI_CHECK(!cdc_usn_state_usable(7, 500, 8, 100, 900), "gap journal changed");
    FI_CHECK(!cdc_usn_state_usable(7, 50, 7, 100, 900), "gap cursor before first");
    FI_CHECK(!cdc_usn_state_usable(7, 950, 7, 100, 900), "gap cursor ahead");
}

static void test_roundtrip(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_err_t err;
    cdc_fileindex_t *idx = cdc_fileindex_open(db, &err);
    FI_CHECK(idx != NULL, "open db: %s", err.message);
    if (!idx) return;
    cdc_index_row_t rows[] = {irow(11, 100, "C:\\a.bin", 4096, 111),
                              irow(11, 101, "C:\\b.bin", 4096, 222)};
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 2) == 2, "upsert 2");
    uint64_t total = 0, with_full = 0;
    cdc_fileindex_counts(idx, &total, &with_full);
    FI_CHECK(total == 2 && with_full == 0, "counts (2,0)");

    cdc_digest_update_t u = upd(11, 100, 4096, 111, 1, 2);
    u.digests.has_head = 1;
    u.digests.head = dig(1);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u, 1, 1000) == 1, "set digests 1");
    cdc_fileindex_counts(idx, &total, &with_full);
    FI_CHECK(total == 2 && with_full == 1, "counts (2,1)");

    /* size 对不上：拒绝写回。 */
    cdc_digest_update_t bad = upd(11, 101, 4097, 222, 1, 3);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &bad, 1, 1000) == 0, "reject size mismatch");
    cdc_fileindex_counts(idx, &total, &with_full);
    FI_CHECK(total == 2 && with_full == 1, "counts still (2,1)");

    /* 改名（同 frn、同 size/mtime）：摘要保留。 */
    cdc_index_row_t renamed = irow(11, 100, "C:\\a-renamed.bin", 4096, 111);
    FI_CHECK(cdc_fileindex_upsert_files(idx, &renamed, 1) == 1, "upsert rename");
    size_t n = 0;
    cdc_index_hash_row_t *hr = cdc_fileindex_rows_needing_full(idx, 11, &n);
    /* 整组返回（含已有 full 的 100）：与 Rust rows_needing_full 的 SQL
     * 一致——组里只要有一人缺 full，整组都返回，调用方只给缺 full 的
     * 行算哈希（见 hash_index_groups）。 */
    FI_CHECK(n == 2, "needing_full whole group (2), got %zu", n);
    int has100full = 0, has101nofull = 0;
    for (size_t i = 0; i < n; i++) {
        if (hr[i].frn == 100 && hr[i].digests.has_full) has100full = 1;
        if (hr[i].frn == 101 && !hr[i].digests.has_full) has101nofull = 1;
    }
    FI_CHECK(has100full && has101nofull, "needing_full group content");
    cdc_index_hash_rows_free(hr, n);

    /* 内容变了（size 变化）：摘要清掉。 */
    cdc_index_row_t changed = irow(11, 100, "C:\\a-renamed.bin", 8192, 333);
    FI_CHECK(cdc_fileindex_upsert_files(idx, &changed, 1) == 1, "upsert changed");
    cdc_fileindex_counts(idx, &total, &with_full);
    FI_CHECK(total == 2 && with_full == 0, "counts (2,0) after change");

    /* USN 状态读写。 */
    uint64_t j = 0;
    int64_t usn = 0;
    FI_CHECK(cdc_fileindex_get_usn_state(idx, 11, &j, &usn) == 0, "usn initially missing");
    FI_CHECK(cdc_fileindex_set_usn_state(idx, 11, 77, 12345) == 0, "set usn");
    FI_CHECK(cdc_fileindex_get_usn_state(idx, 11, &j, &usn) == 1 && j == 77 && usn == 12345,
             "get usn (77,12345)");
    FI_CHECK(cdc_fileindex_set_usn_state(idx, 11, 77, 12399) == 0, "set usn again");
    FI_CHECK(cdc_fileindex_get_usn_state(idx, 11, &j, &usn) == 1 && usn == 12399,
             "get usn updated");
    cdc_fileindex_close(idx);
}

static void test_delete_and_prefix(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_fileindex_t *idx = cdc_fileindex_open(db, NULL);
    FI_CHECK(idx != NULL, "open db");
    if (!idx) return;
    /* 中文目录名：LENGTH() 按字符算，前缀改写不能按字节切。 */
    cdc_index_row_t rows[] = {
        irow(11, 100, "C:\\旧目录\\a.bin", 4096, 111),
        irow(11, 101, "C:\\旧目录\\子目录\\b.bin", 4096, 222),
        irow(11, 102, "C:\\旧目录2\\c.bin", 4096, 333),
        irow(11, 103, "C:\\其他\\d.bin", 4096, 444),
    };
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 4) == 4, "upsert 4 chinese paths");
    uint64_t frns[] = {101, 999};
    FI_CHECK(cdc_fileindex_delete_rows(idx, 11, frns, 2) == 1, "delete 1 of 2");
    uint64_t total = 0, with_full = 0;
    cdc_fileindex_counts(idx, &total, &with_full);
    FI_CHECK(total == 3, "counts 3 after delete");
    FI_CHECK(cdc_fileindex_delete_rows(idx, 11, NULL, 0) == 0, "delete empty");
    size_t changed = cdc_fileindex_update_path_prefix(idx, 11, "C:\\旧目录", "C:\\新目录");
    FI_CHECK(changed == 1, "prefix rewrite 1 row, got %zu", changed);
    /* 校验改写结果：100 换了前缀，102（旧目录2）与 103 不动。 */
    size_t n = 0;
    /* 用 rows_needing_full 把全部行读回：三行同 size 4096 且都缺 full，整组返回。 */
    cdc_index_hash_row_t *hr = cdc_fileindex_rows_needing_full(idx, 11, &n);
    FI_CHECK(n == 3, "needing_full 3 rows, got %zu", n);
    int saw_new = 0, saw_old2 = 0, saw_other = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(hr[i].path, "C:\\新目录\\a.bin") == 0) saw_new = 1;
        if (strcmp(hr[i].path, "C:\\旧目录2\\c.bin") == 0) saw_old2 = 1;
        if (strcmp(hr[i].path, "C:\\其他\\d.bin") == 0) saw_other = 1;
    }
    FI_CHECK(saw_new && saw_old2 && saw_other, "prefix rewrite paths exact");
    cdc_index_hash_rows_free(hr, n);
    cdc_fileindex_close(idx);
}

static void test_needing_full_groups(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_fileindex_t *idx = cdc_fileindex_open(db, NULL);
    FI_CHECK(idx != NULL, "open db");
    if (!idx) return;
    cdc_index_row_t rows[] = {
        irow(11, 100, "C:\\a.bin", 4096, 111), irow(11, 101, "C:\\b.bin", 4096, 222),
        irow(11, 102, "C:\\c.bin", 8192, 333), irow(11, 103, "C:\\d.bin", 8192, 444),
        irow(11, 104, "C:\\e.bin", 2048, 555),
    };
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 5) == 5, "upsert 5");
    cdc_digest_update_t u1 = upd(11, 100, 4096, 111, 1, 9);
    cdc_digest_update_t u2 = upd(11, 102, 8192, 333, 1, 9);
    cdc_digest_update_t u3 = upd(11, 103, 8192, 444, 1, 9);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u1, 1, 1000) == 1, "digest 100");
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u2, 1, 1000) == 1, "digest 102");
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u3, 1, 1000) == 1, "digest 103");
    size_t n = 0;
    cdc_index_hash_row_t *hr = cdc_fileindex_rows_needing_full(idx, 11, &n);
    FI_CHECK(n == 2, "needing_full 2 rows, got %zu", n);
    int all4096 = 1, has100 = 0, has101 = 0;
    for (size_t i = 0; i < n; i++) {
        if (hr[i].size != 4096) all4096 = 0;
        if (hr[i].frn == 100 && hr[i].digests.has_full) has100 = 1;
        if (hr[i].frn == 101 && !hr[i].digests.has_full) has101 = 1;
    }
    FI_CHECK(all4096 && has100 && has101, "needing_full group content");
    cdc_index_hash_rows_free(hr, n);
    cdc_digest_update_t u4 = upd(11, 101, 4096, 222, 1, 8);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u4, 1, 1000) == 1, "digest 101");
    hr = cdc_fileindex_rows_needing_full(idx, 11, &n);
    FI_CHECK(n == 0, "needing_full empty after completion");
    cdc_index_hash_rows_free(hr, n);
    cdc_fileindex_close(idx);
}

static void test_all_digests(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_fileindex_t *idx = cdc_fileindex_open(db, NULL);
    FI_CHECK(idx != NULL, "open db");
    if (!idx) return;
    cdc_index_row_t rows[] = {
        irow(11, 100, "C:\\a.bin", 4096, 111),
        irow(11, 101, "C:\\b.bin", 4096, 222),
        irow(11, 102, "C:\\c.bin", 4096, 333),
        irow(22, 200, "D:\\a.bin", 4096, 111),
    };
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 4) == 4, "upsert 4");
    /* 100：full + head；101：只有 sample；102：无摘要；200（另一卷）：full。 */
    cdc_digest_update_t u1 = upd(11, 100, 4096, 111, 1, 2);
    u1.digests.has_head = 1;
    u1.digests.head = dig(1);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u1, 1, 1000) == 1, "digest 100");
    cdc_digest_update_t u2;
    memset(&u2, 0, sizeof(u2));
    u2.volume_serial = 11;
    u2.frn = 101;
    u2.size = 4096;
    u2.mtime = 222;
    u2.digests.has_sample = 1;
    u2.digests.sample = dig(5);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u2, 1, 1000) == 1, "digest 101 sample");
    cdc_digest_update_t u3 = upd(22, 200, 4096, 111, 1, 7);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u3, 1, 1000) == 1, "digest 200");
    size_t n = 0;
    cdc_index_digest_entry_t *all = cdc_fileindex_all_digests(idx, 11, &n);
    FI_CHECK(n == 2, "all_digests vol 11 has 2, got %zu", n);
    FI_CHECK(n == 2 && all[0].frn == 100 && all[1].frn == 101, "all_digests sorted by frn");
    FI_CHECK(n == 2 && all[0].digests.has_full && all[0].digests.has_head,
             "all_digests 100 full+head");
    FI_CHECK(n == 2 && !all[0].digests.has_sample && !all[0].digests.has_strong,
             "all_digests 100 no sample/strong");
    FI_CHECK(n == 2 && all[1].digests.has_sample && !all[1].digests.has_full,
             "all_digests 101 sample only");
    FI_CHECK(n == 2 && memcmp(all[0].digests.full.bytes, dig(2).bytes, 32) == 0,
             "all_digests 100 full bytes exact");
    free(all);
    /* 改名（同 frn、同 size/mtime）：摘要仍在。 */
    cdc_index_row_t renamed = irow(11, 100, "C:\\a-renamed.bin", 4096, 111);
    FI_CHECK(cdc_fileindex_upsert_files(idx, &renamed, 1) == 1, "upsert rename 100");
    all = cdc_fileindex_all_digests(idx, 11, &n);
    FI_CHECK(n == 2 && all[0].frn == 100 && all[0].digests.has_full,
             "all_digests rename keeps full");
    free(all);
    /* 内容变了（size 变化）：摘要清掉，不再返回。 */
    cdc_index_row_t changed = irow(11, 101, "C:\\b.bin", 8192, 222);
    FI_CHECK(cdc_fileindex_upsert_files(idx, &changed, 1) == 1, "upsert changed 101");
    all = cdc_fileindex_all_digests(idx, 11, &n);
    FI_CHECK(n == 1 && all[0].frn == 100, "all_digests changed row dropped");
    free(all);
    /* 另一卷 / 空卷隔离。 */
    all = cdc_fileindex_all_digests(idx, 22, &n);
    FI_CHECK(n == 1 && all[0].frn == 200 && all[0].digests.has_full,
             "all_digests vol 22 separate");
    free(all);
    all = cdc_fileindex_all_digests(idx, 99, &n);
    FI_CHECK(n == 0, "all_digests empty volume");
    free(all);
    cdc_fileindex_close(idx);
}

static void test_delete_volume(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_fileindex_t *idx = cdc_fileindex_open(db, NULL);
    FI_CHECK(idx != NULL, "open db");
    if (!idx) return;
    cdc_index_row_t rows[] = {irow(11, 100, "C:\\a.bin", 4096, 111),
                              irow(22, 200, "D:\\a.bin", 4096, 111)};
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 2) == 2, "upsert 2 vols");
    FI_CHECK(cdc_fileindex_set_usn_state(idx, 11, 77, 100) == 0, "set usn 11");
    FI_CHECK(cdc_fileindex_set_usn_state(idx, 22, 88, 200) == 0, "set usn 22");
    cdc_fileindex_delete_volume(idx, 11);
    uint64_t total = 0, with_full = 0;
    cdc_fileindex_counts(idx, &total, &with_full);
    FI_CHECK(total == 1, "counts 1 after delete_volume");
    uint64_t j = 0;
    int64_t u = 0;
    FI_CHECK(cdc_fileindex_get_usn_state(idx, 11, &j, &u) == 0, "usn 11 gone");
    FI_CHECK(cdc_fileindex_get_usn_state(idx, 22, &j, &u) == 1 && j == 88 && u == 200,
             "usn 22 kept");
    cdc_fileindex_close(idx);
}

static void test_query_dup_rows(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_fileindex_t *idx = cdc_fileindex_open(db, NULL);
    FI_CHECK(idx != NULL, "open db");
    if (!idx) return;
    cdc_index_row_t rows[] = {
        irow(11, 100, "C:\\a.bin", 4096, 111), irow(11, 101, "C:\\b.bin", 4096, 222),
        irow(22, 200, "D:\\c.bin", 4096, 333), irow(11, 102, "C:\\d.bin", 8192, 444),
        irow(11, 103, "C:\\e.bin", 2048, 555), irow(11, 104, "C:\\f.bin", 2048, 666),
    };
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 6) == 6, "upsert 6");
    cdc_digest_update_t u = upd(11, 100, 4096, 111, 1, 9);
    u.digests.has_strong = 1;
    u.digests.strong = dig(5);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u, 1, 1000) == 1, "digest 100");
    size_t n = 0;
    uint64_t serials[] = {11, 22};
    cdc_index_dup_row_t *dr = cdc_fileindex_query_dup_rows(idx, serials, 2, 4096, &n);
    FI_CHECK(n == 3, "dup rows 3, got %zu", n);
    int all4096 = 1, has100 = 0, has101 = 0, has200 = 0;
    for (size_t i = 0; i < n; i++) {
        if (dr[i].size != 4096) all4096 = 0;
        if (dr[i].frn == 100 && dr[i].digests.has_full &&
            memcmp(dr[i].digests.full.bytes, dig(9).bytes, CDC_DIGEST_LEN) == 0 &&
            dr[i].digests.has_strong)
            has100 = 1;
        if (dr[i].frn == 101 && !dr[i].digests.has_full) has101 = 1;
        if (dr[i].frn == 200) has200 = 1;
    }
    FI_CHECK(all4096 && has100 && has101 && has200, "dup rows content");
    cdc_index_dup_rows_free(dr, n);
    dr = cdc_fileindex_query_dup_rows(idx, serials, 1, 4096, &n);
    FI_CHECK(n == 2, "dup rows C only 2, got %zu", n);
    cdc_index_dup_rows_free(dr, n);
    dr = cdc_fileindex_query_dup_rows(idx, serials, 2, 8192, &n);
    FI_CHECK(n == 0, "dup rows min 8192 empty");
    cdc_index_dup_rows_free(dr, n);
    dr = cdc_fileindex_query_dup_rows(idx, NULL, 0, 4096, &n);
    FI_CHECK(dr == NULL && n == 0, "dup rows empty serials");
    cdc_index_dup_rows_free(dr, n);
    cdc_fileindex_close(idx);
}

static void test_query_big_rows(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_fileindex_t *idx = cdc_fileindex_open(db, NULL);
    FI_CHECK(idx != NULL, "open db");
    if (!idx) return;
    const uint64_t MB = (uint64_t)1 << 20;
    cdc_index_row_t rows[] = {
        irow(11, 100, "C:\\small.bin", 1 * MB, 111),
        irow(11, 101, "C:\\mid.bin", 3 * MB, 222),
        irow(11, 102, "C:\\big.bin", 100 * MB, 333),
        irow(22, 200, "D:\\other.bin", 50 * MB, 444),
    };
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 4) == 4, "upsert 4");
    size_t n = 0;
    uint64_t serials[] = {11, 22};
    cdc_index_big_row_t *br = cdc_fileindex_query_big_rows(idx, serials, 2, &n);
    FI_CHECK(n == 3, "big rows 3, got %zu", n);
    FI_CHECK(n == 3 && br[0].size == 100 * MB && br[1].size == 50 * MB &&
                 br[2].size == 3 * MB,
             "big rows desc order");
    FI_CHECK(n == 3 && strcmp(br[0].path, "C:\\big.bin") == 0, "big rows first path");
    cdc_index_big_rows_free(br, n);
    br = cdc_fileindex_query_big_rows(idx, serials + 1, 1, &n);
    FI_CHECK(n == 1, "big rows D only 1");
    cdc_index_big_rows_free(br, n);
    br = cdc_fileindex_query_big_rows(idx, NULL, 0, &n);
    FI_CHECK(br == NULL && n == 0, "big rows empty serials");
    cdc_index_big_rows_free(br, n);
    cdc_fileindex_close(idx);
}

static void test_counts_and_updated_at(void) {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    cdc_fileindex_t *idx = cdc_fileindex_open(db, NULL);
    FI_CHECK(idx != NULL, "open db");
    if (!idx) return;
    uint64_t total = 0, with_full = 0;
    cdc_fileindex_volume_counts(idx, 11, &total, &with_full);
    FI_CHECK(total == 0 && with_full == 0, "volume counts (0,0)");
    uint64_t at = 0;
    FI_CHECK(cdc_fileindex_usn_state_updated_at(idx, 11, &at) == 0, "updated_at missing");
    cdc_index_row_t rows[] = {irow(11, 100, "C:\\a.bin", 4096, 111),
                              irow(11, 101, "C:\\b.bin", 4096, 222)};
    FI_CHECK(cdc_fileindex_upsert_files(idx, rows, 2) == 2, "upsert 2");
    cdc_digest_update_t u = upd(11, 100, 4096, 111, 1, 9);
    FI_CHECK(cdc_fileindex_set_digests_batch(idx, &u, 1, 1000) == 1, "digest 100");
    cdc_fileindex_volume_counts(idx, 11, &total, &with_full);
    FI_CHECK(total == 2 && with_full == 1, "volume counts (2,1)");
    cdc_fileindex_volume_counts(idx, 22, &total, &with_full);
    FI_CHECK(total == 0 && with_full == 0, "volume counts other (0,0)");
    FI_CHECK(cdc_fileindex_set_usn_state(idx, 11, 77, 12345) == 0, "set usn");
    FI_CHECK(cdc_fileindex_usn_state_updated_at(idx, 11, &at) == 1 && at > 0,
             "updated_at present");
    cdc_fileindex_close(idx);
}

int cdc_fileindex_selftest(void) {
    fi_failures = 0;
    fi_checks = 0;
    test_usn_usable();
    test_roundtrip();
    test_delete_and_prefix();
    test_needing_full_groups();
    test_all_digests();
    test_delete_volume();
    test_query_dup_rows();
    test_query_big_rows();
    test_counts_and_updated_at();
    printf("fileindex selftest: %d checks, %d failures\n", fi_checks, fi_failures);
    return fi_failures;
}
