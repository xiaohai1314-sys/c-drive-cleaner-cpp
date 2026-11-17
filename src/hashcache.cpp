// hashcache.cpp — SQLite 哈希缓存，逐行对照 desktop/src/hashcache.rs。
//
// 与 Rust 版一致的点：
//   - 默认库 cache_dir()/hash-cache-v3.db；表结构与 PRAGMA 逐字一致；
//   - load 只预加载给定路径（不全量读库）；entry_for 要求 (size, mtime)
//     精确匹配；remember 的 size/mtime 变化清摘要规则；
//   - save_dirty 只写脏条目，upsert 的 COALESCE 语义逐字一致，
//     同事务删 last_seen < now-30 天的行；
//   - 旧版 hash-cache-v2.json 迁移（仅表为空时；head 恒为 NULL）。
//
// 有意差别：
//   - 内存条目是按 path 排序的 vector + 二分查找（Rust 是 HashMap），
//     行为一致；dirty 记在条目上，不另建集合。
//   - 打开失败也返回可用的空缓存（与 Rust load_for_paths_at 一致），
//     save_dirty 时再重试打开。
//   - 采样层摘要（XXH3-64）存在 head_xxh / sample_xxh / strong_xxh
//     三列（INTEGER，按位存 uint64），不进 BLAKE3 的 BLOB 列；旧库
//     （Rust 或旧版 C 建的 7 列表）打开时 ALTER TABLE 补列。
//   - v2 JSON 用 fopen_utf8 读（C 版是窄 fopen：Windows 上缓存目录
//     含 "C盘清理工具"，窄路径打不开，迁移会静默跳过）。
#include "hashcache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "mft.h"  // now_seconds
#include "raii.h"
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

namespace cdc {
namespace {

#ifdef _WIN32
constexpr char kPathSep = '\\';
#else
constexpr char kPathSep = '/';
#endif

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool column_digest(sqlite3_stmt *st, int col, Digest &out) {
    const void *blob = sqlite3_column_blob(st, col);
    if (!blob || sqlite3_column_bytes(st, col) != 32) return false;
    std::memcpy(out.bytes, blob, 32);
    return true;
}

bool column_u64(sqlite3_stmt *st, int col, uint64_t &out) {
    if (sqlite3_column_type(st, col) == SQLITE_NULL) return false;
    out = static_cast<uint64_t>(sqlite3_column_int64(st, col));
    return true;
}

uint64_t column_nonneg(sqlite3_stmt *st, int col) {
    sqlite3_int64 v = sqlite3_column_int64(st, col);
    return v > 0 ? static_cast<uint64_t>(v) : 0;
}

void bind_digest(sqlite3_stmt *st, int col, bool has, const Digest &d) {
    if (has)
        sqlite3_bind_blob(st, col, d.bytes, 32, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(st, col);
}

void bind_u64(sqlite3_stmt *st, int col, bool has, uint64_t v) {
    if (has)
        sqlite3_bind_int64(st, col, static_cast<sqlite3_int64>(v));
    else
        sqlite3_bind_null(st, col);
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

std::optional<std::string> read_file_all(const std::string &path) {
    UniqueFile f = fopen_utf8(path.c_str(), "rb");
    if (!f) return std::nullopt;
    std::string out;
    char buf[64 * 1024];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f.get())) > 0) out.append(buf, got);
    if (std::ferror(f.get())) return std::nullopt;
    return out;
}

// 逐级建父目录（与 cache_dir 同一做法）。
void ensure_parent_dir(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] != '/' && path[i] != '\\') continue;
        std::string prefix = path.substr(0, i);
#ifdef _WIN32
        if (auto w = utf8_to_wide(prefix)) CreateDirectoryW(w->c_str(), nullptr);
#else
        mkdir(prefix.c_str(), 0755);
#endif
    }
}

