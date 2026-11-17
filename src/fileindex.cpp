// fileindex.cpp — 常驻索引 SQLite（对应 Rust 版 desktop/src/fileindex.rs）。
// SQL 与 Rust 版逐字一致；本文件只负责 C++ 包装与自测。
#include "fileindex.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "mft.h"  // cache_dir、usn_state_usable
#include "sqlite3.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cdc {
namespace {

constexpr const char *kSchemaSql =
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


// ---------- 小工具 ----------

// u64 -> sqlite3_int64（与 Rust 的 `as i64` 同样 wrap）；读回时 clamp 到 0。
sqlite3_int64 to_i64(uint64_t v) { return static_cast<sqlite3_int64>(v); }
uint64_t from_i64(sqlite3_int64 v) { return v < 0 ? 0 : static_cast<uint64_t>(v); }
uint64_t col_u64(sqlite3_stmt *st, int col) { return from_i64(sqlite3_column_int64(st, col)); }

// 递归建目录（对应 Rust create_dir_all）。path 为 UTF-8。
bool mkdirs(const std::string &path) {
    if (path.empty()) return false;
#ifdef _WIN32
    // 逐级建：用户名里可能有中文，走宽字符 API。
    std::optional<std::wstring> w = utf8_to_wide(path);
    if (!w) return false;
    for (size_t i = 1; i < w->size(); i++) {
        if ((*w)[i] == L'\\' || (*w)[i] == L'/')
            CreateDirectoryW(w->substr(0, i).c_str(), nullptr);
    }
    BOOL ok = CreateDirectoryW(w->c_str(), nullptr);
    return ok || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);  // 已存在不算错
    }
    if (mkdir(path.c_str(), 0777) != 0) {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    return true;
#endif
}

// 建父目录：去掉最后一个分隔符后的部分。
void ensure_parent_dir(std::string_view db_path) {
    size_t slash = db_path.find_last_of("/\\");
    if (slash != std::string_view::npos && slash > 0) mkdirs(std::string(db_path.substr(0, slash)));
}

UniqueStmt prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
        if (st) sqlite3_finalize(st);
        return nullptr;
    }
    return UniqueStmt(st);
}

bool exec(sqlite3 *db, const char *sql) {
    char *emsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &emsg);
    sqlite3_free(emsg);
    return rc == SQLITE_OK;
}

void bind_text(sqlite3_stmt *st, int col, std::string_view s) {
    sqlite3_bind_text(st, col, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
}

void bind_digest_or_null(sqlite3_stmt *st, int col, bool has, const Digest &d) {
    if (has)
        sqlite3_bind_blob(st, col, d.bytes, static_cast<int>(kDigestLen), SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, col);
}

// XXH3-64 按位存 INTEGER；没有时绑 NULL（不覆盖已有值）。
void bind_xxh_or_null(sqlite3_stmt *st, int col, bool has, uint64_t v) {
    if (has)
        sqlite3_bind_int64(st, col, static_cast<sqlite3_int64>(v));
    else
        sqlite3_bind_null(st, col);
}

// 从一列读摘要：BLOB 长度必须正好 32 字节，否则视为没有（与 Rust
// digest_col 的 try_into().ok() 一致）。
bool read_digest(sqlite3_stmt *st, int col, Digest &d) {
    if (sqlite3_column_type(st, col) != SQLITE_BLOB) return false;
    const void *blob = sqlite3_column_blob(st, col);
    if (!blob || sqlite3_column_bytes(st, col) != static_cast<int>(kDigestLen)) return false;
    std::memcpy(d.bytes, blob, kDigestLen);
    return true;
}

// 从一列读 XXH3-64：INTEGER 列按位还原 uint64；NULL 视为没有。
bool read_xxh(sqlite3_stmt *st, int col, uint64_t &v) {
    if (sqlite3_column_type(st, col) != SQLITE_INTEGER) return false;
    v = static_cast<uint64_t>(sqlite3_column_int64(st, col));
    return true;
}

// 从 first 起依次读 head/sample/strong/full（BLOB）+ 三档 xxh（INTEGER）。
void read_digest_set(sqlite3_stmt *st, int first, DigestSet &d, bool with_xxh) {
    d.has_head = read_digest(st, first, d.head);
    d.has_sample = read_digest(st, first + 1, d.sample);
    d.has_strong = read_digest(st, first + 2, d.strong);
    d.has_full = read_digest(st, first + 3, d.full);
    if (!with_xxh) return;
    d.has_head_xxh = read_xxh(st, first + 4, d.head_xxh);
    d.has_sample_xxh = read_xxh(st, first + 5, d.sample_xxh);
    d.has_strong_xxh = read_xxh(st, first + 6, d.strong_xxh);
}

std::string column_text(sqlite3_stmt *st, int col) {
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? std::string(reinterpret_cast<const char *>(t)) : std::string();
}

// "?,?,?"：n 个占位符。
std::string placeholders(size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (i) s += ',';
        s += '?';
    }
    return s;
}

