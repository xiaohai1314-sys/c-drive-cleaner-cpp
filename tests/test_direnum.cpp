/* test_direnum.c — src/util.{c,h} 的目录枚举（cdc_enum_dir）自测。
 *
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Isrc -pthread \
 *       -o build/test_direnum tests/test_direnum.c src/util.c
 *   ./build/test_direnum    # 退出码 0=全过
 *
 * 覆盖：
 *  - POSIX 枚举与 opendir/readdir + lstat 逐项等价（同一棵树：子目录、
 *    空目录、空文件、中文名、$ 开头名、文件/目录符号链接、悬空链接）；
 *  - 缺失目录 -> NOT_FOUND、空目录 -> OK 零项、NULL 参数 -> INVALID_ARG；
 *  - FILE_DIRECTORY_INFORMATION 批量解析（合成缓冲）：多项链、"." ".."
 *    跳过、目录项 size 归零、代理对 / 孤立代理对、截断缓冲报错、
 *    NextEntryOffset 越界报错、回调提前停止。
 *
 * 注意：NtQueryDirectoryFile 的 Windows 真路径本机无 Windows 环境，
 * 只能交叉编译验证；这里的解析器测试覆盖它最容易出错的部分
 * （链式 NextEntryOffset 遍历与 UTF-16LE 名解码）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

#include "util.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ---------------- 小工具 ---------------- */

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_u64le(uint8_t *p, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* 收集到的枚举项（路径相对，排序后比较）。 */
typedef struct {
    char *name; /* 相对枚举根的路径（a/b/c 形），malloc'd */
    int is_dir;
    int is_symlink;
    uint64_t size;
    uint64_t mtime;
    uint32_t attrs;
} Seen;

typedef struct {
    Seen *items;
    size_t n, cap;
} SeenList;

static int seen_push(SeenList *l, const char *name, int is_dir, int is_symlink,
                     uint64_t size, uint64_t mtime, uint32_t attrs) {
    Seen s;
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        Seen *p = (Seen *)realloc(l->items, ncap * sizeof *p);
        if (!p) return -1;
        l->items = p;
        l->cap = ncap;
    }
    s.name = strdup(name);
    if (!s.name) return -1;
    s.is_dir = is_dir;
    s.is_symlink = is_symlink;
    s.size = size;
    s.mtime = mtime;
    s.attrs = attrs;
    l->items[l->n++] = s;
    return 0;
}

static void seen_free(SeenList *l) {
    size_t i;
    for (i = 0; i < l->n; i++) free(l->items[i].name);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static int seen_cmp(const void *a, const void *b) {
    return strcmp(((const Seen *)a)->name, ((const Seen *)b)->name);
}

static void seen_sort(SeenList *l) {
    if (l->n > 1) qsort(l->items, l->n, sizeof(Seen), seen_cmp);
}

static int seen_equal(const SeenList *a, const SeenList *b) {
    size_t i;
    if (a->n != b->n) return 0;
    for (i = 0; i < a->n; i++) {
        const Seen *x = &a->items[i], *y = &b->items[i];
        if (strcmp(x->name, y->name) != 0) return 0;
        if (x->is_dir != y->is_dir || x->is_symlink != y->is_symlink ||
            x->size != y->size || x->mtime != y->mtime || x->attrs != y->attrs)
            return 0;
    }
    return 1;
}

/* ---------------- 参照枚举（opendir/readdir + lstat，与旧代码同语义） ---------------- */

static int ref_walk(const char *dir, const char *rel, SeenList *out) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char *full, *rpath;
        struct stat st;
        size_t dl, nl;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        dl = strlen(dir);
        nl = strlen(e->d_name);
        full = (char *)malloc(dl + 1 + nl + 1);
        rpath = (char *)malloc(strlen(rel) + 1 + nl + 1);
        if (!full || !rpath) {
            free(full);
            free(rpath);
            closedir(d);
            return -1;
        }
        memcpy(full, dir, dl);
        full[dl] = '/';
        memcpy(full + dl + 1, e->d_name, nl + 1);
        if (rel[0] == '\0') {
            memcpy(rpath, e->d_name, nl + 1);
        } else {
            size_t rl = strlen(rel);
            memcpy(rpath, rel, rl);
            rpath[rl] = '/';
            memcpy(rpath + rl + 1, e->d_name, nl + 1);
        }
        if (lstat(full, &st) != 0) {
            free(full);
            free(rpath);
            continue;
        }
        if (seen_push(out, rpath, S_ISDIR(st.st_mode) ? 1 : 0,
                      S_ISLNK(st.st_mode) ? 1 : 0,
                      S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0,
                      (uint64_t)st.st_mtime, (uint32_t)st.st_mode) != 0) {
            free(full);
            free(rpath);
            closedir(d);
            return -1;
        }
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            int rc = ref_walk(full, rpath, out);
            free(full);
            free(rpath);
            if (rc != 0) {
                closedir(d);
                return rc;
            }
        } else {
            free(full);
            free(rpath);
        }
    }
    closedir(d);
    return 0;
}

