/* indextask.h — Windows 计划任务（对应 Rust 版 desktop/src/indextask.rs）
 *
 * 任务 \CDriveCleaner\Index，每日 03:00 跑 --index --quiet，当前用户 +
 * S4U + HighestAvailable。注册只在已提权启动时做；失败只记索引日志。
 *
 * 纯函数（xml_escape / next_0300_date / build_task_xml /
 * write_task_xml_utf16 / decode_task_xml）是跨平台的，Linux 下也可跑自测；
 * schtasks 调用与 SID 获取只在 _WIN32 下有真实实现，其它平台返回
 * "不支持"（task_status 返回不存在，与 Rust 调 schtasks 失败同义）。
 *
 * 依赖：util.h（cdc_err_t、CDC_OK；切片 1 提供）。错误码沿用 fileindex.h
 * 的 CDC_ERR_*（带 #ifndef 保护，两边取值一致）。
 *
 * 返回字符串的函数一律 malloc，调用方 free；msgbuf 形式的函数把说明
 * （成功或失败原因）写进调用方提供的缓冲。
 */
#ifndef CDC_INDEXTASK_H
#define CDC_INDEXTASK_H

#include <stddef.h>

#include "util.h" /* cdc_err_t、CDC_OK、错误码 enum */


/* 任务全名（/tn 用；\CDriveCleaner 文件夹由 /create 隐式创建）。 */
#define CDC_INDEX_TASK_NAME "\\CDriveCleaner\\Index"
/* 临时 XML 文件名（%TEMP% 下，用完删除）。 */
#define CDC_INDEX_TASK_XML_FILE "cdrivecleaner-index-task.xml"

/* XML 转义（exe 路径 / 用户名代入模板时用；中文路径无须转义）。
 * 只转义 & < > " 四个字符，与 Rust xml_escape 一致。返回 malloc 的串。 */
char *cdc_indextask_xml_escape(const char *s);

/* 月天数（闰年感知），供 next_0300_date 做日期进位。 */
int cdc_indextask_days_in_month(int year, int month);

/* 下一个 03:00 的日期（纯函数）：现在不到 03:00 取今天，否则取明天。
 * 输入输出均为公历（year, month, day, hour）。 */
void cdc_indextask_next_0300_date(int year, int month, int day, int hour,
                                  int *out_year, int *out_month, int *out_day);

/* StartBoundary：下一个 03:00，本地时间，无时区后缀（YYYY-MM-DDTHH:00:00
 * 固定为 03:00:00）。用未来的时刻——严格等到下一个 03:00，不立即补跑。
 * 返回 malloc 的串。 */
char *cdc_indextask_next_start_boundary(void);

/* 任务 XML（模板与 Rust build_task_xml 逐字一致）：
 * exe_path / user_id 已做 XML 转义后代入；start_boundary 为
 * "YYYY-MM-DDTHH:MM:SS"。返回 malloc 的串（UTF-8）。 */
char *cdc_indextask_build_task_xml(const char *exe_path, const char *user_id,
                                   const char *start_boundary);

/* 把 XML 写成 UTF-16LE + BOM。schtasks 要求声明与实际字节一致：
 * 声明是 UTF-16 但实际是 UTF-8 时，/create /xml 会报误导性的
 * "Access is denied"。返回 0=成功，-1=失败。 */
int cdc_indextask_write_task_xml_utf16(const char *path, const char *xml_utf8);

/* 解 schtasks /query /xml 导出的字节：UTF-16LE BOM 先按 UTF-16 解，
 * 否则按 UTF-8 解。返回 malloc 的 UTF-8 串。 */
char *cdc_indextask_decode_task_xml(const unsigned char *bytes, size_t len);

/* 当前用户的 SID 字符串（S-1-5-21-…）。用户名改名后 SID 不变，任务不会失效。
 * 失败返回 NULL（调用方回退到 DOMAIN\用户名）。仅 _WIN32 有真实实现。 */
char *cdc_indextask_current_user_sid(void);

/* 当前用户 DOMAIN\用户名（S4U 注册的回退身份；本机账户的 DOMAIN 是计算机名）。
 * 取不到时返回 NULL。 */
char *cdc_indextask_current_user_name(void);

/* 注册用的运行身份：优先 SID（改名不失效），回退 DOMAIN\用户名。 */
char *cdc_indextask_task_user_id(void);

/* 注册（幂等重注册）：写 UTF-16 XML 到 %TEMP%，schtasks /create /tn … /xml … /f，
 * 删临时文件。调用方只在已提权时调用——非提权进程注册 Highest 任务直接报
 * 0x80070005。/f 整体替换已有任务定义，不需要先 /delete。
 * 返回 0=成功（msg 为成功说明），-1=失败（msg/err 说明原因）。
 * ⚠️ S4U + Highest 是否真提权仍需真机验证：注册后点"运行计划任务"，
 * 读 logs/index-YYYYMMDD.log，确认走 MFT 路径且 exit 0。 */
int cdc_indextask_register(char *msg, size_t cap, cdc_err_t *err);

/* 任务状态。返回 0；*exists=任务已注册，*enabled=启用中。
 * /query 退出码非 0 即任务不存在；导出 XML 里出现
 * "<Enabled>false</Enabled>" 即被禁用（模板里的触发器 Enabled 恒为 true，
 * false 只可能来自 /change /disable）。 */
int cdc_indextask_task_status(int *exists, int *enabled);

/* 手动跑一次：立即执行，忽略触发器时间表，但仍用任务里保存的程序路径、
 * 账户和权限。exit 0 只表示"已接受运行请求"，不表示跑完了——跑完与否
 * 看索引日志或游标时间。这也是 S4U 提权的验证入口。 */
int cdc_indextask_run_task(char *msg, size_t cap, cdc_err_t *err);

/* 启用 / 禁用（enabled 非 0=启用）。禁用后任务不再按触发器运行。 */
int cdc_indextask_set_task_enabled(int enabled, char *msg, size_t cap, cdc_err_t *err);

/* 删除任务（卸载流程用；UI 不提供按钮）。
 * 注意：删除任务不会删除 \CDriveCleaner 文件夹。 */
int cdc_indextask_delete_task(char *msg, size_t cap, cdc_err_t *err);

/* 自测：与 Rust indextask.rs 的 #[cfg(test)] 逐项对应
 * （xml_escape、build_task_xml 字段与转义、next_0300_date 各进位、
 * decode_task_xml 两种编码），外加 write_task_xml_utf16 的字节级
 * 回读校验。返回失败断言数，0=全过。 */
int cdc_indextask_selftest(void);

#endif
