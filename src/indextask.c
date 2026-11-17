/* indextask.c — Windows 计划任务（对应 Rust 版 desktop/src/indextask.rs）
 *
 * 模板、消息文案、状态判定（<Enabled>false</Enabled>）与 Rust 版逐字一致。
 * 两处有意差别：
 *   - schtasks 用 %SystemRoot%\System32 下的绝对路径调用（Rust 用裸名；
 *     仓库 main.rs 的 system32() 惯例要求绝对路径，防 PATH 劫持）；
 *   - write_task_xml_utf16 在 Windows 下走 _wfopen（%TEMP% 里可能有中文
 *     用户名，CRT fopen 的 ANSI 路径不可靠）。
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "indextask.h"

#ifdef _WIN32
#include <windows.h>
#include <sddl.h> /* ConvertSidToStringSidW */
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* ---------- XML 转义 ---------- */

char *cdc_indextask_xml_escape(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) + 1, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (const char *p = s; *p; p++) {
        const char *rep = NULL;
        size_t rlen = 0;
        switch (*p) {
        case '&': rep = "&amp;"; rlen = 5; break;
        case '<': rep = "&lt;"; rlen = 4; break;
        case '>': rep = "&gt;"; rlen = 4; break;
        case '"': rep = "&quot;"; rlen = 6; break;
        default: break;
        }
        if (rep) {
            if (len + rlen + 1 > cap) {
                cap = (len + rlen + 1) * 2;
                char *np = realloc(out, cap);
                if (!np) {
                    free(out);
                    return NULL;
                }
                out = np;
            }
            memcpy(out + len, rep, rlen);
            len += rlen;
        } else {
            if (len + 2 > cap) {
                cap *= 2;
                char *np = realloc(out, cap);
                if (!np) {
                    free(out);
                    return NULL;
                }
                out = np;
            }
            out[len++] = *p;
        }
    }
    out[len] = '\0';
    return out;
}

/* ---------- 日期 ---------- */

int cdc_indextask_days_in_month(int year, int month) {
    switch (month) {
    case 1: case 3: case 5: case 7: case 8: case 10: case 12:
        return 31;
    case 4: case 6: case 9: case 11:
        return 30;
    case 2:
        if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) return 29;
        return 28;
    default:
        return 30;
    }
}

void cdc_indextask_next_0300_date(int year, int month, int day, int hour,
                                  int *out_year, int *out_month, int *out_day) {
    int y = year, m = month, d = day;
    if (hour >= 3) {
        d += 1;
        if (d > cdc_indextask_days_in_month(y, m)) {
            d = 1;
            m += 1;
            if (m > 12) {
                m = 1;
                y += 1;
            }
        }
    }
    if (out_year) *out_year = y;
    if (out_month) *out_month = m;
    if (out_day) *out_day = d;
}

char *cdc_indextask_next_start_boundary(void) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    int y = 1900, mo = 1, d = 1, h = 0;
    if (lt) {
        y = lt->tm_year + 1900;
        mo = lt->tm_mon + 1;
        d = lt->tm_mday;
        h = lt->tm_hour;
    }
    int oy = y, om = mo, od = d;
    cdc_indextask_next_0300_date(y, mo, d, h, &oy, &om, &od);
    /* 下面的 %04d/%02d 最多输出 19 字符 + NUL = 20 字节；显式收紧取值
     * 区间，让 -Wformat-truncation 能证明不截断（值本身已是合法日期）。 */
    if (oy < 0) oy = 0;
    if (oy > 9999) oy = 9999;
    if (om < 1) om = 1;
    if (om > 12) om = 12;
    if (od < 1) od = 1;
    if (od > 31) od = 31;
    char *out = malloc(20);
    if (!out) return NULL;
    snprintf(out, 20, "%04d-%02d-%02dT03:00:00", oy, om, od);
    return out;
}

/* ---------- 任务 XML 模板 ---------- */

