/* test_util.c — src/util.{c,h} 的自测。
 *
 *   gcc -O2 -std=c11 -Wall -Wextra -Werror -Isrc -pthread \
 *       -o build/test_util tests/test_util.c src/util.c
 *   ./build/test_util        # 退出码 0=全过
 *
 * 覆盖：动态数组（增长/溢出/所有权交接）、通用向量（越界/pop/clear）、
 * UTF-8/宽字符往返（含非法序列与孤立代理对）、错误码表、日志
 *（目录解析、文件名校验、级别过滤、文件落盘内容）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ---------------- cdc_buf_t ---------------- */

static void test_buf(void) {
    cdc_buf_t b;
    uint8_t *taken;
    char *str;
    size_t i;
    uint8_t chunk[100];

    cdc_buf_init(&b);
    CHECK(b.data == NULL && b.len == 0 && b.cap == 0, "buf init");
    CHECK(cdc_buf_append(&b, NULL, 5) == CDC_ERR_INVALID_ARG, "buf append NULL");
    CHECK(cdc_buf_append(&b, "x", 0) == CDC_OK, "buf append 0 bytes");
    CHECK(cdc_buf_append_str(&b, NULL) == CDC_ERR_INVALID_ARG, "buf append_str NULL");
    CHECK(cdc_buf_push(&b, 'A') == CDC_OK && b.len == 1, "buf push");
    CHECK(cdc_buf_append_str(&b, "BC") == CDC_OK && b.len == 3, "buf append_str");
    CHECK(memcmp(b.data, "ABC", 3) == 0, "buf content ABC");
    cdc_buf_clear(&b);
    CHECK(b.len == 0 && b.cap > 0, "buf clear keeps cap");

    /* 增长：50 次 × 100 字节，内容逐字节可验证。 */
    memset(chunk, 0, sizeof chunk);
    for (i = 0; i < 100; i++) chunk[i] = (uint8_t)(i & 0xFF);
    for (i = 0; i < 50; i++)
        CHECK(cdc_buf_append(&b, chunk, sizeof chunk) == CDC_OK, "buf grow append");
    CHECK(b.len == 5000 && b.cap >= 5000, "buf grown to 5000");
    CHECK(b.data[0] == 0 && b.data[99] == 99 && b.data[100] == 0 &&
          b.data[4999] == 99, "buf grown content");

    /* 溢出：len=5000 时再 reserve SIZE_MAX 必然溢出。 */
    CHECK(cdc_buf_reserve(&b, SIZE_MAX) == CDC_ERR_OVERFLOW, "buf reserve overflow");

    /* 所有权交接：take 后 buf 回到 init 状态。 */
    taken = cdc_buf_take(&b);
    CHECK(taken != NULL && taken[4999] == 99, "buf take");
    CHECK(b.data == NULL && b.len == 0 && b.cap == 0, "buf reset after take");
    free(taken);

    /* take_cstr：空缓冲也返回可用的 ""。 */
    str = cdc_buf_take_cstr(&b);
    CHECK(str != NULL && str[0] == '\0', "buf take_cstr empty");
    free(str);
    CHECK(cdc_buf_append_str(&b, "hello") == CDC_OK, "buf append hello");
    CHECK(cdc_buf_append_str(&b, " world") == CDC_OK, "buf append world");
    str = cdc_buf_take_cstr(&b);
    CHECK(str != NULL && strcmp(str, "hello world") == 0, "buf take_cstr content");
    free(str);
    CHECK(b.data == NULL && b.len == 0, "buf reset after take_cstr");

    cdc_buf_free(&b);
    cdc_buf_free(NULL); /* 空指针 free 不崩。 */
}

/* ---------------- cdc_vec_t ---------------- */

