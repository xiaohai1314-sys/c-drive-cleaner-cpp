/* indextask.cpp — Windows Task Scheduler integration. */
#include "indextask.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#endif

namespace cdc {
namespace {

std::string xml_escape_impl(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

uint32_t utf8_decode(const unsigned char *s, size_t len, size_t &used) {
    used = 0;
    if (!s || len == 0)
        return 0xfffd;
    const unsigned char c = s[0];
    int n = 0;
    uint32_t cp = 0;
    uint32_t min = 0;
    if (c < 0x80) {
        used = 1;
        return c;
    }
    if ((c & 0xe0) == 0xc0) {
        n = 2;
        cp = c & 0x1f;
        min = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        n = 3;
        cp = c & 0x0f;
        min = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        n = 4;
        cp = c & 7;
        min = 0x10000;
    } else {
        used = 1;
        return 0xfffd;
    }
    if (len < static_cast<size_t>(n)) {
        used = 1;
        return 0xfffd;
    }
    for (int i = 1; i < n; ++i) {
        if ((s[i] & 0xc0) != 0x80) {
            used = 1;
            return 0xfffd;
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        used = 1;
        return 0xfffd;
    }
    used = static_cast<size_t>(n);
    return cp;
}

bool put_u16(FILE *f, uint16_t u) {
    return std::fputc(u & 0xff, f) != EOF && std::fputc(u >> 8, f) != EOF;
}

struct FileCloser {
    void operator()(FILE *f) const {
        if (f)
            std::fclose(f);
    }
};

#ifdef _WIN32
struct HandleCloser {
    void operator()(void *h) const {
        if (h)
            ::CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
struct LocalCloser {
    void operator()(void *p) const {
        if (p)
            ::LocalFree(p);
    }
};
using UniqueLocal = std::unique_ptr<wchar_t, LocalCloser>;

std::wstring quote_arg(std::wstring_view arg) {
    // CommandLineToArgvW-compatible quoting: quote every argument and double
    // backslashes before quotes/end, so paths cannot alter the argument list.
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
            continue;
        }
        out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::optional<std::wstring> schtasks_path() {
    std::vector<wchar_t> root(32768);
    DWORD n =
        ::GetEnvironmentVariableW(L"SystemRoot", root.data(), static_cast<DWORD>(root.size()));
    std::wstring base = (n && n < root.size()) ? std::wstring(root.data(), n) : L"C:\\Windows";
    return base + L"\\System32\\schtasks.exe";
}

struct RawResult {
    std::vector<unsigned char> output;
    DWORD code = 0;
};
std::optional<RawResult> run_schtasks_raw(std::wstring_view args) {
    auto exe = schtasks_path();
    if (!exe)
        return std::nullopt;
    std::wstring cmd = quote_arg(*exe) + L" " + std::wstring(args);
    std::vector<wchar_t> command(cmd.begin(), cmd.end());
    command.push_back(L'\0');
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_raw = nullptr, write_raw = nullptr;
    if (!::CreatePipe(&read_raw, &write_raw, &sa, 0))
        return std::nullopt;
    UniqueHandle rd(read_raw), wr(write_raw);
    if (!::SetHandleInformation(rd.get(), HANDLE_FLAG_INHERIT, 0))
        return std::nullopt;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr.get();
    si.hStdError = wr.get();
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi))
        return std::nullopt;
    UniqueHandle process(pi.hProcess), thread(pi.hThread);
    ::CloseHandle(wr.release());
    RawResult result;
    std::array<unsigned char, 4096> chunk{};
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(rd.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) {
            if (::GetLastError() == ERROR_BROKEN_PIPE)
                break;
            return std::nullopt;
        }
        if (got == 0)
            break;
        result.output.insert(result.output.end(), chunk.begin(), chunk.begin() + got);
    }
    if (::WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0 ||
        !::GetExitCodeProcess(process.get(), &result.code))
        return std::nullopt;
    return result;
}

int schtasks_call(std::wstring_view args, const char *action, const char *ok_msg,
                  std::string *msg, Error *err) {
    auto result = run_schtasks_raw(args);
    if (!result) {
        const std::string text = std::string("schtasks ") + action + "失败：启动 schtasks 失败";
        if (msg)
            *msg = text;
        err_set(err, kErrIo, "%s", text.c_str());
        return -1;
    }
    if (result->code == 0) {
        if (msg)
            *msg = ok_msg;
        if (err)
            err_init(err);
        return 0;
    }
    std::string first;
    size_t i = 0;
    while (i < result->output.size()) {
        size_t j = i;
        while (j < result->output.size() && result->output[j] != '\r' && result->output[j] != '\n')
            ++j;
        size_t a = i, b = j;
        while (a < b && (result->output[a] == ' ' || result->output[a] == '\t'))
            ++a;
        while (b > a && (result->output[b - 1] == ' ' || result->output[b - 1] == '\t'))
            --b;
        if (b > a) {
            first.assign(reinterpret_cast<const char *>(result->output.data() + a), b - a);
            break;
        }
        i = j;
        while (i < result->output.size() &&
               (result->output[i] == '\r' || result->output[i] == '\n'))
            ++i;
    }
    std::string text =
        std::string("schtasks ") + action + (first.empty() ? "失败（无输出）" : "失败：" + first);
    if (msg)
        *msg = text;
    err_set(err, kErrIo, "%s", text.c_str());
    return -1;
}
#endif

#ifndef _WIN32
// 仅非 Windows 的桩实现用；Windows 下调用点都不编译，留着会触发 -Werror（unused-function）。
void unsupported(std::string *msg, Error *err) {
    constexpr const char *text = "计划任务仅 Windows 支持";
    if (msg)
        *msg = text;
    err_set(err, kErrIo, "%s", text);
}
#endif

}  // namespace

std::string xml_escape(std::string_view s) { return xml_escape_impl(s); }

int days_in_month(int year, int month) {
    switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
        return 31;
    case 4:
    case 6:
    case 9:
    case 11:
        return 30;
    case 2:
        return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 29 : 28;
    default:
        return 30;
    }
}

Ymd next_0300_date(int year, int month, int day, int hour) {
    if (hour >= 3 && ++day > days_in_month(year, month)) {
        day = 1;
        if (++month > 12) {
            month = 1;
            ++year;
        }
    }
    return {year, month, day};
}

std::string next_start_boundary() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    Ymd d = next_0300_date(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour);
    int y = std::clamp(d.year, 0, 9999);
    int m = std::clamp(d.month, 1, 12);
    int dd = std::clamp(d.day, 1, 31);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT03:00:00", y, m, dd);
    return std::string(buf);
}