// 在一个 BEGIN IMMEDIATE / COMMIT 事务里对每个元素执行一次 sql；
// fn(st, elem) 负责绑定参数，返回本次计入的行数。事务失败返回 0。
template <class T, class Fn>
size_t batch_in_txn(sqlite3 *db, const char *sql, std::span<const T> items, Fn &&fn) {
    if (items.empty()) return 0;
    if (!exec(db, "BEGIN IMMEDIATE")) return 0;
    UniqueStmt st = prepare(db, sql);
    if (!st) {
        exec(db, "ROLLBACK");
        return 0;
    }
    size_t n = 0;
    for (const T &item : items) {
        sqlite3_reset(st.get());
        sqlite3_clear_bindings(st.get());
        n += fn(st.get(), item);
    }
    st.reset();
    if (!exec(db, "COMMIT")) return 0;
    return n;
}

IndexCounts count_pair(sqlite3 *db, const char *sql_total, const char *sql_full,
                       std::optional<uint64_t> serial) {
    IndexCounts c;
    auto one = [&](const char *sql, uint64_t &out) {
        UniqueStmt st = prepare(db, sql);
        if (!st) return;
        if (serial) sqlite3_bind_int64(st.get(), 1, to_i64(*serial));
        if (sqlite3_step(st.get()) == SQLITE_ROW) out = col_u64(st.get(), 0);
    };
    one(sql_total, c.total);
    one(sql_full, c.with_full);
    return c;
}

}  // namespace

// ---------- 打开 ----------

std::unique_ptr<FileIndex> FileIndex::open(const char *db_path, Error *err) {
    if (!db_path || !*db_path) {
        err_set(err, kErrInvalidArg, "索引库路径为空");
        return nullptr;
    }
    ensure_parent_dir(db_path);
    sqlite3 *raw = nullptr;
    int rc = sqlite3_open(db_path, &raw);
    UniqueSqlite db(raw);
    if (rc != SQLITE_OK) {
        err_set(err, kErrIo, "%s", db ? sqlite3_errmsg(db.get()) : "打开索引库失败");
        return nullptr;
    }
    char *emsg = nullptr;
    if (sqlite3_exec(db.get(), kSchemaSql, nullptr, nullptr, &emsg) != SQLITE_OK) {
        err_set(err, kErrIo, "%s", emsg ? emsg : "建索引表失败");
        sqlite3_free(emsg);
        return nullptr;
    }
    // 旧库补列：files 表可能是 Rust 版或旧版 C 建的（无 xxh 三列），
    // CREATE TABLE IF NOT EXISTS 不会补新列，这里按需 ALTER。
    for (const char *col : {"head_xxh", "sample_xxh", "strong_xxh"}) {
        std::string q =
            format("SELECT COUNT(*) FROM pragma_table_info('files') WHERE name = '%s'", col);
        bool has = false;
        if (UniqueStmt ck = prepare(db.get(), q.c_str()))
            has = sqlite3_step(ck.get()) == SQLITE_ROW && sqlite3_column_int(ck.get(), 0) > 0;
        if (!has) {
            std::string alter = format("ALTER TABLE files ADD COLUMN %s INTEGER", col);
            exec(db.get(), alter.c_str());  // 失败忽略：下次打开再试
        }
    }
    if (UniqueStmt st = prepare(db.get(),
                                "INSERT OR IGNORE INTO index_meta (key, value) "
                                "VALUES ('schema_version', ?1)")) {
        bind_text(st.get(), 1, kIndexSchemaVersion);
        sqlite3_step(st.get());
    }
    err_init(err);
    return std::unique_ptr<FileIndex>(new FileIndex(std::move(db)));
}

std::unique_ptr<FileIndex> FileIndex::open_default(Error *err) {
    // 默认路径：cache_dir()（%LOCALAPPDATA%\C盘清理工具\mftcache）。
    std::optional<std::string> dir = cache_dir();
    if (!dir) {
        err_set(err, kErrIo, "取索引缓存目录失败");
        return nullptr;
    }
    std::string path = *dir + "/file-index-v1.db";
    return open(path.c_str(), err);
}

// ---------- 写操作 ----------

size_t FileIndex::upsert_files(std::span<const IndexRow> rows) {
    static const char *const kSql =
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
    return batch_in_txn(db_.get(), kSql, rows, [](sqlite3_stmt *st, const IndexRow &r) -> size_t {
        sqlite3_bind_int64(st, 1, to_i64(r.volume_serial));
        sqlite3_bind_int64(st, 2, to_i64(r.frn));
        bind_text(st, 3, r.path);
        sqlite3_bind_int64(st, 4, to_i64(r.size));
        sqlite3_bind_int64(st, 5, to_i64(r.mtime));
        sqlite3_bind_int64(st, 6, to_i64(r.last_seen));
        return sqlite3_step(st) == SQLITE_DONE ? 1 : 0;
    });
}

