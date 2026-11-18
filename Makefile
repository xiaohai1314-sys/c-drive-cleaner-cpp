# c-drive-cleaner-cpp — GNU make 构建（Linux 原生 + MinGW 交叉）。
#
#   make check                      # Linux 原生：编译 + 跑全部测试（含 selftest）
#   make win WEBVIEW2_INC=<dir>     # Windows x64 交叉：GUI exe + 可交叉的测试 exe
#   make clean
#
# 编译器（可在命令行覆盖）：
#   CXX_NATIVE / CC_NATIVE   默认 g++ / gcc
#   CXX_WIN / CC_WIN         默认 x86_64-w64-mingw32-g++ / -gcc（llvm-mingw 或 mingw-w64）
# 用 zig 作工具链（本仓库的验证方式，见 README）：
#   make check CXX_NATIVE="zig c++" CC_NATIVE="zig cc"
#   make win CXX_WIN="zig c++ -target x86_64-windows-gnu" CC_WIN="zig cc -target x86_64-windows-gnu" \
#            WEBVIEW2_INC=tools/webview2-stub
#
# 约定：自家代码 C++20，-fno-exceptions -fno-rtti，-Wall -Wextra -Werror。
# 第三方库（sqlite3 / blake3 / cjson / xxhash）保持 C 原样，
# 用 C 编译器编、只开 -Wall -Wextra（不 -Werror，上游自带警告），
# 自家代码通过各自头文件的 extern "C" 链接它们。
# cJSON / sqlite 分配的内存一律用 cJSON_free / sqlite3_free 释放。

CC_NATIVE  ?= gcc
CXX_NATIVE ?= g++
CC_WIN     ?= x86_64-w64-mingw32-gcc
CXX_WIN    ?= x86_64-w64-mingw32-g++
AR_NATIVE  ?= ar
AR_WIN     ?= $(AR_NATIVE)
NATIVE_ARCH ?= $(shell uname -m)

B  := build
BN := $(B)/native
BW := $(B)/win

OPT ?= -O2
CXXSTD   := -std=c++20 -fno-exceptions -fno-rtti
WARN     := -Wall -Wextra -Werror
INCS     := -Isrc -Ithird_party -Ithird_party/sqlite3 -Ithird_party/cjson \
            -Ithird_party/blake3 -Ithird_party/xxhash
DEFS     := -DCDC_HAVE_CJSON -D_GNU_SOURCE
CXXFLAGS_BASE := $(OPT) $(CXXSTD) $(WARN) $(INCS) $(DEFS) $(EXTRA_CXXFLAGS)
CXXFLAGS_CDC = $(CXXFLAGS_BASE)
CFLAGS_TP    := $(OPT) -std=c11 -Wall -Wextra $(EXTRA_CFLAGS)

# ---- 自家源码 ----
# libcdc：除 main.cpp 外的全部后端；链接器只拉用到的对象。
LIB_SRCS := util timeutil mft_util bigfile mft_raw mft fastio sampling scan \
            fileindex hashcache safety recycle privacy system rules im shred \
            process indextask logs gui
LIB_NATIVE_OBJS := $(LIB_SRCS:%=$(BN)/obj/%.o)
LIB_WIN_OBJS    := $(LIB_SRCS:%=$(BW)/obj/%.o)

# ---- 第三方（C） ----
BLAKE3_COMMON := blake3 blake3_dispatch blake3_portable
BLAKE3_X86    := blake3_sse2 blake3_sse41 blake3_avx2 blake3_avx512
ifeq ($(NATIVE_ARCH),x86_64)
BLAKE3_NATIVE := $(BLAKE3_COMMON) $(BLAKE3_X86)
else ifneq ($(filter aarch64 arm64,$(NATIVE_ARCH)),)
BLAKE3_NATIVE := $(BLAKE3_COMMON) blake3_neon
else
BLAKE3_NATIVE := $(BLAKE3_COMMON)
BLAKE3_PORTABLE_DEFS := -DBLAKE3_USE_NEON=0 -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512
endif
BLAKE3_WIN := $(BLAKE3_COMMON) $(BLAKE3_X86)
TP_NATIVE_OBJS := $(BN)/tp/sqlite3.o $(BN)/tp/cJSON.o $(BN)/tp/xxhash.o \
                  $(BLAKE3_NATIVE:%=$(BN)/tp/%.o)
