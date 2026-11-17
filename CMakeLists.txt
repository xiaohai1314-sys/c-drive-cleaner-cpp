# c-drive-cleaner-cpp — 顶层构建（CMake）
#
#   Linux 原生：      cmake --preset native  && cmake --build --preset native && ctest --preset native
#   Windows 交叉：    cmake --preset mingw64 -DCDC_WEBVIEW2_INCLUDE_DIR=<...> && cmake --build --preset mingw64
#   Windows MSVC：    cmake -S . -B out/build/msvc -G "Visual Studio 17 2022" -A x64 \
#                       -DCDC_WEBVIEW2_INCLUDE_DIR=<NuGet>/build/native/include \
#                       -DCDC_WEBVIEW2_LOADER_LIB=<NuGet>/build/native/x64/WebView2LoaderStatic.lib
#                     cmake --build out/build/msvc --config Release && ctest --test-dir out/build/msvc -C Release
#
# 约定：自家代码 C++20，不用异常和 RTTI（GCC/Clang：-fno-exceptions -fno-rtti；
# MSVC：/EHs-c- /GR- /D_HAS_EXCEPTIONS=0），-Wall -Wextra -Werror（MSVC：/W4 /WX）。
# 第三方库（sqlite3 / blake3 / cjson / xxhash / mimalloc）保持 C 原样，只开
# -Wall -Wextra，不 -Werror；自家代码经各自头文件的 extern "C" 链接它们。
cmake_minimum_required(VERSION 3.20)
project(c-drive-cleaner-cpp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
  # 静态 CRT，单文件分发（与 MinGW 的 -static 对齐）。
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CheckCCompilerFlag)
include(CTest)
find_package(Threads REQUIRED)

option(CDC_MIMALLOC "Route malloc (sqlite3/cJSON) and operator new/delete to mimalloc" ON)

# 自家目标的统一编译选项。
function(cdc_strict target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /EHs-c- /GR- /permissive- /Zc:__cplusplus)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
  endif()
  target_compile_definitions(${target} PRIVATE CDC_HAVE_CJSON)
  if(NOT CDC_MIMALLOC)
    target_compile_definitions(${target} PRIVATE CDC_NO_MIMALLOC)
  endif()
endfunction()

# TRANSITIONAL：迁移未完成的模块仍直接 malloc/free（与 cJSON 互相释放），
# 需要与 cJSON 相同的宏替换；全部模块迁移完后删除本函数及其调用。
function(cdc_legacy_malloc target)
  target_include_directories(${target} SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/mimalloc/include)
  target_compile_options(${target} PRIVATE ${CDC_MIMALLOC_OVERRIDE})
endfunction()

# ---------------- 第三方库（C，vendored，见 third_party/README.md） ----------------
if(MSVC)
  set(CDC_TP_WARN /W3)
else()
  set(CDC_TP_WARN -Wall -Wextra)
endif()

add_library(mimalloc STATIC third_party/mimalloc/src/static.c)
target_include_directories(mimalloc SYSTEM PUBLIC third_party/mimalloc/include)
target_include_directories(mimalloc PRIVATE third_party/mimalloc/src)
target_link_libraries(mimalloc PRIVATE Threads::Threads)
target_compile_options(mimalloc PRIVATE ${CDC_TP_WARN})
if(WIN32)
  target_link_libraries(mimalloc PRIVATE advapi32 bcrypt psapi)
endif()

# sqlite3 / cJSON 内部的 malloc/free 宏替换到 mi_*（与 C 版一致）。
if(CDC_MIMALLOC)
  if(MSVC)
    set(CDC_MIMALLOC_OVERRIDE /FI${CMAKE_CURRENT_SOURCE_DIR}/src/cdc_mimalloc_override.h)
  else()
    set(CDC_MIMALLOC_OVERRIDE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/cdc_mimalloc_override.h)
  endif()
endif()

