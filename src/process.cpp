/* process.c — 移植自 Rust 版 desktop/src/process.rs + 运行命令的共享函数。 */
#include "cdc_process.h"

#include <stdlib.h>
#include <string.h>

#include "safety.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static char *lower_dup(const char *s) {
    char *out = strdup(s ? s : "");
    if (!out) return NULL;
    for (char *p = out; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + ('a' - 'A'));
    }
    return out;
}

int cdc_running_processes(char ***out_names, size_t *out_n, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out_names || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out_names = NULL;
    *out_n = 0;
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO,
                        "CreateToolhelp32Snapshot failed (%lu)",
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    cdc_vec_t names;
    if (cdc_vec_init(&names, sizeof(char *)) != CDC_OK) {
        CloseHandle(snap);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            /* szExeFile 由 API 保证 NUL 结尾。 */
            char *utf8 = cdc_wide_to_utf8(entry.szExeFile);
            if (!utf8) continue;
            /* 去首尾空白并小写（与 Rust .trim().to_lowercase() 一致）。 */
            char *s = utf8;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            size_t n = strlen(s);
            while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                             s[n - 1] == '\n' || s[n - 1] == '\r'))
                s[--n] = '\0';
            char *name = lower_dup(s);
            free(utf8);
            if (!name) continue;
            if (name[0] == '\0') {
                free(name);
                continue;
            }
            if (cdc_vec_push(&names, &name) != CDC_OK) free(name);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    *out_names = (char **)names.data;
    *out_n = names.len;
    names.data = NULL;
    names.len = names.cap = 0;
    cdc_vec_free(&names);
    return CDC_OK;
#else
    return CDC_OK; /* Linux 下返回空集 */
#endif
}

int cdc_check_running(const char **targets, size_t ntargets,
                      char ***out_running, size_t *out_n, cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (!out_running || !out_n) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "NULL out param");
        return CDC_ERR_INVALID_ARG;
    }
    *out_running = NULL;
    *out_n = 0;
    char **running = NULL;
    size_t nrunning = 0;
    int rc = cdc_running_processes(&running, &nrunning, err);
    if (rc != CDC_OK) return rc;
    cdc_vec_t hit;
    if (cdc_vec_init(&hit, sizeof(char *)) != CDC_OK) {
        cdc_strvec_free(running, nrunning);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    for (size_t i = 0; i < ntargets; i++) {
        char *t = lower_dup(targets[i]);
        if (!t) continue;
        for (size_t j = 0; j < nrunning; j++) {
            if (strcmp(t, running[j]) == 0) {
                char *dup = strdup(t);
                if (dup && cdc_vec_push(&hit, &dup) != CDC_OK) free(dup);
                break;
            }
        }
        free(t);
    }
    cdc_strvec_free(running, nrunning);
    *out_running = (char **)hit.data;
    *out_n = hit.len;
    hit.data = NULL;
    hit.len = hit.cap = 0;
    cdc_vec_free(&hit);
    return CDC_OK;
}

int cdc_kill_process_by_name(const char *target_name, size_t *out_killed,
                             cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (out_killed) *out_killed = 0;
    if (!target_name) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "target_name is NULL");
        return CDC_ERR_INVALID_ARG;
    }