size_t FileIndex::set_digests_batch(std::span<const DigestUpdate> updates, uint64_t now_secs) {
    static const char *const kSql =
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
    sqlite3 *db = db_.get();
    return batch_in_txn(db, kSql, updates, [&](sqlite3_stmt *st, const DigestUpdate &u) -> size_t {
        const DigestSet &d = u.digests;
        sqlite3_bind_int64(st, 1, to_i64(u.volume_serial));
        sqlite3_bind_int64(st, 2, to_i64(u.frn));
        sqlite3_bind_int64(st, 3, to_i64(u.size));
        sqlite3_bind_int64(st, 4, to_i64(u.mtime));
        bind_digest_or_null(st, 5, d.has_head, d.head);
        bind_digest_or_null(st, 6, d.has_sample, d.sample);
        bind_digest_or_null(st, 7, d.has_strong, d.strong);
        bind_digest_or_null(st, 8, d.has_full, d.full);
        bind_xxh_or_null(st, 9, d.has_head_xxh, d.head_xxh);
        bind_xxh_or_null(st, 10, d.has_sample_xxh, d.sample_xxh);
        bind_xxh_or_null(st, 11, d.has_strong_xxh, d.strong_xxh);
        sqlite3_bind_int64(st, 12, to_i64(now_secs));
        return sqlite3_step(st) == SQLITE_DONE ? static_cast<size_t>(sqlite3_changes(db)) : 0;
    });
}

int FileIndex::set_usn_state(uint64_t volume_serial, uint64_t journal_id, int64_t next_usn) {
    UniqueStmt st = prepare(db_.get(),
                            "INSERT INTO usn_state (volume_serial, journal_id, next_usn, updated_at)"
                            " VALUES (?1, ?2, ?3, ?4)"
                            " ON CONFLICT(volume_serial) DO UPDATE SET"
                            "     journal_id = excluded.journal_id,"
                            "     next_usn = excluded.next_usn,"
                            "     updated_at = excluded.updated_at");
    if (!st) return -1;
    sqlite3_bind_int64(st.get(), 1, to_i64(volume_serial));
    sqlite3_bind_int64(st.get(), 2, to_i64(journal_id));
    sqlite3_bind_int64(st.get(), 3, next_usn);
    sqlite3_bind_int64(st.get(), 4, to_i64(static_cast<uint64_t>(std::time(nullptr))));
    return sqlite3_step(st.get()) == SQLITE_DONE ? 0 : -1;
}

std::optional<UsnState> FileIndex::get_usn_state(uint64_t volume_serial) {
    UniqueStmt st = prepare(db_.get(),
                            "SELECT journal_id, next_usn FROM usn_state WHERE volume_serial = ?1");
    if (!st) return std::nullopt;
    sqlite3_bind_int64(st.get(), 1, to_i64(volume_serial));
    if (sqlite3_step(st.get()) != SQLITE_ROW) return std::nullopt;
    return UsnState{col_u64(st.get(), 0), sqlite3_column_int64(st.get(), 1)};
}

size_t FileIndex::delete_rows(uint64_t volume_serial, std::span<const uint64_t> frns) {
    sqlite3 *db = db_.get();
    return batch_in_txn(db, "DELETE FROM files WHERE volume_serial = ?1 AND frn = ?2", frns,
                        [&](sqlite3_stmt *st, uint64_t frn) -> size_t {
                            sqlite3_bind_int64(st, 1, to_i64(volume_serial));
                            sqlite3_bind_int64(st, 2, to_i64(frn));
                            return sqlite3_step(st) == SQLITE_DONE
                                       ? static_cast<size_t>(sqlite3_changes(db))
                                       : 0;
                        });
}

size_t FileIndex::update_path_prefix(uint64_t volume_serial, std::string_view old_prefix,
                                     std::string_view new_prefix) {
    // 去掉尾部 '\' 再补一个（与 Rust trim_end_matches('\\') + format!("{}\\", …) 一致）。
    auto with_sep = [](std::string_view p) {
        while (!p.empty() && p.back() == '\\') p.remove_suffix(1);
        return std::string(p) + '\\';
    };
    std::string old_full = with_sep(old_prefix), new_full = with_sep(new_prefix);
    UniqueStmt st = prepare(db_.get(),
                            "UPDATE files"
                            " SET path = ?2 || SUBSTR(path, LENGTH(?3) + 1)"
                            " WHERE volume_serial = ?1 AND SUBSTR(path, 1, LENGTH(?3)) = ?3");
    if (!st) return 0;
    sqlite3_bind_int64(st.get(), 1, to_i64(volume_serial));
    bind_text(st.get(), 2, new_full);
    bind_text(st.get(), 3, old_full);
    if (sqlite3_step(st.get()) != SQLITE_DONE) return 0;
    return static_cast<size_t>(sqlite3_changes(db_.get()));
}

void FileIndex::delete_volume(uint64_t volume_serial) {
    for (const char *sql : {"DELETE FROM files WHERE volume_serial = ?1",
                            "DELETE FROM usn_state WHERE volume_serial = ?1"}) {
        if (UniqueStmt st = prepare(db_.get(), sql)) {
            sqlite3_bind_int64(st.get(), 1, to_i64(volume_serial));
            sqlite3_step(st.get());
        }
    }
}

