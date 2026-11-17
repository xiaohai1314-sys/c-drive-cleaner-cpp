// test_direnum.cpp — src/util.{h,cpp} 的目录枚举（cdc::enum_dir）自测。
//
// 覆盖：
//  - POSIX 枚举与 opendir/readdir + lstat 逐项等价（同一棵树：子目录、
//    空目录、空文件、中文名、$ 开头名、文件/目录符号链接、悬空链接）；
//  - 缺失目录 -> NOT_FOUND、空目录 -> OK 零项、空参数 -> INVALID_ARG；
//  - FILE_DIRECTORY_INFORMATION 批量解析（合成缓冲）：多项链、"." ".."
//    跳过、目录项 size 归零、代理对 / 孤立代理对、截断缓冲报错、
//    NextEntryOffset 越界报错、回调提前停止。
//
// 注意：NtQueryDirectoryFile 的 Windows 真路径本机无 Windows 环境，
// 只能交叉编译验证；这里的解析器测试覆盖它最容易出错的部分。
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "raii.h"
#include "util.h"

namespace cdc {
namespace {

void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

void put_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// 收集到的枚举项（路径相对枚举根，排序后比较）。
struct Seen {
    std::string name;
    bool is_dir = false;
    bool is_symlink = false;
    uint64_t size = 0;
    uint64_t mtime = 0;
    uint32_t attrs = 0;
    bool operator==(const Seen &o) const {
        return name == o.name && is_dir == o.is_dir && is_symlink == o.is_symlink &&
               size == o.size && mtime == o.mtime && attrs == o.attrs;
    }
};
using SeenList = std::vector<Seen>;

void seen_sort(SeenList &l) {
    std::sort(l.begin(), l.end(), [](const Seen &a, const Seen &b) { return a.name < b.name; });
}

std::string join_rel(const std::string &rel, std::string_view name) {
    std::string r = rel;
    if (!r.empty()) r.push_back('/');
    r += name;
    return r;
}

// ---------------- 参照枚举（opendir/readdir + lstat） ----------------

int ref_walk(const std::string &dir, const std::string &rel, SeenList &out) {
    UniqueDir d(opendir(dir.c_str()));
    if (!d) return -1;
    while (struct dirent *e = readdir(d.get())) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        std::string full = dir + "/" + e->d_name;
        std::string rpath = join_rel(rel, e->d_name);
        struct stat st;
        if (lstat(full.c_str(), &st) != 0) continue;
        out.push_back(Seen{rpath, S_ISDIR(st.st_mode), S_ISLNK(st.st_mode),
                           S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0,
                           static_cast<uint64_t>(st.st_mtime), static_cast<uint32_t>(st.st_mode)});
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            int rc = ref_walk(full, rpath, out);
            if (rc != 0) return rc;
        }
    }
    return 0;
}

// 用 enum_dir 做同样的递归收集。
int enum_walk(const std::string &dir, const std::string &rel, SeenList &out) {
    bool failed = false;
    int rc = enum_dir(dir, [&](const DirEntry &e) {
        std::string rpath = join_rel(rel, e.name);
        out.push_back(Seen{rpath, e.is_dir, e.is_symlink, e.size, e.mtime, e.attrs});
        if (e.is_dir && !e.is_symlink) {
            if (enum_walk(dir + "/" + std::string(e.name), rpath, out) != 0) {
                failed = true;
                return -1;
            }
        }
        return 0;
    });
    return (rc != kOk || failed) ? -1 : 0;
}

// ---------------- 造测试树 ----------------

void write_file(const std::string &path, const char *data, size_t n) {
    UniqueFile f(std::fopen(path.c_str(), "wb"));
    if (f && n > 0) (void)std::fwrite(data, 1, n, f.get());
}

void build_tree(const std::string &root) {
    mkdir(root.c_str(), 0755);
    mkdir((root + "/a").c_str(), 0755);
    mkdir((root + "/a/b").c_str(), 0755);
    mkdir((root + "/empty").c_str(), 0755);
    write_file(root + "/top.txt", "hello", 5);
    write_file(root + "/empty.bin", "", 0);
    write_file(root + "/a/mid.log", "0123456789", 10);
    write_file(root + "/a/b/deep.dat", "xyz", 3);
    write_file(root + "/$dollar.txt", "d", 1);
    write_file(root + "/中文名.txt", "zh", 2);
    write_file(root + "/with space.txt", "sp", 2);
    // 符号链接：指文件、指目录、悬空。
    (void)!symlink("top.txt", (root + "/link_file").c_str());
    (void)!symlink("a", (root + "/link_dir").c_str());
    (void)!symlink("no-such-target", (root + "/link_dangling").c_str());
}

