// test_indextask.cpp — cross-platform tests for the C ABI facade.
#include "check.h"
#include "indextask.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

struct Free {
    void operator()(char *p) const { std::free(p); }
};

void test_escape_and_xml() {
    char *escaped = cdc_indextask_xml_escape("C:\\a&b<c>d\"e");
    CHECK(escaped && std::strcmp(escaped, "C:\\a&amp;b&lt;c&gt;d&quot;e") == 0, "XML escape");
    std::free(escaped);

    char *xml = cdc_indextask_build_task_xml("C:\\Program Files\\C盘清理工具\\app&x.exe\"",
                                             "DOMAIN\\u<ser&\"", "2026-11-18T03:00:00&bad");
    CHECK(xml != nullptr, "XML generated");
    if (!xml)
        return;
    std::string text(xml);
    CHECK(text.find("<Task version=\"1.3\"") != std::string::npos, "Task version");
    CHECK(text.find("<LogonType>S4U</LogonType>") != std::string::npos, "S4U");
    CHECK(text.find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos,
          "highest run level");
    CHECK(text.find("C:\\Program Files\\C盘清理工具\\app&amp;x.exe&quot;") != std::string::npos,
          "command escaped");
    CHECK(text.find("DOMAIN\\u&lt;ser&amp;&quot;") != std::string::npos, "user escaped");
    CHECK(text.find("2026-11-18T03:00:00&amp;bad") != std::string::npos, "boundary escaped");
    CHECK(text.find("<Arguments>--index --quiet</Arguments>") != std::string::npos,
          "fixed arguments");
    std::free(xml);
}

void test_dates() {
    int y = 0, m = 0, d = 0;
    cdc_indextask_next_0300_date(2026, 11, 17, 2, &y, &m, &d);
    CHECK(y == 2026 && m == 11 && d == 17, "before 03:00 uses today");
    cdc_indextask_next_0300_date(2026, 11, 17, 3, &y, &m, &d);
    CHECK(y == 2026 && m == 11 && d == 18, "at 03:00 uses tomorrow");
    cdc_indextask_next_0300_date(2028, 2, 28, 23, &y, &m, &d);
    CHECK(y == 2028 && m == 2 && d == 29, "leap day");
    cdc_indextask_next_0300_date(2100, 2, 28, 23, &y, &m, &d);
    CHECK(y == 2100 && m == 3 && d == 1, "non-leap century");
    cdc_indextask_next_0300_date(2026, 12, 31, 23, &y, &m, &d);
    CHECK(y == 2027 && m == 1 && d == 1, "year rollover");
    CHECK(cdc_indextask_days_in_month(2000, 2) == 29 && cdc_indextask_days_in_month(1900, 2) == 28,
          "Gregorian leap rule");
}

void test_utf16_roundtrip() {
    char *xml =
        cdc_indextask_build_task_xml("C:\\盘\\app.exe", "S-1-5-21-1001", "2026-11-18T03:00:00");
    CHECK(xml != nullptr, "roundtrip XML generated");
    if (!xml)
        return;
#ifdef _WIN32
    const std::string path = "cdc-indextask-test.xml";
#else
    const std::string path = std::string("/tmp/cdc-indextask-test-") +
                             std::to_string(static_cast<long>(::getpid())) + ".xml";
#endif
    CHECK(cdc_indextask_write_task_xml_utf16(path.c_str(), xml) == 0, "UTF-16 write");
    std::FILE *f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr, "UTF-16 readback");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::rewind(f);
        std::vector<unsigned char> bytes(n > 0 ? static_cast<size_t>(n) : 1);
        size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        CHECK(got == static_cast<size_t>(n) && n >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe,
              "UTF-16LE BOM");
        char *decoded = cdc_indextask_decode_task_xml(bytes.data(), got);
        CHECK(decoded && std::strcmp(decoded, xml) == 0, "UTF-16 roundtrip");
        std::free(decoded);
    }
    std::remove(path.c_str());
    std::free(xml);
}

void test_non_windows_contract() {
#ifndef _WIN32
    int exists = 1, enabled = 1;
    CHECK(cdc_indextask_task_status(&exists, &enabled) == 0 && exists == 0 && enabled == 0,
          "non-Windows status fallback");
    char msg[128] = {};
    cdc_err_t err{};
    CHECK(cdc_indextask_register(msg, sizeof(msg), &err) == -1 &&
              std::strstr(msg, "Windows") != nullptr,
          "non-Windows register fallback");
#endif
}

} // namespace

int main() {
    test_escape_and_xml();
    test_dates();
    test_utf16_roundtrip();
    test_non_windows_contract();
    std::printf("test_indextask: %d checks, %d failures\n", cdc_test::checks, cdc_test::failures);
    return cdc_test::failures == 0 ? 0 : 1;
}