// ---------- 读操作 ----------

std::vector<IndexHashRow> FileIndex::rows_needing_full(uint64_t volume_serial) {
    std::vector<IndexHashRow> out;
    UniqueStmt st = prepare(db_.get(),
                            "SELECT frn, path, size, mtime, head, sample, strong, full,"
                            " head_xxh, sample_xxh, strong_xxh"
                            " FROM files"
                            " WHERE volume_serial = ?1 AND size IN ("
                            "     SELECT size FROM files WHERE volume_serial = ?1"
                            "     GROUP BY size HAVING COUNT(*) >= 2 AND COUNT(full) < COUNT(*)"
                            " )"
                            " ORDER BY size, frn");
    if (!st) return out;
    sqlite3_bind_int64(st.get(), 1, to_i64(volume_serial));
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        IndexHashRow &r = out.emplace_back();
        r.frn = col_u64(st.get(), 0);
        r.path = column_text(st.get(), 1);
        r.size = col_u64(st.get(), 2);
        r.mtime = col_u64(st.get(), 3);
        read_digest_set(st.get(), 4, r.digests, true);
    }
    return out;
}

std::vector<IndexDigestEntry> FileIndex::all_digests(uint64_t volume_serial) {
    std::vector<IndexDigestEntry> out;
    UniqueStmt st = prepare(db_.get(),
                            "SELECT frn, head, sample, strong, full"
                            " FROM files"
                            " WHERE volume_serial = ?1"
                            "   AND (head IS NOT NULL OR sample IS NOT NULL"
                            "        OR strong IS NOT NULL OR full IS NOT NULL)"
                            " ORDER BY frn");
    if (!st) return out;
    sqlite3_bind_int64(st.get(), 1, to_i64(volume_serial));
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        IndexDigestEntry &e = out.emplace_back();
        e.frn = col_u64(st.get(), 0);
        read_digest_set(st.get(), 1, e.digests, false);
    }
    return out;
}

IndexCounts FileIndex::counts() {
    return count_pair(db_.get(), "SELECT COUNT(*) FROM files",
                      "SELECT COUNT(*) FROM files WHERE full IS NOT NULL", std::nullopt);
}

IndexCounts FileIndex::volume_counts(uint64_t volume_serial) {
    return count_pair(db_.get(), "SELECT COUNT(*) FROM files WHERE volume_serial = ?1",
                      "SELECT COUNT(*) FROM files WHERE volume_serial = ?1 AND full IS NOT NULL",
                      volume_serial);
}

std::optional<uint64_t> FileIndex::usn_state_updated_at(uint64_t volume_serial) {
    UniqueStmt st =
        prepare(db_.get(), "SELECT updated_at FROM usn_state WHERE volume_serial = ?1");
    if (!st) return std::nullopt;
    sqlite3_bind_int64(st.get(), 1, to_i64(volume_serial));
    if (sqlite3_step(st.get()) != SQLITE_ROW) return std::nullopt;
    return col_u64(st.get(), 0);
}

std::vector<IndexDupRow> FileIndex::query_dup_rows(std::span<const uint64_t> serials,
                                                   uint64_t min_size) {
    std::vector<IndexDupRow> out;
    if (serials.empty()) return out;
    // SQL 模板与 Rust 版一致（IN 列表出现两次：外层与子查询）。
    std::string ph = placeholders(serials.size());
    std::string sql = format(
        "SELECT volume_serial, frn, path, size, mtime, head, sample, strong, full,"
        " head_xxh, sample_xxh, strong_xxh"
        " FROM files"
        " WHERE volume_serial IN (%s) AND size >= ? AND size IN ("
        "     SELECT size FROM files WHERE volume_serial IN (%s)"
        "     GROUP BY size HAVING COUNT(*) >= 2"
        " )"
        " ORDER BY size DESC",
        ph.c_str(), ph.c_str());
    UniqueStmt st = prepare(db_.get(), sql.c_str());
    if (!st) return out;
    int col = 1;
    for (uint64_t s : serials) sqlite3_bind_int64(st.get(), col++, to_i64(s));
    sqlite3_bind_int64(st.get(), col++, to_i64(min_size));
    for (uint64_t s : serials) sqlite3_bind_int64(st.get(), col++, to_i64(s));
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        IndexDupRow &r = out.emplace_back();
        r.volume_serial = col_u64(st.get(), 0);
        r.frn = col_u64(st.get(), 1);
        r.path = column_text(st.get(), 2);
        r.size = col_u64(st.get(), 3);
        r.mtime = col_u64(st.get(), 4);
        read_digest_set(st.get(), 5, r.digests, true);
    }
    return out;
}

