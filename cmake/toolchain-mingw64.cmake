# MinGW 交叉工具链（llvm-mingw）：cmake --preset mingw64 或
#   cmake -S . -B out/build/mingw64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
# LLVM_MINGW_DIR 可通过 -D 或环境变量覆盖；默认用本机磁盘上的 llvm-mingw。
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED LLVM_MINGW_DIR)
  if(DEFINED ENV{LLVM_MINGW_DIR})
    set(LLVM_MINGW_DIR "$ENV{LLVM_MINGW_DIR}")
  else()
    set(LLVM_MINGW_DIR "/home/hatch/workspace/.scratch/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64")
  endif()
endif()
set(LLVM_MINGW_DIR "${LLVM_MINGW_DIR}" CACHE PATH "llvm-mingw root directory")

set(CMAKE_C_COMPILER   "${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-gcc")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-g++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_DIR}/bin/x86_64-w64-mingw32-windres")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_DIR}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
