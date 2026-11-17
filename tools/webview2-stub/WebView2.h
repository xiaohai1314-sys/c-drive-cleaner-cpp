// tools/webview2-stub/WebView2.h — WebView2 SDK 的最小桩头文件。
//
// !!! 仅用于没有 NuGet 包 Microsoft.Web.WebView2 时做交叉编译验证 !!!
// 不是真 SDK：接口只声明了 host/ 用到的方法，vtable 顺序与真头不同，
// 链接出的 exe 不能运行；CreateCoreWebView2EnvironmentWithOptions 是返回
// E_NOTIMPL 的 inline 桩。正式构建（含 CI）一律用 NuGet 包里的
// build/native/include/WebView2.h。
//
// 方法名、参数类型、IID 与真头（SDK 1.0.x 的 WebView2.h）一致，
// 保证 host 代码在真头下也能编过（真头编译另见 README 的验证记录）。
#pragma once

#include <objidl.h>
#include <unknwn.h>
#include <windows.h>

#ifndef __eventtoken_h__
#define __eventtoken_h__
typedef struct EventRegistrationToken {
    __int64 value;
} EventRegistrationToken;
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define CDC_WV2_IFACE(name, guid) struct __declspec(uuid(guid)) __declspec(novtable) name
#else
#define CDC_WV2_IFACE(name, guid) struct name
#endif

typedef enum COREWEBVIEW2_WEB_RESOURCE_CONTEXT {
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL = 0
} COREWEBVIEW2_WEB_RESOURCE_CONTEXT;

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;
struct ICoreWebView2EnvironmentOptions;
struct ICoreWebView2WebResourceResponse;

CDC_WV2_IFACE(ICoreWebView2Settings, "e562e4f0-d7fa-43ac-8d71-c05150499f00") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_AreDevToolsEnabled(BOOL *enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AreDevToolsEnabled(BOOL enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AreDefaultContextMenusEnabled(BOOL *enabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AreDefaultContextMenusEnabled(BOOL enabled) = 0;
};

CDC_WV2_IFACE(ICoreWebView2HttpRequestHeaders, "e86cac0e-5523-465c-b536-8fb9fc8c8c60") : public IUnknown {};

CDC_WV2_IFACE(ICoreWebView2WebResourceRequest, "97055cd4-512c-4264-8b5f-e3f446cea6a5") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR *uri) = 0;
};

CDC_WV2_IFACE(ICoreWebView2WebResourceResponse, "aafcc94f-fa27-48fd-97df-830ef75aaec9") : public IUnknown {};

CDC_WV2_IFACE(ICoreWebView2WebResourceRequestedEventArgs, "453e667f-12c7-49d4-be6d-ddbe7956f57a") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Request(ICoreWebView2WebResourceRequest **request) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Response(ICoreWebView2WebResourceResponse **response) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Response(ICoreWebView2WebResourceResponse *response) = 0;
};

CDC_WV2_IFACE(ICoreWebView2WebMessageReceivedEventArgs, "0f99a40c-e962-4207-9e92-e3d542eff849") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Source(LPWSTR *source) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_WebMessageAsJson(LPWSTR *webMessageAsJson) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetWebMessageAsString(LPWSTR *webMessageAsString) = 0;
};

CDC_WV2_IFACE(ICoreWebView2NavigationStartingEventArgs, "5b495469-e119-438a-9b18-7604f25f2e49") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR *uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsUserInitiated(BOOL *isUserInitiated) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRedirected(BOOL *isRedirected) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_RequestHeaders(ICoreWebView2HttpRequestHeaders **requestHeaders) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Cancel(BOOL *cancel) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Cancel(BOOL cancel) = 0;
};

CDC_WV2_IFACE(ICoreWebView2NewWindowRequestedEventArgs, "34acb11c-fc37-4418-9132-f9c21d1eafb9") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Uri(LPWSTR *uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_NewWindow(ICoreWebView2 *newWindow) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_NewWindow(ICoreWebView2 **newWindow) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Handled(BOOL handled) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Handled(BOOL *handled) = 0;
};

CDC_WV2_IFACE(ICoreWebView2WebMessageReceivedEventHandler, "57213f19-00e6-49fa-8e07-898ea01ecbd2") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) = 0;
};
CDC_WV2_IFACE(ICoreWebView2WebResourceRequestedEventHandler, "ab00b74c-15f1-4646-80e8-e76341d25d71") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2WebResourceRequestedEventArgs *args) = 0;
};
CDC_WV2_IFACE(ICoreWebView2NavigationStartingEventHandler, "9adbe429-f36d-432b-9ddc-f8881fbd76e3") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) = 0;
};
CDC_WV2_IFACE(ICoreWebView2NewWindowRequestedEventHandler, "d4c185fe-c81c-4989-97af-2d3fa7ab5651") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args) = 0;
};
CDC_WV2_IFACE(ICoreWebView2ExecuteScriptCompletedHandler, "49511172-cc67-4bca-9923-137112f4c4cc") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR resultObjectAsJson) = 0;
};
CDC_WV2_IFACE(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, "6c4819f3-c9b7-4260-8127-c9f5bde7f68c") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Controller *createdController) = 0;
};
CDC_WV2_IFACE(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, "4e8a3389-c9d8-4bd2-b6b5-124fee6cc14d") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, ICoreWebView2Environment *createdEnvironment) = 0;
};

CDC_WV2_IFACE(ICoreWebView2, "76eceacb-0462-4d94-ac83-423a6793775e") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Settings(ICoreWebView2Settings **settings) = 0;
    virtual HRESULT STDMETHODCALLTYPE Navigate(LPCWSTR uri) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NavigationStarting(ICoreWebView2NavigationStartingEventHandler *eventHandler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NavigationStarting(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_NewWindowRequested(ICoreWebView2NewWindowRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_NewWindowRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_WebMessageReceived(ICoreWebView2WebMessageReceivedEventHandler *handler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_WebMessageReceived(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_WebResourceRequested(ICoreWebView2WebResourceRequestedEventHandler *eventHandler, EventRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_WebResourceRequested(EventRegistrationToken token) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddWebResourceRequestedFilter(LPCWSTR uri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT resourceContext) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteScript(LPCWSTR javaScript, ICoreWebView2ExecuteScriptCompletedHandler *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE PostWebMessageAsJson(LPCWSTR webMessageAsJson) = 0;
};

CDC_WV2_IFACE(ICoreWebView2Controller, "4d00c0d1-9434-4eb6-8078-8697a560334f") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_IsVisible(BOOL *isVisible) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsVisible(BOOL isVisible) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Bounds(RECT *bounds) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Bounds(RECT bounds) = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_CoreWebView2(ICoreWebView2 **coreWebView2) = 0;
};

CDC_WV2_IFACE(ICoreWebView2Environment, "b96d755e-0319-4e92-a296-23436f46a1fc") : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateCoreWebView2Controller(HWND parentWindow, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWebResourceResponse(IStream *content, int statusCode, LPCWSTR reasonPhrase, LPCWSTR headers, ICoreWebView2WebResourceResponse **response) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BrowserVersionString(LPWSTR *versionInfo) = 0;
};

#if defined(__MINGW32__) && defined(__CRT_UUID_DECL)
__CRT_UUID_DECL(ICoreWebView2NavigationStartingEventHandler, 0x9adbe429, 0xf36d, 0x432b, 0x9d, 0xdc, 0xf8, 0x88, 0x1f, 0xbd, 0x76, 0xe3)
__CRT_UUID_DECL(ICoreWebView2NewWindowRequestedEventHandler, 0xd4c185fe, 0xc81c, 0x4989, 0x97, 0xaf, 0x2d, 0x3f, 0xa7, 0xab, 0x56, 0x51)
#endif

// 桩：真 SDK 由 WebView2Loader 提供。这里直接返回 E_NOTIMPL，
// 让桩头构建也能链接成功（exe 不可用于运行）。
inline HRESULT CreateCoreWebView2EnvironmentWithOptions(
    PCWSTR /*browserExecutableFolder*/, PCWSTR /*userDataFolder*/,
    ICoreWebView2EnvironmentOptions * /*environmentOptions*/,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler * /*environmentCreatedHandler*/) {
    return E_NOTIMPL;
}