/* 用 cdc_enum_dir 做同样的递归收集。 */
typedef struct {
    SeenList *out;
    const char *dir; /* 当前目录（绝对路径） */
    const char *rel; /* 当前相对路径 */
    int failed;
} EnumCtx;

static int enum_cb(const char *name, int is_dir, int is_symlink, uint64_t size,
                   uint64_t mtime, uint32_t attrs, void *ctx) {
    EnumCtx *c = (EnumCtx *)ctx;
    char *rpath;
    size_t rl = strlen(c->rel), nl = strlen(name);
    if (rl == 0) {
        rpath = strdup(name);
    } else {
        rpath = (char *)malloc(rl + 1 + nl + 1);
        if (rpath) {
            memcpy(rpath, c->rel, rl);
            rpath[rl] = '/';
            memcpy(rpath + rl + 1, name, nl + 1);
        }
    }
    if (!rpath) {
        c->failed = 1;
        return -1;
    }
    if (seen_push(c->out, rpath, is_dir, is_symlink, size, mtime, attrs) != 0) {
        free(rpath);
        c->failed = 1;
        return -1;
    }
    free(rpath);
    if (is_dir && !is_symlink) {
        char *full;
        size_t dl = strlen(c->dir);
        EnumCtx sub;
        int rc;
        full = (char *)malloc(dl + 1 + nl + 1);
        if (!full) {
            c->failed = 1;
            return -1;
        }
        memcpy(full, c->dir, dl);
        full[dl] = '/';
        memcpy(full + dl + 1, name, nl + 1);
        {
            char *subrel;
            if (rl == 0) {
                subrel = strdup(name);
            } else {
                subrel = (char *)malloc(rl + 1 + nl + 1);
                if (subrel) {
                    memcpy(subrel, c->rel, rl);
                    subrel[rl] = '/';
                    memcpy(subrel + rl + 1, name, nl + 1);
                }
            }
            if (!subrel) {
                free(full);
                c->failed = 1;
                return -1;
            }
            sub.out = c->out;
            sub.dir = full;
            sub.rel = subrel;
            sub.failed = 0;
            rc = cdc_enum_dir(full, enum_cb, &sub);
            free(full);
            free(subrel);
            if (rc != CDC_OK || sub.failed) {
                c->failed = 1;
                return -1;
            }
        }
    }
    return 0;
}

static int enum_walk(const char *dir, const char *rel, SeenList *out) {
    EnumCtx c;
    int rc;
    c.out = out;
    c.dir = dir;
    c.rel = rel;
    c.failed = 0;
    rc = cdc_enum_dir(dir, enum_cb, &c);
    if (rc != CDC_OK || c.failed) return -1;
    return 0;
}

/* ---------------- 造测试树 ---------------- */