std::string build_task_xml(std::string_view exe_path, std::string_view user_id,
                           std::string_view start_boundary) {
    static constexpr std::string_view tmpl =
        "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        "<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        "  <RegistrationInfo>\n    <Author>C盘清理工具</Author>\n"
        "    <Description>每日 03:00 维护重复文件 / "
        "大文件索引（--index），空闲时预先算好文件指纹，查询时直接读库。</Description>\n"
        "    <Version>1.0</Version>\n  </RegistrationInfo>\n  <Triggers>\n    <CalendarTrigger>\n"
        "      <Enabled>true</Enabled>\n      <StartBoundary>%s</StartBoundary>\n      "
        "<ScheduleByDay>\n"
        "        <DaysInterval>1</DaysInterval>\n      </ScheduleByDay>\n    </CalendarTrigger>\n  "
        "</Triggers>\n"
        "  <Principals>\n    <Principal id=\"Author\">\n      <UserId>%s</UserId>\n      "
        "<LogonType>S4U</LogonType>\n      <RunLevel>HighestAvailable</RunLevel>\n    "
        "</Principal>\n  </Principals>\n"
        "  <Settings>\n    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n    "
        "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n    "
        "<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n    "
        "<AllowStartOnDemand>true</AllowStartOnDemand>\n    "
        "<StartWhenAvailable>true</StartWhenAvailable>\n"
        "    <WakeToRun>false</WakeToRun>\n    <ExecutionTimeLimit>PT2H</ExecutionTimeLimit>\n  "
        "</Settings>\n"
        "  <Actions Context=\"Author\">\n    <Exec>\n      <Command>%s</Command>\n      "
        "<Arguments>--index --quiet</Arguments>\n    </Exec>\n  </Actions>\n</Task>\n";
    std::string xml;
    const std::string a = xml_escape(start_boundary);
    const std::string b = xml_escape(user_id);
    const std::string c = xml_escape(exe_path);
    auto p = tmpl.find("%s");
    if (p == std::string_view::npos)
        return std::string();
    xml.assign(tmpl.substr(0, p));
    xml += a;
    auto rest = tmpl.substr(p + 2);
    p = rest.find("%s");
    if (p == std::string_view::npos)
        return std::string();
    xml += rest.substr(0, p);
    xml += b;
    rest = rest.substr(p + 2);
    p = rest.find("%s");
    if (p == std::string_view::npos)
        return std::string();
    xml += rest.substr(0, p);
    xml += c;
    xml += rest.substr(p + 2);
    return xml;
}

int write_task_xml_utf16(std::string_view path, std::string_view xml_utf8) {
    std::unique_ptr<FILE, FileCloser> f;
#ifdef _WIN32
    auto wpath = utf8_to_wide(std::string(path));
    if (!wpath)
        return -1;
    f.reset(_wfopen(wpath->c_str(), L"wb"));
#else
    f.reset(std::fopen(std::string(path).c_str(), "wb"));
#endif
    if (!f)
        return -1;
    if (!put_u16(f.get(), 0xfeff))
        return -1;
    const auto *p = reinterpret_cast<const unsigned char *>(xml_utf8.data());
    size_t left = xml_utf8.size();
    while (left) {
        size_t used = 0;
        uint32_t cp = utf8_decode(p, left, used);
        if (!used)
            return -1;
        p += used;
        left -= used;
        if (cp <= 0xffff) {
            if (!put_u16(f.get(), static_cast<uint16_t>(cp)))
                return -1;
        } else {
            cp -= 0x10000;
            if (!put_u16(f.get(), static_cast<uint16_t>(0xd800 | (cp >> 10))) ||
                !put_u16(f.get(), static_cast<uint16_t>(0xdc00 | (cp & 0x3ff))))
                return -1;
        }
    }
    FILE *raw = f.release();
    return std::fclose(raw) == 0 ? 0 : -1;
}

std::string decode_task_xml(std::span<const unsigned char> bytes) {
    std::string out;
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        for (size_t i = 2; i + 1 < bytes.size();) {
            uint16_t hi = static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
            i += 2;
            uint32_t cp = hi;
            if (hi >= 0xd800 && hi <= 0xdbff && i + 1 < bytes.size()) {
                uint16_t lo = static_cast<uint16_t>(bytes[i] | (bytes[i + 1] << 8));
                if (lo >= 0xdc00 && lo <= 0xdfff) {
                    cp = 0x10000 + ((hi - 0xd800) << 10) + lo - 0xdc00;
                    i += 2;
                } else
                    cp = 0xfffd;
            } else if (hi >= 0xd800 && hi <= 0xdfff)
                cp = 0xfffd;
            append_utf8(out, cp);
        }
    } else
        out.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return out;
}

#ifdef _WIN32
std::optional<std::string> current_user_sid() {
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return std::nullopt;
    UniqueHandle token(raw);
    DWORD len = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &len);
    if (!len)
        return std::nullopt;
    std::vector<unsigned char> buf(len);
    DWORD got = 0;
    if (!::GetTokenInformation(token.get(), TokenUser, buf.data(), len, &got))
        return std::nullopt;
    LPWSTR raw_sid = nullptr;
    if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid, &raw_sid))
        return std::nullopt;
    UniqueLocal sid(raw_sid);
    return wide_to_utf8(sid.get());
}
std::optional<std::string> current_user_name() {
    const char *user = std::getenv("USERNAME");
    if (!user || !*user)
        return std::nullopt;
    const char *domain = std::getenv("USERDOMAIN");
    if (!domain || !*domain)
        domain = std::getenv("COMPUTERNAME");
    return domain && *domain ? std::string(domain) + "\\" + user : std::string(user);
}
#else
std::optional<std::string> current_user_sid() { return std::nullopt; }
std::optional<std::string> current_user_name() { return std::nullopt; }
#endif

std::optional<std::string> task_user_id() {
    if (auto sid = current_user_sid())
        return sid;
    return current_user_name();
}

#ifdef _WIN32
int register_task(std::string *msg, Error *err) {
    std::array<wchar_t, 32768> exe{};
    DWORD n = ::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!n || n >= exe.size()) {
        err_set(err, kErrIo, "取当前 exe 路径失败");
        if (msg)
            *msg = "取当前 exe 路径失败";
        return -1;
    }
    auto exe_u8 = wide_to_utf8(exe.data());
    auto user = task_user_id();
    if (!exe_u8 || !user) {
        err_set(err, kErrIo, "取当前用户身份失败");
        if (msg)
            *msg = "取当前用户身份失败";
        return -1;
    }
    std::string boundary = next_start_boundary();
    std::string xml = build_task_xml(*exe_u8, *user, boundary);
    if (xml.empty()) {
        err_set(err, kErrIo, "生成任务 XML 失败");
        return -1;
    }
    std::array<wchar_t, 32768> temp{};
    n = ::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (!n || n >= temp.size()) {
        err_set(err, kErrIo, "获取临时目录失败");
        return -1;
    }
    std::wstring tmp(temp.data(), n);
    tmp += L"cdrivecleaner-index-task.xml";
    struct TempFileCleanup {
        const std::wstring &path;
        ~TempFileCleanup() { ::DeleteFileW(path.c_str()); }
    } cleanup{tmp};
    auto tmp_u8 = wide_to_utf8(tmp);
    if (!tmp_u8 || write_task_xml_utf16(*tmp_u8, xml) != 0) {
        err_set(err, kErrIo, "写任务 XML 失败");
        return -1;
    }
    std::wstring args =
        L"/create /tn " + quote_arg(L"\\CDriveCleaner\\Index") + L" /xml " + quote_arg(tmp) + L" /f";
    std::string ok = "计划任务已注册：" + std::string(kIndexTaskName) + "（每日 03:00，" + *user +
                     "）";
    return schtasks_call(args, "注册", ok.c_str(), msg, err);
}
TaskStatus task_status() {
    TaskStatus s;
    auto r = run_schtasks_raw(L"/query /tn \"\\CDriveCleaner\\Index\" /xml");
    if (!r || r->code != 0)
        return s;
    std::string text = decode_task_xml(r->output);
    s.exists = true;
    s.enabled = text.find("<Enabled>false</Enabled>") == std::string::npos;
    return s;
}
int run_task(std::string *msg, Error *err) {
    return schtasks_call(L"/run /tn \"\\CDriveCleaner\\Index\"", "/run",
                         "已请求运行计划任务（后台执行，稍后看索引状态与日志）", msg, err);
}
int set_task_enabled(bool enabled, std::string *msg, Error *err) {
    return schtasks_call(
        enabled ? L"/change /tn \"\\CDriveCleaner\\Index\" /enable"
                : L"/change /tn \"\\CDriveCleaner\\Index\" /disable",
        "/change", enabled ? "计划任务已启用（每日 03:00）" : "计划任务已禁用（不再自动同步）", msg,
        err);
}
int delete_task(std::string *msg, Error *err) {
    return schtasks_call(L"/delete /tn \"\\CDriveCleaner\\Index\" /f", "/delete", "计划任务已删除",
                         msg, err);
}
#else
int register_task(std::string *msg, Error *err) {
    unsupported(msg, err);
    return -1;
}
TaskStatus task_status() { return TaskStatus{}; }
int run_task(std::string *msg, Error *err) {
    unsupported(msg, err);
    return -1;
}
int set_task_enabled(bool, std::string *msg, Error *err) {
    unsupported(msg, err);
    return -1;
}
int delete_task(std::string *msg, Error *err) {
    unsupported(msg, err);
    return -1;
}
#endif

int indextask_selftest() {
    int failures = 0;
    if (xml_escape("a&<>") != "a&amp;&lt;&gt;")
        ++failures;
    std::string xml = build_task_xml("C:\\a&b.exe", "S-1-5-21-1", "2026-11-18T03:00:00");
    if (xml.empty() || xml.find("<Command>C:\\a&amp;b.exe</Command>") == std::string::npos)
        ++failures;
    Ymd d = next_0300_date(2028, 2, 28, 3);
    if (d.year != 2028 || d.month != 2 || d.day != 29)
        ++failures;
    const unsigned char utf8[] = "<Enabled>false</Enabled>";
    std::string decoded = decode_task_xml(
        std::span<const unsigned char>(utf8, sizeof(utf8) - 1));
    if (decoded != "<Enabled>false</Enabled>")
        ++failures;
    return failures;
}

}  // namespace cdc
