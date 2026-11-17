# MinGW-w64 交叉工具链：cmake --preset mingw64 或
#   cmake -S . -B out/build/mingw64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
# 编译器可用 -D / 环境变量覆盖：
#   CDC_MINGW_CC / CDC_MINGW_CXX（默认 x86_64-w64-mingw32-gcc / -g++，需在 PATH 里，
#   或用 LLVM_MINGW_DIR 指向 llvm-mingw 根目录）。
# 用 zig：CDC_MINGW_CC="zig;cc;-target;x86_64-windows-gnu" 不被 CMake 接受（编译器必须是单个可执行），
# 请写两个小包装脚本（见 README）。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{CDC_MINGW_CC} AND NOT DEFINED CDC_MINGW_CC)
  set(CDC_MINGW_CC "$ENV{CDC_MINGW_CC}")
endif()
if(DEFINED ENV{CDC_MINGW_CXX} AND NOT DEFINED CDC_MINGW_CXX)
  set(CDC_MINGW_CXX "$ENV{CDC_MINGW_CXX}")
endif()
if(NOT DEFINED LLVM_MINGW_DIR AND DEFINED ENV{LLVM_MINGW_DIR})
  set(LLVM_MINGW_DIR "$ENV{LLVM_MINGW_DIR}")
endif()
if(DEFINED LLVM_MINGW_DIR)
  set(_cdc_prefix "${LLVM_MINGW_DIR}/bin/")
  set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_DIR}/x86_64-w64-mingw32")
else()
  set(_cdc_prefix "")
endif()
if(NOT DEFINED CDC_MINGW_CC)
  set(CDC_MINGW_CC "${_cdc_prefix}x86_64-w64-mingw32-gcc")
endif()
if(NOT DEFINED CDC_MINGW_CXX)
  set(CDC_MINGW_CXX "${_cdc_prefix}x86_64-w64-mingw32-g++")
endif()
set(CMAKE_C_COMPILER "${CDC_MINGW_CC}")
set(CMAKE_CXX_COMPILER "${CDC_MINGW_CXX}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