UniqueSqlite open_conn(const std::string &db_path) {
    ensure_parent_dir(db_path);
    sqlite3 *raw = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &raw);  // sqlite3_open 的路径是 UTF-8
    UniqueSqlite db(raw);
    if (rc != SQLITE_OK) return nullptr;
    static const char *const kSchema =
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
    if (!exec(db.get(), kSchema)) return nullptr;
    // 旧库补列：表可能是 Rust 版或旧版 C 建的（只有 7 列），
    // CREATE TABLE IF NOT EXISTS 不会补新列，这里按需 ALTER。
    for (const char *col : {"head_xxh", "sample_xxh", "strong_xxh"}) {
        std::string q = format(
            "SELECT COUNT(*) FROM pragma_table_info('file_hashes') WHERE name = '%s'", col);
        bool has = false;
        if (UniqueStmt ck = prepare(db.get(), q.c_str())) {
            if (sqlite3_step(ck.get()) == SQLITE_ROW) has = sqlite3_column_int(ck.get(), 0) > 0;
        }
        if (!has) {
            std::string alter = format("ALTER TABLE file_hashes ADD COLUMN %s INTEGER", col);
            exec(db.get(), alter.c_str());  // 失败忽略：下次打开再试
        }
    }
    return db;
}

// 旧版 v2 JSON 迁移（仅表为空时）。无 cJSON 编译时直接跳过。
void maybe_migrate_legacy(sqlite3 *db) {
    long count = 1;
    if (UniqueStmt st = prepare(db, "SELECT COUNT(*) FROM file_hashes")) {
        if (sqlite3_step(st.get()) == SQLITE_ROW)
            count = static_cast<long>(sqlite3_column_int64(st.get(), 0));
    }
    if (count > 0) return;
    std::optional<std::string> dir = cache_dir();
    if (!dir) return;
    std::optional<std::string> text = read_file_all(*dir + kPathSep + "hash-cache-v2.json");
    if (!text) return;
#ifdef CDC_HAVE_CJSON
    UniqueJson root(cJSON_Parse(text->c_str()));
    if (!root) return;
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root.get(), "entries");
    if (!cJSON_IsObject(entries)) return;
    auto num = [](const cJSON *parent, const char *key) -> sqlite3_int64 {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(parent, key);
        return cJSON_IsNumber(j) ? static_cast<sqlite3_int64>(static_cast<uint64_t>(j->valuedouble))
                                 : 0;
    };
    auto hex = [](const cJSON *parent, const char *key, Digest &out) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(parent, key);
        return cJSON_IsString(j) && j->valuestring && hex_to_digest(j->valuestring, out);
    };
    exec(db, "BEGIN IMMEDIATE");
    if (UniqueStmt ins = prepare(db,
                                 "INSERT OR IGNORE INTO file_hashes"
                                 " (path, size, mtime, head, sample, strong, full, last_seen)"
                                 " VALUES (?1, ?2, ?3, NULL, ?4, ?5, ?6, ?7)")) {
        sqlite3_stmt *st = ins.get();
        for (const cJSON *child = entries->child; child; child = child->next) {
            if (!child->string) continue;
            Digest d_sample{}, d_strong{}, d_full{};
            bool has_sample = hex(child, "sample", d_sample);
            bool has_strong = hex(child, "strong", d_strong);
            bool has_full = hex(child, "full", d_full);
            if (!has_sample && !has_strong && !has_full) continue;
            sqlite3_bind_text(st, 1, child->string, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, num(child, "size"));
            sqlite3_bind_int64(st, 3, num(child, "mtime"));
            bind_digest(st, 4, has_sample, d_sample);
            bind_digest(st, 5, has_strong, d_strong);
            bind_digest(st, 6, has_full, d_full);
            sqlite3_bind_int64(st, 7, num(child, "last_seen"));
            sqlite3_step(st);
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
        }
    }
    exec(db, "COMMIT");
#endif
}