std::vector<IndexBigRow> FileIndex::query_big_rows(std::span<const uint64_t> serials) {
    std::vector<IndexBigRow> out;
    if (serials.empty()) return out;
    std::string sql = format(
        "SELECT path, size, mtime FROM files"
        " WHERE volume_serial IN (%s) AND size >= ?"
        " ORDER BY size DESC",
        placeholders(serials.size()).c_str());
    UniqueStmt st = prepare(db_.get(), sql.c_str());
    if (!st) return out;
    int col = 1;
    for (uint64_t s : serials) sqlite3_bind_int64(st.get(), col++, to_i64(s));
    sqlite3_bind_int64(st.get(), col++, to_i64(kIndexBigMinSize));
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        IndexBigRow &r = out.emplace_back();
        r.path = column_text(st.get(), 0);
        r.size = col_u64(st.get(), 1);
        r.mtime = col_u64(st.get(), 2);
    }
    return out;
}

// ---------- 自测（与 Rust fileindex.rs #[cfg(test)] 逐项对应） ----------

namespace {

int fi_failures = 0;
int fi_checks = 0;

#define FI_CHECK(cond, ...) do { \
    fi_checks++; \
    if (!(cond)) { fi_failures++; std::printf("FAIL fileindex: "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

void set_counts(IndexCounts c, uint64_t &total, uint64_t &with_full) {
    total = c.total;
    with_full = c.with_full;
}

// get_usn_state 的 1=找到 / 0=没有 形式（自测沿用 C 版断言写法）。
int get_usn(FileIndex &idx, uint64_t serial, uint64_t &journal, int64_t &usn) {
    std::optional<UsnState> s = idx.get_usn_state(serial);
    if (!s) return 0;
    journal = s->journal_id;
    usn = s->next_usn;
    return 1;
}

Digest dig(uint8_t fill) {
    Digest d;
    std::memset(d.bytes, fill, kDigestLen);
    return d;
}

DigestUpdate upd(uint64_t serial, uint64_t frn, uint64_t size, uint64_t mtime,
                               bool has_full, uint8_t fill) {
    DigestUpdate u;
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

IndexRow irow(uint64_t serial, uint64_t frn, const char *path,
                            uint64_t size, uint64_t mtime) {
    IndexRow r;
    r.volume_serial = serial;
    r.frn = frn;
    r.path = path;
    r.size = size;
    r.mtime = mtime;
    r.last_seen = 1000;
    return r;
}

/* 临时库路径：Linux 用 mkdtemp；Windows 每次用 GetTempFileNameW 取唯一文件。 */
int fi_tmp_db(char *buf, size_t cap) {
    if (!buf || cap == 0) return -1;
#ifdef _WIN32
    wchar_t wtmp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, wtmp);
    if (n == 0 || n > MAX_PATH) return -1;
    // GetTempFileNameW 同时创建空文件；SQLite 可直接打开并初始化它。
    wchar_t wdb[MAX_PATH + 1];
    if (GetTempFileNameW(wtmp, L"cfi", 0, wdb) == 0) return -1;
    std::optional<std::string> db = wide_to_utf8(wdb);
    if (!db || db->size() >= cap) {
        DeleteFileW(wdb);
        return -1;
    }
    std::memcpy(buf, db->c_str(), db->size() + 1);
    return 0;
#else
    char dir[] = "/tmp/cdc-fi-test-XXXXXX";
    if (!mkdtemp(dir)) return -1;
    if (std::snprintf(buf, cap, "%s/test.db", dir) >= (int)cap) return -1;
    return 0;
#endif
}

void test_usn_usable() {
    FI_CHECK(cdc::usn_state_usable(7, 500, 7, 100, 500), "usable exact");
    FI_CHECK(cdc::usn_state_usable(7, 500, 7, 100, 900), "usable behind");
    FI_CHECK(cdc::usn_state_usable(7, 100, 7, 100, 900), "usable at first_usn");
    FI_CHECK(!cdc::usn_state_usable(7, 500, 8, 100, 900), "gap journal changed");
    FI_CHECK(!cdc::usn_state_usable(7, 50, 7, 100, 900), "gap cursor before first");
    FI_CHECK(!cdc::usn_state_usable(7, 950, 7, 100, 900), "gap cursor ahead");
}

void test_roundtrip() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    Error err;
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, &err);
    FI_CHECK(idx != nullptr, "open db: %s", err.message);
    if (!idx) return;
    IndexRow rows[] = {irow(11, 100, "C:\\a.bin", 4096, 111),
                              irow(11, 101, "C:\\b.bin", 4096, 222)};
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 2)) == 2, "upsert 2");
    uint64_t total = 0, with_full = 0;
    set_counts(idx->counts(), total, with_full);
    FI_CHECK(total == 2 && with_full == 0, "counts (2,0)");

    DigestUpdate u = upd(11, 100, 4096, 111, 1, 2);
    u.digests.has_head = 1;
    u.digests.head = dig(1);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u, 1), 1000) == 1, "set digests 1");
    set_counts(idx->counts(), total, with_full);
    FI_CHECK(total == 2 && with_full == 1, "counts (2,1)");

    /* size 对不上：拒绝写回。 */
    DigestUpdate bad = upd(11, 101, 4097, 222, 1, 3);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&bad, 1), 1000) == 0, "reject size mismatch");
    set_counts(idx->counts(), total, with_full);
    FI_CHECK(total == 2 && with_full == 1, "counts still (2,1)");

    /* 改名（同 frn、同 size/mtime）：摘要保留。 */
    IndexRow renamed = irow(11, 100, "C:\\a-renamed.bin", 4096, 111);
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(&renamed, 1)) == 1, "upsert rename");
    size_t n = 0;
    std::vector<IndexHashRow> hr = idx->rows_needing_full(11);
    n = hr.size();
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

    /* 内容变了（size 变化）：摘要清掉。 */
    IndexRow changed = irow(11, 100, "C:\\a-renamed.bin", 8192, 333);
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(&changed, 1)) == 1, "upsert changed");
    set_counts(idx->counts(), total, with_full);
    FI_CHECK(total == 2 && with_full == 0, "counts (2,0) after change");

    /* USN 状态读写。 */
    uint64_t j = 0;
    int64_t usn = 0;
    FI_CHECK(get_usn(*idx, 11, j, usn) == 0, "usn initially missing");
    FI_CHECK(idx->set_usn_state(11, 77, 12345) == 0, "set usn");
    FI_CHECK(get_usn(*idx, 11, j, usn) == 1 && j == 77 && usn == 12345,
             "get usn (77,12345)");
    FI_CHECK(idx->set_usn_state(11, 77, 12399) == 0, "set usn again");
    FI_CHECK(get_usn(*idx, 11, j, usn) == 1 && usn == 12399,
             "get usn updated");
}