static int write_file(const char *path, const char *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n > 0 && fwrite(data, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void build_tree(const char *root) {
    char p[1024];
    mkdir(root, 0755);
    snprintf(p, sizeof p, "%s/a", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/a/b", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/empty", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/top.txt", root);
    (void)write_file(p, "hello", 5);
    snprintf(p, sizeof p, "%s/empty.bin", root);
    (void)write_file(p, "", 0);
    snprintf(p, sizeof p, "%s/a/mid.log", root);
    (void)write_file(p, "0123456789", 10);
    snprintf(p, sizeof p, "%s/a/b/deep.dat", root);
    (void)write_file(p, "xyz", 3);
    snprintf(p, sizeof p, "%s/$dollar.txt", root);
    (void)write_file(p, "d", 1);
    snprintf(p, sizeof p, "%s/中文名.txt", root);
    (void)write_file(p, "zh", 2);
    snprintf(p, sizeof p, "%s/with space.txt", root);
    (void)write_file(p, "sp", 2);
    /* 符号链接：指文件、指目录、悬空。 */
    snprintf(p, sizeof p, "%s/link_file", root);
    { int sr = symlink("top.txt", p); (void)sr; }
    snprintf(p, sizeof p, "%s/link_dir", root);
    { int sr = symlink("a", p); (void)sr; }
    snprintf(p, sizeof p, "%s/link_dangling", root);
    { int sr = symlink("no-such-target", p); (void)sr; }
}

static void test_posix_equivalence(void) {
    char root_tmpl[] = "/tmp/cdc-direnum-XXXXXX";
    char *root = mkdtemp(root_tmpl);
    SeenList ref, got;
    char missing[1024], afile[1024], empty[1024];
    int rc;
    CHECK(root != NULL, "mkdtemp test root");
    if (!root) return;
    build_tree(root);
    memset(&ref, 0, sizeof ref);
    memset(&got, 0, sizeof got);
    rc = ref_walk(root, "", &ref);
    CHECK(rc == 0, "ref walk rc==0");
    rc = enum_walk(root, "", &got);
    CHECK(rc == 0, "enum walk rc==0 (got %d)", rc);
    seen_sort(&ref);
    seen_sort(&got);
    CHECK(ref.n == 13, "ref sees 13 entries (got %zu)", ref.n);
    CHECK(got.n == 13, "enum sees 13 entries (got %zu)", got.n);
    CHECK(seen_equal(&ref, &got), "enum == ref (names/dirs/links/sizes/mtimes)");
    seen_free(&ref);
    seen_free(&got);

    /* 空目录：OK，零项。 */
    {
        SeenList e2;
        EnumCtx c2;
        snprintf(empty, sizeof empty, "%s/empty", root);
        memset(&e2, 0, sizeof e2);
        c2.out = &e2;
        c2.dir = empty;
        c2.rel = "";
        c2.failed = 0;
        rc = cdc_enum_dir(empty, enum_cb, &c2);
        CHECK(rc == CDC_OK, "empty dir -> OK (got %d)", rc);
        CHECK(e2.n == 0, "empty dir -> 0 entries (got %zu)", e2.n);
        seen_free(&e2);
    }

    /* 缺失目录：NOT_FOUND（回调不会被调，ctx 传 NULL 也行）。 */
    snprintf(missing, sizeof missing, "%s/no-such-dir", root);
    rc = cdc_enum_dir(missing, enum_cb, NULL);
    CHECK(rc == CDC_ERR_NOT_FOUND, "missing dir -> NOT_FOUND (got %d)", rc);

    /* 对文件调枚举：打不开为目录 -> 非 0。 */
    snprintf(afile, sizeof afile, "%s/top.txt", root);
    rc = cdc_enum_dir(afile, enum_cb, NULL);
    CHECK(rc != CDC_OK, "file as dir -> error (got %d)", rc);

    /* NULL 参数。 */
    CHECK(cdc_enum_dir(NULL, enum_cb, NULL) == CDC_ERR_INVALID_ARG,
          "NULL dir -> INVALID_ARG");
    CHECK(cdc_enum_dir(root, NULL, NULL) == CDC_ERR_INVALID_ARG,
          "NULL cb -> INVALID_ARG");
    {
        char cmd[1100];
        int src;
        snprintf(cmd, sizeof cmd, "rm -rf %s", root);
        src = system(cmd);
        (void)src;
    }
}

/* ---------------- FILE_DIRECTORY_INFORMATION 合成缓冲 ---------------- */

/* 在 buf 末尾追加一项；返回追加后的长度。name 为 UTF-16LE 码元数组。 */
static size_t append_entry(uint8_t *buf, size_t off, uint32_t next_off,
                           int64_t mtime_ft, int64_t size, uint32_t attrs,
                           const uint16_t *name_units, size_t n_units) {
    size_t nlen = n_units * 2;
    put_u32le(buf + off + 0, next_off);
    put_u32le(buf + off + 4, 0); /* FileIndex */
    put_u64le(buf + off + 8, 0); /* CreationTime */
    put_u64le(buf + off + 16, 0); /* LastAccessTime */
    put_u64le(buf + off + 24, (uint64_t)mtime_ft);
    put_u64le(buf + off + 32, 0); /* ChangeTime */
    put_u64le(buf + off + 40, (uint64_t)size);
    put_u64le(buf + off + 48, (uint64_t)size); /* AllocationSize */
    put_u32le(buf + off + 56, attrs);
    put_u32le(buf + off + 60, (uint32_t)nlen);
    {
        size_t i;
        for (i = 0; i < n_units; i++) {
            buf[off + 64 + i * 2] = (uint8_t)(name_units[i] & 0xFF);
            buf[off + 64 + i * 2 + 1] = (uint8_t)(name_units[i] >> 8);
        }
    }
    return off + 64 + nlen;
}

typedef struct {
    char names[8][64];
    int n;
    int stop_after; /* >=0：第几次回调返回 1 */
} ParseSeen;

static int parse_cb(const char *name, int is_dir, int is_symlink, uint64_t size,
                    uint64_t mtime, uint32_t attrs, void *ctx) {
    ParseSeen *s = (ParseSeen *)ctx;
    (void)is_symlink;
    (void)attrs;
    if (s->n < 8) {
        snprintf(s->names[s->n], sizeof s->names[s->n], "%s|%d|%llu|%llu",
                 name, is_dir, (unsigned long long)size,
                 (unsigned long long)mtime);
    }
    s->n++;
    if (s->stop_after >= 0 && s->n > s->stop_after) return 1;
    return 0;
}

static void test_parse_batch(void) {
    /* 2020-01-01 00:00:00 UTC 的 FILETIME。 */
    const int64_t ft2020 = 132223104000000000LL;
    uint8_t buf[4096];
    cdc_buf_t scratch;
    ParseSeen s;
    int rc;
    size_t len, e1, e2;
    const uint16_t n_dot[1] = { 0x002E };
    const uint16_t n_dotdot[2] = { 0x002E, 0x002E };
    const uint16_t n_a[5] = { 0x0061, 0x002E, 0x0074, 0x0078, 0x0074 }; /* a.txt */
    const uint16_t n_dir[3] = { 0x0073, 0x0075, 0x0062 }; /* sub */
    const uint16_t n_emoji[2] = { 0xD83D, 0xDE00 }; /* U+1F600 代理对 */
    const uint16_t n_lone[2] = { 0x0061, 0xD800 }; /* a + 孤立高代理 */

    cdc_buf_init(&scratch);

    /* 两项链 + "." ".." 跳过：. 、.. 、a.txt（文件 123456789）、sub（目录）。 */
    len = 0;
    e1 = append_entry(buf, len, 0, ft2020, 0, 0x10, n_dot, 1);
    len = e1;
    e1 = append_entry(buf, len, 0, ft2020, 0, 0x10, n_dotdot, 2);
    len = e1;
    /* a.txt 这一项先占位，下一项偏移随后补。 */
    e1 = len;
    len = append_entry(buf, len, 0, ft2020, 123456789, 0x20, n_a, 5);
    e2 = len;
    len = append_entry(buf, len, 0, ft2020, 0, 0x10, n_dir, 3);
    /* 回填 a.txt 的 NextEntryOffset（指向 sub 项）。 */
    put_u32le(buf + e1 + 0, (uint32_t)(e2 - e1));
    /* "." 项也需要链到 ".." 项，否则解析在第一项就停了。 */
    put_u32le(buf + 0, (uint32_t)(64 + 2 - 0));
    put_u32le(buf + (64 + 2) + 0, (uint32_t)(e1 - (64 + 2)));
    memset(&s, 0, sizeof s);
    s.stop_after = -1;
    rc = cdc_direnum_parse_batch(buf, len, parse_cb, &s, &scratch);
    CHECK(rc == CDC_OK, "parse 4 entries rc==OK (got %d)", rc);
    CHECK(s.n == 2, "parse skips . .., delivers 2 (got %d)", s.n);
    CHECK(strcmp(s.names[0], "a.txt|0|123456789|1577836800") == 0,
          "parse a.txt fields (got '%s')", s.names[0]);
    CHECK(strcmp(s.names[1], "sub|1|0|1577836800") == 0,
          "parse sub dir fields (got '%s')", s.names[1]);

    /* 代理对：U+1F600 应解成 4 字节 UTF-8。 */
    len = append_entry(buf, 0, 0, ft2020, 7, 0x20, n_emoji, 2);
    memset(&s, 0, sizeof s);
    s.stop_after = -1;
    rc = cdc_direnum_parse_batch(buf, len, parse_cb, &s, &scratch);
    CHECK(rc == CDC_OK && s.n == 1, "parse surrogate pair rc/n (got %d/%d)",
          rc, s.n);
    CHECK(strcmp(s.names[0], "\xF0\x9F\x98\x80|0|7|1577836800") == 0,
          "surrogate pair decodes to U+1F600 (got '%s')", s.names[0]);

    /* 孤立代理对 -> U+FFFD。 */
    len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_lone, 2);
    memset(&s, 0, sizeof s);
    s.stop_after = -1;
    rc = cdc_direnum_parse_batch(buf, len, parse_cb, &s, &scratch);
    CHECK(rc == CDC_OK && s.n == 1, "parse lone surrogate rc/n (got %d/%d)",
          rc, s.n);
    CHECK(strcmp(s.names[0], "a\xEF\xBF\xBD|0|1|1577836800") == 0,
          "lone surrogate -> U+FFFD (got '%s')", s.names[0]);

    /* 空缓冲：OK，零回调。 */
    memset(&s, 0, sizeof s);
    s.stop_after = -1;
    rc = cdc_direnum_parse_batch(buf, 0, parse_cb, &s, &scratch);
    CHECK(rc == CDC_OK && s.n == 0, "parse empty buffer OK/0 (got %d/%d)",
          rc, s.n);

    /* 截断：FileNameLength 超出缓冲 -> IO 错。 */
    len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_a, 5);
    memset(&s, 0, sizeof s);
    s.stop_after = -1;
    rc = cdc_direnum_parse_batch(buf, len - 3, parse_cb, &s, &scratch);
    CHECK(rc == CDC_ERR_IO, "parse truncated name -> IO (got %d)", rc);
    CHECK(s.n == 0, "truncated entry not delivered (got %d)", s.n);

    /* NextEntryOffset 越界 -> IO 错（第一项已交付）。 */
    len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_a, 5);
    put_u32le(buf + 0, 100000);
    memset(&s, 0, sizeof s);
    s.stop_after = -1;
    rc = cdc_direnum_parse_batch(buf, len, parse_cb, &s, &scratch);
    CHECK(rc == CDC_ERR_IO, "parse offset overflow -> IO (got %d)", rc);
    CHECK(s.n == 1, "first entry delivered before bad offset (got %d)", s.n);

    /* NextEntryOffset 非零但 < 64 -> IO 错。 */
    len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_a, 5);
    put_u32le(buf + 0, 8);
    memset(&s, 0, sizeof s);
    s.stop_after = -1;
    rc = cdc_direnum_parse_batch(buf, len, parse_cb, &s, &scratch);
    CHECK(rc == CDC_ERR_IO, "parse tiny offset -> IO (got %d)", rc);

    /* 回调提前停止：返回 1，解析立即停。 */
    len = 0;
    e1 = len;
    len = append_entry(buf, len, 0, ft2020, 1, 0x20, n_a, 5);
    e2 = len;
    len = append_entry(buf, len, 0, ft2020, 2, 0x20, n_dir, 3);
    put_u32le(buf + e1 + 0, (uint32_t)(e2 - e1));
    memset(&s, 0, sizeof s);
    s.stop_after = 0; /* 第 1 次回调即返回 1 */
    rc = cdc_direnum_parse_batch(buf, len, parse_cb, &s, &scratch);
    CHECK(rc == 1, "parse early stop returns cb value (got %d)", rc);
    CHECK(s.n == 1, "early stop delivers only 1 (got %d)", s.n);

    cdc_buf_free(&scratch);
}

int main(void) {
    test_posix_equivalence();
    test_parse_batch();
    printf("test_direnum: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