#ifdef _WIN32
    char *target = lower_dup(target_name);
    if (!target) {
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    /* 去首尾空白（Rust .trim().to_lowercase()）。 */
    char *s = target;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    memmove(target, s, strlen(s) + 1);
    size_t n = strlen(target);
    while (n > 0 && (target[n - 1] == ' ' || target[n - 1] == '\t' ||
                     target[n - 1] == '\n' || target[n - 1] == '\r'))
        target[--n] = '\0';
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        free(target);
        if (err)
            cdc_err_set(err, CDC_ERR_IO,
                        "CreateToolhelp32Snapshot failed (%lu)",
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    size_t killed = 0;
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            /* szExeFile 由 API 保证 NUL 结尾。 */
            char *utf8 = cdc_wide_to_utf8(entry.szExeFile);
            if (!utf8) continue;
            char *name = lower_dup(utf8);
            free(utf8);
            if (!name) continue;
            /* trim */
            char *t2 = name;
            while (*t2 == ' ' || *t2 == '\t' || *t2 == '\n' || *t2 == '\r')
                t2++;
            memmove(name, t2, strlen(t2) + 1);
            size_t n2 = strlen(name);
            while (n2 > 0 && (name[n2 - 1] == ' ' || name[n2 - 1] == '\t' ||
                              name[n2 - 1] == '\n' || name[n2 - 1] == '\r'))
                name[--n2] = '\0';
            if (strcmp(name, target) == 0) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE,
                                       entry.th32ProcessID);
                if (h && h != INVALID_HANDLE_VALUE) {
                    if (TerminateProcess(h, 1)) killed++;
                    CloseHandle(h);
                }
            }
            free(name);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    free(target);
    if (out_killed) *out_killed = killed;
    return CDC_OK;
#else
    (void)target_name;
    return CDC_OK;
#endif
}

/* ---------------- 运行命令 ---------------- */