void test_posix_equivalence() {
    char root_tmpl[] = "/tmp/cdc-direnum-XXXXXX";
    char *root_c = mkdtemp(root_tmpl);
    CHECK(root_c != nullptr, "mkdtemp test root");
    if (!root_c) return;
    std::string root = root_c;
    build_tree(root);
    SeenList ref, got;
    int rc = ref_walk(root, "", ref);
    CHECK(rc == 0, "ref walk rc==0");
    rc = enum_walk(root, "", got);
    CHECK(rc == 0, "enum walk rc==0 (got %d)", rc);
    seen_sort(ref);
    seen_sort(got);
    CHECK(ref.size() == 13, "ref sees 13 entries (got %zu)", ref.size());
    CHECK(got.size() == 13, "enum sees 13 entries (got %zu)", got.size());
    CHECK(ref == got, "enum == ref (names/dirs/links/sizes/mtimes)");

    // 空目录：OK，零项。
    {
        SeenList e2;
        rc = enum_dir(root + "/empty", [&](const DirEntry &e) {
            e2.push_back(Seen{std::string(e.name)});
            return 0;
        });
        CHECK(rc == kOk, "empty dir -> OK (got %d)", rc);
        CHECK(e2.empty(), "empty dir -> 0 entries (got %zu)", e2.size());
    }

    auto never = [](const DirEntry &) { return 0; };
    // 缺失目录：NOT_FOUND。
    rc = enum_dir(root + "/no-such-dir", never);
    CHECK(rc == kErrNotFound, "missing dir -> NOT_FOUND (got %d)", rc);

    // 对文件调枚举：打不开为目录 -> 非 0。
    rc = enum_dir(root + "/top.txt", never);
    CHECK(rc != kOk, "file as dir -> error (got %d)", rc);

    // 空参数。
    CHECK(enum_dir(static_cast<const char *>(nullptr), never) == kErrInvalidArg,
          "NULL dir -> INVALID_ARG");
    CHECK(enum_dir(root, DirEntryFn()) == kErrInvalidArg, "NULL cb -> INVALID_ARG");
    std::string cmd = "rm -rf " + root;
    (void)!std::system(cmd.c_str());
}

// ---------------- FILE_DIRECTORY_INFORMATION 合成缓冲 ----------------

// 在 buf 的 off 处写一项；返回写完后的长度。name 为 UTF-16LE 码元数组。
size_t append_entry(uint8_t *buf, size_t off, uint32_t next_off, int64_t mtime_ft, int64_t size,
                    uint32_t attrs, const uint16_t *name_units, size_t n_units) {
    size_t nlen = n_units * 2;
    put_u32le(buf + off + 0, next_off);
    put_u32le(buf + off + 4, 0);   // FileIndex
    put_u64le(buf + off + 8, 0);   // CreationTime
    put_u64le(buf + off + 16, 0);  // LastAccessTime
    put_u64le(buf + off + 24, static_cast<uint64_t>(mtime_ft));
    put_u64le(buf + off + 32, 0);  // ChangeTime
    put_u64le(buf + off + 40, static_cast<uint64_t>(size));
    put_u64le(buf + off + 48, static_cast<uint64_t>(size));  // AllocationSize
    put_u32le(buf + off + 56, attrs);
    put_u32le(buf + off + 60, static_cast<uint32_t>(nlen));
    for (size_t i = 0; i < n_units; i++) {
        buf[off + 64 + i * 2] = static_cast<uint8_t>(name_units[i] & 0xFF);
        buf[off + 64 + i * 2 + 1] = static_cast<uint8_t>(name_units[i] >> 8);
    }
    return off + 64 + nlen;
}

struct ParseSeen {
    std::vector<std::string> names;
    int stop_after = -1;  // >=0：第几次回调返回 1
    int operator()(const DirEntry &e) {
        names.push_back(format("%.*s|%d|%llu|%llu", static_cast<int>(e.name.size()),
                               e.name.data(), e.is_dir ? 1 : 0,
                               static_cast<unsigned long long>(e.size),
                               static_cast<unsigned long long>(e.mtime)));
        if (stop_after >= 0 && static_cast<int>(names.size()) > stop_after) return 1;
        return 0;
    }
    int n() const { return static_cast<int>(names.size()); }
    const char *at(size_t i) const { return i < names.size() ? names[i].c_str() : ""; }
};

