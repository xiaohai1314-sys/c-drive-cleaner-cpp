// indextask.h — Windows 计划任务（对应 Rust 版 desktop/src/indextask.rs）
//
// 任务 \CDriveCleaner\Index，每日 03:00 跑 --index --quiet，当前用户 +
// S4U + HighestAvailable。注册只在已提权启动时做；失败只记索引日志。
//
// 纯函数（xml_escape / next_0300_date / build_task_xml /
// write_task_xml_utf16 / decode_task_xml）是跨平台的，Linux 下也可跑自测；
// schtasks 调用与 SID 获取只在 _WIN32 下有真实实现，其它平台返回
// "不支持"（task_status 返回不存在，与 Rust 调 schtasks 失败同义）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "util.h"

namespace cdc {

/* 任务全名（/tn 用；\CDriveCleaner 文件夹由 /create 隐式创建）。 */
inline constexpr std::string_view kIndexTaskName = "\\CDriveCleaner\\Index";
/* 临时 XML 文件名（%TEMP% 下，用完删除）。 */
inline constexpr std::string_view kIndexTaskXmlFile = "cdrivecleaner-index-task.xml";

/* XML 转义（exe 路径 / 用户名代入模板时用；中文路径无须转义）。
 * 只转义 & < > " 四个字符，与 Rust xml_escape 一致。 */
std::string xml_escape(std::string_view s);

/* 月天数（闰年感知），供 next_0300_date 做日期进位。 */
int days_in_month(int year, int month);

struct Ymd {
    int year = 0;
    int month = 0;
    int day = 0;
};

/* 下一个 03:00 的日期（纯函数）：现在不到 03:00 取今天，否则取明天。 */
Ymd next_0300_date(int year, int month, int day, int hour);

/* StartBoundary：下一个 03:00，本地时间，无时区后缀（YYYY-MM-DDTHH:00:00
 * 固定为 03:00:00）。用未来的时刻——严格等到下一个 03:00，不立即补跑。 */
std::string next_start_boundary();

/* 任务 XML（模板与 Rust build_task_xml 逐字一致）：
 * exe_path / user_id 已做 XML 转义后代入；start_boundary 为
 * "YYYY-MM-DDTHH:MM:SS"。 */
std::string build_task_xml(std::string_view exe_path, std::string_view user_id,
                           std::string_view start_boundary);

/* 把 XML 写成 UTF-16LE + BOM。schtasks 要求声明与实际字节一致：
 * 声明是 UTF-16 但实际是 UTF-8 时，/create /xml 会报误导性的
 * "Access is denied"。返回 0=成功，-1=失败。 */
int write_task_xml_utf16(std::string_view path, std::string_view xml_utf8);

/* 解 schtasks /query /xml 导出的字节：UTF-16LE BOM 先按 UTF-16 解，
 * 否则按 UTF-8 解。 */
std::string decode_task_xml(std::span<const unsigned char> bytes);

/* 当前用户的 SID 字符串（S-1-5-21-…）。用户名改名后 SID 不变，任务不会失效。
 * 失败返回 nullopt（调用方回退到 DOMAIN\用户名）。仅 _WIN32 有真实实现。 */
std::optional<std::string> current_user_sid();

/* 当前用户 DOMAIN\用户名（S4U 注册的回退身份；本机账户的 DOMAIN 是计算机名）。
 * 取不到时返回 nullopt。 */
std::optional<std::string> current_user_name();

/* 注册用的运行身份：优先 SID（改名不失效），回退 DOMAIN\用户名。 */
std::optional<std::string> task_user_id();

/* 注册（幂等重注册）：写 UTF-16 XML 到 %TEMP%，schtasks /create /tn … /xml … /f，
 * 删临时文件。调用方只在已提权时调用——非提权进程注册 Highest 任务直接报
 * 0x80070005。/f 整体替换已有任务定义，不需要先 /delete。
 * 返回 0=成功（msg 为成功说明），-1=失败（msg/err 说明原因）。
 * ⚠️ S4U + Highest 是否真提权仍需真机验证：注册后点"运行计划任务"，
 * 读 logs/index-YYYYMMDD.log，确认走 MFT 路径且 exit 0。 */
int register_task(std::string *msg, Error *err);

struct TaskStatus {
    bool exists = false;
    bool enabled = false;
};

/* 任务状态。/query 退出码非 0 即任务不存在；导出 XML 里出现
 * "<Enabled>false</Enabled>" 即被禁用（模板里的触发器 Enabled 恒为 true，
 * false 只可能来自 /change /disable）。 */
TaskStatus task_status();

/* 手动跑一次：立即执行，忽略触发器时间表，但仍用任务里保存的程序路径、
 * 账户和权限。exit 0 只表示"已接受运行请求"，不表示跑完了——跑完与否
 * 看索引日志或游标时间。这也是 S4U 提权的验证入口。 */
int run_task(std::string *msg, Error *err);

/* 启用 / 禁用。禁用后任务不再按触发器运行。 */
int set_task_enabled(bool enabled, std::string *msg, Error *err);

/* 删除任务（卸载流程用；UI 不提供按钮）。
 * 注意：删除任务不会删除 \CDriveCleaner 文件夹。 */
int delete_task(std::string *msg, Error *err);

/* 自测：与 Rust indextask.rs 的 #[cfg(test)] 逐项对应
 * （xml_escape、build_task_xml 字段与转义、next_0300_date 各进位、
 * decode_task_xml 两种编码），外加 write_task_xml_utf16 的字节级
 * 回读校验。返回失败断言数，0=全过。 */
int indextask_selftest();

}  // namespace cdc