TP_WIN_OBJS    := $(BW)/tp/sqlite3.o $(BW)/tp/cJSON.o $(BW)/tp/xxhash.o \
                  $(BLAKE3_WIN:%=$(BW)/tp/%.o)

NATIVE_LIBS := -pthread -ldl -lm
WIN_SYSLIBS := -ladvapi32 -lshell32 -lwinhttp -lbcrypt -lntdll -lole32 -luser32 -lshlwapi -luuid
# MinGW 下静态链接 C++ 运行时，保持单文件 exe（libc++ 用 -lc++，libstdc++ 用 -static-libstdc++）。
WIN_CXXRT ?= -static

TESTS := test_util test_direnum test_safety test_slice3 test_scan test_gui \
         test_indextask test_host_security
# 能在 Windows 上编译运行的测试（其余用 mkdtemp/symlink 等 POSIX API，仅 Linux）。
WIN_TESTS := test_util test_safety test_slice3 test_indextask test_host_security

.PHONY: all native check win win-tests clean frontend
all: native

native: $(BN)/cdc-cpp $(TESTS:%=$(BN)/%)

check: native
	@test -f tests/fixtures/setup.exe || sh tests/gen_fixtures.sh
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$(BN)/$$t; done
	@echo "== selftest"; ./$(BN)/cdc-cpp selftest

# ---- 原生链接 ----
$(BN)/libcdc.a: $(LIB_NATIVE_OBJS)
	$(AR_NATIVE) rcs $@ $^
$(BN)/cdc-cpp: $(BN)/obj/main.o $(BN)/libcdc.a $(TP_NATIVE_OBJS)
	$(CXX_NATIVE) $(OPT) -o $@ $^ $(NATIVE_LIBS)
$(BN)/test_%: $(BN)/obj/tests/test_%.o $(BN)/libcdc.a $(TP_NATIVE_OBJS)
	$(CXX_NATIVE) $(OPT) -o $@ $^ $(NATIVE_LIBS)

