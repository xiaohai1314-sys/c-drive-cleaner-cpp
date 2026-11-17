CC_NATIVE ?= gcc
CC_WIN ?= x86_64-w64-mingw32-gcc
# 与 AGENTS.md 一致：apt 没有 gcc-mingw-w64-x86-64，用磁盘上的 llvm-mingw
MINGW_BIN ?= /home/hatch/workspace/.scratch/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64/bin

CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -Isrc -Ithird_party/sqlite3 -Ithird_party/blake3 -Ithird_party/xxhash
SRCS := src/main.c src/mft_util.c src/bigfile.c src/mft_raw.c src/util.c \
        src/fileindex.c src/indextask.c src/hashcache.c src/scan.c src/fastio.c \
        src/sampling.c
# ---- mimalloc 全局替换（性能优化项 6：分配器） ----
# 机制：自家源码 + sqlite3.c + cJSON.c 编译时都加
#   -isystem third_party/mimalloc/include -include src/cdc_mimalloc_override.h
# 把 malloc/calloc/realloc/free/strdup 等宏重定向到 mi_*。mimalloc 自身
# （static.c）编译时不加该宏；BLAKE3 不调用 malloc，不用加。
# 覆盖入口必须是 wrapper（src/cdc_mimalloc_override.h），不能直接 -include
# mimalloc-override.h：wrapper 先包含系统头再包含覆盖头，避开 glibc
# <stdlib.h> 里 __attr_dealloc (reallocarray, 1) 的宏冲突（见该文件头注释）。
# CDC_MIMALLOC=0 可关掉替换（编译不带宏、链接不带 mimalloc 对象），
# 用于 ASan 验证等场景：make check CDC_MIMALLOC=0 CFLAGS="... -fsanitize=address"。
ifeq ($(CDC_MIMALLOC),0)
MIMALLOC_CFLAGS :=
MIMALLOC_NATIVE_OBJ :=
MIMALLOC_WIN_OBJ :=
else
MIMALLOC_CFLAGS := -isystem third_party/mimalloc/include -include src/cdc_mimalloc_override.h
MIMALLOC_NATIVE_OBJ := build/mimalloc-native.o
MIMALLOC_WIN_OBJ := build/mimalloc-win.o
endif
# host/bridge.cpp 与 host/host.cpp 只显式调 mi_*、不加 -include 宏覆盖
#（见两文件头注释：C++ 翻译单元里宏覆盖会干扰标准库头文件），但它们
# 直接 #include "mimalloc.h"，所以 C++ 编译行只加 include 目录。
MIMALLOC_INC := -isystem third_party/mimalloc/include
CFLAGS += $(MIMALLOC_CFLAGS)
# mft.c / sqlite3.c / blake3 对象不在 SRCS 里，见下面单独的编译规则。
MFT_NATIVE_OBJ := build/mft-native.o
MFT_WIN_OBJ := build/mft-win.o
SQLITE3_NATIVE_OBJ := build/sqlite3-native.o
SQLITE3_WIN_OBJ := build/sqlite3-win.o
# BLAKE3 对象（与下面切片 2 的 CORE_BLAKE3_* 是同一组文件；这些变量
# 必须在 native/win 规则之前定义——make 在读 Makefile 时就展开依赖表，
# 写在后面的变量在前面的规则里是空的，链接会找不到 .o；recipe 里的
# 引用是执行时才展开，所以链接命令里会出现 .o 但从没编过）。
CORE_BUILD ?= build
CORE_BLAKE3_OBJS := $(CORE_BUILD)/blake3-core.o $(CORE_BUILD)/blake3_dispatch-core.o \
	$(CORE_BUILD)/blake3_portable-core.o $(CORE_BUILD)/blake3_sse2-core.o \
	$(CORE_BUILD)/blake3_sse41-core.o $(CORE_BUILD)/blake3_avx2-core.o \
	$(CORE_BUILD)/blake3_avx512-core.o