// 把 src 里 has_* 为真的档位并入 dst；返回是否有变化。
bool merge_digests(DigestSet &dst, const DigestSet &src) {
    bool changed = false;
    auto merge32 = [&](bool &dst_has, Digest &dst_d, bool src_has, const Digest &src_d) {
        if (src_has && (!dst_has || std::memcmp(dst_d.bytes, src_d.bytes, 32) != 0)) {
            dst_has = true;
            dst_d = src_d;
            changed = true;
        }
    };
    auto merge64 = [&](bool &dst_has, uint64_t &dst_v, bool src_has, uint64_t src_v) {
        if (src_has && (!dst_has || dst_v != src_v)) {
            dst_has = true;
            dst_v = src_v;
            changed = true;
        }
    };
    merge32(dst.has_head, dst.head, src.has_head, src.head);
    merge32(dst.has_sample, dst.sample, src.has_sample, src.sample);
    merge32(dst.has_strong, dst.strong, src.has_strong, src.strong);
    merge32(dst.has_full, dst.full, src.has_full, src.full);
    merge64(dst.has_head_xxh, dst.head_xxh, src.has_head_xxh, src.head_xxh);
    merge64(dst.has_sample_xxh, dst.sample_xxh, src.has_sample_xxh, src.sample_xxh);
    merge64(dst.has_strong_xxh, dst.strong_xxh, src.has_strong_xxh, src.strong_xxh);
    return changed;
}

}  // namespace

// ---------------- hex ----------------

void digest_to_hex(const Digest &d, char out[65]) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = kHex[(d.bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[d.bytes[i] & 0x0F];
    }
    out[64] = '\0';
}

std::string digest_to_hex(const Digest &d) {
    char buf[65];
    digest_to_hex(d, buf);
    return std::string(buf, 64);
}

bool hex_to_digest(std::string_view hex, Digest &out) {
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        int hi = hex_nibble(hex[i * 2]), lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out.bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// ---------------- 打开 / 加载 ----------------

std::optional<std::string> hashcache_default_path() {
    std::optional<std::string> dir = cache_dir();
    if (!dir) return std::nullopt;
    return *dir + kPathSep + "hash-cache-v3.db";
}

bool HashCache::find(std::string_view path, size_t &pos) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), path,
                               [](const Entry &e, std::string_view p) { return e.path < p; });
    pos = static_cast<size_t>(it - entries_.begin());
    return it != entries_.end() && it->path == path;
}

std::unique_ptr<HashCache> HashCache::load_at(const char *db_path,
                                              std::vector<std::string_view> paths,
                                              bool migrate_legacy) {
    std::unique_ptr<HashCache> c(new HashCache());
    if (!db_path) return c;  // 纯内存态
    c->db_path_ = db_path;
    UniqueSqlite db = open_conn(*c->db_path_);
    if (!db) return c;  // 打不开也返回空缓存
    if (migrate_legacy) maybe_migrate_legacy(db.get());
    // 输入路径先排序去重：entries_ 按 path 有序，排好序后每次插入都在末尾
    // 追加（C 版未排序时 50k 命中是 O(n²)，约 5.9s）。只动副本，不改调用方顺序。
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    UniqueStmt stmt = prepare(db.get(),
                              "SELECT size, mtime, head, sample, strong, full,"
                              " head_xxh, sample_xxh, strong_xxh, last_seen"
                              " FROM file_hashes WHERE path = ?1");
    if (!stmt) return c;
    sqlite3_stmt *st = stmt.get();
    c->entries_.reserve(paths.size());
    for (std::string_view path : paths) {
        sqlite3_bind_text(st, 1, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            Entry e;
            e.path.assign(path);
            e.size = column_nonneg(st, 0);
            e.mtime = column_nonneg(st, 1);
            e.last_seen = column_nonneg(st, 9);
            DigestSet &d = e.digests;
            d.has_head = column_digest(st, 2, d.head);
            d.has_sample = column_digest(st, 3, d.sample);
            d.has_strong = column_digest(st, 4, d.strong);
            d.has_full = column_digest(st, 5, d.full);
            d.has_head_xxh = column_u64(st, 6, d.head_xxh);
            d.has_sample_xxh = column_u64(st, 7, d.sample_xxh);
            d.has_strong_xxh = column_u64(st, 8, d.strong_xxh);
            c->entries_.push_back(std::move(e));  // 有序输入 => 追加即有序
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
    }
    return c;
}

std::unique_ptr<HashCache> HashCache::load_for_paths(std::span<const char *const> paths) {
    std::vector<std::string_view> v;
    v.reserve(paths.size());
    for (const char *p : paths)
        if (p) v.emplace_back(p);
    std::optional<std::string> db = hashcache_default_path();
    if (!db) return load_at(nullptr, {}, false);  // 默认路径都取不到：纯内存态
    return load_at(db->c_str(), std::move(v), true);
}

std::unique_ptr<HashCache> HashCache::load_for_paths(std::span<const std::string> paths) {
    std::vector<std::string_view> v(paths.begin(), paths.end());
    std::optional<std::string> db = hashcache_default_path();
    if (!db) return load_at(nullptr, {}, false);
    return load_at(db->c_str(), std::move(v), true);
}

std::unique_ptr<HashCache> HashCache::open_at(const char *db_path, bool migrate_legacy) {
    return load_at(db_path, {}, migrate_legacy);
}

HashCache::~HashCache() { save_dirty(); }

std::optional<DigestSet> HashCache::entry_for(std::string_view path, uint64_t size,
                                              uint64_t mtime) const {
    size_t pos = 0;
    if (!find(path, pos)) return std::nullopt;
    const Entry &e = entries_[pos];
    if (e.size != size || e.mtime != mtime) return std::nullopt;
    return e.digests;
}

void HashCache::remember(std::string_view path, uint64_t size, uint64_t mtime,
                         const DigestSet &digests, uint64_t now) {
    size_t pos = 0;
    bool changed = false;
    if (!find(path, pos)) {
        Entry ne;
        ne.path.assign(path);
        ne.size = size;
        ne.mtime = mtime;
        ne.last_seen = now;
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(pos), std::move(ne));
        changed = true;
    } else if (entries_[pos].size != size || entries_[pos].mtime != mtime) {
        entries_[pos].size = size;
        entries_[pos].mtime = mtime;
        entries_[pos].digests = DigestSet{};
        changed = true;
    }
    Entry &e = entries_[pos];
    if (merge_digests(e.digests, digests)) changed = true;
    if (changed) {
        e.last_seen = now;
        e.dirty = true;
    }
}