char *cdc_indextask_build_task_xml(const char *exe_path, const char *user_id,
                                   const char *start_boundary) {
    static const char *TMPL =
        "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        "<Task version=\"1.3\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        "  <RegistrationInfo>\n"
        "    <Author>C盘清理工具</Author>\n"
        "    <Description>每日 03:00 维护重复文件 / 大文件索引（--index），空闲时预先算好文件指纹，查询时直接读库。</Description>\n"
        "    <Version>1.0</Version>\n"
        "  </RegistrationInfo>\n"
        "  <Triggers>\n"
        "    <CalendarTrigger>\n"
        "      <Enabled>true</Enabled>\n"
        "      <StartBoundary>%s</StartBoundary>\n"
        "      <ScheduleByDay>\n"
        "        <DaysInterval>1</DaysInterval>\n"
        "      </ScheduleByDay>\n"
        "    </CalendarTrigger>\n"
        "  </Triggers>\n"
        "  <Principals>\n"
        "    <Principal id=\"Author\">\n"
        "      <UserId>%s</UserId>\n"
        "      <LogonType>S4U</LogonType>\n"
        "      <RunLevel>HighestAvailable</RunLevel>\n"
        "    </Principal>\n"
        "  </Principals>\n"
        "  <Settings>\n"
        "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        "    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
        "    <StartWhenAvailable>true</StartWhenAvailable>\n"
        "    <WakeToRun>false</WakeToRun>\n"
        "    <ExecutionTimeLimit>PT2H</ExecutionTimeLimit>\n"
        "  </Settings>\n"
        "  <Actions Context=\"Author\">\n"
        "    <Exec>\n"
        "      <Command>%s</Command>\n"
        "      <Arguments>--index --quiet</Arguments>\n"
        "    </Exec>\n"
        "  </Actions>\n"
        "</Task>\n";
    char *esc_user = cdc_indextask_xml_escape(user_id ? user_id : "");
    char *esc_exe = cdc_indextask_xml_escape(exe_path ? exe_path : "");
    if (!esc_user || !esc_exe) {
        free(esc_user);
        free(esc_exe);
        return NULL;
    }
    int need = snprintf(NULL, 0, TMPL, start_boundary ? start_boundary : "",
                        esc_user, esc_exe);
    char *out = malloc((size_t)need + 1);
    if (out) {
        snprintf(out, (size_t)need + 1, TMPL, start_boundary ? start_boundary : "",
                 esc_user, esc_exe);
    }
    free(esc_user);
    free(esc_exe);
    return out;
}

/* ---------- UTF-8 / UTF-16 编解码（可移植实现；写 XML 与解导出都用它） ---------- */

/* 解一个 UTF-8 码点；非法时返回 U+FFFD 并消费 1 字节。 */
static uint32_t utf8_decode(const unsigned char *s, size_t len, size_t *consumed) {
    if (len == 0) {
        *consumed = 0;
        return 0xFFFD;
    }
    unsigned char c = s[0];
    if (c < 0x80) {
        *consumed = 1;
        return c;
    }
    int n = 0;
    uint32_t cp = 0;
    uint32_t min = 0;
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        cp = c & 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        cp = c & 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        cp = c & 0x07;
        min = 0x10000;
    } else {
        *consumed = 1;
        return 0xFFFD;
    }
    if (len < (size_t)n) {
        *consumed = 1;
        return 0xFFFD;
    }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *consumed = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *consumed = 1;
        return 0xFFFD;
    }
    *consumed = (size_t)n;
    return cp;
}

/* 把码点编成 UTF-8，写进 out（至少 4 字节空间），返回字节数。 */
static size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int write_u16le(FILE *f, uint16_t u) {
    return fputc(u & 0xFF, f) == EOF || fputc((u >> 8) & 0xFF, f) == EOF ? -1 : 0;
}

