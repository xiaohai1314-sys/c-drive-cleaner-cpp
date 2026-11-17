/* EventToken.h — 补 zig 自带 mingw-w64 头里缺的 EventToken.h（WebView2.h 需要）。
 * 仅在工具链缺这个头时通过 WIN_COMPAT_INC=tools/mingw-compat 加入搜索路径；
 * MSVC / 完整的 mingw-w64 / llvm-mingw 自带，不用它。定义与 Windows SDK 一致。 */
#ifndef CDC_COMPAT_EVENTTOKEN_H
#define CDC_COMPAT_EVENTTOKEN_H
#ifndef __eventtoken_h__
#define __eventtoken_h__
typedef struct EventRegistrationToken {
    __int64 value;
} EventRegistrationToken;
#endif
#endif