int HashCache::save_dirty() {
    if (std::none_of(entries_.begin(), entries_.end(), [](const Entry &e) { return e.dirty; }))
        return 0;
    if (!db_path_) {
        for (Entry &e : entries_) e.dirty = false;
        return 0;
    }
    UniqueSqlite db = open_conn(*db_path_);
    if (!db) return -1;
    uint64_t now = now_seconds();
    constexpr uint64_t kThirtyDays = 30ULL * 86400;
    uint64_t cutoff = now >= kThirtyDays ? now - kThirtyDays : 0;
    if (!exec(db.get(), "BEGIN IMMEDIATE")) return -1;
    static const char *const kUpsert =
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
    bool ok = false;
    if (UniqueStmt stmt = prepare(db.get(), kUpsert)) {
        sqlite3_stmt *st = stmt.get();
        ok = true;
        for (const Entry &e : entries_) {
            if (!e.dirty) continue;
            const DigestSet &d = e.digests;
            sqlite3_bind_text(st, 1, e.path.data(), static_cast<int>(e.path.size()),
                              SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(e.size));
            sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(e.mtime));
            bind_digest(st, 4, d.has_head, d.head);
            bind_digest(st, 5, d.has_sample, d.sample);
            bind_digest(st, 6, d.has_strong, d.strong);
            bind_digest(st, 7, d.has_full, d.full);
            bind_u64(st, 8, d.has_head_xxh, d.head_xxh);
            bind_u64(st, 9, d.has_sample_xxh, d.sample_xxh);
            bind_u64(st, 10, d.has_strong_xxh, d.strong_xxh);
            sqlite3_bind_int64(st, 11, static_cast<sqlite3_int64>(e.last_seen));
            if (sqlite3_step(st) != SQLITE_DONE) ok = false;
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
        }
    }
    if (ok) {
        if (UniqueStmt del = prepare(db.get(), "DELETE FROM file_hashes WHERE last_seen < ?1")) {
            sqlite3_bind_int64(del.get(), 1, static_cast<sqlite3_int64>(cutoff));
            sqlite3_step(del.get());
        }
        ok = exec(db.get(), "COMMIT");
    }
    if (!ok) {
        exec(db.get(), "ROLLBACK");
        return -1;
    }
    for (Entry &e : entries_) e.dirty = false;
    return 0;
}

}  // namespace cdc