int cdc_indextask_write_task_xml_utf16(const char *path, const char *xml_utf8) {
    if (!path || !xml_utf8) return -1;
    FILE *f = NULL;
#ifdef _WIN32
    /* %TEMP% 里可能有中文用户名，CRT fopen 的 ANSI 路径不可靠，走宽字符。 */
    wchar_t *w = cdc_utf8_to_wide(path);
    if (!w) return -1;
    f = _wfopen(w, L"wb");
    free(w);
#else
    f = fopen(path, "wb");
#endif
    if (!f) return -1;
    int rc = 0;
    if (fputc(0xFF, f) == EOF || fputc(0xFE, f) == EOF) rc = -1;
    const unsigned char *p = (const unsigned char *)xml_utf8;
    size_t left = strlen(xml_utf8);
    while (rc == 0 && left > 0) {
        size_t used = 0;
        uint32_t cp = utf8_decode(p, left, &used);
        if (used == 0) break;
        p += used;
        left -= used;
        if (cp < 0x10000) {
            if (write_u16le(f, (uint16_t)cp) != 0) rc = -1;
        } else {
            uint32_t v = cp - 0x10000;
            if (write_u16le(f, (uint16_t)(0xD800 + (v >> 10))) != 0 ||
                write_u16le(f, (uint16_t)(0xDC00 + (v & 0x3FF))) != 0)
                rc = -1;
        }
    }
    if (fclose(f) != 0) rc = -1;
    return rc;
}

char *cdc_indextask_decode_task_xml(const unsigned char *bytes, size_t len) {
    if (!bytes) return strdup("");
    if (len >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        size_t cap = len + 1, n = 0;
        char *out = malloc(cap);
        if (!out) return NULL;
        size_t i = 2;
        while (i + 1 < len) {
            uint16_t u = (uint16_t)(bytes[i] | (bytes[i + 1] << 8));
            i += 2;
            uint32_t cp = u;
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
                uint16_t lo = (uint16_t)(bytes[i] | (bytes[i + 1] << 8));
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((uint32_t)(u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                } else {
                    cp = 0xFFFD;
                }
            } else if (u >= 0xD800 && u <= 0xDFFF) {
                cp = 0xFFFD;
            }
            char enc[4];
            size_t m = utf8_encode(cp, enc);
            if (n + m + 1 > cap) {
                cap = (n + m + 1) * 2;
                char *np = realloc(out, cap);
                if (!np) {
                    free(out);
                    return NULL;
                }
                out = np;
            }
            memcpy(out + n, enc, m);
            n += m;
        }
        out[n] = '\0';
        return out;
    }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, bytes, len);
    out[len] = '\0';
    return out;
}

/* ---------- 当前用户身份 ---------- */

#ifdef _WIN32

/* 任务名（宽字符版，与 CDC_INDEX_TASK_NAME 同值；schtasks 参数用）。 */
#define CDC_TASK_NAME_W L"\\CDriveCleaner\\Index"

char *cdc_indextask_current_user_sid(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) || !token)
        return NULL;
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &len);
    char *sid_str = NULL;
    if (len > 0) {
        BYTE *buf = malloc(len);
        if (buf) {
            DWORD got = 0;
            if (GetTokenInformation(token, TokenUser, buf, len, &got)) {
                TOKEN_USER *tu = (TOKEN_USER *)buf;
                if (tu->User.Sid) {
                    LPWSTR out = NULL;
                    if (ConvertSidToStringSidW(tu->User.Sid, &out) && out) {
                        sid_str = cdc_wide_to_utf8(out);
                        LocalFree(out);
                    }
                }
            }
            free(buf);
        }
    }
    CloseHandle(token);
    return sid_str;
}

char *cdc_indextask_current_user_name(void) {
    const char *user = getenv("USERNAME");
    if (!user || !*user) return NULL;
    const char *domain = getenv("USERDOMAIN");
    if (!domain || !*domain) domain = getenv("COMPUTERNAME");
    if (!domain || !*domain) return strdup(user);
    size_t need = strlen(domain) + strlen(user) + 2;
    char *out = malloc(need);
    if (!out) return NULL;
    snprintf(out, need, "%s\\%s", domain, user);
    return out;
}

#else /* 非 Windows：没有 SID / schtasks，全部回退为空 */

char *cdc_indextask_current_user_sid(void) { return NULL; }

char *cdc_indextask_current_user_name(void) { return NULL; }

#endif

char *cdc_indextask_task_user_id(void) {
    char *sid = cdc_indextask_current_user_sid();
    if (sid) return sid;
    return cdc_indextask_current_user_name();
}

/* ---------- schtasks 封装（仅 _WIN32） ---------- */

