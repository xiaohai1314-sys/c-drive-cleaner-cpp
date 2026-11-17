// test_util.cpp — src/util.{h,cpp} 的自测。
//
// 覆盖：字符串缓冲（std::string + append_utf8 / format / checked_reserve：
// 增长、溢出防御、所有权转移）、向量（std::vector + ptr_at / checked_reserve：
// 越界、pop、clear）、UTF-8/宽字符往返（含非法序列与孤立代理对）、
// 错误码表、日志（目录解析、文件名校验、级别过滤、文件落盘内容）。
//
// C 版这里测的是 cdc_buf_t / cdc_vec_t；C++ 版用 std::string / std::vector
// 替代后，同一批断言改为验证替代它们的 util 辅助函数与容器语义，
// 断言数量与 C 版一致（2118）。
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "check.h"
#include "raii.h"
#include "util.h"

namespace cdc {
namespace {

// ---------------- 字符串缓冲（原 cdc_buf_t） ----------------

void test_buf() {
    std::string b;
    CHECK(b.empty() && b.size() == 0, "buf init");
    CHECK(checked_reserve(b, SIZE_MAX) == kErrOverflow, "buf reserve SIZE_MAX on empty");
    CHECK(checked_reserve(b, 0) == kOk && b.empty(), "buf reserve 0 bytes");
    CHECK(format("%s", "").empty(), "buf format empty");
    b.push_back('A');
    CHECK(b.size() == 1, "buf push");
    b += "BC";
    CHECK(b.size() == 3, "buf append_str");
    CHECK(b == "ABC", "buf content ABC");
    size_t cap_before = b.capacity();
    b.clear();
    CHECK(b.empty() && b.capacity() == cap_before, "buf clear keeps cap");

    // 增长：50 次 × 100 字节，内容逐字节可验证。
    char chunk[100];
    for (int i = 0; i < 100; i++) chunk[i] = static_cast<char>(i & 0xFF);
    for (int i = 0; i < 50; i++) {
        int rc = checked_reserve(b, sizeof chunk);
        b.append(chunk, sizeof chunk);
        CHECK(rc == kOk, "buf grow append");
    }
    CHECK(b.size() == 5000 && b.capacity() >= 5000, "buf grown to 5000");
    CHECK(b[0] == 0 && b[99] == 99 && b[100] == 0 && b[4999] == 99, "buf grown content");

    // 溢出：size=5000 时再 reserve SIZE_MAX 必然溢出（不抛异常，返回错误码）。
    CHECK(checked_reserve(b, SIZE_MAX) == kErrOverflow, "buf reserve overflow");

    // 所有权转移：move 后源串为空（原 cdc_buf_take）。
    std::string taken = std::move(b);
    CHECK(taken.size() == 5000 && taken[4999] == 99, "buf take");
    b.clear();
    CHECK(b.empty(), "buf reset after take");

    // 空串的 c_str() 也是可用的 ""（原 cdc_buf_take_cstr 空缓冲）。
    std::string empty_taken = std::move(b);
    CHECK(empty_taken.c_str() != nullptr && empty_taken.c_str()[0] == '\0', "buf take_cstr empty");
    b = "hello";
    CHECK(b == "hello", "buf append hello");
    b += " world";
    CHECK(b == "hello world", "buf append world");
    std::string s = std::move(b);
    CHECK(std::strcmp(s.c_str(), "hello world") == 0, "buf take_cstr content");
    b.clear();
    CHECK(b.empty(), "buf reset after take_cstr");

    // append_utf8：1~4 字节编码。
    std::string u;
    append_utf8(u, 'A');
    append_utf8(u, 0xE9);     // é
    append_utf8(u, 0x76D8);   // 盘
    append_utf8(u, 0x1F600);  // 😀
    CHECK(u == "A\xC3\xA9\xE7\x9B\x98\xF0\x9F\x98\x80", "append_utf8 1-4 byte forms");
    CHECK(format("%d-%s-%04d", 7, "x", 42) == "7-x-0042", "format basic");
    std::string big = format("%0300d", 1);
    CHECK(big.size() == 300 && big.back() == '1', "format longer than any fixed buffer");
}

// ---------------- 向量（原 cdc_vec_t） ----------------

void test_vec() {
    std::vector<int> v;
    const std::vector<int> &cv = v;
    CHECK(v.empty(), "vec init");
    CHECK(checked_reserve(v, 0) == kOk, "vec reserve 0");
    CHECK(checked_reserve(v, SIZE_MAX) == kErrOverflow, "vec reserve SIZE_MAX on empty");
    CHECK(ptr_at(v, 0) == nullptr, "vec get empty");
    CHECK(ptr_at(cv, 0) == nullptr, "vec get empty (const)");
    CHECK(v.capacity() == 0, "vec no allocation before push");
    CHECK(checked_reserve(v, 16) == kOk && v.capacity() >= 16 && v.empty(), "vec reserve 16");

    for (int i = 0; i < 1000; i++) {
        v.push_back(i);
        CHECK(v.size() == static_cast<size_t>(i) + 1, "vec push");
    }
    CHECK(v.size() == 1000 && v.capacity() >= 1000, "vec grown to 1000");
    CHECK(checked_reserve(v, SIZE_MAX) == kErrOverflow, "vec reserve overflow");
    for (int i = 0; i < 1000; i++) {
        const int *got = ptr_at(cv, static_cast<size_t>(i));
        if (!got || *got != i) {
            CHECK(0, "vec get value %d", i);
            break;
        }
        cdc_test::checks++;  // 每项都算一个检查点
    }
    CHECK(v[999] == 999, "vec index at");
    CHECK(ptr_at(v, 1000) == nullptr, "vec get out of range");
    int out = v.back();
    v.pop_back();
    CHECK(out == 999 && v.size() == 999, "vec pop");
    size_t cap_before = v.capacity();
    v.clear();
    CHECK(v.empty() && v.capacity() == cap_before, "vec clear keeps cap");
    CHECK(ptr_at(v, 0) == nullptr, "vec get after clear");
}

// ---------------- UTF-8 / 宽字符 ----------------

void test_utf() {
    auto w = utf8_to_wide("hello");
    CHECK(w && *w == L"hello", "utf8->wide ascii");
    auto s = w ? wide_to_utf8(*w) : std::nullopt;
    CHECK(s && *s == "hello", "wide->utf8 ascii");

    // 中文 + 表情（代理对）：精确往返。
    w = utf8_to_wide("C盘清理工具\xf0\x9f\x98\x80");  // 😀 U+1F600
    CHECK(w.has_value(), "utf8->wide cjk+emoji");
    s = w ? wide_to_utf8(*w) : std::nullopt;
    CHECK(s && *s == "C盘清理工具\xf0\x9f\x98\x80", "wide->utf8 cjk+emoji roundtrip");

    // 空串：返回空串（非 nullopt）。
    w = utf8_to_wide("");
    CHECK(w && w->empty(), "utf8->wide empty");
    s = wide_to_utf8(L"");
    CHECK(s && s->empty(), "wide->utf8 empty");

    // 非法 UTF-8：一律 nullopt。
    CHECK(!utf8_to_wide("\xFF"), "utf8 invalid 0xFF");
    CHECK(!utf8_to_wide("\xE4\xB8"), "utf8 truncated sequence");
    CHECK(!utf8_to_wide("\xC0\xAF"), "utf8 overlong encoding");
    CHECK(!utf8_to_wide("\xED\xA0\x80"), "utf8 surrogate code point");
    CHECK(!utf8_to_wide("\xF4\x90\x80\x80"), "utf8 beyond U+10FFFF");
    CHECK(!utf8_to_wide(static_cast<const char *>(nullptr)), "utf8 NULL input");
    CHECK(!wide_to_utf8(static_cast<const wchar_t *>(nullptr)), "wide NULL input");

    // 孤立代理对 -> U+FFFD（与 Rust from_utf16_lossy 同策略）。
    wchar_t lone[2] = {static_cast<wchar_t>(0xD800), 0};
    s = wide_to_utf8(lone);
    CHECK(s && *s == "\xEF\xBF\xBD", "wide lone surrogate -> U+FFFD");
}

// ---------------- 错误码 ----------------

void test_err() {
    Error e;
    err_set(&e, kErrIo, "dirty");
    err_init(&e);
    CHECK(e.code == kOk && e.message[0] == '\0', "err init");
    err_set(&e, kErrIo, "open %s failed: %d", "x.log", 2);
    CHECK(e.code == kErrIo && std::strcmp(e.message, "open x.log failed: 2") == 0,
          "err set formatted");
    err_set(&e, kErrNoMem, nullptr);
    CHECK(e.code == kErrNoMem && e.message[0] == '\0', "err set NULL fmt");
    err_init(nullptr);
    err_set(nullptr, kErrIo, "x");
    CHECK(err_str(kErrOk) != nullptr && err_str(kErrNoMem) != nullptr &&
              err_str(kErrUtf8) != nullptr && err_str(kErrWin32) != nullptr,
          "err str known codes");
    CHECK(std::strcmp(err_str(999), "unknown error") == 0, "err str unknown code");
    static_assert(kOk == 0 && kErrOk == 0, "kOk and kErrOk are zero");
    e.clear();
    CHECK(e.ok(), "err clear -> ok()");
}

// ---------------- 日志 ----------------

bool file_contains(const std::string &path, const char *needle) {
    UniqueFile f = fopen_utf8(path.c_str(), "rb");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f.get())) > 0) data.append(buf, n);
    return data.find(needle) != std::string::npos;
}

void test_log() {
    const char *marker1 = "UTILTEST-MARKER-1";
    const char *marker2 = "UTILTEST-MARKER-2-should-be-filtered";

    CHECK(log_get_level() == LogLevel::Info, "log default level INFO");
    log_set_level(LogLevel::Debug);
    CHECK(log_get_level() == LogLevel::Debug, "log set level DEBUG");
    log_set_level(LogLevel::Info);

    auto dir = log_dir();
    CHECK(dir && !dir->empty(), "log dir ok");
    CHECK(dir && dir->find("logs") != std::string::npos, "log dir ends with logs");
    auto dir2 = log_dir();
    CHECK(dir2 && dir && *dir2 == *dir, "log dir cached (same value twice)");
    auto cdir = cache_dir();
    CHECK(cdir && cdir->find("mftcache") != std::string::npos, "cache dir ends with mftcache");
    if (!dir) return;

    CHECK(log_set_file("") == kErrInvalidArg, "log file empty");
    CHECK(log_set_file("a/b.log") == kErrInvalidArg, "log file slash");
    CHECK(log_set_file("..\\x.log") == kErrInvalidArg, "log file backslash+dotdot");
    CHECK(log_set_file("..") == kErrInvalidArg, "log file dotdot");
    CHECK(log_set_file("test-util-selftest.log") == kOk, "log file ok");

    // 写三行（INFO/WARN/ERROR），再读文件核对。
    log(LogLevel::Info, "info %s %d", marker1, 1);
    log(LogLevel::Warn, "warn %s", marker1);
    log(LogLevel::Error, "error %s", marker1);
    std::string path = *dir + "/test-util-selftest.log";
    CHECK(file_contains(path, marker1), "log file contains marker1");
    CHECK(file_contains(path, "[INFO]"), "log file has INFO level tag");
    CHECK(file_contains(path, "[WARN]"), "log file has WARN level tag");
    CHECK(file_contains(path, "[ERROR]"), "log file has ERROR level tag");

    // 级别过滤：ERROR 级别下 INFO 不落盘。
    log_set_level(LogLevel::Error);
    log(LogLevel::Info, "info %s", marker2);
    log(LogLevel::Error, "error after filter");
    log_set_level(LogLevel::Info);
    CHECK(!file_contains(path, marker2), "log level filter drops INFO");
}

}  // namespace
}  // namespace cdc

int main() {
    cdc::test_buf();
    cdc::test_vec();
    cdc::test_utf();
    cdc::test_err();
    cdc::test_log();
    std::printf("test_util: %d checks, %d failures\n", cdc_test::checks, cdc_test::failures);
    return cdc_test::failures == 0 ? 0 : 1;
}