void test_parse_batch() {
    const int64_t ft2020 = 132223104000000000LL;  // 2020-01-01 00:00:00 UTC
    uint8_t buf[4096];
    std::string scratch;
    const uint16_t n_dot[1] = {0x002E};
    const uint16_t n_dotdot[2] = {0x002E, 0x002E};
    const uint16_t n_a[5] = {0x0061, 0x002E, 0x0074, 0x0078, 0x0074};  // a.txt
    const uint16_t n_dir[3] = {0x0073, 0x0075, 0x0062};                // sub
    const uint16_t n_emoji[2] = {0xD83D, 0xDE00};                      // U+1F600
    const uint16_t n_lone[2] = {0x0061, 0xD800};                       // a + 孤立高代理

    // 两项链 + "." ".." 跳过：. 、.. 、a.txt（文件 123456789）、sub（目录）。
    size_t len = append_entry(buf, 0, 0, ft2020, 0, 0x10, n_dot, 1);
    len = append_entry(buf, len, 0, ft2020, 0, 0x10, n_dotdot, 2);
    size_t e1 = len;
    len = append_entry(buf, len, 0, ft2020, 123456789, 0x20, n_a, 5);
    size_t e2 = len;
    len = append_entry(buf, len, 0, ft2020, 0, 0x10, n_dir, 3);
    put_u32le(buf + e1 + 0, static_cast<uint32_t>(e2 - e1));
    put_u32le(buf + 0, static_cast<uint32_t>(64 + 2));
    put_u32le(buf + (64 + 2) + 0, static_cast<uint32_t>(e1 - (64 + 2)));
    {
        ParseSeen s;
        int rc = direnum_parse_batch(buf, len, s, scratch);
        CHECK(rc == kOk, "parse 4 entries rc==OK (got %d)", rc);
        CHECK(s.n() == 2, "parse skips . .., delivers 2 (got %d)", s.n());
        CHECK(std::strcmp(s.at(0), "a.txt|0|123456789|1577836800") == 0,
              "parse a.txt fields (got '%s')", s.at(0));
        CHECK(std::strcmp(s.at(1), "sub|1|0|1577836800") == 0, "parse sub dir fields (got '%s')",
              s.at(1));
    }
    {  // 代理对：U+1F600 应解成 4 字节 UTF-8。
        len = append_entry(buf, 0, 0, ft2020, 7, 0x20, n_emoji, 2);
        ParseSeen s;
        int rc = direnum_parse_batch(buf, len, s, scratch);
        CHECK(rc == kOk && s.n() == 1, "parse surrogate pair rc/n (got %d/%d)", rc, s.n());
        CHECK(std::strcmp(s.at(0), "\xF0\x9F\x98\x80|0|7|1577836800") == 0,
              "surrogate pair decodes to U+1F600 (got '%s')", s.at(0));
    }
    {  // 孤立代理对 -> U+FFFD。
        len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_lone, 2);
        ParseSeen s;
        int rc = direnum_parse_batch(buf, len, s, scratch);
        CHECK(rc == kOk && s.n() == 1, "parse lone surrogate rc/n (got %d/%d)", rc, s.n());
        CHECK(std::strcmp(s.at(0), "a\xEF\xBF\xBD|0|1|1577836800") == 0,
              "lone surrogate -> U+FFFD (got '%s')", s.at(0));
    }
    {  // 空缓冲：OK，零回调。
        ParseSeen s;
        int rc = direnum_parse_batch(buf, 0, s, scratch);
        CHECK(rc == kOk && s.n() == 0, "parse empty buffer OK/0 (got %d/%d)", rc, s.n());
    }
    {  // 截断：FileNameLength 超出缓冲 -> IO 错。
        len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_a, 5);
        ParseSeen s;
        int rc = direnum_parse_batch(buf, len - 3, s, scratch);
        CHECK(rc == kErrIo, "parse truncated name -> IO (got %d)", rc);
        CHECK(s.n() == 0, "truncated entry not delivered (got %d)", s.n());
    }
    {  // NextEntryOffset 越界 -> IO 错（第一项已交付）。
        len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_a, 5);
        put_u32le(buf + 0, 100000);
        ParseSeen s;
        int rc = direnum_parse_batch(buf, len, s, scratch);
        CHECK(rc == kErrIo, "parse offset overflow -> IO (got %d)", rc);
        CHECK(s.n() == 1, "first entry delivered before bad offset (got %d)", s.n());
    }
    {  // NextEntryOffset 非零但 < 64 -> IO 错。
        len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_a, 5);
        put_u32le(buf + 0, 8);
        ParseSeen s;
        int rc = direnum_parse_batch(buf, len, s, scratch);
        CHECK(rc == kErrIo, "parse tiny offset -> IO (got %d)", rc);
    }
    {  // 回调提前停止：返回 1，解析立即停。
        e1 = 0;
        len = append_entry(buf, 0, 0, ft2020, 1, 0x20, n_a, 5);
        e2 = len;
        len = append_entry(buf, len, 0, ft2020, 2, 0x20, n_dir, 3);
        put_u32le(buf + e1 + 0, static_cast<uint32_t>(e2 - e1));
        ParseSeen s;
        s.stop_after = 0;  // 第 1 次回调即返回 1
        int rc = direnum_parse_batch(buf, len, s, scratch);
        CHECK(rc == 1, "parse early stop returns cb value (got %d)", rc);
        CHECK(s.n() == 1, "early stop delivers only 1 (got %d)", s.n());
    }
}

}  // namespace
}  // namespace cdc

int main() {
    cdc::test_posix_equivalence();
    cdc::test_parse_batch();
    std::printf("test_direnum: %d checks, %d failures\n", cdc_test::checks, cdc_test::failures);
    return cdc_test::failures ? 1 : 0;
}
