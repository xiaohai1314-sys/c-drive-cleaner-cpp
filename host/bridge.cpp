// cdc_bridge.cpp — WebView2 原生 COM 消息桥实现（切片 5）。
//
// 只依赖 Win32 + WebView2 SDK（<WebView2.h> 来自 Microsoft.Web.WebView2
// NuGet 包；见 host/CMakeLists.txt）。不依赖任何第三方 JSON 库：信封解析
// 是手写的字符串感知扫描（只抽 id / cmd / args 三个键），回复的 data
// 直接嵌入后端返回的 JSON 文本，不再二次解析。

#include "bridge.h"
#include "security.h"

#include <WebView2.h>

#ifdef __MINGW32__
#include "mingw_uuids.h"
#endif

#include <stdlib.h>
#include <string.h>

// mimalloc（性能优化项 6）：桥只显式调 mi_*，不用 -include
// mimalloc-override.h 的宏覆盖（C++ 翻译单元里宏覆盖会干扰标准库头文件）。
#include "mimalloc.h"

#include <mutex>
#include <new>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// 命令表
// ---------------------------------------------------------------------------

namespace {

struct CommandEntry {
  std::string cmd;
  cdc_command_handler handler;
};

std::mutex g_table_mutex;
std::vector<CommandEntry> g_commands;

cdc_command_handler find_handler(const std::string &cmd) {
  std::lock_guard<std::mutex> lock(g_table_mutex);
  for (const auto &entry : g_commands) {
    if (entry.cmd == cmd) return entry.handler;
  }
  return nullptr;
}

}  // namespace

extern "C" int cdc_host_is_trusted_origin(const wchar_t *uri) {
  return uri && cdc::host::is_trusted_origin(uri) ? 1 : 0;
}

extern "C" int cdc_bridge_register(const char *cmd, cdc_command_handler handler) {
  if (!cmd || !*cmd || !handler) return -1;
  std::lock_guard<std::mutex> lock(g_table_mutex);
  for (const auto &entry : g_commands) {
    if (entry.cmd == cmd) return -2;
  }
  if (g_commands.size() >= CDC_BRIDGE_MAX_COMMANDS) return -1;
  g_commands.push_back(CommandEntry{cmd, handler});
  return 0;
}

// ---------------------------------------------------------------------------
// UTF-8 / UTF-16 转换（桥内部用；后端 util.h 落地后可合过去）
// ---------------------------------------------------------------------------

namespace {

std::string wide_to_utf8(const wchar_t *wstr) {
  if (!wstr) return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return std::string();
  std::string out(static_cast<size_t>(len), '\0');
  int written = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
  if (written != len) return std::string();
  out.resize(static_cast<size_t>(len - 1));
  return out;
}

std::wstring utf8_to_wide(const std::string &str) {
  if (str.empty()) return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                                nullptr, 0);
  if (len <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                      out.data(), len);
  return out;
}

// JSON 字符串转义（只用于 error 文本；data 是后端已生成的 JSON，不转义）。
std::string json_escape(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 2);
  for (char ch : in) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)ch < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)ch);
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 信封解析：{"id": 12, "cmd": "scan_big", "args": {...}}
// 只做一层扫描：按字符串感知跳过值，不做完整 JSON 解析。
// ---------------------------------------------------------------------------