void test_delete_and_prefix() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, nullptr);
    FI_CHECK(idx != nullptr, "open db");
    if (!idx) return;
    /* 中文目录名：LENGTH() 按字符算，前缀改写不能按字节切。 */
    IndexRow rows[] = {
        irow(11, 100, "C:\\旧目录\\a.bin", 4096, 111),
        irow(11, 101, "C:\\旧目录\\子目录\\b.bin", 4096, 222),
        irow(11, 102, "C:\\旧目录2\\c.bin", 4096, 333),
        irow(11, 103, "C:\\其他\\d.bin", 4096, 444),
    };
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 4)) == 4, "upsert 4 chinese paths");
    uint64_t frns[] = {101, 999};
    FI_CHECK(idx->delete_rows(11, frns) == 1, "delete 1 of 2");
    uint64_t total = 0, with_full = 0;
    set_counts(idx->counts(), total, with_full);
    FI_CHECK(total == 3, "counts 3 after delete");
    FI_CHECK(idx->delete_rows(11, {}) == 0, "delete empty");
    size_t changed = idx->update_path_prefix(11, "C:\\旧目录", "C:\\新目录");
    FI_CHECK(changed == 1, "prefix rewrite 1 row, got %zu", changed);
    /* 校验改写结果：100 换了前缀，102（旧目录2）与 103 不动。 */
    size_t n = 0;
    /* 用 rows_needing_full 把全部行读回：三行同 size 4096 且都缺 full，整组返回。 */
    std::vector<IndexHashRow> hr = idx->rows_needing_full(11);
    n = hr.size();
    FI_CHECK(n == 3, "needing_full 3 rows, got %zu", n);
    int saw_new = 0, saw_old2 = 0, saw_other = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(hr[i].path.c_str(), "C:\\新目录\\a.bin") == 0) saw_new = 1;
        if (strcmp(hr[i].path.c_str(), "C:\\旧目录2\\c.bin") == 0) saw_old2 = 1;
        if (strcmp(hr[i].path.c_str(), "C:\\其他\\d.bin") == 0) saw_other = 1;
    }
    FI_CHECK(saw_new && saw_old2 && saw_other, "prefix rewrite paths exact");
}

void test_needing_full_groups() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, nullptr);
    FI_CHECK(idx != nullptr, "open db");
    if (!idx) return;
    IndexRow rows[] = {
        irow(11, 100, "C:\\a.bin", 4096, 111), irow(11, 101, "C:\\b.bin", 4096, 222),
        irow(11, 102, "C:\\c.bin", 8192, 333), irow(11, 103, "C:\\d.bin", 8192, 444),
        irow(11, 104, "C:\\e.bin", 2048, 555),
    };
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 5)) == 5, "upsert 5");
    DigestUpdate u1 = upd(11, 100, 4096, 111, 1, 9);
    DigestUpdate u2 = upd(11, 102, 8192, 333, 1, 9);
    DigestUpdate u3 = upd(11, 103, 8192, 444, 1, 9);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u1, 1), 1000) == 1, "digest 100");
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u2, 1), 1000) == 1, "digest 102");
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u3, 1), 1000) == 1, "digest 103");
    size_t n = 0;
    std::vector<IndexHashRow> hr = idx->rows_needing_full(11);
    n = hr.size();
    FI_CHECK(n == 2, "needing_full 2 rows, got %zu", n);
    int all4096 = 1, has100 = 0, has101 = 0;
    for (size_t i = 0; i < n; i++) {
        if (hr[i].size != 4096) all4096 = 0;
        if (hr[i].frn == 100 && hr[i].digests.has_full) has100 = 1;
        if (hr[i].frn == 101 && !hr[i].digests.has_full) has101 = 1;
    }
    FI_CHECK(all4096 && has100 && has101, "needing_full group content");
    DigestUpdate u4 = upd(11, 101, 4096, 222, 1, 8);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u4, 1), 1000) == 1, "digest 101");
    hr = idx->rows_needing_full(11);
    n = hr.size();
    FI_CHECK(n == 0, "needing_full empty after completion");
}

