/* cdc_process.h — 进程检测与管理，移植自 Rust 版 desktop/src/process.rs。
 * 注意：原名 process.h 会遮住 CRT 的 <process.h>（-Isrc 下 sqlite3.c
 * 的 MinGW 编译报 _beginthreadex 未声明），已改名 cdc_process.h。
 * 另含一个通用的“运行系统命令并等待”函数（system.c / privacy.c /
 * im.c 共用：reg.exe、sc.exe、net.exe、powercfg、Dism、powershell、
 * cmd.exe 一律走 %SystemRoot%\System32 的绝对路径，CREATE_NO_WINDOW）。
 */
#ifndef CDC_PROCESS_H
#define CDC_PROCESS_H

#include <stddef.h>

#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 当前所有正在运行的进程名集合（统一小写，如 "wechat.exe"）。
 * Linux 下返回空集。 */
int cdc_running_processes(char ***out_names, size_t *out_n, cdc_err_t *err);

/* 检查指定的进程名列表中哪些正在运行（targets 会被小写化）。
 * 返回命中的进程名（小写）。 */
int cdc_check_running(const char **targets, size_t ntargets,
                      char ***out_running, size_t *out_n, cdc_err_t *err);

/* 按可执行文件名关闭进程（如 "wechat.exe"），返回杀掉的数量。
 * Linux 下返回 0。 */
int cdc_kill_process_by_name(const char *target_name, size_t *out_killed,
                             cdc_err_t *err);

/* 运行 exe（UTF-8 路径）+ 已格式化好的参数串（调用方负责引号，如
 * "/h off" 或 "mklink /J \"a\" \"b\""），CREATE_NO_WINDOW，等待退出。
 * capture 非 0 时捕获 stdout/stderr（UTF-8 lossy，调用方 free()）。
 * 返回 CDC_OK 且 *exit_code==0 表示成功；否则返回错误码，
 * *exit_code 为进程退出码（未能启动时为 -1）。
 * Linux 下返回 CDC_ERR_IO（仅供 Windows 代码路径使用）。 */
int cdc_run_command(const char *exe, const char *args, int capture,
                    char **out_stdout, char **out_stderr, long *exit_code,
                    cdc_err_t *err);

/* 注册 hooks 的回调类型见 cdc_hooks.h；本模块不使用进度回调。 */

#ifdef __cplusplus
}
#endif

#endif /* CDC_PROCESS_H */