CORE_BLAKE3_WIN_OBJS := $(CORE_BUILD)/blake3-core-win.o $(CORE_BUILD)/blake3_dispatch-core-win.o \
	$(CORE_BUILD)/blake3_portable-core-win.o $(CORE_BUILD)/blake3_sse2-core-win.o \
	$(CORE_BUILD)/blake3_sse41-core-win.o $(CORE_BUILD)/blake3_avx2-core-win.o \
	$(CORE_BUILD)/blake3_avx512-core-win.o
# xxHash 对象（采样层）：与 BLAKE3 同样必须在 native/win 规则之前定义。
CORE_XXHASH_OBJS := $(CORE_BUILD)/xxhash-core.o
CORE_XXHASH_WIN_OBJS := $(CORE_BUILD)/xxhash-core-win.o

.PHONY: all native win clean check time-build test_util test_gui check-slice3 win-slice3 check-direnum

all: native

native: build/cdc-c-pilot
build/cdc-c-pilot: $(SRCS) $(MFT_NATIVE_OBJ) $(SQLITE3_NATIVE_OBJ) $(CORE_BLAKE3_OBJS) $(CORE_XXHASH_OBJS) $(MIMALLOC_NATIVE_OBJ) src/*.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -pthread -o $@ $(SRCS) $(MFT_NATIVE_OBJ) $(SQLITE3_NATIVE_OBJ) $(CORE_BLAKE3_OBJS) $(CORE_XXHASH_OBJS) $(MIMALLOC_NATIVE_OBJ)

# ---- GUI（src/gui.c + 45 命令适配 + host/ WebView2 宿主，见任务 B） ----
# win 目标编出的 exe 就是 GUI 程序（无参数进图形界面；--selftest /
# --index 仍走 CLI 分支）。Linux 目标（native/check）不链接 host/。
# WEBVIEW2_INC：WebView2 SDK 的 include 目录（NuGet 包 build/native/
# include，含 WebView2.h）；未传时报错提示。验证用的最小桩头在
# ~/workspace/.scratch/webview2-stub（不进仓库），用法：
#   make win WEBVIEW2_INC=$HOME/workspace/.scratch/webview2-stub
WEBVIEW2_INC ?=
CXX_WIN ?= x86_64-w64-mingw32-g++
# gui.c 之外的切片 3 源码（util.c / mft_util.c 已在 SRCS 里，不重复列；
# 同一 .c 编两次会在链接期报 multiple definition）。
# timeutil.c 不在其中：它在 -Werror 下有 -Wformat-truncation 误报，
# 见下单独编译（与 check-slice3 / win-slice3 的处理一致）。
GUI_WIN_SRCS := src/gui.c src/safety.c src/rules.c src/im.c src/system.c \
	src/privacy.c src/shred.c src/recycle.c src/process.c src/logs.c
GUI_WIN_DEFS := -D_GNU_SOURCE
GUI_WIN_INC := -Ihost -Ithird_party -Ithird_party/cjson -Ithird_party/blake3
# C++ 运行时静态链接：llvm-mingw 的 C++ 库是 libc++，默认动态链接
# （exe 会依赖 libc++.dll + libunwind.dll，不是系统 DLL，用户机器上
# 没有）；-Wl,-Bstatic 把 libc++/libc++abi/libunwind 静态链进 exe，
# 只剩系统 DLL。MSVC 构建不用这一行（MSVC 用自己的运行时）。
GUI_WIN_LIBS := -ladvapi32 -lshell32 -lwinhttp -lbcrypt -lntdll -lole32 -luser32 -lshlwapi -luuid -Wl,-Bstatic -lc++ -lc++abi -lunwind -Wl,-Bdynamic

win: build/cdc-c-pilot.exe build/test_util.exe
# WEBVIEW2_LOADER_LIB：MinGW + 真实头文件时需传 NuGet 包里的
# build/native/x64/WebView2Loader.dll.lib（host.cpp 直接调
# CreateCoreWebView2EnvironmentWithOptions，链接期要这个导入库；
# MSVC 侧由 CMake 的 CDC_WEBVIEW2_LOADER_LIB 接 WebView2LoaderStatic.lib）。
# 用验证桩头时不传（桩头里该函数是 inline 的 E_NOTIMPL 实现）。
WEBVIEW2_LOADER_LIB ?=
build/cdc-c-pilot.exe: $(SRCS) $(GUI_WIN_SRCS) src/timeutil.c third_party/cjson/cJSON.c host/bridge.cpp host/host.cpp $(MFT_WIN_OBJ) $(SQLITE3_WIN_OBJ) $(CORE_BLAKE3_WIN_OBJS) $(CORE_XXHASH_WIN_OBJS) $(MIMALLOC_WIN_OBJ) src/*.h host/*.h
	@if [ -z "$(WEBVIEW2_INC)" ]; then \
		echo "error: WEBVIEW2_INC 未设置：请传 WebView2 SDK 的 include 目录"; \
		echo "  make win WEBVIEW2_INC=<NuGet 包 build/native/include>"; \
		echo "  （验证用最小桩头：WEBVIEW2_INC=$$HOME/workspace/.scratch/webview2-stub）"; \
		exit 1; \
	fi
	$(MINGW_BIN)/$(CXX_WIN) -O2 -std=c++17 -Wall -Wextra -Ihost -I$(WEBVIEW2_INC) $(MIMALLOC_INC) -c host/bridge.cpp -o build/bridge-win.o
	$(MINGW_BIN)/$(CXX_WIN) -O2 -std=c++17 -Wall -Wextra -Ihost -I$(WEBVIEW2_INC) $(MIMALLOC_INC) -c host/host.cpp -o build/host-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c third_party/cjson/cJSON.c -o build/cjson-gui-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 $(GUI_WIN_DEFS) $(SLICE3_INC) $(MIMALLOC_CFLAGS) -c src/timeutil.c -o build/timeutil-gui-win.o
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(GUI_WIN_DEFS) $(GUI_WIN_INC) -o $@ $(SRCS) $(GUI_WIN_SRCS) build/timeutil-gui-win.o build/bridge-win.o build/host-win.o $(MFT_WIN_OBJ) $(SQLITE3_WIN_OBJ) $(CORE_BLAKE3_WIN_OBJS) $(CORE_XXHASH_WIN_OBJS) $(MIMALLOC_WIN_OBJ) build/cjson-gui-win.o $(GUI_WIN_LIBS) $(WEBVIEW2_LOADER_LIB)

# sqlite3.c 单独编：vendored 代码只开 -Wall -Wextra、不开 -Werror
# （与 CMakeLists.txt 的策略一致，上游源码自带 -Wunused-parameter
# 等警告）。注意：这里不加 -Isrc——历史原因：src/process.h 曾遮住
# CRT 的 <process.h>，导致 _beginthreadex/_endthreadex 未声明；
# 该文件已改名 cdc_process.h，地雷已拆，但本规则保持最小 flag 不变。
$(SQLITE3_NATIVE_OBJ): third_party/sqlite3/sqlite3.c
	@mkdir -p build
	$(CC_NATIVE) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 $(MIMALLOC_CFLAGS) -c $< -o $@
$(SQLITE3_WIN_OBJ): third_party/sqlite3/sqlite3.c
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 $(MIMALLOC_CFLAGS) -c $< -o $@

# mft.c 单独编：需 POSIX 声明（lstat），故加 -D_POSIX_C_SOURCE=200809L
# -D_DEFAULT_SOURCE=1。-Wno-error=unused-function 放宽已于 2026-11-17
# 去掉：mft.c 里只在 _WIN32 代码里被调用的 static 函数（快照内存缓存、
# u64 哈希表、互斥/线程、resolve_dir 等）已用 #ifdef _WIN32 隔离，
# Linux 下严格编译干净。Windows 下这两个问题都不存在。
$(MFT_NATIVE_OBJ): src/mft.c src/mft.h
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE=1 -c $< -o $@
$(MFT_WIN_OBJ): src/mft.c src/mft.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -c $< -o $@

# mimalloc：只编 src/static.c 一个翻译单元（单文件聚合整个库）。
# vendored 代码只开 -Wall -Wextra、不开 -Werror（上游源码自带
# -Wunused-variable 等警告）。注意：这里不加 $(MIMALLOC_CFLAGS)——
# mimalloc 自身实现不能被宏覆盖。CDC_MIMALLOC=0 时不编这两个目标。
ifneq ($(CDC_MIMALLOC),0)
$(MIMALLOC_NATIVE_OBJ): third_party/mimalloc/src/static.c third_party/mimalloc/include/mimalloc.h
	@mkdir -p build
	$(CC_NATIVE) -O2 -std=c11 -Wall -Wextra -isystem third_party/mimalloc/include -Ithird_party/mimalloc/src -c $< -o $@
$(MIMALLOC_WIN_OBJ): third_party/mimalloc/src/static.c third_party/mimalloc/include/mimalloc.h
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Wall -Wextra -isystem third_party/mimalloc/include -Ithird_party/mimalloc/src -c $< -o $@
endif

# util 自测：Linux 原生跑；Windows 交叉只编不过不跑（无 Windows 环境）
test_util: build/test_util
	./build/test_util
build/test_util: tests/test_util.c src/util.c src/util.h $(MIMALLOC_NATIVE_OBJ)
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -pthread -o $@ tests/test_util.c src/util.c $(MIMALLOC_NATIVE_OBJ)
build/test_util.exe: tests/test_util.c src/util.c src/util.h $(MIMALLOC_WIN_OBJ)
	@mkdir -p build
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) -o $@ tests/test_util.c src/util.c $(MIMALLOC_WIN_OBJ)

check: native test_util check-core test_gui check-direnum
	./build/cdc-c-pilot selftest

# 目录枚举自测：Linux 原生跑；Windows 交叉只编 util.c（在 win 目标里），
# NtQueryDirectoryFile 真路径无 Windows 环境可跑，见 tests/test_direnum.c 注释。
check-direnum: build/test_direnum
	./build/test_direnum
build/test_direnum: tests/test_direnum.c src/util.c src/util.h $(MIMALLOC_NATIVE_OBJ)
	@mkdir -p build
	$(CC_NATIVE) $(CFLAGS) -pthread -o $@ tests/test_direnum.c src/util.c $(MIMALLOC_NATIVE_OBJ)

# 干净编译计时：与 Rust 版 cargo check ~15 秒、CI release 构建 5 分 20 秒对照
time-build:
	@$(MAKE) -s clean >/dev/null
	@echo "--- C native clean build ---"
	@bash -c 'time $(MAKE) -s native'
	@$(MAKE) -s clean >/dev/null
	@echo "--- C Windows cross clean build ---"
	@bash -c 'time $(MAKE) -s win'

clean:
	rm -rf build

# 切片 4 备注：
#  - fileindex.c / indextask.c 已并入 SRCS（--index 编排在 main.c 里）。
#  - 切片 3 真管线已落地：hashcache.c / scan.c / fastio.c 在 SRCS 里，
#    临时桩 src/hashcache_scan_stub.c 已删除（main.c 的 CROSS-SLICE
#    INTERFACE 本地块已移入 hashcache.h / scan.h）；--index 现在跑真
#    哈希管线，不再是"只写元数据 + 游标"。
#  - /tmp 私有副本的端到端 --index 验证（桩 + 夹具树）见切片 4 汇报；
#    真管线落地后应在同一副本上重跑（见切片 3 收尾汇报）。

# ---- 切片 3（safety/rules/删除/隐私/系统/IM）自测（增补） ----
# 只加目标，不改现有目标。验证在 /tmp 私有副本里跑，不要和别的
# 切片抢 ~/workspace/cdc-c/build/：SLICE3_BUILD 可覆盖输出目录。
SLICE3_BUILD ?= build
SLICE3_SRCS := src/safety.c src/util.c src/mft_util.c src/rules.c \
	src/im.c src/system.c src/privacy.c src/shred.c src/recycle.c src/process.c
# logs.c / timeutil.c（均为本切片文件）单独编译：timeutil.c 在
# -std=c11 -Werror 下有 -Wformat-truncation 误报（年月日来自系统时间，
# 范围已定），仍不带 -Werror；logs.c 的 cdc_write_log 里 index>10000
# 分支曾有真 use-after-free（free(stamp) 后又在错误文案中读 stamp，
# 同一秒内 10001 个日志文件才触发），2026-11-17 已修（先拼文案再 free），
# 现与其它自家文件一样带 -Werror 编译。
# -D_GNU_SOURCE：-std=c11 下 strdup/setenv/readlink 等 POSIX 函数
# 需要它才有声明（logs.c / 测试文件都用到了）。
SLICE3_DEPS := src/logs.c src/timeutil.c
SLICE3_INC := -Isrc -Ithird_party -Ithird_party/sqlite3 -Ithird_party/cjson
SLICE3_DEFS := -D_GNU_SOURCE
SLICE3_LIBS_WIN := -ladvapi32 -lshell32 -lwinhttp -lbcrypt -lntdll

check-slice3: $(SLICE3_BUILD)/test_safety $(SLICE3_BUILD)/test_slice3
	./$(SLICE3_BUILD)/test_safety
	./$(SLICE3_BUILD)/test_slice3

$(SLICE3_BUILD)/test_safety: tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_DEPS) third_party/cjson/cJSON.c src/*.h $(MIMALLOC_NATIVE_OBJ)
	@mkdir -p $(SLICE3_BUILD)
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs.o
	$(CC_NATIVE) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) $(MIMALLOC_CFLAGS) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil.o
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson.o
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -pthread -o $@ tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_BUILD)/logs.o $(SLICE3_BUILD)/timeutil.o $(SLICE3_BUILD)/cjson.o $(MIMALLOC_NATIVE_OBJ)

# 第三方 amalgamation（sqlite3/cJSON）单独编译，不带 -Wall -Wextra -Werror
#（上游代码自带 -Wunused-parameter 等警告，不是本切片的问题）。
$(SLICE3_BUILD)/test_slice3: tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_DEPS) third_party/sqlite3/sqlite3.c third_party/cjson/cJSON.c src/*.h $(MIMALLOC_NATIVE_OBJ)
	@mkdir -p $(SLICE3_BUILD)
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs.o
	$(CC_NATIVE) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) $(MIMALLOC_CFLAGS) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil.o
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/sqlite3 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c third_party/sqlite3/sqlite3.c -o $(SLICE3_BUILD)/sqlite3.o
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson.o
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -pthread -o $@ tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_BUILD)/logs.o $(SLICE3_BUILD)/timeutil.o $(SLICE3_BUILD)/sqlite3.o $(SLICE3_BUILD)/cjson.o $(MIMALLOC_NATIVE_OBJ) -ldl -lm

win-slice3: $(SLICE3_BUILD)/test_safety.exe $(SLICE3_BUILD)/test_slice3.exe

$(SLICE3_BUILD)/test_safety.exe: tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_DEPS) third_party/cjson/cJSON.c src/*.h $(MIMALLOC_WIN_OBJ)
	@mkdir -p $(SLICE3_BUILD)
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) $(MIMALLOC_CFLAGS) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson-win.o
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -o $@ tests/test_safety.c src/safety.c src/util.c src/mft_util.c $(SLICE3_BUILD)/logs-win.o $(SLICE3_BUILD)/timeutil-win.o $(SLICE3_BUILD)/cjson-win.o $(MIMALLOC_WIN_OBJ) $(SLICE3_LIBS_WIN)

# BLAKE3：rules.c 的可选 pin 校验只在 Windows 用（WinHTTP update 内）；
# 配方照抄 CMakeLists.txt：3 个通用文件 + x86 SIMD 文件（各带 -m 开关）。
$(SLICE3_BUILD)/test_slice3.exe: tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_DEPS) third_party/sqlite3/sqlite3.c third_party/cjson/cJSON.c third_party/blake3/blake3.c src/*.h $(MIMALLOC_WIN_OBJ)
	@mkdir -p $(SLICE3_BUILD)
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -c src/logs.c -o $(SLICE3_BUILD)/logs-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) $(MIMALLOC_CFLAGS) -c src/timeutil.c -o $(SLICE3_BUILD)/timeutil-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/sqlite3 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c third_party/sqlite3/sqlite3.c -o $(SLICE3_BUILD)/sqlite3-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c third_party/cjson/cJSON.c -o $(SLICE3_BUILD)/cjson-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c third_party/blake3/blake3.c -o $(SLICE3_BUILD)/blake3-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c third_party/blake3/blake3_dispatch.c -o $(SLICE3_BUILD)/blake3_dispatch-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c third_party/blake3/blake3_portable.c -o $(SLICE3_BUILD)/blake3_portable-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse2 -Ithird_party/blake3 -c third_party/blake3/blake3_sse2.c -o $(SLICE3_BUILD)/blake3_sse2-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse4.1 -Ithird_party/blake3 -c third_party/blake3/blake3_sse41.c -o $(SLICE3_BUILD)/blake3_sse41-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx2 -Ithird_party/blake3 -c third_party/blake3/blake3_avx2.c -o $(SLICE3_BUILD)/blake3_avx2-win.o
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx512f -mavx512vl -Ithird_party/blake3 -c third_party/blake3/blake3_avx512.c -o $(SLICE3_BUILD)/blake3_avx512-win.o
	$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -o $@ tests/test_slice3.c $(SLICE3_SRCS) $(SLICE3_BUILD)/logs-win.o $(SLICE3_BUILD)/timeutil-win.o $(SLICE3_BUILD)/sqlite3-win.o $(SLICE3_BUILD)/cjson-win.o $(SLICE3_BUILD)/blake3-win.o $(SLICE3_BUILD)/blake3_dispatch-win.o $(SLICE3_BUILD)/blake3_portable-win.o $(SLICE3_BUILD)/blake3_sse2-win.o $(SLICE3_BUILD)/blake3_sse41-win.o $(SLICE3_BUILD)/blake3_avx2-win.o $(SLICE3_BUILD)/blake3_avx512-win.o $(MIMALLOC_WIN_OBJ) $(SLICE3_LIBS_WIN)

# ---- 切片 2（mft/fastio/hashcache/scan）自测（增补） ----
# 只加目标，不改现有目标（check 除外：按任务要求并入 make check）。
# 验证在 /tmp 私有副本里跑，不要和别的切片抢 ~/workspace/cdc-c/build/：
# CORE_BUILD 可覆盖输出目录（默认 build）。
CORE_BUILD ?= build
CORE_SRCS := src/mft.c src/fastio.c src/hashcache.c src/scan.c src/util.c \
	src/bigfile.c src/mft_util.c src/sampling.c
CORE_INC := -Isrc -Ithird_party/sqlite3 -Ithird_party/blake3 -Ithird_party/cjson -Ithird_party/xxhash
CORE_DEFS := -D_GNU_SOURCE
# cJSON 在树里时才开 v2 迁移（CDC_HAVE_CJSON）；不在时 hashcache 的
# 迁移为 no-op，测试的迁移段自动跳过（#ifdef CDC_HAVE_CJSON）。
ifneq ($(wildcard third_party/cjson/cJSON.c),)
CORE_DEFS += -DCDC_HAVE_CJSON
CORE_CJSON_SRC := third_party/cjson/cJSON.c
CORE_CJSON_OBJ := $(CORE_BUILD)/cjson-core.o
CORE_CJSON_WIN_OBJ := $(CORE_BUILD)/cjson-core-win.o
else
CORE_CJSON_SRC :=
CORE_CJSON_OBJ :=
CORE_CJSON_WIN_OBJ :=
endif
# BLAKE3：照抄 CMakeLists.txt / 切片 3 的配方：3 个通用文件 + x86 SIMD
# 文件（各带 -m 开关），不用 BLAKE3_NO_* portable-only 定义。
# 对象列表见文件顶部（CORE_BLAKE3_OBJS / CORE_BLAKE3_WIN_OBJS，
# 必须在 native/win 规则之前定义，见那里注释）。
CORE_BLAKE3_SRCS := third_party/blake3/blake3.c third_party/blake3/blake3_dispatch.c \
	third_party/blake3/blake3_portable.c third_party/blake3/blake3_sse2.c \
	third_party/blake3/blake3_sse41.c third_party/blake3/blake3_avx2.c \
	third_party/blake3/blake3_avx512.c
CORE_THIRDPARTY_OBJS := $(CORE_BUILD)/sqlite3-core.o $(CORE_CJSON_OBJ) $(CORE_BLAKE3_OBJS) $(CORE_XXHASH_OBJS)
CORE_THIRDPARTY_WIN_OBJS := $(CORE_BUILD)/sqlite3-core-win.o $(CORE_CJSON_WIN_OBJ) $(CORE_BLAKE3_WIN_OBJS) $(CORE_XXHASH_WIN_OBJS)

check-core: $(CORE_BUILD)/test_scan
	@test -f tests/fixtures/setup.exe || sh tests/gen_fixtures.sh
	./$(CORE_BUILD)/test_scan

$(CORE_BUILD)/test_scan: tests/test_scan.c $(CORE_SRCS) third_party/sqlite3/sqlite3.c $(CORE_CJSON_SRC) $(CORE_BLAKE3_SRCS) third_party/xxhash/xxhash.c src/*.h $(CORE_THIRDPARTY_OBJS) $(MIMALLOC_NATIVE_OBJ)
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) $(CFLAGS) $(CORE_DEFS) $(CORE_INC) -pthread -o $@ tests/test_scan.c $(CORE_SRCS) $(CORE_THIRDPARTY_OBJS) $(MIMALLOC_NATIVE_OBJ) -ldl -lm

# Windows 交叉：只编译不链接（test_scan 是 Linux-only，用的 mkdtemp/link/
# pwrite 等 POSIX API 在 MinGW 下没有）。编 CORE_SRCS + 第三方对象
# 即证明切片 2 交叉编译干净。
core-win: $(CORE_THIRDPARTY_WIN_OBJS)
	@mkdir -p $(CORE_BUILD)
	@for f in $(CORE_SRCS); do \
		$(MINGW_BIN)/$(CC_WIN) $(CFLAGS) $(CORE_DEFS) $(CORE_INC) -c $$f -o $(CORE_BUILD)/$$(basename $$f .c)-win.o || exit 1; \
	done
	@echo "core-win: all slice-2 sources cross-compile clean"

# 第三方 amalgamation 单独编，不带 -Werror（上游代码自带警告）。
$(CORE_BUILD)/sqlite3-core.o: third_party/sqlite3/sqlite3.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 $(MIMALLOC_CFLAGS) -c $< -o $@
$(CORE_BUILD)/sqlite3-core-win.o: third_party/sqlite3/sqlite3.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Wall -Wextra -Ithird_party/sqlite3 $(MIMALLOC_CFLAGS) -c $< -o $@
$(CORE_BUILD)/cjson-core.o: third_party/cjson/cJSON.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c $< -o $@
$(CORE_BUILD)/cjson-core-win.o: third_party/cjson/cJSON.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/cjson $(MIMALLOC_CFLAGS) -c $< -o $@

# BLAKE3 对象（native）。
$(CORE_BUILD)/blake3-core.o: third_party/blake3/blake3.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_dispatch-core.o: third_party/blake3/blake3_dispatch.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_portable-core.o: third_party/blake3/blake3_portable.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse2-core.o: third_party/blake3/blake3_sse2.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -msse2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse41-core.o: third_party/blake3/blake3_sse41.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -msse4.1 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx2-core.o: third_party/blake3/blake3_avx2.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -mavx2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx512-core.o: third_party/blake3/blake3_avx512.c
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -mavx512f -mavx512vl -Ithird_party/blake3 -c $< -o $@

# BLAKE3 对象（Windows 交叉，-m 开关同 native）。
$(CORE_BUILD)/blake3-core-win.o: third_party/blake3/blake3.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_dispatch-core-win.o: third_party/blake3/blake3_dispatch.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_portable-core-win.o: third_party/blake3/blake3_portable.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse2-core-win.o: third_party/blake3/blake3_sse2.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_sse41-core-win.o: third_party/blake3/blake3_sse41.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -msse4.1 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx2-core-win.o: third_party/blake3/blake3_avx2.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx2 -Ithird_party/blake3 -c $< -o $@
$(CORE_BUILD)/blake3_avx512-core-win.o: third_party/blake3/blake3_avx512.c
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -mavx512f -mavx512vl -Ithird_party/blake3 -c $< -o $@

# xxHash 对象（native / Windows 交叉）：vendored 代码不带 -Werror。
$(CORE_BUILD)/xxhash-core.o: third_party/xxhash/xxhash.c third_party/xxhash/xxhash.h
	@mkdir -p $(CORE_BUILD)
	$(CC_NATIVE) -O2 -std=c11 -Wall -Wextra -Ithird_party/xxhash -c $< -o $@
$(CORE_BUILD)/xxhash-core-win.o: third_party/xxhash/xxhash.c third_party/xxhash/xxhash.h
	@mkdir -p $(CORE_BUILD)
	$(MINGW_BIN)/$(CC_WIN) -O2 -std=c11 -Wall -Wextra -Ithird_party/xxhash -c $< -o $@

# ---- GUI 适配层自测（tests/test_gui.c） ----
# 只测 gui.c 的纯函数（frag 解析、任务坞、dup 缓存键、索引作用域、
# path_under_dir、扫描状态、缓存失效）；Windows 段（45 个适配器）
# 只在交叉编译里验证，不在 Linux 上跑。
# 链接集：gui.c 纯段用到 cJSON（任务/扫描 JSON）+ BLAKE3（缓存键）
# + safety.c（cdc_walk_dir，缓存失效用）+ logs.c/timeutil.c；
# timeutil.c 单独编（不带 -Werror，-Wformat-truncation 误报）。
test_gui: build/test_gui
	./build/test_gui
build/test_gui: tests/test_gui.c src/gui.c src/util.c src/safety.c src/mft_util.c src/logs.c src/timeutil.c $(CORE_BLAKE3_OBJS) $(CORE_CJSON_OBJ) $(SQLITE3_NATIVE_OBJ) $(MIMALLOC_NATIVE_OBJ) src/*.h
	@mkdir -p build
	$(CC_NATIVE) -O2 -std=c11 $(SLICE3_DEFS) $(SLICE3_INC) $(MIMALLOC_CFLAGS) -c src/timeutil.c -o build/timeutil-gui.o
	$(CC_NATIVE) $(CFLAGS) $(SLICE3_DEFS) $(SLICE3_INC) -Ithird_party/blake3 -pthread -o $@ tests/test_gui.c src/gui.c src/util.c src/safety.c src/mft_util.c src/logs.c build/timeutil-gui.o $(CORE_BLAKE3_OBJS) $(CORE_CJSON_OBJ) $(SQLITE3_NATIVE_OBJ) $(MIMALLOC_NATIVE_OBJ) -ldl -lm