add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)
target_include_directories(sqlite3 SYSTEM PRIVATE third_party/mimalloc/include)
target_compile_options(sqlite3 PRIVATE ${CDC_TP_WARN} ${CDC_MIMALLOC_OVERRIDE})
if(UNIX)
  target_link_libraries(sqlite3 PUBLIC ${CMAKE_DL_LIBS} m Threads::Threads)
endif()

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)
target_include_directories(cjson SYSTEM PRIVATE third_party/mimalloc/include)
target_compile_options(cjson PRIVATE ${CDC_TP_WARN} ${CDC_MIMALLOC_OVERRIDE})

add_library(xxhash STATIC third_party/xxhash/xxhash.c)
target_include_directories(xxhash PUBLIC third_party/xxhash)
target_compile_options(xxhash PRIVATE ${CDC_TP_WARN})

# BLAKE3：intrinsics 版，分档照抄上游 c/CMakeLists.txt。
add_library(blake3 STATIC
  third_party/blake3/blake3.c
  third_party/blake3/blake3_dispatch.c
  third_party/blake3/blake3_portable.c)
target_include_directories(blake3 PUBLIC third_party/blake3)
target_compile_options(blake3 PRIVATE ${CDC_TP_WARN})
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|[Xx]86)$")
  if(MSVC)
    set(BLAKE3_FLAG_SSE2 "")
    set(BLAKE3_FLAG_SSE41 "/arch:AVX")
    set(BLAKE3_FLAG_AVX2 "/arch:AVX2")
    set(BLAKE3_FLAG_AVX512 "/arch:AVX512")
  else()
    set(BLAKE3_FLAG_SSE2 "-msse2")
    set(BLAKE3_FLAG_SSE41 "-msse4.1")
    set(BLAKE3_FLAG_AVX2 "-mavx2")
    set(BLAKE3_FLAG_AVX512 "-mavx512f -mavx512vl")
  endif()
  check_c_compiler_flag("${BLAKE3_FLAG_AVX512}" CDC_HAVE_AVX512)
  if(CDC_HAVE_AVX512)
    target_sources(blake3 PRIVATE third_party/blake3/blake3_avx512.c)
    set_source_files_properties(third_party/blake3/blake3_avx512.c PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_AVX512}")
  else()
    target_compile_definitions(blake3 PRIVATE BLAKE3_NO_AVX512)
  endif()
  target_sources(blake3 PRIVATE
    third_party/blake3/blake3_sse2.c
    third_party/blake3/blake3_sse41.c
    third_party/blake3/blake3_avx2.c)
  if(NOT BLAKE3_FLAG_SSE2 STREQUAL "")
    set_source_files_properties(third_party/blake3/blake3_sse2.c PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_SSE2}")
  endif()
  set_source_files_properties(third_party/blake3/blake3_sse41.c PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_SSE41}")
  set_source_files_properties(third_party/blake3/blake3_avx2.c PROPERTIES COMPILE_FLAGS "${BLAKE3_FLAG_AVX2}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
  target_sources(blake3 PRIVATE third_party/blake3/blake3_neon.c)
  target_compile_definitions(blake3 PRIVATE BLAKE3_USE_NEON=1)
else()
  target_compile_definitions(blake3 PRIVATE BLAKE3_USE_NEON=0 BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif()

# ---------------- 自家后端：cdc_core（除 main.cpp 外的全部模块） ----------------
add_library(cdc_core STATIC
  src/util.cpp src/legacy_c.cpp src/timeutil.cpp
  src/mft_util.cpp src/bigfile.cpp src/mft_raw.cpp src/mft.cpp src/fastio.cpp
  src/sampling.cpp src/scan.cpp src/fileindex.cpp src/hashcache.cpp
  src/safety.cpp src/recycle.cpp src/privacy.cpp src/system.cpp src/rules.cpp
  src/im.cpp src/shred.cpp src/process.cpp
  src/indextask.cpp src/logs.cpp
  src/gui.cpp)
target_include_directories(cdc_core PUBLIC src third_party)
target_link_libraries(cdc_core PUBLIC sqlite3 cjson blake3 xxhash Threads::Threads)
cdc_strict(cdc_core)
cdc_legacy_malloc(cdc_core)
if(WIN32)
  target_link_libraries(cdc_core PUBLIC advapi32 shell32 winhttp bcrypt ntdll ole32 user32 shlwapi uuid)
endif()

# operator new/delete -> mimalloc：独立对象库，直接链进每个可执行文件
#（放进静态库会被链接器丢弃）。
if(CDC_MIMALLOC)
  add_library(cdc_new_delete OBJECT src/mimalloc_new_delete.cpp)
  target_link_libraries(cdc_new_delete PRIVATE mimalloc)
  cdc_strict(cdc_new_delete)
endif()

function(cdc_link_runtime target)
  if(CDC_MIMALLOC)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:cdc_new_delete>)
    target_link_libraries(${target} PRIVATE mimalloc)
  endif()
  if(MINGW)
    target_link_options(${target} PRIVATE -static)
  endif()
endfunction()

# ---------------- WebView2 宿主（仅 Windows） ----------------
# CDC_WEBVIEW2_INCLUDE_DIR：NuGet 包 Microsoft.Web.WebView2 的 build/native/include；
# 仅做交叉编译验证时可传仓库内桩头 tools/webview2-stub（不能运行）。
# CDC_WEBVIEW2_LOADER_LIB：MSVC 用 WebView2LoaderStatic.lib，MinGW 用 WebView2Loader.dll.lib。
if(DEFINED CDC_WEBVIEW2_INCLUDE_DIR AND CDC_WEBVIEW2_INCLUDE_DIR STREQUAL "")
  unset(CDC_WEBVIEW2_INCLUDE_DIR CACHE)
endif()
option(CDC_BUILD_HOST "Build the WebView2 host (Windows only)" ON)
if(WIN32 AND CDC_BUILD_HOST)
  add_subdirectory(host)
  if(DEFINED CDC_WEBVIEW2_INCLUDE_DIR)
    target_include_directories(cdc_core SYSTEM PRIVATE ${CDC_WEBVIEW2_INCLUDE_DIR})
  endif()
  if(DEFINED CDC_COMPAT_INCLUDE_DIR)
    target_include_directories(cdc_core SYSTEM PRIVATE ${CDC_COMPAT_INCLUDE_DIR})
  endif()
  target_include_directories(cdc_core PRIVATE host)
else()
  message(STATUS "host: skipped (WebView2 host is Windows-only)")
endif()

# ---------------- 主程序 ----------------
add_executable(cdc-cpp src/main.cpp)
target_link_libraries(cdc-cpp PRIVATE cdc_core)
cdc_strict(cdc-cpp)
cdc_legacy_malloc(cdc-cpp)
cdc_link_runtime(cdc-cpp)
if(WIN32 AND CDC_BUILD_HOST)
  target_link_libraries(cdc-cpp PRIVATE cdc_host)
endif()

# ---------------- 测试 ----------------
# Windows 上能编译运行的：test_util / test_safety / test_slice3（+ selftest）；
# test_direnum / test_scan / test_gui 用 mkdtemp/symlink/setenv 等 POSIX API，仅非 Windows。
set(CDC_TESTS test_util test_safety test_slice3)
if(NOT WIN32)
  list(APPEND CDC_TESTS test_direnum test_scan test_gui)
endif()
foreach(t IN LISTS CDC_TESTS)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE cdc_core)
  cdc_strict(${t})
  cdc_legacy_malloc(${t})
  cdc_link_runtime(${t})
  add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
add_test(NAME selftest COMMAND cdc-cpp selftest WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
  add_test(NAME gen_fixtures COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen_fixtures.sh)
  set_tests_properties(gen_fixtures PROPERTIES FIXTURES_SETUP scan_fixtures)
  set_tests_properties(test_scan PROPERTIES FIXTURES_REQUIRED scan_fixtures)
endif()