#ifdef _WIN32

/* System32 下 schtasks.exe 的绝对路径（宽字符）。 */
static wchar_t *schtasks_path(void) {
    wchar_t root[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"SystemRoot", root, MAX_PATH);
    wchar_t *base = NULL;
    if (n > 0 && n < MAX_PATH) {
        base = _wcsdup(root);
    } else {
        base = _wcsdup(L"C:\\Windows");
    }
    if (!base) return NULL;
    size_t need = wcslen(base) + wcslen(L"\\System32\\schtasks.exe") + 1;
    wchar_t *out = malloc(need * sizeof(wchar_t));
    if (!out) {
        free(base);
        return NULL;
    }
    swprintf(out, need, L"%s\\System32\\schtasks.exe", base);
    free(base);
    return out;
}

/* 跑一次 schtasks。args 为宽字符参数串（不含程序名）。
 * 成功启动并等到退出返回 0，*exit_code 为进程退出码；
 * out_bytes 与 out_len 为合并后的 stdout+stderr 原始字节（调用方 free）。
 * 决策只看退出码，不解析输出文本（中文系统上输出是 GBK）。 */
static int run_schtasks_raw(const wchar_t *args, unsigned char **out_bytes,
                            size_t *out_len, DWORD *exit_code) {
    *out_bytes = NULL;
    *out_len = 0;
    *exit_code = 0;
    wchar_t *exe = schtasks_path();
    if (!exe) return -1;
    size_t cmdlen = wcslen(exe) + wcslen(args) + 4;
    wchar_t *cmd = malloc(cmdlen * sizeof(wchar_t));
    if (!cmd) {
        free(exe);
        return -1;
    }
    swprintf(cmd, cmdlen, L"\"%s\" %s", exe, args);
    free(exe);

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        free(cmd);
        return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                             &si, &pi);
    free(cmd);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    /* 输出很小（schtasks 只有几行），等退出后再读完即可。 */
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    size_t cap = 4096, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) {
        CloseHandle(rd);
        return -1;
    }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            unsigned char *np = realloc(buf, cap);
            if (!np) {
                free(buf);
                CloseHandle(rd);
                return -1;
            }
            buf = np;
        }
        DWORD got = 0;
        if (!ReadFile(rd, buf + len, (DWORD)(cap - len), &got, NULL) || got == 0) break;
        len += got;
    }
    CloseHandle(rd);
    *out_bytes = buf;
    *out_len = len;
    return 0;
}

/* 取输出的首个非空行（按 \r / \n 分行，两端去空白），写进 msg。
 * 与 Rust run_schtasks 的 first-line 逻辑一致。 */
static void first_line_msg(const unsigned char *bytes, size_t len, char *msg, size_t cap) {
    if (cap == 0) return;
    msg[0] = '\0';
    if (!bytes || len == 0) return;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && bytes[j] != '\r' && bytes[j] != '\n') j++;
        size_t a = i, b = j;
        while (a < b && (bytes[a] == ' ' || bytes[a] == '\t')) a++;
        while (b > a && (bytes[b - 1] == ' ' || bytes[b - 1] == '\t')) b--;
        if (b > a) {
            size_t m = b - a;
            if (m >= cap) m = cap - 1;
            memcpy(msg, bytes + a, m);
            msg[m] = '\0';
            return;
        }
        i = j;
        while (i < len && (bytes[i] == '\r' || bytes[i] == '\n')) i++;
    }
}

/* 通用 schtasks 调用：成功返回 0 并把成功说明写进 msg；
 * 失败返回 -1，msg/err 写 "schtasks <动作>失败：<首行>" 或 "（无输出）"。 */