void test_all_digests() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, nullptr);
    FI_CHECK(idx != nullptr, "open db");
    if (!idx) return;
    IndexRow rows[] = {
        irow(11, 100, "C:\\a.bin", 4096, 111),
        irow(11, 101, "C:\\b.bin", 4096, 222),
        irow(11, 102, "C:\\c.bin", 4096, 333),
        irow(22, 200, "D:\\a.bin", 4096, 111),
    };
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 4)) == 4, "upsert 4");
    /* 100：full + head；101：只有 sample；102：无摘要；200（另一卷）：full。 */
    DigestUpdate u1 = upd(11, 100, 4096, 111, 1, 2);
    u1.digests.has_head = 1;
    u1.digests.head = dig(1);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u1, 1), 1000) == 1, "digest 100");
    DigestUpdate u2;
    u2.volume_serial = 11;
    u2.frn = 101;
    u2.size = 4096;
    u2.mtime = 222;
    u2.digests.has_sample = 1;
    u2.digests.sample = dig(5);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u2, 1), 1000) == 1, "digest 101 sample");
    DigestUpdate u3 = upd(22, 200, 4096, 111, 1, 7);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u3, 1), 1000) == 1, "digest 200");
    size_t n = 0;
    std::vector<IndexDigestEntry> all = idx->all_digests(11);
    n = all.size();
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
    /* 改名（同 frn、同 size/mtime）：摘要仍在。 */
    IndexRow renamed = irow(11, 100, "C:\\a-renamed.bin", 4096, 111);
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(&renamed, 1)) == 1, "upsert rename 100");
    all = idx->all_digests(11);
    n = all.size();
    FI_CHECK(n == 2 && all[0].frn == 100 && all[0].digests.has_full,
             "all_digests rename keeps full");
    /* 内容变了（size 变化）：摘要清掉，不再返回。 */
    IndexRow changed = irow(11, 101, "C:\\b.bin", 8192, 222);
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(&changed, 1)) == 1, "upsert changed 101");
    all = idx->all_digests(11);
    n = all.size();
    FI_CHECK(n == 1 && all[0].frn == 100, "all_digests changed row dropped");
    /* 另一卷 / 空卷隔离。 */
    all = idx->all_digests(22);
    n = all.size();
    FI_CHECK(n == 1 && all[0].frn == 200 && all[0].digests.has_full,
             "all_digests vol 22 separate");
    all = idx->all_digests(99);
    n = all.size();
    FI_CHECK(n == 0, "all_digests empty volume");
}

void test_delete_volume() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, nullptr);
    FI_CHECK(idx != nullptr, "open db");
    if (!idx) return;
    IndexRow rows[] = {irow(11, 100, "C:\\a.bin", 4096, 111),
                              irow(22, 200, "D:\\a.bin", 4096, 111)};
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 2)) == 2, "upsert 2 vols");
    FI_CHECK(idx->set_usn_state(11, 77, 100) == 0, "set usn 11");
    FI_CHECK(idx->set_usn_state(22, 88, 200) == 0, "set usn 22");
    idx->delete_volume(11);
    uint64_t total = 0, with_full = 0;
    set_counts(idx->counts(), total, with_full);
    FI_CHECK(total == 1, "counts 1 after delete_volume");
    uint64_t j = 0;
    int64_t u = 0;
    FI_CHECK(get_usn(*idx, 11, j, u) == 0, "usn 11 gone");
    FI_CHECK(get_usn(*idx, 22, j, u) == 1 && j == 88 && u == 200,
             "usn 22 kept");
}

