// cdc_bridge.h — WebView2 原生 COM 消息桥（切片 5：前端桥接）+ 宿主入口。
//
// 协议（与 docs/bridge-commands.md 一致）：
//   JS -> C : chrome.webview.postMessage(JSON.stringify({ id, cmd, args }))
//   C  -> JS: window.__cdc_onMessage({ id, ok: true, data }) /
//             window.__cdc_onMessage({ id, ok: false, error })
//
// 线程模型：WebMessageReceived 回调在 UI 线程触发；命令分发到工作线程执行
// （扫描/索引等长命令不能堵 UI 线程），结果经 PostMessage 回到 UI 线程
// 再 ExecuteScript。host.cpp 的 WndProc 收到 WM_CDC_BRIDGE_REPLY 后调
// cdc_bridge_flush_replies()。
//
// 所有权：handler 返回的 *out_json / *out_error 由 malloc 分配（全局
// 替换为 mimalloc，见 Makefile / CMakeLists 的 mimalloc 一节），桥接层
// 用 mi_free() 释放；调用方不要用 new 分配。
// 命令表容量 CDC_BRIDGE_MAX_COMMANDS（64），够放 45 个 Tauri 命令 +
// window_* 宿主命令；注册满了返回 -1。
//
// 本头可从 C 包含（main.c 调 cdc_host_prewarm / cdc_host_run /
// cdc_bridge_register）；用到 ICoreWebView2* 的声明只在 C++ 下可见，
// WebView2.h 来自 Microsoft.Web.WebView2 NuGet 包，见 host/CMakeLists.txt。

#pragma once

#include <windows.h>

#ifdef __cplusplus
#include <WebView2.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 后端命令处理函数：args_json 是 args 对象的 JSON 文本（保证是合法 JSON，
// 无参命令传 "{}"）；成功时 *out_json 填返回值 JSON 文本，返回 0；
// 失败时 *out_error 填人类可读的错误字符串，返回非 0。
// 只填其中一个，另一个保持 NULL。
typedef int (*cdc_command_handler)(const char *args_json, char **out_json,
                                    char **out_error);

#define CDC_BRIDGE_MAX_COMMANDS 64

// 桥回复消息：host.cpp 的 WndProc 收到后调 cdc_bridge_flush_replies()。
// WM_APP + 0xDC（WM_APP 段合法范围是 0x8000–0xBFFF，别越界）。
#define WM_CDC_BRIDGE_REPLY (WM_APP + 0xDC)

// 注册一个命令。返回 0 成功；-1 表满；-2 名字重复。
int cdc_bridge_register(const char *cmd, cdc_command_handler handler);

// 宿主入口（host.cpp，extern "C"，C 侧 main.c 直接调用）：
//   cdc_host_prewarm() — 尽早启动 WebView2 环境创建（异步），与后端
//     初始化重叠；返回 0 成功，非 0 为 COM 初始化或环境创建提交失败。
//   cdc_host_run(instance, cmd_show) — 建窗口、跑消息循环，返回进程退出码。
//     调用前须完成后端初始化 + cdc_bridge_register() 全部后端命令。
int cdc_host_prewarm(void);
int cdc_host_run(HINSTANCE instance, int cmd_show);

// 供单测/宿主调试：解析信封。id < 0 表示解析失败。
// cmd_out / args_out 由调用方提供缓冲区；args_out 收到 args 值的原始
// JSON 文本（对象/数组/标量均可），含首尾空白。
// 返回 0 成功；-1 缺 id；-2 缺 cmd；-3 args 不是合法 JSON 值。
int cdc_bridge_parse_envelope(const char *json, long *id_out, char *cmd_out,
                              size_t cmd_cap, char *args_out, size_t args_cap);

#ifdef __cplusplus
// 以下声明用到 ICoreWebView2*，只在 C++ 下可见。

// 只信任应用自身的 https://cdc.local origin；路径可变，端口/凭据/其他
// host 一律拒绝。返回 1 表示可信，0 表示不可信。
int cdc_host_is_trusted_origin(const wchar_t *uri);

// 处理一条前端发来的 postMessage（UTF-16 JSON 文本）。
// 只是解析信封并把执行排到工作线程；回复异步到达。
// 返回 0 表示已受理（哪怕命令未知——未知命令会异步回 error）；
// 返回非 0 表示信封本身无法解析（连 id 都拿不到），消息被丢弃。
int cdc_bridge_handle_message(ICoreWebView2 *webview, HWND hwnd,
                              const wchar_t *message_json);

// C -> JS 事件推送（无需 id；前端 bridge.ts 的 onEvent 接收）。
// data_json 是事件数据的 JSON 文本，可为 NULL（表示 null）。
// 只能在 UI 线程调用（host 内部调用点都在 UI 线程）。
void cdc_bridge_push_event(ICoreWebView2 *webview, const char *event,
                           const char *data_json);

// 排出待回复队列：在 UI 线程调用，每条回复走一次 ExecuteScript。
// 由 WndProc 在 WM_CDC_BRIDGE_REPLY 时调用。
void cdc_bridge_flush_replies(ICoreWebView2 *webview);

// 建一个 WebMessageReceived 回调对象（host.cpp 注册到 WebView2 上）。
// 返回的对象引用计数为 1；add_WebMessageReceived 成功后调用方 Release。
// 需要 <WebView2.h>。
ICoreWebView2WebMessageReceivedEventHandler *
cdc_bridge_create_message_handler(ICoreWebView2 *webview, HWND hwnd);

#endif /* __cplusplus — COM 类型声明结束 */

#ifdef __cplusplus
}
#endif