static int schtasks_call(const wchar_t *args, const char *action_zh,
                         const char *ok_msg, char *msg, size_t cap, cdc_err_t *err) {
    unsigned char *out = NULL;
    size_t out_len = 0;
    DWORD code = 0;
    if (run_schtasks_raw(args, &out, &out_len, &code) != 0) {
        char m[512];
        snprintf(m, sizeof(m), "schtasks %s失败：启动 schtasks 失败", action_zh);
        if (msg && cap) snprintf(msg, cap, "%s", m);
        cdc_err_set(err, CDC_ERR_IO, "%s", m);
        return -1;
    }
    if (code == 0) {
        free(out);
        if (msg && cap) snprintf(msg, cap, "%s", ok_msg);
        if (err) cdc_err_init(err);
        return 0;
    }
    char first[1024];
    first_line_msg(out, out_len, first, sizeof(first));
    free(out);
    char m[1280];
    if (first[0]) {
        snprintf(m, sizeof(m), "schtasks %s失败：%s", action_zh, first);
    } else {
        snprintf(m, sizeof(m), "schtasks %s失败（无输出）", action_zh);
    }
    if (msg && cap) snprintf(msg, cap, "%s", m);
    cdc_err_set(err, CDC_ERR_IO, "%s", m);
    return -1;
}

static wchar_t *temp_xml_path(void) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return NULL;
    size_t need = wcslen(dir) + strlen(CDC_INDEX_TASK_XML_FILE) + 1;
    wchar_t *out = malloc(need * sizeof(wchar_t));
    if (!out) return NULL;
    swprintf(out, need, L"%s%hs", dir, CDC_INDEX_TASK_XML_FILE);
    return out;
}

int cdc_indextask_register(char *msg, size_t cap, cdc_err_t *err) {
    wchar_t exe_w[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe_w, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        const char *m = "取当前 exe 路径失败";
        if (msg && cap) snprintf(msg, cap, "%s", m);
        cdc_err_set(err, CDC_ERR_IO, "%s", m);
        return -1;
    }
    char *exe_path = cdc_wide_to_utf8(exe_w);
    char *user_id = cdc_indextask_task_user_id();
    if (!exe_path || !user_id) {
        const char *m = "取当前用户身份失败";
        free(exe_path);
        free(user_id);
        if (msg && cap) snprintf(msg, cap, "%s", m);
        cdc_err_set(err, CDC_ERR_IO, "%s", m);
        return -1;
    }
    char *boundary = cdc_indextask_next_start_boundary();
    char *xml = cdc_indextask_build_task_xml(exe_path, user_id, boundary ? boundary : "");
    free(boundary);
    if (!xml) {
        const char *m = "生成任务 XML 失败";
        free(exe_path);
        free(user_id);
        if (msg && cap) snprintf(msg, cap, "%s", m);
        cdc_err_set(err, CDC_ERR_IO, "%s", m);
        return -1;
    }
    wchar_t *tmp_w = temp_xml_path();
    char *tmp_u8 = tmp_w ? cdc_wide_to_utf8(tmp_w) : NULL;
    int rc = -1;
    if (tmp_u8 && cdc_indextask_write_task_xml_utf16(tmp_u8, xml) == 0) {
        size_t arglen = wcslen(tmp_w) + 128;
        wchar_t *args = malloc(arglen * sizeof(wchar_t));
        if (args) {
            swprintf(args, arglen, L"/create /tn \"%s\" /xml \"%s\" /f",
                     CDC_TASK_NAME_W, tmp_w);
            char ok[1024];
            snprintf(ok, sizeof(ok), "计划任务已注册：%s（每日 03:00，%s）",
                     CDC_INDEX_TASK_NAME, user_id);
            rc = schtasks_call(args, "注册", ok, msg, cap, err);
            free(args);
        } else {
            const char *m = "schtasks 注册失败：分配参数失败";
            if (msg && cap) snprintf(msg, cap, "%s", m);
            cdc_err_set(err, CDC_ERR_IO, "%s", m);
        }
    } else {
        const char *m = "写任务 XML 失败";
        if (msg && cap) snprintf(msg, cap, "%s", m);
        cdc_err_set(err, CDC_ERR_IO, "%s", m);
    }
    if (tmp_w) DeleteFileW(tmp_w);
    free(tmp_w);
    free(tmp_u8);
    free(xml);
    free(exe_path);
    free(user_id);
    return rc;
}

