// test_indextask.cpp — cross-platform tests for the indextask module.
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

using namespace cdc;

void test_escape_and_xml() {
    CHECK(xml_escape("C:\\a&b<c>d\"e") == "C:\\a&amp;b&lt;c&gt;d&quot;e", "XML escape");

    std::string xml = build_task_xml("C:\\Program Files\\C盘清理工具\\app&x.exe\"",
                                     "DOMAIN\\u<ser&\"", "2026-11-18T03:00:00&bad");
    CHECK(!xml.empty(), "XML generated");
    CHECK(xml.find("<Task version=\"1.3\"") != std::string::npos, "Task version");
    CHECK(xml.find("<LogonType>S4U</LogonType>") != std::string::npos, "S4U");
    CHECK(xml.find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos,
          "highest run level");
    CHECK(xml.find("C:\\Program Files\\C盘清理工具\\app&amp;x.exe&quot;") != std::string::npos,
          "command escaped");
    CHECK(xml.find("DOMAIN\\u&lt;ser&amp;&quot;") != std::string::npos, "user escaped");
    CHECK(xml.find("2026-11-18T03:00:00&amp;bad") != std::string::npos, "boundary escaped");
    CHECK(xml.find("<Arguments>--index --quiet</Arguments>") != std::string::npos,
          "fixed arguments");
}

void test_dates() {
    Ymd d = next_0300_date(2026, 11, 17, 2);
    CHECK(d.year == 2026 && d.month == 11 && d.day == 17, "before 03:00 uses today");
    d = next_0300_date(2026, 11, 17, 3);
    CHECK(d.year == 2026 && d.month == 11 && d.day == 18, "at 03:00 uses tomorrow");
    d = next_0300_date(2028, 2, 28, 23);
    CHECK(d.year == 2028 && d.month == 2 && d.day == 29, "leap day");
    d = next_0300_date(2100, 2, 28, 23);
    CHECK(d.year == 2100 && d.month == 3 && d.day == 1, "non-leap century");
    d = next_0300_date(2026, 12, 31, 23);
    CHECK(d.year == 2027 && d.month == 1 && d.day == 1, "year rollover");
    CHECK(days_in_month(2000, 2) == 29 && days_in_month(1900, 2) == 28,
          "Gregorian leap rule");
}

void test_utf16_roundtrip() {
    std::string xml = build_task_xml("C:\\盘\\app.exe", "S-1-5-21-1001", "2026-11-18T03:00:00");
    CHECK(!xml.empty(), "roundtrip XML generated");
#ifdef _WIN32
    const std::string path = "cdc-indextask-test.xml";
#else
    const std::string path = std::string("/tmp/cdc-indextask-test-") +
                             std::to_string(static_cast<long>(::getpid())) + ".xml";
#endif
    CHECK(write_task_xml_utf16(path, xml) == 0, "UTF-16 write");
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
        std::string decoded =
            decode_task_xml(std::span<const unsigned char>(bytes.data(), got));
        CHECK(decoded == xml, "UTF-16 roundtrip");
    }
    std::remove(path.c_str());
}

void test_non_windows_contract() {
#ifndef _WIN32
    TaskStatus s = task_status();
    CHECK(!s.exists && !s.enabled, "non-Windows status fallback");
    std::string msg;
    Error err{};
    CHECK(register_task(&msg, &err) == -1 && msg.find("Windows") != std::string::npos,
          "non-Windows register fallback");
#endif
}

void test_windows_current_user_sid() {
#ifdef _WIN32
    auto sid = current_user_sid();
    CHECK(sid && sid->rfind("S-", 0) == 0, "current user SID string");
#endif
}

}  // namespace

int main() {
    test_escape_and_xml();
    test_dates();
    test_utf16_roundtrip();
    test_non_windows_contract();
    test_windows_current_user_sid();
    std::printf("test_indextask: %d checks, %d failures\n", cdc_test::checks, cdc_test::failures);
    return cdc_test::failures == 0 ? 0 : 1;
}
