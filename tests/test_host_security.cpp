#include <cstdio>
#include <cstdlib>

#include "../host/security.h"

namespace {
int checks = 0;
int failures = 0;

void check(bool condition, const char *label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}
} // namespace

int main() {
    using cdc::host::is_safe_resource_relative_path;
    using cdc::host::is_trusted_origin;

    check(is_trusted_origin(L"https://cdc.local/"), "trusted origin root");
    check(is_trusted_origin(L"HTTPS://CDC.LOCAL/index.html?q=1"),
          "trusted origin is ASCII case-insensitive");
    check(is_trusted_origin(L"https://cdc.local"), "trusted origin without path");
    check(!is_trusted_origin(L"http://cdc.local/"), "reject plain HTTP");
    check(!is_trusted_origin(L"https://cdc.local.evil/"), "reject lookalike host");
    check(!is_trusted_origin(L"https://evil.cdc.local/"), "reject subdomain");
    check(!is_trusted_origin(L"https://cdc.local:444/"), "reject alternate port");
    check(!is_trusted_origin(L"https://user@cdc.local/"), "reject credentials");
    check(!is_trusted_origin(L"https://cdc.local@evil.test/"), "reject userinfo host trick");

    check(is_safe_resource_relative_path(L"index.html"), "relative root asset");
    check(is_safe_resource_relative_path(L"assets/app.js"), "nested asset");
    check(!is_safe_resource_relative_path(L""), "reject empty resource path");
    check(!is_safe_resource_relative_path(L"/index.html"), "reject rooted path");
    check(!is_safe_resource_relative_path(L"\\\\server\\share"), "reject UNC path");
    check(!is_safe_resource_relative_path(L"assets/../secret.txt"), "reject parent traversal");
    check(!is_safe_resource_relative_path(L"assets/./app.js"), "reject dot component");
    check(!is_safe_resource_relative_path(L"assets//app.js"), "reject empty component");
    check(!is_safe_resource_relative_path(L"assets/file.txt:stream"), "reject ADS syntax");
    check(!is_safe_resource_relative_path(std::wstring_view(L"assets/a\0b", 10)),
          "reject embedded NUL");

    std::printf("test_host_security: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