int cdc_indextask_task_status(int *exists, int *enabled) {
    if (exists) *exists = 0;
    if (enabled) *enabled = 0;
    unsigned char *out = NULL;
    size_t out_len = 0;
    DWORD code = 0;
    wchar_t args[128];
    swprintf(args, 128, L"/query /tn \"%s\" /xml", CDC_TASK_NAME_W);
    if (run_schtasks_raw(args, &out, &out_len, &code) != 0 || code != 0) {
        free(out);
        return 0;
    }
    char *text = cdc_indextask_decode_task_xml(out, out_len);
    free(out);
    if (!text) return 0;
    if (exists) *exists = 1;
    if (enabled) *enabled = strstr(text, "<Enabled>false</Enabled>") == NULL;
    free(text);
    return 0;
}

int cdc_indextask_run_task(char *msg, size_t cap, cdc_err_t *err) {
    wchar_t args[128];
    swprintf(args, 128, L"/run /tn \"%s\"", CDC_TASK_NAME_W);
    return schtasks_call(args, "/run",
                         "已请求运行计划任务（后台执行，稍后看索引状态与日志）",
                         msg, cap, err);
}

int cdc_indextask_set_task_enabled(int enabled, char *msg, size_t cap, cdc_err_t *err) {
    wchar_t args[160];
    swprintf(args, 160, L"/change /tn \"%s\" %s", CDC_TASK_NAME_W,
             enabled ? L"/enable" : L"/disable");
    return schtasks_call(args, "/change",
                         enabled ? "计划任务已启用（每日 03:00）"
                                 : "计划任务已禁用（不再自动同步）",
                         msg, cap, err);
}

int cdc_indextask_delete_task(char *msg, size_t cap, cdc_err_t *err) {
    wchar_t args[128];
    swprintf(args, 128, L"/delete /tn \"%s\" /f", CDC_TASK_NAME_W);
    return schtasks_call(args, "/delete", "计划任务已删除", msg, cap, err);
}

#else /* 非 Windows */

int cdc_indextask_register(char *msg, size_t cap, cdc_err_t *err) {
    const char *m = "计划任务仅 Windows 支持";
    if (msg && cap) snprintf(msg, cap, "%s", m);
    cdc_err_set(err, CDC_ERR_IO, "%s", m);
    return -1;
}

int cdc_indextask_task_status(int *exists, int *enabled) {
    if (exists) *exists = 0;
    if (enabled) *enabled = 0;
    return 0;
}

int cdc_indextask_run_task(char *msg, size_t cap, cdc_err_t *err) {
    const char *m = "计划任务仅 Windows 支持";
    if (msg && cap) snprintf(msg, cap, "%s", m);
    cdc_err_set(err, CDC_ERR_IO, "%s", m);
    return -1;
}

int cdc_indextask_set_task_enabled(int enabled, char *msg, size_t cap, cdc_err_t *err) {
    (void)enabled;
    const char *m = "计划任务仅 Windows 支持";
    if (msg && cap) snprintf(msg, cap, "%s", m);
    cdc_err_set(err, CDC_ERR_IO, "%s", m);
    return -1;
}

int cdc_indextask_delete_task(char *msg, size_t cap, cdc_err_t *err) {
    const char *m = "计划任务仅 Windows 支持";
    if (msg && cap) snprintf(msg, cap, "%s", m);
    cdc_err_set(err, CDC_ERR_IO, "%s", m);
    return -1;
}

#endif

/* ---------- 自测（与 Rust indextask.rs #[cfg(test)] 逐项对应） ---------- */

static int it_failures = 0;
static int it_checks = 0;

