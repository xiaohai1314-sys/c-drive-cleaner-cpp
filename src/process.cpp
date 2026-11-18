/* process.cpp — 进程检测与管理，移植自 Rust 版 desktop/src/process.rs
 * + 运行命令的共享函数。 */
#include "cdc_process.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "safety.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cdc {
namespace {

/* 去首尾空白并小写（与 Rust .trim().to_lowercase() 一致）。 */
std::string normalize_name(std::string_view s) {
    while (!s.empty() && std::strchr(" \t\n\r", s.front()))
        s.remove_prefix(1);
    while (!s.empty() && std::strchr(" \t\n\r", s.back()))
        s.remove_suffix(1);
    std::string out(s);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + ('a' - 'A'));
    }
    return out;
}

#ifdef _WIN32
/* 管道输出是系统 ANSI 代码页文本：按 GBK/ANSI 解码为 UTF-8。
 * Rust 的 String::from_utf8_lossy 会把非 UTF-8 字节换成 U+FFFD；
 * 中文 Windows 的 reg.exe 输出是 GBK，直接 lossy 会乱码。
 * 这里用 MultiByteToWideChar(CP_ACP) 转 UTF-8，中文正常显示。 */
std::string acp_to_utf8(const std::string &bytes) {
    if (bytes.empty())
        return std::string();
    int nw = MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), NULL, 0);
    if (nw <= 0)
        return std::string();
    std::wstring w(nw, L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes.data(), (int)bytes.size(), w.data(), nw);
    return wide_to_utf8(w).value_or(std::string());
}

/* 从管道排空剩余数据到 buf。 */
void drain_pipe(HANDLE h, std::string &buf) {
    if (!h)
        return;
    uint8_t chunk[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail == 0)
            break;
        DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
        DWORD got = 0;
        if (!ReadFile(h, chunk, want, &got, NULL) || got == 0)
            break;
        buf.append((const char *)chunk, got);
    }
}
#endif

}  // namespace

int running_processes(std::vector<std::string> *out, Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        if (err)
            err_set(err, kErrIo,
                    "CreateToolhelp32Snapshot failed (%lu)",
                    (unsigned long)GetLastError());
        return kErrIo;
    }
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            /* szExeFile 由 API 保证 NUL 结尾。 */
            auto utf8 = wide_to_utf8(entry.szExeFile);
            if (!utf8) continue;
            std::string name = normalize_name(*utf8);
            if (!name.empty())
                out->push_back(std::move(name));
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return kOk;
#else
    return kOk; /* Linux 下返回空集 */
#endif
}

int check_running(std::span<const std::string> targets, std::vector<std::string> *out,
                  Error *err) {
    if (err) err_init(err);
    if (!out) {
        if (err) err_set(err, kErrInvalidArg, "NULL out param");
        return kErrInvalidArg;
    }
    out->clear();
    std::vector<std::string> running;
    int rc = running_processes(&running, err);
    if (rc != kOk) return rc;
    for (const std::string &t : targets) {
        std::string low = normalize_name(t);
        for (const std::string &r : running) {
            if (low == r) {
                out->push_back(low);
                break;
            }
        }
    }
    return kOk;
}

int kill_process_by_name(std::string_view target_name, size_t *out_killed, Error *err) {
    if (err) err_init(err);
    if (out_killed) *out_killed = 0;
#ifdef _WIN32
    std::string target = normalize_name(target_name);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        if (err)
            err_set(err, kErrIo,
                    "CreateToolhelp32Snapshot failed (%lu)",
                    (unsigned long)GetLastError());
        return kErrIo;
    }
    size_t killed = 0;
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            /* szExeFile 由 API 保证 NUL 结尾。 */
            auto utf8 = wide_to_utf8(entry.szExeFile);
            if (!utf8) continue;
            if (normalize_name(*utf8) == target) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE,
                                       entry.th32ProcessID);
                if (h && h != INVALID_HANDLE_VALUE) {
                    if (TerminateProcess(h, 1)) killed++;
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    if (out_killed) *out_killed = killed;
    return kOk;
#else
    (void)target_name;
    return kOk;
#endif
}

/* ---------------- 运行命令 ---------------- */

int run_command(std::string_view exe, std::string_view args, bool capture,
                std::string *out_stdout, std::string *out_stderr, long *exit_code,
                Error *err) {
    if (err) err_init(err);
    if (out_stdout) out_stdout->clear();
    if (out_stderr) out_stderr->clear();
    if (exit_code) *exit_code = -1;
    if (exe.empty()) {
        if (err) err_set(err, kErrInvalidArg, "exe is empty");
        return kErrInvalidArg;
    }
#ifdef _WIN32
    /* 命令行：可执行路径加引号 + 参数串（调用方已格式化好，如 Rust 的
     * Command::args / raw_arg 拼出的命令行）。 */
    std::string cmdline = "\"" + std::string(exe) + "\"";
    if (!args.empty()) {
        cmdline += " ";
        cmdline += args;
    }
    auto wcmd = utf8_to_wide(cmdline);
    if (!wcmd) {
        if (err) err_set(err, kErrUtf8, "invalid UTF-8 command");
        return kErrUtf8;
    }
    HANDLE hOutRead = NULL, hOutWrite = NULL;
    HANDLE hErrRead = NULL, hErrWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (capture) {
        if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0) ||
            !SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0) ||
            !CreatePipe(&hErrRead, &hErrWrite, &sa, 0) ||
            !SetHandleInformation(hErrRead, HANDLE_FLAG_INHERIT, 0)) {
            if (hOutRead) CloseHandle(hOutRead);
            if (hOutWrite) CloseHandle(hOutWrite);
            if (hErrRead) CloseHandle(hErrRead);
            if (hErrWrite) CloseHandle(hErrWrite);
            if (err)
                err_set(err, kErrIo, "CreatePipe failed (%lu)",
                        (unsigned long)GetLastError());
            return kErrIo;
        }
    }
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL; /* 子进程不读 stdin（与 Rust Command 默认一致） */
    si.hStdOutput = capture ? hOutWrite : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = capture ? hErrWrite : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    /* CreateProcessW 会修改命令行缓冲，故传可写的 wcmd。 */
    BOOL ok = CreateProcessW(NULL, wcmd->data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (capture) {
        if (hOutWrite) CloseHandle(hOutWrite);
        if (hErrWrite) CloseHandle(hErrWrite);
    }
    if (!ok) {
        if (capture) {
            if (hOutRead) CloseHandle(hOutRead);
            if (hErrRead) CloseHandle(hErrRead);
        }
        if (err)
            err_set(err, kErrIo, "启动进程失败：%s（Win32 %lu）", std::string(exe).c_str(),
                    (unsigned long)GetLastError());
        return kErrIo;
    }
    CloseHandle(pi.hThread);
    /* 读输出（边等边读，避免子进程写满管道阻塞）。 */
    std::string out_buf, err_buf;
    if (capture) {
        /* 先等进程退出再读：输出量小（reg/sc/powercfg 的文本输出），
         * 管道缓冲 4KB+，本模块的调用方输出均在几 KB 内；为保险起见，
         * 用 PeekNamedPipe 轮询，避免死锁。 */
        for (;;) {
            drain_pipe(hOutRead, out_buf);
            drain_pipe(hErrRead, err_buf);
            DWORD wr = WaitForSingleObject(pi.hProcess, 50);
            if (wr == WAIT_OBJECT_0) {
                /* 退出后再排空一次管道。 */
                drain_pipe(hOutRead, out_buf);
                drain_pipe(hErrRead, err_buf);
                break;
            }
            if (wr != WAIT_TIMEOUT) break;
        }
        CloseHandle(hOutRead);
        CloseHandle(hErrRead);
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (capture) {
        if (out_stdout) *out_stdout = acp_to_utf8(out_buf);
        if (out_stderr) *out_stderr = acp_to_utf8(err_buf);
    }
    if (exit_code) *exit_code = (long)code;
    if (code != 0) {
        if (err)
            err_set(err, kErrIo, "命令退出码 %lu：%s",
                    (unsigned long)code, std::string(exe).c_str());
        return kErrIo;
    }
    return kOk;
#else
    (void)args;
    (void)capture;
    if (err)
        err_set(err, kErrIo, "cdc_run_command is Windows-only");
    return kErrIo;
#endif
}

}  // namespace cdc