int cdc_run_command(const char *exe, const char *args, int capture,
                    char **out_stdout, char **out_stderr, long *exit_code,
                    cdc_err_t *err) {
    if (err) cdc_err_init(err);
    if (out_stdout) *out_stdout = NULL;
    if (out_stderr) *out_stderr = NULL;
    if (exit_code) *exit_code = -1;
    if (!exe) {
        if (err) cdc_err_set(err, CDC_ERR_INVALID_ARG, "exe is NULL");
        return CDC_ERR_INVALID_ARG;
    }
#ifdef _WIN32
    /* 命令行：可执行路径加引号 + 参数串（调用方已格式化好，如 Rust 的
     * Command::args / raw_arg 拼出的命令行）。 */
    cdc_buf_t cmd;
    cdc_buf_init(&cmd);
    cdc_buf_append_str(&cmd, "\"");
    cdc_buf_append_str(&cmd, exe);
    cdc_buf_append_str(&cmd, "\"");
    if (args && args[0]) {
        cdc_buf_append_str(&cmd, " ");
        cdc_buf_append_str(&cmd, args);
    }
    char *cmdline = cdc_buf_take_cstr(&cmd);
    if (!cmdline) {
        cdc_buf_free(&cmd);
        if (err) cdc_err_set(err, CDC_ERR_NOMEM, "out of memory");
        return CDC_ERR_NOMEM;
    }
    wchar_t *wcmd = cdc_utf8_to_wide(cmdline);
    free(cmdline);
    if (!wcmd) {
        if (err) cdc_err_set(err, CDC_ERR_UTF8, "invalid UTF-8 command");
        return CDC_ERR_UTF8;
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
            free(wcmd);
            if (err)
                cdc_err_set(err, CDC_ERR_IO, "CreatePipe failed (%lu)",
                            (unsigned long)GetLastError());
            return CDC_ERR_IO;
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
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(wcmd);
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
            cdc_err_set(err, CDC_ERR_IO, "启动进程失败：%s（Win32 %lu）", exe,
                        (unsigned long)GetLastError());
        return CDC_ERR_IO;
    }
    CloseHandle(pi.hThread);
    /* 读输出（边等边读，避免子进程写满管道阻塞）。 */
    cdc_buf_t out_buf, err_buf;
    cdc_buf_init(&out_buf);
    cdc_buf_init(&err_buf);
    if (capture) {
        uint8_t chunk[4096];
        DWORD got = 0;
        /* 先等进程退出再读：输出量小（reg/sc/powercfg 的文本输出），
         * 管道缓冲 4KB+，本模块的调用方输出均在几 KB 内；为保险起见，
         * 用 PeekNamedPipe 轮询，避免死锁。 */
        for (;;) {
            DWORD avail = 0;
            if (hOutRead &&
                PeekNamedPipe(hOutRead, NULL, 0, NULL, &avail, NULL) &&
                avail > 0) {
                DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk)
                                                   : avail;
                if (ReadFile(hOutRead, chunk, want, &got, NULL) && got > 0)
                    cdc_buf_append(&out_buf, chunk, got);
            }
            if (hErrRead &&
                PeekNamedPipe(hErrRead, NULL, 0, NULL, &avail, NULL) &&
                avail > 0) {
                DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk)
                                                   : avail;
                if (ReadFile(hErrRead, chunk, want, &got, NULL) && got > 0)
                    cdc_buf_append(&err_buf, chunk, got);
            }
            DWORD wr = WaitForSingleObject(pi.hProcess, 50);
            if (wr == WAIT_OBJECT_0) {
                /* 退出后再排空一次管道。 */
                while (hOutRead &&
                       PeekNamedPipe(hOutRead, NULL, 0, NULL, &avail, NULL) &&
                       avail > 0) {
                    DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk)
                                                       : avail;
                    if (!ReadFile(hOutRead, chunk, want, &got, NULL) ||
                        got == 0)
                        break;
                    cdc_buf_append(&out_buf, chunk, got);
                }
                while (hErrRead &&
                       PeekNamedPipe(hErrRead, NULL, 0, NULL, &avail, NULL) &&
                       avail > 0) {
                    DWORD want = avail > sizeof(chunk) ? (DWORD)sizeof(chunk)
                                                       : avail;
                    if (!ReadFile(hErrRead, chunk, want, &got, NULL) ||
                        got == 0)
                        break;
                    cdc_buf_append(&err_buf, chunk, got);
                }
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
        /* 管道输出是系统 ANSI 代码页文本：按 GBK/ANSI 解码为 UTF-8。
         * Rust 的 String::from_utf8_lossy 会把非 UTF-8 字节换成 U+FFFD；
         * 中文 Windows 的 reg.exe 输出是 GBK，直接 lossy 会乱码。
         * 这里用 MultiByteToWideChar(CP_ACP) 转 UTF-8，中文正常显示。 */
        if (out_stdout) {
            char *s = NULL;
            if (out_buf.len > 0) {
                int nw = MultiByteToWideChar(CP_ACP, 0, (const char *)out_buf.data,
                                             (int)out_buf.len, NULL, 0);
                if (nw > 0) {
                    wchar_t *w = (wchar_t *)malloc((nw + 1) * sizeof(wchar_t));
                    if (w) {
                        MultiByteToWideChar(CP_ACP, 0, (const char *)out_buf.data,
                                            (int)out_buf.len, w, nw);
                        w[nw] = 0;
                        s = cdc_wide_to_utf8(w);
                        free(w);
                    }
                }
            }
            *out_stdout = s ? s : strdup("");
        }
        if (out_stderr) {
            char *s = NULL;
            if (err_buf.len > 0) {
                int nw = MultiByteToWideChar(CP_ACP, 0, (const char *)err_buf.data,
                                             (int)err_buf.len, NULL, 0);
                if (nw > 0) {
                    wchar_t *w = (wchar_t *)malloc((nw + 1) * sizeof(wchar_t));
                    if (w) {
                        MultiByteToWideChar(CP_ACP, 0, (const char *)err_buf.data,
                                            (int)err_buf.len, w, nw);
                        w[nw] = 0;
                        s = cdc_wide_to_utf8(w);
                        free(w);
                    }
                }
            }
            *out_stderr = s ? s : strdup("");
        }
    }
    cdc_buf_free(&out_buf);
    cdc_buf_free(&err_buf);
    if (exit_code) *exit_code = (long)code;
    if (code != 0) {
        if (err)
            cdc_err_set(err, CDC_ERR_IO, "命令退出码 %lu：%s",
                        (unsigned long)code, exe);
        return CDC_ERR_IO;
    }
    return CDC_OK;
#else
    (void)exe;
    (void)args;
    (void)capture;
    if (err)
        cdc_err_set(err, CDC_ERR_IO, "cdc_run_command is Windows-only");
    return CDC_ERR_IO;
#endif
}