void test_query_dup_rows() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, nullptr);
    FI_CHECK(idx != nullptr, "open db");
    if (!idx) return;
    IndexRow rows[] = {
        irow(11, 100, "C:\\a.bin", 4096, 111), irow(11, 101, "C:\\b.bin", 4096, 222),
        irow(22, 200, "D:\\c.bin", 4096, 333), irow(11, 102, "C:\\d.bin", 8192, 444),
        irow(11, 103, "C:\\e.bin", 2048, 555), irow(11, 104, "C:\\f.bin", 2048, 666),
    };
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 6)) == 6, "upsert 6");
    DigestUpdate u = upd(11, 100, 4096, 111, 1, 9);
    u.digests.has_strong = 1;
    u.digests.strong = dig(5);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u, 1), 1000) == 1, "digest 100");
    size_t n = 0;
    uint64_t serials[] = {11, 22};
    std::vector<IndexDupRow> dr = idx->query_dup_rows(std::span<const uint64_t>(serials, 2), 4096);
    n = dr.size();
    FI_CHECK(n == 3, "dup rows 3, got %zu", n);
    int all4096 = 1, has100 = 0, has101 = 0, has200 = 0;
    for (size_t i = 0; i < n; i++) {
        if (dr[i].size != 4096) all4096 = 0;
        if (dr[i].frn == 100 && dr[i].digests.has_full &&
            memcmp(dr[i].digests.full.bytes, dig(9).bytes, kDigestLen) == 0 &&
            dr[i].digests.has_strong)
            has100 = 1;
        if (dr[i].frn == 101 && !dr[i].digests.has_full) has101 = 1;
        if (dr[i].frn == 200) has200 = 1;
    }
    FI_CHECK(all4096 && has100 && has101 && has200, "dup rows content");
    dr = idx->query_dup_rows(std::span<const uint64_t>(serials, 1), 4096);
    n = dr.size();
    FI_CHECK(n == 2, "dup rows C only 2, got %zu", n);
    dr = idx->query_dup_rows(std::span<const uint64_t>(serials, 2), 8192);
    n = dr.size();
    FI_CHECK(n == 0, "dup rows min 8192 empty");
    dr = idx->query_dup_rows({}, 4096);
    n = dr.size();
    FI_CHECK(dr.empty() && n == 0, "dup rows empty serials");
}

void test_query_big_rows() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, nullptr);
    FI_CHECK(idx != nullptr, "open db");
    if (!idx) return;
    const uint64_t MB = (uint64_t)1 << 20;
    IndexRow rows[] = {
        irow(11, 100, "C:\\small.bin", 1 * MB, 111),
        irow(11, 101, "C:\\mid.bin", 3 * MB, 222),
        irow(11, 102, "C:\\big.bin", 100 * MB, 333),
        irow(22, 200, "D:\\other.bin", 50 * MB, 444),
    };
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 4)) == 4, "upsert 4");
    size_t n = 0;
    uint64_t serials[] = {11, 22};
    std::vector<IndexBigRow> br = idx->query_big_rows(serials);
    n = br.size();
    FI_CHECK(n == 3, "big rows 3, got %zu", n);
    FI_CHECK(n == 3 && br[0].size == 100 * MB && br[1].size == 50 * MB &&
                 br[2].size == 3 * MB,
             "big rows desc order");
    FI_CHECK(n == 3 && strcmp(br[0].path.c_str(), "C:\\big.bin") == 0, "big rows first path");
    br = idx->query_big_rows(std::span<const uint64_t>(serials + 1, 1));
    n = br.size();
    FI_CHECK(n == 1, "big rows D only 1");
    br = idx->query_big_rows({});
    n = br.size();
    FI_CHECK(br.empty() && n == 0, "big rows empty serials");
}

void test_counts_and_updated_at() {
    char db[4096];
    if (fi_tmp_db(db, sizeof(db)) != 0) {
        FI_CHECK(0, "tmp db path");
        return;
    }
    std::unique_ptr<FileIndex> idx = FileIndex::open(db, nullptr);
    FI_CHECK(idx != nullptr, "open db");
    if (!idx) return;
    uint64_t total = 0, with_full = 0;
    set_counts(idx->volume_counts(11), total, with_full);
    FI_CHECK(total == 0 && with_full == 0, "volume counts (0,0)");
    uint64_t at = 0;
    FI_CHECK(!idx->usn_state_updated_at(11).has_value(), "updated_at missing");
    IndexRow rows[] = {irow(11, 100, "C:\\a.bin", 4096, 111),
                              irow(11, 101, "C:\\b.bin", 4096, 222)};
    FI_CHECK(idx->upsert_files(std::span<const IndexRow>(rows, 2)) == 2, "upsert 2");
    DigestUpdate u = upd(11, 100, 4096, 111, 1, 9);
    FI_CHECK(idx->set_digests_batch(std::span<const DigestUpdate>(&u, 1), 1000) == 1, "digest 100");
    set_counts(idx->volume_counts(11), total, with_full);
    FI_CHECK(total == 2 && with_full == 1, "volume counts (2,1)");
    set_counts(idx->volume_counts(22), total, with_full);
    FI_CHECK(total == 0 && with_full == 0, "volume counts other (0,0)");
    FI_CHECK(idx->set_usn_state(11, 77, 12345) == 0, "set usn");
    at = idx->usn_state_updated_at(11).value_or(0);
    FI_CHECK(at > 0,
             "updated_at present");
}

}  // namespace

int fileindex_selftest() {
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
    std::printf("fileindex selftest: %d checks, %d failures\n", fi_checks, fi_failures);
    return fi_failures;
}

}  // namespace cdc