# ---- 原生编译 ----
$(BN)/obj/%.o: src/%.cpp src/*.h
	@mkdir -p $(@D)
	$(CXX_NATIVE) $(CXXFLAGS_CDC) -c $< -o $@
$(BN)/obj/tests/%.o: tests/%.cpp src/*.h
	@mkdir -p $(@D)
	$(CXX_NATIVE) $(CXXFLAGS_CDC) -c $< -o $@
$(BN)/tp/sqlite3.o: third_party/sqlite3/sqlite3.c
	@mkdir -p $(@D)
	$(CC_NATIVE) $(CFLAGS_TP) -Ithird_party/sqlite3 -c $< -o $@
$(BN)/tp/cJSON.o: third_party/cjson/cJSON.c
	@mkdir -p $(@D)
	$(CC_NATIVE) $(CFLAGS_TP) -Ithird_party/cjson -c $< -o $@
$(BN)/tp/xxhash.o: third_party/xxhash/xxhash.c
	@mkdir -p $(@D)
	$(CC_NATIVE) $(CFLAGS_TP) -Ithird_party/xxhash -c $< -o $@
$(BN)/tp/%.o: third_party/blake3/%.c
	@mkdir -p $(@D)
	$(CC_NATIVE) $(CFLAGS_TP) $(BLAKE3_PORTABLE_DEFS) $(BLAKE3_SIMD_FLAGS) -Ithird_party/blake3 -c $< -o $@

# ---- Windows 交叉 ----
# WEBVIEW2_INC：WebView2 SDK 的 include 目录（NuGet 包 Microsoft.Web.WebView2
# 的 build/native/include）。仅做交叉编译验证时可用仓库里的最小桩头
# tools/webview2-stub（不是真 SDK，CreateCoreWebView2EnvironmentWithOptions
# 是返回 E_NOTIMPL 的 inline 桩）。
# WEBVIEW2_LOADER_LIB：真 SDK 时传 build/native/x64/WebView2Loader.dll.lib。
WEBVIEW2_INC ?=
WEBVIEW2_LOADER_LIB ?=
# WIN_COMPAT_INC：工具链缺 EventToken.h 时（如 zig 自带的 mingw 头）传 tools/mingw-compat。
WIN_COMPAT_INC ?=
WV2_INC_FLAGS = $(if $(WEBVIEW2_INC),-isystem $(WEBVIEW2_INC)) $(if $(WIN_COMPAT_INC),-isystem $(WIN_COMPAT_INC))
# Release（默认）关闭 DevTools 与默认右键菜单；CDC_DEBUG_HOST=1 打开。
HOST_DEFS := $(if $(filter 1,$(CDC_DEBUG_HOST)),-DCDC_HOST_DEBUG=1,-DNDEBUG)
HOST_SRCS := bridge host
HOST_WIN_OBJS := $(HOST_SRCS:%=$(BW)/obj/host/%.o)

win: $(BW)/cdc-cpp.exe win-tests
win-tests: $(WIN_TESTS:%=$(BW)/%.exe)

$(BW)/libcdc.a: $(LIB_WIN_OBJS)
	$(AR_WIN) rcs $@ $^
$(BW)/cdc-cpp.exe: $(BW)/obj/main.o $(HOST_WIN_OBJS) $(BW)/libcdc.a $(TP_WIN_OBJS)
	$(CXX_WIN) $(OPT) -o $@ $^ $(WIN_SYSLIBS) $(WEBVIEW2_LOADER_LIB) $(WIN_CXXRT)
$(BW)/test_%.exe: $(BW)/obj/tests/test_%.o $(BW)/libcdc.a $(TP_WIN_OBJS)
	$(CXX_WIN) $(OPT) -o $@ $^ $(WIN_SYSLIBS) $(WIN_CXXRT)

$(BW)/obj/host/%.o: host/%.cpp host/*.h src/*.h
	@if [ -z "$(WEBVIEW2_INC)" ]; then \
		echo "error: WEBVIEW2_INC 未设置：请传 WebView2 SDK 的 include 目录"; \
		echo "  make win WEBVIEW2_INC=<NuGet 包 build/native/include>"; \
		echo "  （仅交叉编译验证：WEBVIEW2_INC=tools/webview2-stub）"; \
		exit 1; \
	fi
	@mkdir -p $(@D)
	$(CXX_WIN) $(CXXFLAGS_BASE) $(HOST_DEFS) -Ihost $(WV2_INC_FLAGS) -c $< -o $@
$(BW)/obj/%.o: src/%.cpp src/*.h
	@mkdir -p $(@D)
	$(CXX_WIN) $(CXXFLAGS_CDC) -Ihost $(WV2_INC_FLAGS) -c $< -o $@
$(BW)/obj/tests/%.o: tests/%.cpp src/*.h
	@mkdir -p $(@D)
	$(CXX_WIN) $(CXXFLAGS_CDC) -c $< -o $@
$(BW)/tp/sqlite3.o: third_party/sqlite3/sqlite3.c
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS_TP) -Ithird_party/sqlite3 -c $< -o $@
$(BW)/tp/cJSON.o: third_party/cjson/cJSON.c
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS_TP) -Ithird_party/cjson -c $< -o $@
$(BW)/tp/xxhash.o: third_party/xxhash/xxhash.c
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS_TP) -Ithird_party/xxhash -c $< -o $@
$(BW)/tp/%.o: third_party/blake3/%.c
	@mkdir -p $(@D)
	$(CC_WIN) $(CFLAGS_TP) $(BLAKE3_SIMD_FLAGS) -Ithird_party/blake3 -c $< -o $@

# BLAKE3 x86 SIMD 文件各自的 -m 开关（与上游 CMake 一致）。
$(BN)/tp/blake3_sse2.o $(BW)/tp/blake3_sse2.o: BLAKE3_SIMD_FLAGS := -msse2
$(BN)/tp/blake3_sse41.o $(BW)/tp/blake3_sse41.o: BLAKE3_SIMD_FLAGS := -msse4.1
$(BN)/tp/blake3_avx2.o $(BW)/tp/blake3_avx2.o: BLAKE3_SIMD_FLAGS := -mavx2
$(BN)/tp/blake3_avx512.o $(BW)/tp/blake3_avx512.o: BLAKE3_SIMD_FLAGS := -mavx512f -mavx512vl

frontend:
	cd web && npm ci && npm run build

clean:
	rm -rf $(B)

.SECONDARY:
