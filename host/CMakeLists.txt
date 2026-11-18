# host/CMakeLists.txt — WebView2 宿主（C++20，仅 Windows）。
#
# 顶层 CMake 用 add_subdirectory(host) 接入，并提供：
#   CDC_WEBVIEW2_INCLUDE_DIR — WebView2 SDK 的 include 目录（NuGet 包
#     Microsoft.Web.WebView2 的 build/native/include，含 WebView2.h）。
#   CDC_WEBVIEW2_LOADER_LIB  — WebView2Loader 导入库/静态库（MSVC：
#     WebView2LoaderStatic.lib；MinGW：WebView2Loader.dll.lib）。
#   CDC_COMPAT_INCLUDE_DIR   — 可选，工具链缺 EventToken.h 时传 tools/mingw-compat。
#
# Release 构建（非 Debug 配置）关闭 DevTools 与默认右键菜单；Debug 配置
# 定义 CDC_HOST_DEBUG=1 打开二者。源码含中文宽字符串，MSVC 须 /utf-8（顶层已加）。
add_library(cdc_host STATIC bridge.cpp host.cpp)
target_include_directories(cdc_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(DEFINED CDC_WEBVIEW2_INCLUDE_DIR)
  target_include_directories(cdc_host SYSTEM PUBLIC ${CDC_WEBVIEW2_INCLUDE_DIR})
else()
  message(WARNING "CDC_WEBVIEW2_INCLUDE_DIR not set: WebView2.h must be findable (Microsoft.Web.WebView2 NuGet package, build/native/include)")
endif()
if(DEFINED CDC_COMPAT_INCLUDE_DIR)
  target_include_directories(cdc_host SYSTEM PUBLIC ${CDC_COMPAT_INCLUDE_DIR})
endif()
target_link_libraries(cdc_host PUBLIC cdc_core ole32 user32 shlwapi uuid)
target_compile_definitions(cdc_host PRIVATE $<$<CONFIG:Debug>:CDC_HOST_DEBUG=1>)
cdc_strict(cdc_host)
if(DEFINED CDC_WEBVIEW2_LOADER_LIB)
  target_link_libraries(cdc_host PUBLIC ${CDC_WEBVIEW2_LOADER_LIB})
endif()