#define IT_CHECK(cond, ...) do { \
    it_checks++; \
    if (!(cond)) { it_failures++; printf("FAIL indextask: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int contains(const char *hay, const char *needle) {
    return hay && needle && strstr(hay, needle) != NULL;
}

static void test_xml_escape(void) {
    char *e = cdc_indextask_xml_escape("C:\\a&b<c>d\"e");
    IT_CHECK(e && strcmp(e, "C:\\a&amp;b&lt;c&gt;d&quot;e") == 0, "xml_escape specials");
    free(e);
    e = cdc_indextask_xml_escape("C:\\盘清理工具\\app.exe");
    IT_CHECK(e && strcmp(e, "C:\\盘清理工具\\app.exe") == 0, "xml_escape chinese passthrough");
    free(e);
}

static void test_build_task_xml(void) {
    char *xml = cdc_indextask_build_task_xml(
        "C:\\Program Files\\C盘清理工具\\c-drive-cleaner.exe",
        "S-1-5-21-1-2-3-1001", "2026-11-18T03:00:00");
    IT_CHECK(xml != NULL, "build xml non-null");
    if (!xml) return;
    const char *decl = "<?xml version=\"1.0\" encoding=\"UTF-16\"?>";
    IT_CHECK(strncmp(xml, decl, strlen(decl)) == 0, "xml decl prefix");
    IT_CHECK(contains(xml, "<Task version=\"1.3\""), "xml task version");
    IT_CHECK(contains(xml, "<StartBoundary>2026-11-18T03:00:00</StartBoundary>"),
             "xml start boundary");
    IT_CHECK(contains(xml, "<UserId>S-1-5-21-1-2-3-1001</UserId>"), "xml user id");
    IT_CHECK(contains(xml, "<LogonType>S4U</LogonType>"), "xml S4U");
    IT_CHECK(contains(xml, "<RunLevel>HighestAvailable</RunLevel>"), "xml highest");
    IT_CHECK(contains(xml, "<Command>C:\\Program Files\\C盘清理工具\\c-drive-cleaner.exe</Command>"),
             "xml command");
    IT_CHECK(contains(xml, "<Arguments>--index --quiet</Arguments>"), "xml arguments");
    IT_CHECK(contains(xml, "<ExecutionTimeLimit>PT2H</ExecutionTimeLimit>"), "xml time limit");
    IT_CHECK(contains(xml, "<StartWhenAvailable>true</StartWhenAvailable>"),
             "xml start when available");
    IT_CHECK(contains(xml, "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"),
             "xml batteries");
    IT_CHECK(!contains(xml, "{{"), "xml no placeholders left");
    free(xml);

    xml = cdc_indextask_build_task_xml("C:\\a&b\\app.exe", "PC\\user",
                                       "2026-11-18T03:00:00");
    IT_CHECK(xml && contains(xml, "<Command>C:\\a&amp;b\\app.exe</Command>"),
             "xml escapes exe path");
    IT_CHECK(xml && contains(xml, "<UserId>PC\\user</UserId>"), "xml user id plain");
    free(xml);
}

static void test_next_0300(void) {
    int y, m, d;
    cdc_indextask_next_0300_date(2026, 11, 17, 2, &y, &m, &d);
    IT_CHECK(y == 2026 && m == 11 && d == 17, "0300 before 3am today");
    cdc_indextask_next_0300_date(2026, 11, 17, 0, &y, &m, &d);
    IT_CHECK(y == 2026 && m == 11 && d == 17, "0300 midnight today");
    cdc_indextask_next_0300_date(2026, 11, 17, 3, &y, &m, &d);
    IT_CHECK(y == 2026 && m == 11 && d == 18, "0300 at 3am tomorrow");
    cdc_indextask_next_0300_date(2026, 11, 17, 23, &y, &m, &d);
    IT_CHECK(y == 2026 && m == 11 && d == 18, "0300 late tomorrow");
    cdc_indextask_next_0300_date(2026, 11, 30, 5, &y, &m, &d);
    IT_CHECK(y == 2026 && m == 12 && d == 1, "0300 month rollover");
    cdc_indextask_next_0300_date(2026, 12, 31, 5, &y, &m, &d);
    IT_CHECK(y == 2027 && m == 1 && d == 1, "0300 year rollover");
    cdc_indextask_next_0300_date(2028, 2, 28, 5, &y, &m, &d);
    IT_CHECK(y == 2028 && m == 2 && d == 29, "0300 leap feb 28");
    cdc_indextask_next_0300_date(2028, 2, 29, 5, &y, &m, &d);
    IT_CHECK(y == 2028 && m == 3 && d == 1, "0300 leap feb 29");
    cdc_indextask_next_0300_date(2027, 2, 28, 5, &y, &m, &d);
    IT_CHECK(y == 2027 && m == 3 && d == 1, "0300 non-leap feb");
    cdc_indextask_next_0300_date(2000, 2, 28, 5, &y, &m, &d);
    IT_CHECK(y == 2000 && m == 2 && d == 29, "0300 year 2000 leap");
    cdc_indextask_next_0300_date(2100, 2, 28, 5, &y, &m, &d);
    IT_CHECK(y == 2100 && m == 3 && d == 1, "0300 year 2100 not leap");
}

static void test_decode_xml(void) {
    const char *xml = "<Task><Description>每日</Description><Enabled>false</Enabled></Task>";
    size_t ulen = strlen(xml);
    unsigned char *bytes = malloc(2 + ulen * 2);
    IT_CHECK(bytes != NULL, "decode alloc");
    if (!bytes) return;
    bytes[0] = 0xFF;
    bytes[1] = 0xFE;
    /* 手工 UTF-8 -> UTF-16LE（与 write 函数不同的路径，避免同错同对）。 */
    size_t bi = 2;
    for (size_t i = 0; i < ulen;) {
        unsigned char c = (unsigned char)xml[i];
        uint32_t cp;
        size_t adv;
        if (c < 0x80) {
            cp = c;
            adv = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = ((c & 0x1F) << 6) | ((unsigned char)xml[i + 1] & 0x3F);
            adv = 2;
        } else {
            cp = ((c & 0x0F) << 12) | (((unsigned char)xml[i + 1] & 0x3F) << 6) |
                 ((unsigned char)xml[i + 2] & 0x3F);
            adv = 3;
        }
        bytes[bi++] = (unsigned char)(cp & 0xFF);
        bytes[bi++] = (unsigned char)((cp >> 8) & 0xFF);
        i += adv;
    }
    char *dec = cdc_indextask_decode_task_xml(bytes, bi);
    IT_CHECK(dec && strcmp(dec, xml) == 0, "decode utf16 roundtrip incl. chinese");
    free(dec);
    free(bytes);
    dec = cdc_indextask_decode_task_xml((const unsigned char *)xml, ulen);
    IT_CHECK(dec && strcmp(dec, xml) == 0, "decode utf8 passthrough");
    free(dec);
}

static void test_write_xml(void) {
    char *xml = cdc_indextask_build_task_xml(
        "C:\\Program Files\\C盘清理工具\\app.exe", "S-1-5-21-1001",
        "2026-11-18T03:00:00");
    IT_CHECK(xml != NULL, "write test xml non-null");
    if (!xml) return;
    char path[4096];
#ifdef _WIN32
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    IT_CHECK(n > 0 && n < sizeof(tmp), "temp path");
    if (n == 0 || n >= sizeof(tmp)) {
        free(xml);
        return;
    }
    snprintf(path, sizeof(path), "%scdc-it-test.xml", tmp);
#else
    snprintf(path, sizeof(path), "/tmp/cdc-it-test-%ld.xml", (long)getpid());
#endif
    IT_CHECK(cdc_indextask_write_task_xml_utf16(path, xml) == 0, "write utf16 xml");
    FILE *f = fopen(path, "rb");
    IT_CHECK(f != NULL, "read back xml file");
    if (!f) {
        free(xml);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *bytes = malloc((size_t)(sz > 0 ? sz : 1));
    size_t got = bytes ? fread(bytes, 1, (size_t)sz, f) : 0;
    fclose(f);
    IT_CHECK(sz > 2 && bytes[0] == 0xFF && bytes[1] == 0xFE, "xml has utf16le bom");
    IT_CHECK(got == (size_t)sz, "xml read full");
    char *dec = cdc_indextask_decode_task_xml(bytes, got);
    IT_CHECK(dec && strcmp(dec, xml) == 0, "xml write/decode roundtrip");
    free(dec);
    free(bytes);
    remove(path);
    free(xml);
}

int cdc_indextask_selftest(void) {
    it_failures = 0;
    it_checks = 0;
    test_xml_escape();
    test_build_task_xml();
    test_next_0300();
    test_decode_xml();
    test_write_xml();
    printf("indextask selftest: %d checks, %d failures\n", it_checks, it_failures);
    return it_failures;
}
