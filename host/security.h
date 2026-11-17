#pragma once

#include <string_view>

namespace cdc::host {

inline bool ascii_iequals(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        wchar_t a = left[i];
        wchar_t b = right[i];
        if (a >= L'A' && a <= L'Z')
            a = static_cast<wchar_t>(a + (L'a' - L'A'));
        if (b >= L'A' && b <= L'Z')
            b = static_cast<wchar_t>(b + (L'a' - L'A'));
        if (a != b)
            return false;
    }
    return true;
}

// Only the app's HTTPS origin is trusted; ports, credentials, lookalike hosts,
// HTTP, and non-ASCII host spellings are deliberately rejected.
inline bool is_trusted_origin(std::wstring_view uri) {
    constexpr std::wstring_view scheme = L"https://";
    constexpr std::wstring_view host = L"cdc.local";
    if (uri.size() < scheme.size() + host.size() ||
        !ascii_iequals(uri.substr(0, scheme.size()), scheme))
        return false;

    const std::wstring_view authority_and_path = uri.substr(scheme.size());
    const size_t end = authority_and_path.find_first_of(L"/?#");
    const std::wstring_view authority = authority_and_path.substr(0, end);
    return ascii_iequals(authority, host);
}

// The URL has already had its query/fragment removed and percent-decoded.
// Reject rooted paths, empty/dot components, ADS syntax, NUL and controls.
inline bool is_safe_resource_relative_path(std::wstring_view path) {
    if (path.empty() || path.front() == L'/' || path.front() == L'\\')
        return false;
    std::size_t component_start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i < path.size()) {
            const wchar_t ch = path[i];
            if (ch == L':' || ch == L'\0' || ch < 0x20)
                return false;
            if (ch != L'/' && ch != L'\\')
                continue;
        }
        const std::wstring_view component = path.substr(component_start, i - component_start);
        if (component.empty() || component == L"." || component == L"..")
            return false;
        component_start = i + 1;
    }
    return true;
}

} // namespace cdc::host