namespace {

// 跳过空白，返回新下标。
size_t skip_ws(const char *s, size_t i, size_t n) {
  while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  return i;
}

// 跳过一个 JSON 字符串（s[i] 必须是 '"'），返回字符串结束引号之后的位置；
// 失败返回 npos。
size_t skip_string(const char *s, size_t i, size_t n) {
  if (i >= n || s[i] != '"') return std::string::npos;
  ++i;
  while (i < n) {
    if (s[i] == '\\') {
      i += 2;
    } else if (s[i] == '"') {
      return i + 1;
    } else {
      ++i;
    }
  }
  return std::string::npos;
}

// 把 s[i..] 的 JSON 字符串解码进 out（处理 \" \\ \/ \b \f \n \r \t \uXXXX）。
// 返回字符串结束引号之后的位置；失败返回 npos。
size_t parse_string(const char *s, size_t i, size_t n, std::string *out) {
  if (i >= n || s[i] != '"') return std::string::npos;
  ++i;
  out->clear();
  while (i < n) {
    char ch = s[i];
    if (ch == '"') return i + 1;
    if (ch == '\\') {
      if (i + 1 >= n) return std::string::npos;
      char esc = s[i + 1];
      switch (esc) {
        case '"': out->push_back('"'); i += 2; break;
        case '\\': out->push_back('\\'); i += 2; break;
        case '/': out->push_back('/'); i += 2; break;
        case 'b': out->push_back('\b'); i += 2; break;
        case 'f': out->push_back('\f'); i += 2; break;
        case 'n': out->push_back('\n'); i += 2; break;
        case 'r': out->push_back('\r'); i += 2; break;
        case 't': out->push_back('\t'); i += 2; break;
        case 'u': {
          if (i + 5 >= n) return std::string::npos;
          unsigned code = 0;
          for (int k = 0; k < 4; ++k) {
            char h = s[i + 2 + k];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
            else return std::string::npos;
          }
          // BMP 内直接转 UTF-8；代理对（surrogate pair）不在信封键里出现，
          // 出现则按 U+FFFD 处理，不让解析失败。
          if (code >= 0xD800 && code <= 0xDFFF) {
            *out += "\xEF\xBF\xBD";
          } else if (code < 0x80) {
            out->push_back((char)code);
          } else if (code < 0x800) {
            out->push_back((char)(0xC0 | (code >> 6)));
            out->push_back((char)(0x80 | (code & 0x3F)));
          } else {
            out->push_back((char)(0xE0 | (code >> 12)));
            out->push_back((char)(0x80 | ((code >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (code & 0x3F)));
          }
          i += 6;
          break;
        }
        default: return std::string::npos;
      }
    } else {
      out->push_back(ch);
      ++i;
    }
  }
  return std::string::npos;
}

// 跳过一个 JSON 值（对象/数组/字符串/数字/字面量），返回其后位置；
// 失败返回 npos。
size_t skip_value(const char *s, size_t i, size_t n) {
  i = skip_ws(s, i, n);
  if (i >= n) return std::string::npos;
  char ch = s[i];
  if (ch == '"') return skip_string(s, i, n);
  if (ch == '{' || ch == '[') {
    char open = ch;
    char close = (ch == '{') ? '}' : ']';
    ++i;
    i = skip_ws(s, i, n);
    if (i < n && s[i] == close) return i + 1;
    while (i < n) {
      if (open == '{') {
        // key
        i = skip_string(s, skip_ws(s, i, n), n);
        if (i == std::string::npos) return std::string::npos;
        i = skip_ws(s, i, n);
        if (i >= n || s[i] != ':') return std::string::npos;
        ++i;
      }
      i = skip_value(s, i, n);
      if (i == std::string::npos) return std::string::npos;
      i = skip_ws(s, i, n);
      if (i < n && s[i] == ',') { ++i; continue; }
      if (i < n && s[i] == close) return i + 1;
      return std::string::npos;
    }
    return std::string::npos;
  }
  // 数字 / true / false / null：读到分隔符为止。
  while (i < n && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
         s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') {
    ++i;
  }
  return i;
}

}  // namespace

extern "C" int cdc_bridge_parse_envelope(const char *json, long *id_out,
                                         char *cmd_out, size_t cmd_cap,
                                         char *args_out, size_t args_cap) {
  if (!json || !id_out || !cmd_out || cmd_cap == 0 || !args_out || args_cap == 0) {
    return -1;
  }
  const size_t n = strlen(json);
  size_t i = skip_ws(json, 0, n);
  if (i >= n || json[i] != '{') return -1;
  ++i;

  bool have_id = false;
  bool have_cmd = false;
  long id = -1;
  std::string cmd;
  std::string args = "{}";  // args 缺省按空对象处理。

  i = skip_ws(json, i, n);
  if (i < n && json[i] == '}') {
    return -1;  // 空对象：既无 id 也无 cmd。
  }
  while (i < n) {
    std::string key;
    i = parse_string(json, skip_ws(json, i, n), n, &key);
    if (i == std::string::npos) return -1;
    i = skip_ws(json, i, n);
    if (i >= n || json[i] != ':') return -1;
    ++i;
    i = skip_ws(json, i, n);
    if (key == "id") {
      // id 只接受 JSON 数字（前端 bridge.ts 发自增整数）。
      char *end = nullptr;
      long value = strtol(json + i, &end, 10);
      if (end == json + i) return -1;
      id = value;
      have_id = true;
      i = static_cast<size_t>(end - json);
    } else if (key == "cmd") {
      i = parse_string(json, i, n, &cmd);
      if (i == std::string::npos) return -1;
      have_cmd = true;
    } else if (key == "args") {
      size_t begin = i;
      i = skip_value(json, i, n);
      if (i == std::string::npos) return -3;
      args.assign(json + begin, i - begin);
    } else {
      i = skip_value(json, i, n);
      if (i == std::string::npos) return -1;
    }
    i = skip_ws(json, i, n);
    if (i < n && json[i] == ',') { ++i; continue; }
    if (i < n && json[i] == '}') break;
    return -1;
  }

  if (!have_id) return -1;
  if (!have_cmd) return -2;
  *id_out = id;
  if (cmd.size() >= cmd_cap) return -2;
  if (!cmd.empty()) memcpy(cmd_out, cmd.data(), cmd.size());
  cmd_out[cmd.size()] = '\0';
  if (args.size() >= args_cap) return -3;
  if (!args.empty()) memcpy(args_out, args.data(), args.size());
  args_out[args.size()] = '\0';
  return 0;
}

// ---------------------------------------------------------------------------
// 回复队列：工作线程生产，UI 线程消费（ExecuteScript 必须在 UI 线程）。
// ---------------------------------------------------------------------------

namespace {

std::mutex g_reply_mutex;
std::queue<std::wstring> g_replies;  // 已拼好的 JS 调用文本。

void enqueue_reply(HWND hwnd, const std::string &js_utf8) {
  {
    std::lock_guard<std::mutex> lock(g_reply_mutex);
    g_replies.push(utf8_to_wide(js_utf8));
  }
  PostMessageW(hwnd, WM_CDC_BRIDGE_REPLY, 0, 0);
}

std::string build_reply(long id, bool ok, const char *data_json,
                        const char *error) {
  std::string out = "window.__cdc_onMessage({\"id\":";
  out += std::to_string(id);
  out += ",\"ok\":";
  out += ok ? "true" : "false";
  if (ok) {
    out += ",\"data\":";
    out += data_json ? data_json : "null";
  } else {
    out += ",\"error\":\"";
    out += json_escape(error ? error : "unknown error");
    out += "\"";
  }
  out += "})";
  return out;
}

void dispatch_and_reply(HWND hwnd, long id, const std::string &cmd,
                        const std::string &args_json) {
  cdc_command_handler handler = find_handler(cmd);
  if (!handler) {
    std::string error = "unknown command: " + cmd;
    enqueue_reply(hwnd, build_reply(id, false, nullptr, error.c_str()));
    return;
  }
  char *out_json = nullptr;
  char *out_error = nullptr;
  int rc = handler(args_json.c_str(), &out_json, &out_error);
  if (rc == 0) {
    enqueue_reply(hwnd, build_reply(id, true, out_json, nullptr));
  } else {
    enqueue_reply(hwnd, build_reply(id, false, nullptr,
                                    out_error ? out_error : "command failed"));
  }
  // 后端返回的串由 malloc（全局替换为 mimalloc）分配，这里必须用 mi_free。
  mi_free(out_json);
  mi_free(out_error);
}

}  // namespace

extern "C" int cdc_bridge_handle_message(ICoreWebView2 *webview, HWND hwnd,
                                         const wchar_t *message_json) {
  (void)webview;  // 回复经由队列 + flush，不直接持有 webview。
  if (!hwnd || !message_json) return -1;
  std::string json = wide_to_utf8(message_json);
  long id = -1;
  char cmd[128];
  // args 可能很大（delete_dups 的 items 列表），给 1MB 上限。
  std::vector<char> args_buf(1024 * 1024);
  int rc = cdc_bridge_parse_envelope(json.c_str(), &id, cmd, sizeof(cmd),
                                     args_buf.data(), args_buf.size());
  if (rc != 0) return -1;
  std::string cmd_str(cmd);
  std::string args_str(args_buf.data());
  // 工作线程执行；回复经 WM_CDC_BRIDGE_REPLY 回到 UI 线程。
  std::thread([hwnd, id, cmd_str, args_str]() {
    dispatch_and_reply(hwnd, id, cmd_str, args_str);
  }).detach();
  return 0;
}

extern "C" void cdc_bridge_push_event(ICoreWebView2 *webview, const char *event,
                                      const char *data_json) {
  if (!webview || !event) return;
  std::string js = "window.__cdc_onMessage({\"event\":\"";
  js += json_escape(event);
  js += "\",\"data\":";
  js += data_json ? data_json : "null";
  js += "})";
  webview->ExecuteScript(utf8_to_wide(js).c_str(), nullptr);
}

extern "C" void cdc_bridge_flush_replies(ICoreWebView2 *webview) {
  if (!webview) return;
  for (;;) {
    std::wstring js;
    {
      std::lock_guard<std::mutex> lock(g_reply_mutex);
      if (g_replies.empty()) return;
      js = std::move(g_replies.front());
      g_replies.pop();
    }
    webview->ExecuteScript(js.c_str(), nullptr);
  }
}

// ---------------------------------------------------------------------------
// WebMessageReceived 回调对象：host.cpp 注册到 WebView2 上。
// ---------------------------------------------------------------------------

namespace {

class WebMessageHandler final
    : public ICoreWebView2WebMessageReceivedEventHandler {
 public:
  WebMessageHandler(ICoreWebView2 *webview, HWND hwnd)
      : ref_(1), webview_(webview), hwnd_(hwnd) {
    if (webview_) webview_->AddRef();
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(ICoreWebView2WebMessageReceivedEventHandler)) {
      *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --ref_;
    if (count == 0) delete this;
    return count;
  }

  // ICoreWebView2WebMessageReceivedEventHandler
  HRESULT STDMETHODCALLTYPE Invoke(
      ICoreWebView2 *sender,
      ICoreWebView2WebMessageReceivedEventArgs *args) override {
    (void)sender;
    if (!args) return S_OK;
    LPWSTR source = nullptr;
    HRESULT source_hr = args->get_Source(&source);
    const bool trusted = SUCCEEDED(source_hr) && source &&
                         cdc_host_is_trusted_origin(source) != 0;
    if (source) CoTaskMemFree(source);
    if (!trusted) return S_OK;

    LPWSTR json = nullptr;
    if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
      cdc_bridge_handle_message(webview_, hwnd_, json);
      CoTaskMemFree(json);
    }
    return S_OK;
  }

 private:
  ULONG ref_;
  ICoreWebView2 *webview_;
  HWND hwnd_;
};

}  // namespace

extern "C" ICoreWebView2WebMessageReceivedEventHandler *
cdc_bridge_create_message_handler(ICoreWebView2 *webview, HWND hwnd) {
  return new (std::nothrow) WebMessageHandler(webview, hwnd);
}