static void test_vec(void) {
    cdc_vec_t v;
    int i, out = -1;
    int *got;

    CHECK(cdc_vec_init(NULL, sizeof(int)) == CDC_ERR_INVALID_ARG, "vec init NULL");
    CHECK(cdc_vec_init(&v, 0) == CDC_ERR_INVALID_ARG, "vec init elem_size 0");
    CHECK(cdc_vec_init(&v, sizeof(int)) == CDC_OK, "vec init");
    CHECK(cdc_vec_push(&v, NULL) == CDC_ERR_INVALID_ARG, "vec push NULL");
    CHECK(cdc_vec_pop(&v, NULL) == CDC_ERR_INVALID_ARG, "vec pop empty");
    CHECK(cdc_vec_get(&v, 0) == NULL, "vec get empty");
    CHECK(cdc_vec_get(NULL, 0) == NULL, "vec get NULL vec");

    for (i = 0; i < 1000; i++)
        CHECK(cdc_vec_push(&v, &i) == CDC_OK, "vec push");
    CHECK(v.len == 1000 && v.cap >= 1000, "vec grown to 1000");
    CHECK(cdc_vec_reserve(&v, SIZE_MAX) == CDC_ERR_OVERFLOW, "vec reserve overflow");
    for (i = 0; i < 1000; i++) {
        got = (int *)cdc_vec_get(&v, (size_t)i);
        if (!got || *got != i) {
            CHECK(0, "vec get value %d", i);
            break;
        }
        checks++; /* 每项都算一个检查点 */
    }
    CHECK(CDC_VEC_AT(&v, int, 999) == 999, "vec macro at");
    CHECK(cdc_vec_get(&v, 1000) == NULL, "vec get out of range");
    CHECK(cdc_vec_pop(&v, &out) == CDC_OK && out == 999 && v.len == 999, "vec pop");
    cdc_vec_clear(&v);
    CHECK(v.len == 0 && v.cap > 0, "vec clear keeps cap");
    CHECK(cdc_vec_pop(&v, NULL) == CDC_ERR_INVALID_ARG, "vec pop after clear");
    cdc_vec_free(&v);
    cdc_vec_free(NULL);
}

/* ---------------- UTF-8 / 宽字符 ---------------- */

static void test_utf(void) {
    wchar_t *w;
    char *s;

    /* ASCII 往返。 */
    w = cdc_utf8_to_wide("hello");
    CHECK(w != NULL && wcscmp(w, L"hello") == 0, "utf8->wide ascii");
    s = cdc_wide_to_utf8(w);
    CHECK(s != NULL && strcmp(s, "hello") == 0, "wide->utf8 ascii");
    free(w);
    free(s);

    /* 中文 + 表情（代理对）：精确往返。 */
    w = cdc_utf8_to_wide("C盘清理工具\xf0\x9f\x98\x80"); /* 😀 U+1F600 */
    CHECK(w != NULL, "utf8->wide cjk+emoji");
    if (w) {
        s = cdc_wide_to_utf8(w);
        CHECK(s != NULL && strcmp(s, "C盘清理工具\xf0\x9f\x98\x80") == 0,
              "wide->utf8 cjk+emoji roundtrip");
        free(s);
        free(w);
    }

    /* 空串：返回指向单个 NUL 的分配（非 NULL）。 */
    w = cdc_utf8_to_wide("");
    CHECK(w != NULL && w[0] == 0, "utf8->wide empty");
    free(w);
    s = cdc_wide_to_utf8(L"");
    CHECK(s != NULL && s[0] == '\0', "wide->utf8 empty");
    free(s);

    /* 非法 UTF-8：一律 NULL。 */
    CHECK(cdc_utf8_to_wide("\xFF") == NULL, "utf8 invalid 0xFF");
    CHECK(cdc_utf8_to_wide("\xE4\xB8") == NULL, "utf8 truncated sequence");
    CHECK(cdc_utf8_to_wide("\xC0\xAF") == NULL, "utf8 overlong encoding");
    CHECK(cdc_utf8_to_wide("\xED\xA0\x80") == NULL, "utf8 surrogate code point");
    CHECK(cdc_utf8_to_wide("\xF4\x90\x80\x80") == NULL, "utf8 beyond U+10FFFF");
    CHECK(cdc_utf8_to_wide(NULL) == NULL, "utf8 NULL input");
    CHECK(cdc_wide_to_utf8(NULL) == NULL, "wide NULL input");

    /* 孤立代理对 -> U+FFFD（与 Rust from_utf16_lossy 同策略）。 */
    {
        wchar_t lone[2];
        lone[0] = (wchar_t)0xD800;
        lone[1] = 0;
        s = cdc_wide_to_utf8(lone);
        CHECK(s != NULL && strcmp(s, "\xEF\xBF\xBD") == 0, "wide lone surrogate -> U+FFFD");
        free(s);
    }
}

/* ---------------- 错误码 ---------------- */

static void test_err(void) {
    cdc_err_t e;
    cdc_err_init(&e);
    CHECK(e.code == CDC_OK && e.message[0] == '\0', "err init");
    cdc_err_set(&e, CDC_ERR_IO, "open %s failed: %d", "x.log", 2);
    CHECK(e.code == CDC_ERR_IO && strcmp(e.message, "open x.log failed: 2") == 0,
          "err set formatted");
    cdc_err_set(&e, CDC_ERR_NOMEM, NULL);
    CHECK(e.code == CDC_ERR_NOMEM && e.message[0] == '\0', "err set NULL fmt");
    cdc_err_init(NULL);
    cdc_err_set(NULL, CDC_ERR_IO, "x");
    CHECK(cdc_err_str(CDC_ERR_OK) != NULL && cdc_err_str(CDC_ERR_NOMEM) != NULL &&
          cdc_err_str(CDC_ERR_UTF8) != NULL && cdc_err_str(CDC_ERR_WIN32) != NULL,
          "err str known codes");
    CHECK(strcmp(cdc_err_str(999), "unknown error") == 0, "err str unknown code");
    CHECK(CDC_OK == 0 && CDC_ERR_OK == 0, "CDC_OK is 0");
}

/* ---------------- 日志 ---------------- */

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;
    int found = 0;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 0;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return 0;
    }
    buf[size] = '\0';
    found = strstr(buf, needle) != NULL;
    free(buf);
    fclose(f);
    return found;
}

static void test_log(void) {
    char dir[4096];
    char path[8192]; /* dir 最长 4095 + '/' + 文件名 22 + NUL < 8192，不触发 format-truncation */
    const char *marker1 = "UTILTEST-MARKER-1";
    const char *marker2 = "UTILTEST-MARKER-2-should-be-filtered";

    CHECK(cdc_log_get_level() == CDC_LOG_INFO, "log default level INFO");
    cdc_log_set_level(CDC_LOG_DEBUG);
    CHECK(cdc_log_get_level() == CDC_LOG_DEBUG, "log set level DEBUG");
    cdc_log_set_level(CDC_LOG_INFO);

    CHECK(cdc_log_dir(NULL, 0) == CDC_ERR_INVALID_ARG, "log dir NULL");
    CHECK(cdc_log_dir(dir, sizeof dir) == CDC_OK && dir[0] != '\0', "log dir ok");
    CHECK(strstr(dir, "logs") != NULL, "log dir ends with logs");
    CHECK(cdc_log_dir(dir, 4) == CDC_ERR_OVERFLOW, "log dir small cap");

    CHECK(cdc_log_set_file("") == CDC_ERR_INVALID_ARG, "log file empty");
    CHECK(cdc_log_set_file("a/b.log") == CDC_ERR_INVALID_ARG, "log file slash");
    CHECK(cdc_log_set_file("..\\x.log") == CDC_ERR_INVALID_ARG, "log file backslash+dotdot");
    CHECK(cdc_log_set_file("..") == CDC_ERR_INVALID_ARG, "log file dotdot");
    CHECK(cdc_log_set_file("test-util-selftest.log") == CDC_OK, "log file ok");

    /* 写三行（INFO/WARN/ERROR），再读文件核对。 */
    cdc_log(CDC_LOG_INFO, "info %s %d", marker1, 1);
    cdc_log(CDC_LOG_WARN, "warn %s", marker1);
    cdc_log(CDC_LOG_ERROR, "error %s", marker1);
    snprintf(path, sizeof path, "%s/%s", dir, "test-util-selftest.log");
    CHECK(file_contains(path, marker1), "log file contains marker1");
    CHECK(file_contains(path, "[INFO]"), "log file has INFO level tag");
    CHECK(file_contains(path, "[WARN]"), "log file has WARN level tag");
    CHECK(file_contains(path, "[ERROR]"), "log file has ERROR level tag");

    /* 级别过滤：ERROR 级别下 INFO 不落盘。 */
    cdc_log_set_level(CDC_LOG_ERROR);
    cdc_log(CDC_LOG_INFO, "info %s", marker2);
    cdc_log(CDC_LOG_ERROR, "error after filter");
    cdc_log_set_level(CDC_LOG_INFO);
    CHECK(!file_contains(path, marker2), "log level filter drops INFO");
}

int main(void) {
    test_buf();
    test_vec();
    test_utf();
    test_err();
    test_log();
    printf("test_util: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
