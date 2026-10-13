# Lume — a strongly-typed research language whose native path is libLLVM.
# This tree is the host-independent fork of lume: same front
# end, same emitters, but no dependency on agent-httpd / libagenthttpd.a.
# Nothing here links against anything besides libc, libLLVM (optional),
# libssl/libcrypto (optional) and libsqlite3 (optional — probed, and only
# linked when src/builtins_sql.c is compiled in).
# 与宿主版的区别只有一处: 摘掉了 agent-httpd 桥(bridge.c / iquest.*)与
# 依赖宿主 db layer 的 builtins_sql.c(sqlite 后端已自包含下沉进本树,
# 见 src/builtins_sql.c; 探测不到 libsqlite3 的构建出运行时 stub)。
CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g
CFLAGS   += -I src
# 原生后端(--compile)的两个编译期常量:
#  - TARGET_TRIPLE:手写 IR 不打 triple 的话 LLVM 按通用目标降级,ABI 选错,
#    printf 会直接打印乱码(不是报错,是乱码)。
#  - LUME_RT_SRC:runtime helper 的源码绝对路径。它只在 --compile 时被 clang
#    单独编译,不会进 build/,所以不能写成相对路径(运行时 cwd 不可知)。
TARGET_TRIPLE := $(shell $(CC) -print-target-triple 2>/dev/null)
TARGET_TRIPLE_DEFS := -DTARGET_TRIPLE=\"$(TARGET_TRIPLE)\"
# RT_DEFS 必须单独拎成一个变量, 不能跟上一行挤在一起: make pack 要靠
# filter-out 把它摘掉、换成一个不含本机路径的字面量再重链(见 PACK_RT_DEFS)。
# 字面量是 strip 去不掉的 —— DWARF 里的编译目录能被 strip 抹掉, 但 -D 烤进
# 代码段的那串 /Users/<user>/... 不会。
# (注释里一律写 /Users/<user> 这种占位写法: scripts/check-tree-privacy.sh
#  会把注释中的真实字面量也判成泄漏, 守卫生效时这里必须是绿的。)
RT_DEFS            := -DLUME_RT_SRC=\"$(CURDIR)/src/rt.c\" -DLUME_NATIVE_SRC=\"$(CURDIR)/src/bridge_native.c\"
CFLAGS   += $(TARGET_TRIPLE_DEFS) $(RT_DEFS)

# 版本号单一来源: lume-core --version 与 LSP/MCP serverInfo 都读这个宏,
# 不再让 0.5.0 在 main.c / bridge_lsp.c / bridge_mcp.c 多处硬编码漂移。
# 打 release 前在此递增值, 保证二进制自报与 GitHub Release tag 一致
# (make pack 走 PACK_CFLAGS, 这个 -D 不在 RT_DEFS 里, 会一并带进去)。
LUME_CORE_VERSION ?= 0.7.0
CFLAGS   += -DLUME_CORE_VERSION=\"$(LUME_CORE_VERSION)\"

# --- 平台 feature-test: 与宿主版/agent-httpd/Makefile:8-22 同口径, 但这里不 ---
# main.c 用 sigaction/sigemptyset (--watch 热重载的信号处理), 它们是 POSIX
# 199309 定义; glibc 不会默认放行, 要 -D_GNU_SOURCE 才显; macOS clang 则
# 默认全量 BSD 声明 (宿主 make 从不需要)。行为两侧必须一致 —— 宿主 make
# 编出的 bin/lume 与容器内重编的 bin/lume 都要能编, 所以两家都带, 而不是
# 只在镜像侧补 (镜像侧补 = macOS 宿主永远测不到这条平台差异)。
UNAME_S := $(shell uname -s)
# Windows (MSYS2 / mingw-w64 / Cygwin) is detected by substring because
# `uname -s` reports MINGW64_NT-..., MSYS_NT-... or CYGWIN_NT-.... The native
# Windows port targets mingw-w64: the source carries #ifdef _WIN32 shims for
# the platform-specific spots, and HTTP/TLS is opted out (see below).
# The $(OS) check comes first: it is a cmd.exe environment variable and stays
# reliable even when uname is not on PATH (mingw32-make + plain cmd).
IS_WINDOWS :=
ifeq ($(OS),Windows_NT)
IS_WINDOWS := 1
else ifneq ($(findstring MINGW,$(UNAME_S)),)
IS_WINDOWS := 1
else ifneq ($(findstring MSYS,$(UNAME_S)),)
IS_WINDOWS := 1
else ifneq ($(findstring CYGWIN,$(UNAME_S)),)
IS_WINDOWS := 1
endif
# HTTP/TLS is on by default; the Windows/mingw port turns it off and excludes
# src/builtins_http.c (the always-compiled b_http_* wrappers then bind to the
# native_http_unavailable stubs in builtins.c). Defining it globally (not just
# inside interp.c) lets every TU agree on whether the real http builtins exist.
LUME_HAS_HTTP := 1
ifeq ($(UNAME_S),Linux)
    CFLAGS_EXTRA += -D_GNU_SOURCE
    # glibc fortify 与 agent-httpd 同开: Ubuntu 默认注入, 显式开启使本地
    # Linux 构建与 CI 一致(realpath 等 _chk 调用会校验 PATH_MAX)。
    CFLAGS_EXTRA += -D_FORTIFY_SOURCE=2
    # stringop-truncation: fortify 下 strncpy 会报可截断告警(此处用法安全:
    # 64B 零初始化缓冲 + 复制 63B, 尾部 NUL 保底); 与 agent-httpd 同款豁免。
    CFLAGS_EXTRA += -Wno-stringop-truncation
    LDFLAGS_EXTRA += -lcrypt -lm
else ifeq ($(UNAME_S),Darwin)
    CFLAGS_EXTRA += -D_DARWIN_C_SOURCE
endif
# BSD stat takes the format as -f%z, GNU stat as -c%s. Hardcoding one of them
# (native-bench used to) makes that target fail on the other platform.
STAT_SIZE := $(if $(filter Darwin,$(UNAME_S)),stat -f %z,stat -c %s)
CFLAGS  += $(CFLAGS_EXTRA)
LDFLAGS += $(LDFLAGS_EXTRA)

LLVM_CONFIG ?= $(shell command -v llvm-config 2>/dev/null)
ifeq ($(strip $(LLVM_CONFIG)),)
    # brew 的 llvm-config 不在 PATH 上(darwin/ARM 常见),但 opt/ 里的版本
    # 符号链接是稳定的 —— 没有再绕一层 find 的必要。
    LLVM_CONFIG := $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/llvm-config))
endif
# 「llvm-config 路径非空」不等于「llvm-config 能用」:brew 升级会把 opt/ 下的
# 符号链接换掉, 有人也可能手传了一个不存在的 LLVM_CONFIG。这两种情况下按旧
# 口径 HAVE_LLVM=1, 接着编 src/llvm_codegen.c 就是一记
# "fatal error: 'llvm-c/Core.h' file not found" —— 看着像缺依赖, 其实是探测
# 写活了。真的能问出版本号才算探测到, 否则老老实实走纯文本后端。
HAVE_LLVM := $(if $(strip $(shell $(LLVM_CONFIG) --version 2>/dev/null)),1,0)
# LLVM_CONFIG 要进依赖图: HAVE_LIBLLVM 是编译期宏、烧在 main.o 里, 探测结果
# 一变(比如容器里刚装完 llvm-dev)必须整批重编, 否则链接出的是「半新」二进制:
# llvm_codegen.o 是新编的, main.o 还是没那个宏的, --compile-llvm 照样拒绝。
# 但直接把 $(LLVM_CONFIG) 当目标名不行 —— 传裸名(在 PATH 上、当前目录里没这
# 个文件)或者路径还没装好时, make 报 "No rule to make target 'llvm-config-16'"
# 直接挂, 比不重编还糟。先归一成一个落地路径(存在的优先, 其次 PATH 解析)。
LLVM_CONFIG_PATH := $(or $(wildcard $(LLVM_CONFIG)),$(shell command -v $(LLVM_CONFIG) 2>/dev/null))

# 产物名刻意不叫 lume: 宿主 lume 编出来的也叫 bin/lume, 两棵树摆
# 在同一台机器上时能撞成「分不清哪个是哪个」(旧事故: PATH 上那个 lume 是旧产物、
# 不认新语法, 报错却怪到新代码头上)。这里统一加 -core 后缀, 宿主与语言本体一眼
# 分得开。另: 只改链接产物名, 不动 Makefile:17 那两个编译期 -D, 所以普通 make
# 就够, 不必 make -B —— 全量重建只在改目录名/改宏/改源文件时才必要。
TARGET   := bin/lume-core
# The fork ships the language-only examples. The server/demo examples from the
# host tree (hub.lume, invest.lume, react-ssr, abac, the sqlite demos) live in
# lume and are deliberately not carried over.
HELLO    := examples/hello.lume
MODULE_APP := examples/modules/app.lume
# --check on every example the suite type-checks. The native-* pair lives
# here too: they are the two scripts that exercise the --compile-* emitters,
# and without them a type error in a native example (a struct field, a member
# assignment) only showed up when someone hand-ran `make native-bench`.
EXAMPLES := $(HELLO) examples/lang-basics.lume $(MODULE_APP) examples/native-fact.lume examples/native-bench.lume examples/http-get.lume
# make native / make native-llvm 的靶子: 纯计算脚本(不含 server/route/tool),
# 原生后端覆盖得到。make native-bench 换成长循环的那个(显优化差别)。
NATIVE_EX := examples/native-fact.lume
BENCH_EX  := examples/native-bench.lume
# dev / dev-minimal 用的默认端口 (echo 与启动前清端口用)。
PORT ?= 8082
HUB_PORT ?= 8083

# $(call KILL_SERVER,port,bracket-stem) — 启动前清场:杀掉当前监听着 :port 的
# 进程,也按进程名杀掉仍在伺服该示例的旧 lume(bracket-stem 如 [h]ub.lume,
# 使 grep 模式匹配真正的进程,又不会匹配到杀手自己这条 sh)。普通与 --watch
# 两种启动形式都覆盖(模式里 .* 兼容 --watch 标志),然后轮询直到端口确实
# 释放,保证下一行能直接 bind,不会 "Address already in use"。
define KILL_SERVER
	@echo "==> freeing :$(1): killing stale lume ($(2))"; \
	{ lsof -ti tcp:$(1) 2>/dev/null; pgrep -f '$(TARGET).*$(2)' 2>/dev/null; } | sort -nu | xargs kill -9 2>/dev/null || true; \
	i=0; while lsof -ti tcp:$(1) >/dev/null 2>&1; do \
		lsof -ti tcp:$(1) 2>/dev/null | xargs kill -9 2>/dev/null || true; \
		i=$$((i+1)); [ $$i -ge 10 ] && break; sleep 0.3; \
	done
endef
SRCS     := src/main.c src/lexer.c src/parser.c src/parser_stmt.c src/parser_expr.c \
            src/value.c src/typecheck.c src/typecheck_expr.c src/typecheck_stmt.c \
            src/interp.c src/builtins.c src/builtins_fs.c src/os_win32.c \
            src/builtins_catalog.c src/builtins_hof.c src/builtins_str.c src/builtins_math.c src/builtins_crypt.c src/loader.c src/vdom.c \
            src/token.c src/bridge_stub.c src/bridge_serve.c src/bridge_mcp.c src/bridge_lsp.c src/bridge_native.c src/builtins_http.c \
            src/codegen.c src/codegen_types.c src/codegen_expr.c src/codegen_scan.c src/codegen_sig.c src/codegen_stmt.c src/irbuf.c src/backend.c
# --- 第二个原生后端:libLLVM C API(可选) ---------------------------------
# 手写 IR 文本那条路(codegen*.c + clang)不依赖 LLVM:bin/lume-core 保持 ~2MB。
# codegen.c 只是入口,发射器按区段拆在 codegen_{types,expr,scan,sig,stmt}.c,
# 共享面在 src/codegen_internal.h(Makefile 的 INT_HDRS 会通配到它)。
# 探测到 llvm-config 就额外编 src/llvm_codegen.c + src/backend_llvm.c, 用 LLVM
# 的 C API 建 IR、用 LLVMTargetMachineEmitToFile 自己出目标文件(--compile-llvm,
# 见 native-llvm)。探测不到就当它不存在, --compile 照旧可用。
# --- 出站 TLS(可选):http_get() 的 https:// --------------------------------
# 与 libLLVM 同款「有就用、没有只是缺」的口径:探测到 openssl 就把
# src/builtins_http.c 里 TLS 那段编进去(-DHAVE_OPENSSL=1 + -lssl -lcrypto);
# 探测不到时本树照样能编,http_get 对 https:// 直接报 "needs libssl" 而不是
# 悄悄退化成明文。传输层仍是裸 socket —— libssl 只负责 TLS 那一段。
# 与 LLVM 段一样用 +=,且必须待在 SRCS := 之后。
# pkg-config 在 macOS 上默认不存在(这台机器上 command -v pkg-config 是空),
# 所以不能只认它:没有它时退回 brew 的 openssl 前缀,再按前缀落盘地查头文件。
# 两条路都查不到就 HAVE_OPENSSL=0 —— build 照旧,只是 https:// 报 needs libssl。
OPENSSL_PKG := $(shell command -v pkg-config 2>/dev/null)
ifeq ($(strip $(OPENSSL_PKG)),)
    OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
    ifeq ($(strip $(OPENSSL_PREFIX)),)
        OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
    endif
else
    OPENSSL_PREFIX := $(shell pkg-config --variable=prefix openssl 2>/dev/null)
endif
HAVE_OPENSSL := $(if $(IS_WINDOWS),0,$(if $(and $(strip $(OPENSSL_PREFIX)),\
    $(wildcard $(OPENSSL_PREFIX)/include/openssl/ssl.h)),1,0))
ifeq ($(HAVE_OPENSSL),1)
    CFLAGS   += -I$(OPENSSL_PREFIX)/include -DHAVE_OPENSSL=1
    LDFLAGS  += -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
    # brew 的 dylib 在 opt/ 里,运行时 dyld 未必找得到:${prefix}/lib 显式加 rpath,
    # 免得链接过了、跑起来才 "image not found"。
    LDFLAGS  += -Wl,-rpath,$(OPENSSL_PREFIX)/lib
endif

# --- SQL(可选):sql_query / sql_write 的 SQLite 后端 ----------------------
# 与 openssl 同款「有就用、没有只是缺」的口径:能用 $(CC) 编过
# `#include <sqlite3.h>` 就把 src/builtins_sql.c 编进去(-DHAVE_SQLITE=1 +
# -lsqlite3);探测不到时本树照样能编,sql_* 仍在类型检查与运行时注册,
# 调用时明确报 "built without libsqlite3" 而不是静默缺失或 undefined。
# 探测用编译试编而不是 wildcard:macOS 的 SDK 头不在 /usr/include 下,
# Linux 只有装了 libsqlite3-dev 才有头文件,一条试编把两种口径都盖住。
# 注意 conftest 里不能出现 '#': make 解析期把它当注释吃掉会砍断 $(shell),
# 所以用 -include sqlite3.h 让编译器代为预包含。
# Windows/mingw 的 sysroot 没有 sqlite3,自然落进 stub 分支(与宿主
# Makefile 摘 -lsqlite3 的口径一致)。
SQLITE_CONF := $(shell printf 'int main(void){return 0;}\n' > build_conftest.c 2>/dev/null && $(CC) $(CFLAGS) -include sqlite3.h -c build_conftest.c -o build_conftest.o 2>/dev/null && echo 1; rm -f build_conftest.c build_conftest.o)
HAVE_SQLITE := $(if $(strip $(SQLITE_CONF)),1,0)
ifeq ($(HAVE_SQLITE),1)
    SRCS     += src/builtins_sql.c
    CFLAGS   += -DHAVE_SQLITE=1
    LDFLAGS  += -lsqlite3
endif
CFLAGS += -DLUME_HAS_SQLITE=$(HAVE_SQLITE)

# --- Windows / mingw-w64 native port -----------------------------------
# HTTP/TLS is opted out: builtins_http.c is excluded and LUME_HAS_HTTP is
# forced off, so the always-compiled b_http_* wrappers bind to the
# native_http_unavailable stubs in builtins.c instead of the (excluded)
# real implementation. --watch is already compiled out by #ifdef _WIN32 in
# main.c. The remaining platform spots (mkdir/flock/realpath) are handled
# with shims in the source, so a plain `make` in an MSYS2 mingw64 shell
# builds bin/lume-core.exe.
ifeq ($(IS_WINDOWS),1)
    SRCS := $(filter-out src/builtins_http.c,$(SRCS))
    HAVE_OPENSSL := 0
    LUME_HAS_HTTP := 0
    # bridge_serve.c / bridge_native.c link against winsock (winsock2.h).
    LDFLAGS_EXTRA += -lws2_32
endif
# Propagate the http flag to every TU so builtins.c emits the native_http_*
# stubs exactly when builtins_http.c is excluded (Windows) and not otherwise.
CFLAGS += -DLUME_HAS_HTTP=$(LUME_HAS_HTTP)

# 注意:这段必须在 SRCS := 之后 —— 这里用的是 +=,提前写会被上面的 := 盖掉。
ifeq ($(HAVE_LLVM),1)
    SRCS     += src/llvm_codegen.c src/backend_llvm.c
    CFLAGS   += $(shell $(LLVM_CONFIG) --cflags) -DHAVE_LIBLLVM=1
    LDFLAGS  += $(shell $(LLVM_CONFIG) --ldflags) $(shell $(LLVM_CONFIG) --libs core)
endif
OBJS     := $(SRCS:src/%.c=build/%.o)
# everything except main.o: the headless smoke driver calls the VM directly
CORE_OBJS := $(filter-out build/main.o, $(OBJS))

# 内部头:任一 * 片的共享声明变化,所有依赖它的 .o 都要重建
INT_HDRS := $(wildcard src/*_internal.h)

all: bin $(TARGET)

# Directory targets: on non-Windows (sh) `mkdir -p` is idempotent and stays
# in the recipe. On Windows, mingw32-make may run recipes under cmd.exe where
# mkdir has no -p and errors on existing directories (and the SHELL env var
# is unreliable - Git Bash injects sh paths even when cmd runs the recipe),
# so the Windows branch creates build/ and bin/ at parse time via a plain
# `$(shell mkdir -p build bin 2>/dev/null)`. The previous form used
# `$(shell cmd /c "if not exist build mkdir build & if not exist bin mkdir
# bin")`, whose `cmd`/`&`/quoting confused the CI make/SHELL into
# "/bin/sh: --: invalid option" + "missing separator" at parse time. CI's
# $(shell) runs /bin/sh, so a bare POSIX `mkdir -p` is robust and needs no
# quoting. The `build: ;` / `bin: ;` empty recipes keep `make -B` from
# failing on directories that already exist.
ifeq ($(strip $(IS_WINDOWS)),)
build:
	mkdir -p build
bin:
	mkdir -p bin
else
$(shell mkdir -p build bin 2>/dev/null)
build: ;
bin: ;
endif

build/%.o: src/%.c src/lume.h $(INT_HDRS) | build $(AH_LIB)
	$(CC) $(CFLAGS) -c $< -o $@

# HAVE_LIBLLVM is a compile-time macro, so it is baked into main.o, not only
# into llvm_codegen.o. Rebuilding only the libLLVM translation units after
# llvm-config showed up (or after a version bump) links a half-fresh binary:
# the new IR emitter, the old refusal branch, and `--compile-llvm` still
# answering "needs a build with libLLVM". Making the probe result a prerequisite
# rebuilds the whole batch the moment it changes — including the first time it
# goes from empty to found, which is the case above.
$(OBJS): $(LLVM_CONFIG_PATH)

$(TARGET): $(OBJS) $(AH_LIB) | bin
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(AH_LIB) $(LDFLAGS) -lm

check: all crypt-test builtins-test
	@for f in $(EXAMPLES); do ./$(TARGET) --check $$f || exit 1; done

# 原生后端端到端: --compile 产出可执行, 跑一遍再和解释器口径下的期望比对。
# 产物落在 out/<stem>, 同时落 out/<stem>.ll / .o, 链接失败时可以直接看 IR。
# 默认原生路径。--compile 现在走的是「默认后端」:编进 libLLVM 就用 libLLVM
# (IR 由 LLVM 自己校验形状),没编就用文本后端;想固定用文本走 make native-text。
native: all
	@set -e; \
	stem=$$(basename "$(NATIVE_EX)" .lume); \
	./$(TARGET) --compile $(NATIVE_EX); \
	./out/$$stem > out/$$stem.log; \
	if diff -u tests/$$stem.expected out/$$stem.log > out/$$stem.diff; then \
	    echo "PASS native $$stem"; \
	else \
	    echo "FAIL native $$stem"; cat out/$$stem.diff; exit 1; \
	fi

# 文本后端端到端(--compile-text): 手写 IR 文本 + clang -O2 收尾。显式指定,
# 这样即使本机编进了 libLLVM 也能固定走这条回归路径。
native-text: all
	@set -e; \
	stem=$$(basename "$(NATIVE_EX)" .lume); \
	./$(TARGET) --compile-text $(NATIVE_EX); \
	./out/$$stem > out/$$stem.log; \
	if diff -u tests/$$stem.expected out/$$stem.log > out/$$stem.diff; then \
	    echo "PASS native-text $$stem"; \
	else \
	    echo "FAIL native-text $$stem"; cat out/$$stem.diff; exit 1; \
	fi

# 两个 IR 发射器(手写文本 vs LLVM C API)的横向一致性。tests/native_backends.sh
# 跑 tests/native-consistency.lume 三种路径(解释器 / 文本 / libLLVM), 各自跟
# 基线比对, 再互比, 并且把编译器任何 stderr 都当失败 —— 让校验器抱怨的 IR 不算
# 「编译通过」。挂在 make test 上, 分叉就会被套件拦住而不是以后以错答案的形式
# 冒出来。
# LUME_BIN 必须显式注入: native_backends.sh 里那个默认值还是宿主的产物名
# bin/lume, 不传就会拿一个根本不存在的文件当编译器, 报 "bin/lume missing — run
# make first" —— 看着像没先 make, 其实是路径没对上。asan 那条腿早就这么传了,
# 这里把口径补齐; 以后再改产物名, 这条腿不会跟着哑掉。
native-consistency: all
	@chmod +x tests/native_backends.sh && LUME_BIN=./$(TARGET) ./tests/native_backends.sh

# libLLVM 后端端到端(--compile-llvm): IR 由 LLVM 的 C API 建出来, 目标文件
# 也由 LLVM 自己出。回归口径与 native 一致 —— 同一份期望基线, 两个后端都得对。
native-llvm: all
ifeq ($(HAVE_LLVM),1)
	@set -e; \
	stem=$$(basename "$(NATIVE_EX)" .lume); \
	./$(TARGET) --compile-llvm $(NATIVE_EX); \
	./out/$$stem > out/$$stem.log; \
	if diff -u tests/$$stem.expected out/$$stem.log > out/$$stem.diff; then \
	    echo "PASS native-llvm $$stem"; \
	else \
	    echo "FAIL native-llvm $$stem"; cat out/$$stem.diff; exit 1; \
	fi
else
	@echo "SKIP native-llvm: llvm-config not found (text backend still available)"; exit 0
endif

# 两个原生后端的横向对比。靶子是循环密集的 examples/native-bench.lume ——
# 局部变量都躺在 alloca 槽里,所以「能不能把 alloca 提升成寄存器」这件事
# 在这里直接表现为运行耗时(文本后端靠 clang -O2 的 mem2reg,libLLVM 后端
# 跑 LLVMRunPasses 的 default<O2>)。每一行做过输出校验,跑歪了直接 FAIL。
#
# 通了 pass 之后 run-ms 那一列基本只能说「两边都跑上了优化」,真正的哨兵是
# 最后一列 nopass-ms:它用 --no-pass 跳过 pipeline 再编一次同一份源码,
# 于是「mem2reg 到底有没有生效」又变成能看见的数字(实测 llvm 腿 6ms → 244ms)。
# 哪天 pipeline 悄悄失效,这一列会先跳起来,而 run-ms 那一列还傻乎乎地报很快。
# 列: 编译总耗时 / IR 体积 / 目标文件体积 / 可执行体积 / 运行耗时 / 无 pass 对照
# (3 次取中位)。
native-bench: all
	@set -e; \
	stem=$$(basename "$(BENCH_EX)" .lume); \
	kb() { if [ -f "$$1" ]; then echo $$(( `$(STAT_SIZE) "$$1"` / 1024 )); else echo 0; fi; }; \
	printf '%-9s %9s %8s %8s %8s %9s %10s\n' backend compile-ms ir-KiB obj-KiB bin-KiB run-ms nopass-ms; \
	for be in text llvm; do \
	    if [ "$$be" = llvm ] && [ "$(HAVE_LLVM)" != 1 ]; then \
	        printf '%-9s %9s\n' llvm "(skipped: no llvm-config)"; continue; \
	    fi; \
	    if [ "$$be" = text ]; then flag=--compile-text; else flag=--compile-llvm; fi; \
	    rm -f out/$$stem.ll out/$$stem.o out/$$stem; \
	    s=$$(date +%s%N); \
	    ./$(TARGET) $$flag "$(BENCH_EX)" >/dev/null; \
	    e=$$(date +%s%N); \
	    ./out/$$stem > out/$$stem.log; \
	    if ! diff -q tests/$$stem.expected out/$$stem.log >/dev/null; then \
	        echo "FAIL native-bench $$be"; diff -u tests/$$stem.expected out/$$stem.log; exit 1; \
	    fi; \
	    run=$$(i=0; while [ $$i -lt 3 ]; do a=$$(date +%s%N); \
	        ./out/$$stem >/dev/null; b=$$(date +%s%N); \
	        echo $$(( (b - a) / 1000000 )); i=$$((i+1)); done | sort -n | sed -n 2p); \
	    if [ "$$be" = text ]; then nop='-'; else \
	        rm -f out/$$stem; \
	        ./$(TARGET) --compile-llvm --no-pass "$(BENCH_EX)" >/dev/null; \
	        ./out/$$stem > out/$$stem.log; \
	        if ! diff -q tests/$$stem.expected out/$$stem.log >/dev/null; then \
	            echo "FAIL native-bench $$be (no-pass build)"; \
	            diff -u tests/$$stem.expected out/$$stem.log; exit 1; \
	        fi; \
	        nop=$$(i=0; while [ $$i -lt 3 ]; do a=$$(date +%s%N); \
	            ./out/$$stem >/dev/null; b=$$(date +%s%N); \
	            echo $$(( (b - a) / 1000000 )); i=$$((i+1)); done | sort -n | sed -n 2p); \
	    fi; \
	    printf '%-9s %9s %8s %8s %8s %9s %10s\n' "$$be" $$(( (e - s) / 1000000 )) \
	        "$$(kb out/$$stem.ll)" "$$(kb out/$$stem.o)" "$$(kb out/$$stem)" "$$run" "$$nop"; \
	done

dump: all
	./$(TARGET) --dump $(HELLO)

# The VS Code C/C++ extension reads its include path from
# .vscode/c_cpp_properties.json, not from this Makefile — so without it
# src/backend_llvm.h and src/llvm_codegen.c show
# "cannot find source file llvm-c/Core.h ... (1696)". The path is
# version-stamped, so it is generated, not hardcoded: re-run this after a brew
# upgrade of llvm (or on a fresh clone where LLVM lives elsewhere) instead of
# hand-editing the JSON. Needs python3.
vscode-cpp:
	@mkdir -p .vscode
	@python3 scripts/gen_c_cpp_properties.py "$(LLVM_CONFIG)"


# tests/tools-bin (tests/tools_driver.c) is not carried into this fork: that
# driver links the agent-httpd tool tables. The interpreter unit tests, the
# crypto unit test and both native emitters are what `test` covers here.
# Core regression: interpreter unit tests + crypto + both native paths. There
# is no server here, so there is no run_all.sh leg (that is the host tree's
# HTTP/e2e suite) and no `ui` bundle — those only exist in lume.
test: all backend-parity tests/smoke-bin crypt-test builtins-test ternary-test enum-test match2-test eq2-test interp2-test http-noproxy-test map-index-test sql-test docroot-test native-consistency case-parity \
      native native-text treecheck

# --- 两后端 infer_node_type/str() 分派表 parity 闸门 -----------------------
# 两个原生后端是独立翻译单元，各持一份私有 infer_node_type；某个节点/类型
# kind 只加在一边 = "一边接受一边拒绝" 的静默漂移（struct-update 轮的
# N_STRUCT_UPDATE、str() 轮的 TY_STRING 都是这么进来的）。运行期三后端对拍
# 归 native-consistency 管，这里静态抓"缺 case"这一类。
case-parity:
	@bash tests/case-parity.sh

# --- 发布面 (make pack) ---------------------------------------------------
# 这条路以前根本不存在: 没有任何 pack/release/tar 目标, .gitignore 连 /bin/ 都
# 忽略, 于是「发一个语言包」只能手工 cp —— 没有清单、没有版本、发完也复现不了。
# 产物名先改成 lume-core 就是为了跟宿主的 lume 分开, 现在顺手把通路补上。
# 包里带 README / README.zh / CHANGELOG: 解开就能看懂这是什么, 而不是一个裸
# 二进制加一句「用吧」。os/arch 从 uname 推, 要别的口径就 make pack PKG_NAME=…
# 覆盖。注意不要在这里用 tar 的 --transform/-s 做改名: macOS 是 bsdtar(认 -s)、
# 容器里是 GNU tar(不认 -s), 跨平台脚本会一条腿挂掉。
UNAME_M := $(shell uname -m)
ifeq ($(IS_WINDOWS),1)
    PKG_OS := windows
else ifeq ($(UNAME_S),Darwin)
    PKG_OS := darwin
else
    PKG_OS := linux
endif
PKG_NAME ?= lume-core-$(PKG_OS)-$(UNAME_M)
# 发布口径跟开发口径不是同一套, 差别只有一处: PACK_CFLAGS 把 rt.c 的绝对
# 路径摘掉了(filter-out RT_DEFS), backend.c 于是走它自己的 fallback 值
# "src/rt.c"。而且必须**整批重编**到 build-pack/, 光重链没用 ——
#   - 那个绝对路径先是 -DLUME_RT_SRC 在**编译期**烤进 backend.o rodata 的
#     字符串字面量(-g 又另存了一份在 DWARF 里)。重链换不掉字面量,
#     strip 也剥不掉字面量; 只有让 backend.c 带着新值重新编译才行。
#     strip 能剥掉的是 DWARF 那份, 不是字面量那份。
# 所以 strip 和 PACK_CFLAGS 都得在, packcheck 是唯一能证明两条都办到了的
# 东西: 只 strip 的包照样带着字面量。
# 不往 bin/lume-core 上重链: 那是开发产物, 得留着本机那份 rt 路径。
PACK_RT_DEFS  := -DLUME_RT_SRC=\"src/rt.c\"
PACK_CFLAGS   := $(filter-out $(RT_DEFS),$(CFLAGS)) $(PACK_RT_DEFS)
PACK_BIN      := build-pack/bin/lume-core
PACK_OBJS     := $(SRCS:src/%.c=build-pack/%.o)

# strip 的口径按平台分。ld64(cctools)的 `-u -r` 在 GNU binutils 上根本不存在
# ——`-u` 只有 cctools 有(实测 `strip --help` 里既无 `-u` 也无 `--keep-undefined`),
# GNU strip 见到它直接打 usage 退出(make 报 Error 1),而 CI 的 pack 腿就跑在
# ubuntu 上:一条 macOS 专用的开关让整条发布腿常年红。
# 两边要的东西是同一件:剥掉带编译目录的 DWARF,同时留下能正常加载的二进制。
#   - darwin: `-u -r` 保住未定义符号与重定位(裸 strip 会把动态链接要用的东西
#     一起削掉);被剥掉的仍是调试段那份路径。
#   - 其余: `--strip-debug` 就是「只去调试段」,语义与上面一一对应。用长选项
#     而不是 `-g`:GNU strip 与 llvm-strip 都认 `--strip-debug`(实测两家
#     `--help` 都在),写全名就不必赌哪家有哪些短别名。
ifeq ($(UNAME_S),Darwin)
    STRIP_FLAGS := -u -r
else
    STRIP_FLAGS := --strip-debug
endif

build-pack:
	mkdir -p build-pack

build-pack/%.o: src/%.c src/lume.h $(INT_HDRS) | build-pack
	$(CC) $(PACK_CFLAGS) -c $< -o $@

$(PACK_BIN): $(PACK_OBJS) | bin
	@mkdir -p $(dir $@)   # 输出在 build-pack/bin/ 下, | bin 管不到它
	$(CC) $(PACK_CFLAGS) -o $@ $(PACK_OBJS) $(AH_LIB) $(LDFLAGS) -lm
	@strip $(STRIP_FLAGS) $@

pack: all $(PACK_BIN)
	@mkdir -p dist
	# tar 只能有一个 -C, 而文档在仓库根、产物在 build-pack/, 先归拢到同一处
	@cp README.md README.zh.md CHANGELOG.md build-pack/
	tar -czf dist/$(PKG_NAME).tar.gz -C build-pack \
	    README.md README.zh.md CHANGELOG.md bin/lume-core
	@echo "==> packed dist/$(PKG_NAME).tar.gz (bin/lume-core + docs)"
	@./scripts/check-pack-privacy.sh dist/$(PKG_NAME).tar.gz
	@echo "==> packcheck passed: no build-machine paths in dist/$(PKG_NAME).tar.gz"

# --- 严格静态检查 (make lint) ------------------------------------------------
# 项目自己的 CFLAGS 只到 -Wall -Wextra。lint 再往上加一层「诚实度」开关: 它们
# 不查逻辑 bug, 查的是**类型有没有撒谎** —— 字段声明成 const 却被写/被 free,
# 或者一个非 static 函数从来没被原型声明过(签名改了没人拦)。
# 现在刻意不含 -Wcast-qual / -Wwrite-strings: 全树只剩一处(codegen.c 与
# llvm_codegen.c 给合成的顶层函数赋 `as.func.name = "top"`), 而那个字段正是
# node_free_own() 会 free 的 —— 想消掉它只能再补一次「诚实地说这是谎」的强转,
# 那比留着一条告警更糟。等 parser 把 node_free_own 导出成可跨 TU 调用, 这里再补。
# -Werror 不是洁癖: cc 即使打了告警也返 0(实测 `cc -fsyntax-only -Wall` 撞一条
# unused-variable 退出码仍是 0),不加它这条 lint 永远绿 —— 等于没守。
LINT_FLAGS := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wno-unused-parameter -Werror
# rt.c 平时是 backend.c 用裸 `cc -O2 -c`(零开关)在构建期单独编的, 所以它的签名
# 漂移从来没人查; 这里补上。llvm_codegen.c 要 LLVM 头文件, 不在 lint 范围内 ——
# 它本来也不在这个 fork 的默认构建里, 走 --compile-llvm 的人才编得到。
LINT_SRCS  := src/rt.c $(filter-out src/llvm_codegen.c,$(SRCS))

lint:
	@echo "==> lint $(words $(LINT_SRCS)) source file(s)"
	@$(CC) $(CFLAGS) $(LINT_FLAGS) $(LINT_SRCS) -fsyntax-only

# 单独跑: 检查打好的包里没有构建机路径 (dist/ 被 gitignore, 日常构建不产包,
# 所以这条平时只在 make pack 之后、或 CI 里有意义)。
packcheck:
	@pkg="dist/$(PKG_NAME).tar.gz"; \
	if [ ! -f "$$pkg" ]; then \
	    echo "==> FAIL $$pkg not found, run 'make pack' first"; exit 1; \
	fi; \
	./scripts/check-pack-privacy.sh "$$pkg"

# 仓库自身(不是发布包)也不能带构建机路径。make pack 那条只覆盖 dist/, 而一个
# 未 strip 的开发二进制(packcheck 根本不看它)能把 /Users/<user>/... 直接 commit
# 进仓库 —— 2026-10-05 的 backups/*.bak 就是这么进来的。挂进 test, 挡在
# 「误把产物 commit 进来」这一层上。
treecheck:
	@chmod +x scripts/check-tree-privacy.sh
	@./scripts/check-tree-privacy.sh
	@echo "==> treecheck passed: no build-machine path tracked in HEAD"

clean:
	rm -rf build build-pack bin build-asan

# --- ASan/UBSan 构建 (make asan) ------------------------------------------
# 独立构建目录 build-asan/,不污染正常 build/。检出 lume 侧代码的
# 堆/栈越界与 UB。
# 本树没有静态库的「不插桩」问题:全部对象都是这一个二进制的一部分,插桩覆盖
# 到每一处内存访问。原先 `asan-e2e` 单独存在,是因为它要覆盖 agent-httpd 的
# 静态库(宿主编译、不插桩);这条腿随 bridge/iquest 一起没迁过来,`asan` 现在
# 只跑语言侧。别因此以为「asan 绿 = 宿主侧也干净」。
# macOS clang 与 Linux gcc 均支持 -fsanitize=address,undefined。
ASAN_CFLAGS   := -fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_LDFLAGS  := -fsanitize=address,undefined
# Sanitizer 运行期选项, `asan` 目标里 8 条腿共用这一处。**默认 LSan 就是开
# 的** (空值 = ASan 默认值 detect_leaks=1), 实测数字见 `asan` 目标上方。
# 想临时豁免某次排查: `make asan ASAN_BASE_OPTS="detect_leaks=0"`。
# 别去逐条删腿: 那 8 处必须同步, 漏一条那条腿就静默豁免了。
ASAN_BASE_OPTS ?=
# Exported rather than folded into ASAN_BASE_OPTS: LSan reads LSAN_OPTIONS, and
# doing it here means the flag survives without the 8 ASAN_OPTIONS call sites
# having to name a suppression file each (one of them missed it is how a leg
# silently stopped being leak-checked). Override with `make LSAN_OPTIONS=`.
# print_suppressions=0 because ASan echoes the suppression table on stderr
# whenever one is used, and tests/native_backends.sh reads stderr as compiler
# diagnostics — the table was failing the libLLVM leg for not being one.
# The suppressions= key has to live here rather than in ASAN_OPTIONS: passed
# that way it does not parse at all (LSan answers "failed to parse
# suppressions" and exits 1 even for a file holding nothing but the rule).
# This entry is load-bearing, not cosmetic: without it the libLLVM leg leaks
# one byte and fails.
LSAN_OPTIONS ?= $(if $(wildcard tests/lsan-suppressions.txt),suppressions=$(CURDIR)/tests/lsan-suppressions.txt print_suppressions=0)
export LSAN_OPTIONS
# 与主产物同款后缀, 免得 bin/lume-asan 和 bin/lume-core 混着看不出配套关系。
ASAN_TARGET   := bin/lume-core-asan
ASAN_OBJS     := $(SRCS:src/%.c=build-asan/%.o)
# Same reason as the $(OBJS) prerequisite above: swapping llvm-config in or
# changing its version changes -DHAVE_LIBLLVM, which main.o has to be rebuilt
# with. Without this the sanitized binary keeps its refusal branch and the
# libLLVM leg stays skipped under a build that does have libLLVM.
$(ASAN_OBJS): $(LLVM_CONFIG_PATH) | build-asan
ASAN_CORE_OBJS := $(filter-out build-asan/main.o, $(ASAN_OBJS))

build-asan:
	mkdir -p build-asan

build/tests:
	mkdir -p build/tests

build-asan/tests:
	mkdir -p build-asan/tests

build-asan/%.o: src/%.c src/lume.h $(INT_HDRS) | build-asan $(AH_LIB)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) -c $< -o $@

$(ASAN_TARGET): $(ASAN_OBJS) $(AH_LIB) | build-asan bin
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) -o $@ $(ASAN_OBJS) $(AH_LIB) $(LDFLAGS) $(ASAN_LDFLAGS) -lm

# was missing in the agent-httpd detach: `test:` referenced tests/smoke-bin
# but nothing ever compiled it, so the interpreter unit tests kept running
# whatever stale binary happened to be on disk. Rebuilt whenever smoke.c
# changes now.
tests/smoke-bin: tests/smoke.c $(CORE_OBJS) build/tests | $(AH_LIB)
	$(CC) $(CFLAGS) -o $@ tests/smoke.c $(CORE_OBJS) $(AH_LIB) $(LDFLAGS) -lm

tests/smoke-bin-asan: tests/smoke.c $(ASAN_CORE_OBJS) build-asan/tests | $(AH_LIB)
	$(CC) $(CFLAGS) $(ASAN_CFLAGS) -o $@ tests/smoke.c $(ASAN_CORE_OBJS) $(AH_LIB) $(LDFLAGS) $(ASAN_LDFLAGS) -lm

# tests/tools-bin-asan is not carried in (same reason as tests/tools-bin).
asan: $(ASAN_TARGET) tests/smoke-bin-asan
	@echo "==> ASan/UBSan: --check 全部示例 + lang-basics 直跑 + 单测 + 工具派发"
	# LSan 现在是开着的 (ASAN_BASE_OPTS 默认为空 = ASan 默认 detect_leaks=1)。
	# 开放之前的实测基线, 2026-10-03 在 gcc:12 / Ubuntu bookworm arm64 容器里
	# 跑 LSan 全开: SUMMARY: 6091427 byte(s) leaked in 42939 allocation(s), rc=1
	# —— 也就是说 CI 一放开就是稳定红, 当时只能维持 detect_leaks=0。
	#
	# 归零靠的是把编译期对象图真的补上 free, 不是 suppression。收敛过程中:
	#   - smoke: 389 字节 / 24 块 -> 262 / 7 -> **0** (117 tests, 0 failed)
	#   - tools: 2500 -> **0**;  examples/*.lume --check: **0**; lang-basics: **0**
	#   - --compile-text: 222 / 32 -> **0**;  --compile-llvm: 13001 / 299 -> **0**
	# 六类腿现在都是 SUMMARY: 0 byte(s) leaked。
	# 其中三条根因值得记下来, 都不是「ck_expr 的中间 Type」那种误报:
	#   - parser.c 孤儿表: nalloc() 登记每个节点, parse_program() 放弃时整批
	#     node_free_orphan() 回收。递归下降在语句中途失败时 returns NULL, 沿途
	#     各自丢弃已经拼好的半棵树 —— `print("a" 1);` 就漏掉一个 call 节点。
	#   - node_free 拆成两趟 (kids / own) 才有孤儿表可用: 批量回收时子节点可能
	#     已被前一次回收释放, walk 进去就是 use-after-free。
	#   - value.c 的 gc_collect() 只 free(o) 不调 obj_release(), 于是每次 GC 都
	#     漏掉 Obj 内部的 gc_cstr 名与 map/env 键。只有 --compile-* 这种会反复
	#     触发 GC 的腿跑得够大能显形; --check 和单测一直绿着。
	# 另外两条: main.c 的 --compile-llvm 拒绝路径在 vm_init 之后 return 1, 漏了
	# vm_teardown (整堆); codegen_expr.c 的 val_make() 本来就 xstrdup 一次, 外面再
	# 套一层 xstrdup(name) 就是双重分配 (val_take 接手所有权 + cg_free 收尾)。
	# 剩下那条「写 suppression 豁免」的选项被否决: 代价是以后新增的泄漏被静默
	# 吞掉, suppression 自己也会随代码腐化 —— 而且真补完发现一个都不需要。
	#
	# ASAN_BASE_OPTS 是唯一开关, 别去逐条删腿: 那 8 处必须同步, 漏一条那条腿就
	# 静默豁免了。想临时豁免一次排查: make asan ASAN_BASE_OPTS="detect_leaks=0"。
	# --watch reload 走 fork+exec 子进程重启, 无累积路径, 那一维同样干净。
	#
	# 两个平台的 LSan 完全是两回事, 别混为一谈:
	#   - macOS: LSan 根本不可用 (ASan 打印 "detect_leaks is not supported on
	#     this platform"), 这条选项在这里是 no-op。别以为本地 leak 检测在跑;
	#     它从来没跑起来过, 也就没法在本地证明绿。
	#   - CI (.github/workflows/ci.yml 的 asan job 在 ubuntu-latest 跑
	#     `make -j4 asan`): LSan 可用。这正是之前那条 detect_leaks=0 唯一真正
	#     生效的地方, 现在它默认开着 —— CI 对泄漏有覆盖了, 也正是在 CI 上才会
	#     红。新增泄漏请在那边看。该 job 装了 llvm-dev, 所以 libLLVM 后端的腿
	#     是真的在跑, 不是 skip —— ubuntu-24.04 的 llvm-dev 解析到 LLVM 18
	#     (noble 上 llvm-dev = 1:18.0-59~exp2), 这条腿在 LLVM 14 / 16 / 18 上
	#     都实测过: 三后端输出互相对齐 + LSan 全开无 SUMMARY。CI 首次跑的就是 18。
	#
	# 全表只有一条 suppression (tests/lsan-suppressions.txt), 别当成「可以
	# 随手加」的口子: libLLVM 的 LLVMVerifyModule 在 LLVM 14 / 16 / 18 上都会
	# 丢一个 1 字节的空串, 那份指针 C API 不交出来, lume 侧无从释放 —— 用独立
	# 的最小 C 程序(只调 C API)复现过, 不是我们自己的分配。按函数名匹配而
	# 不是按库名, 所以 llvm_codegen.c / backend_llvm.c 自己的泄漏照样报。
	# 它是承重的: 抽掉这条, libLLVM 腿就漏这 1 字节并 fail。
	# 它经 LSAN_OPTIONS 导出, 不用去改那 8 处 ASAN_OPTIONS —— 反过来把
	# suppressions= 写进 ASAN_OPTIONS 是不认的 (LSan 直接 "failed to parse
	# suppressions" 并 exit 1, 连只有规则行的文件也一样)。
	@for f in $(EXAMPLES); do ASAN_OPTIONS=$(ASAN_BASE_OPTS) ./$(ASAN_TARGET) --check $$f || exit 1; done
	@ASAN_OPTIONS=$(ASAN_BASE_OPTS) ./$(ASAN_TARGET) examples/lang-basics.lume >/dev/null || exit 1
	# The two IR emitters only run on --compile-*, so without this the native
	# backends (codegen.c / llvm_codegen.c) are never exercised under a
	# sanitizer at all.
	@ASAN_OPTIONS=$(ASAN_BASE_OPTS) ./$(ASAN_TARGET) --compile-text examples/native-fact.lume >/dev/null || exit 1
ifeq ($(HAVE_LLVM),1)
	@ASAN_OPTIONS=$(ASAN_BASE_OPTS) ./$(ASAN_TARGET) --compile-llvm examples/native-fact.lume >/dev/null || exit 1
endif
	# The legs above pin the leak count, but a backend can be leak-clean and
	# still wrong in a way no exit code sees: IR that compiles, links, exits
	# 0 and prints nothing (codegen.c once freed the synthetic `top` node
	# before the entry point read it, and main() lost its `call void @L_top()`
	# — every leg above stayed green). native_backends.sh diffs each leg
	# against tests/native-consistency.expected and each pair against the
	# others, and treats any compiler diagnostic as a failure, so the shape is
	# caught here instead of becoming a wrong answer elsewhere.
	@ASAN_OPTIONS=$(ASAN_BASE_OPTS) LUME_BIN=./$(ASAN_TARGET) \
		./tests/native_backends.sh || exit 1
	@ASAN_OPTIONS=$(ASAN_BASE_OPTS) ./tests/smoke-bin-asan || exit 1
	@echo "ok   ASan/UBSan all passed"

# `build` / `bin` / `build-asan` are listed too even though they are also real
# directory names: their recipe is just `mkdir -p`, so claiming them phony
# costs nothing and stops make from ever trying to "rebuild" the directory
# after a `make clean` removed it.
.PHONY: all build bin build-asan build-pack check dump vscode-cpp test clean asan native native-text \
         native-llvm native-bench native-consistency crypt-test backend-parity pack packcheck \
         treecheck
# crypt_sha512 内建单测（glibc 生成 $6$ / macOS 平台报错 都算 PASS）。
crypt-test: all
	@./$(TARGET) tests/test-crypt.lume > /tmp/lume-crypt-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-crypt-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-crypt-test.out; then \
		echo "==> crypt 单测失败 (rc=$$rc)"; exit 1; fi; \
	echo "==> crypt 单测全部通过"

# sort() / str_match() / regex() 通用内建回归(解释器路径, 与 json() 一致)。
builtins-test: all
	@./$(TARGET) tests/builtins-sort-regex.lume > /tmp/lume-builtins-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-builtins-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-builtins-test.out; then \
		echo "==> builtins 单测失败 (rc=$$rc)"; exit 1; fi; \
	echo "==> builtins 单测全部通过"

# 三元运算符 (2026-10-12): 值/优先级/右结合/int-float 加宽/map 值位置/
# 与 `f()?` 传播后缀的前瞻消歧/惰性分支。解释器口径; 三后端一致值由
# native-consistency 覆盖。
ternary-test: all
	@./$(TARGET) tests/ternary.lume > /tmp/lume-ternary-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-ternary-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-ternary-test.out; then \
		echo "==> ternary 单测失败 (rc=$$rc)"; exit 1; fi; \
	echo "==> ternary 单测全部通过"

# enum + match (2026-10-12): 解释器 19 断言全 PASS; 8 个编译期拒绝各断言
# 自己的诊断句(穷尽性/未知变体/重复臂/arity/载荷类型/非 enum match/构造名
# 遮蔽/bind 数); 双原生后端同文件跑出同一份输出。值语义的横向一致由
# native-consistency.lume 的 enum 探针段覆盖。
enum-test: all
	@./$(TARGET) tests/enum.lume > /tmp/lume-enum-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-enum-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-enum-test.out; then \
		echo "==> enum 单测失败 (rc=$$rc)"; exit 1; fi; \
	set -e; \
	check_bad() { f=$$1; key=$$2; \
	  rc=0; ./$(TARGET) --check tests/enum-bad/$$f.lume > /tmp/lume-enum-bad.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q "$$key" /tmp/lume-enum-bad.out; then \
	    echo "==> enum 负例 $$f 未被拒或诊断不符 (rc=$$rc):"; cat /tmp/lume-enum-bad.out; exit 1; fi; \
	  echo "ok   enum-bad $$f: 拒绝且诊断准确"; }; \
	check_bad bad-arity "expects 1 argument(s), got 2"; \
	check_bad bad-payload-type "cannot use a value of type string where float is expected"; \
	check_bad dup-arm "duplicate match arm"; \
	check_bad match-non-enum "expected variant name in pattern"; \
	check_bad missing-variant "does not cover variant"; \
	check_bad shadow-ctor "shadows an enum constructor"; \
	check_bad unknown-variant "has no variant"; \
	check_bad wrong-binds "payload field(s), arm binds"; \
	for be in text llvm; do \
	  if [ "$$be" = llvm ] && ./$(TARGET) --compile-llvm tests/enum.lume -o /dev/null 2>&1 | grep -q "needs a build with libLLVM"; then \
	    echo "ok   enum: libLLVM 后端跳过(无 libLLVM)"; continue; fi; \
	  if [ "$$be" = llvm ]; then flag=--compile-llvm; else flag=--compile-text; fi; \
	  ./$(TARGET) $$flag tests/enum.lume -o /tmp/enum_$$be > /dev/null 2>&1 || { echo "==> enum $$be 编译失败"; exit 1; }; \
	  /tmp/enum_$$be > /tmp/enum_$$be.out 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ] || grep -q FAIL /tmp/enum_$$be.out || [ $$(grep -c PASS /tmp/enum_$$be.out) -ne 19 ]; then \
	    echo "==> enum $$be 输出不符 (rc=$$rc):"; cat /tmp/enum_$$be.out; exit 1; fi; \
	  echo "ok   enum: $$be 后端 19/19"; \
	done; \
	echo "==> enum 单测通过"

# or 模式 / 匹配守卫 / if-let 单测(tests/match2.lume + tests/match2-bad/*)。
# 正向: 解释器 20 条断言 + 两个原生后端各编各跑, 输出必须全 PASS 无 FAIL;
# 负例: 9 个 --check 拒绝, 诊断关键字逐个钉死(改名/改措辞都会在这里红)。
match2-test: all
	@./$(TARGET) tests/match2.lume > /tmp/lume-match2-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-match2-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-match2-test.out; then \
		echo "==> match2 单测失败 (rc=$$rc)"; exit 1; fi; \
	if [ $$(grep -c PASS /tmp/lume-match2-test.out) -ne 20 ]; then \
		echo "==> match2 断言数不符 (期望 20)"; exit 1; fi; \
	set -e; \
	check_bad() { f=$$1; key=$$2; \
	  rc=0; ./$(TARGET) --check tests/match2-bad/$$f.lume > /tmp/lume-match2-bad.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q "$$key" /tmp/lume-match2-bad.out; then \
	    echo "==> match2 负例 $$f 未被拒或诊断不符 (rc=$$rc):"; cat /tmp/lume-match2-bad.out; exit 1; fi; \
	  echo "ok   match2-bad $$f: 拒绝且诊断准确"; }; \
	check_bad or-bind-names "or-pattern alternatives must bind the same names"; \
	check_bad or-bind-types "or-pattern binding .* has conflicting types"; \
	check_bad guard-only "does not cover variant"; \
	check_bad guard-non-bool "expected bool, got float"; \
	check_bad iflet-non-enum "if-let needs an enum value"; \
	check_bad iflet-binds "payload field(s), arm binds"; \
	check_bad iflet-scope "undefined variable"; \
	check_bad iflet-unknown "has no variant"; \
	check_bad or-dup "duplicate match pattern"; \
	for be in text llvm; do \
	  if [ "$$be" = llvm ] && ./$(TARGET) --compile-llvm tests/match2.lume -o /dev/null 2>&1 | grep -q "needs a build with libLLVM"; then \
	    echo "ok   match2: libLLVM 后端跳过(无 libLLVM)"; continue; fi; \
	  if [ "$$be" = llvm ]; then flag=--compile-llvm; else flag=--compile-text; fi; \
	  ./$(TARGET) $$flag tests/match2.lume -o /tmp/match2_$$be > /dev/null 2>&1 || { echo "==> match2 $$be 编译失败"; exit 1; }; \
	  /tmp/match2_$$be > /tmp/match2_$$be.out 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ] || grep -q FAIL /tmp/match2_$$be.out || [ $$(grep -c PASS /tmp/match2_$$be.out) -ne 20 ]; then \
	    echo "==> match2 $$be 输出不符 (rc=$$rc):"; cat /tmp/match2_$$be.out; exit 1; fi; \
	  echo "ok   match2: $$be 后端 20/20"; \
	done; \
	echo "==> match2 单测通过"

# 结构性相等单测(tests/eq2.lume + tests/eq2-bad/*)。SPEC §1.8:
# == / != 对容器是结构比较(list 逐元素、map 键顺序无关、enum 比 "$"+载荷、
# struct 按字段), null 只与 i8* 类比较, 跨等价类静态拒绝。
# 正向: 解释器 43 条断言 + 两个原生后端各编各跑全 PASS;
# 负例: 7 个 --check 拒绝, 诊断逐个钉死(改名/改措辞都会在这里红)。
eq2-test: all
	@./$(TARGET) tests/eq2.lume > /tmp/lume-eq2-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-eq2-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-eq2-test.out; then \
		echo "==> eq2 单测失败 (rc=$$rc)"; exit 1; fi; \
	if [ $$(grep -c PASS /tmp/lume-eq2-test.out) -ne 43 ]; then \
		echo "==> eq2 断言数不符 (期望 43)"; exit 1; fi; \
	set -e; \
	check_bad() { f=$$1; key=$$2; \
	  rc=0; ./$(TARGET) --check tests/eq2-bad/$$f.lume > /tmp/lume-eq2-bad.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q "$$key" /tmp/lume-eq2-bad.out; then \
	    echo "==> eq2 负例 $$f 未被拒或诊断不符 (rc=$$rc):"; cat /tmp/lume-eq2-bad.out; exit 1; fi; \
	  echo "ok   eq2-bad $$f: 拒绝且诊断准确"; }; \
	check_bad num-str "does not apply to int and string"; \
	check_bad bool-num "does not apply to bool and int"; \
	check_bad str-bool "does not apply to string and bool"; \
	check_bad ctr-num "does not apply to int\[\] and int"; \
	check_bad map-str "does not apply to map and string"; \
	check_bad num-null "does not apply to int and null"; \
	check_bad bool-null "does not apply to bool and null"; \
	for be in text llvm; do \
	  if [ "$$be" = llvm ] && ./$(TARGET) --compile-llvm tests/eq2.lume -o /dev/null 2>&1 | grep -q "needs a build with libLLVM"; then \
	    echo "ok   eq2: libLLVM 后端跳过(无 libLLVM)"; continue; fi; \
	  if [ "$$be" = llvm ]; then flag=--compile-llvm; else flag=--compile-text; fi; \
	  ./$(TARGET) $$flag tests/eq2.lume -o /tmp/eq2_$$be > /dev/null 2>&1 || { echo "==> eq2 $$be 编译失败"; exit 1; }; \
	  /tmp/eq2_$$be > /tmp/eq2_$$be.out 2>&1; rc=$$?; \
	  if [ $$rc -ne 0 ] || grep -q FAIL /tmp/eq2_$$be.out || [ $$(grep -c PASS /tmp/eq2_$$be.out) -ne 43 ]; then \
	    echo "==> eq2 $$be 输出不符 (rc=$$rc):"; cat /tmp/eq2_$$be.out; exit 1; fi; \
	  echo "ok   eq2: $$be 后端 43/43"; \
	done; \
	echo "==> eq2 单测通过"

# --- interp2: 字符串插值 s!"..." + 结构体更新 { base with ... } (仅解释器) ---
# 三后端一致由 native-consistency 的插值/更新探针段覆盖, 这里只验解释器腿的
# 39 条断言 + 8 个负例诊断键。
interp2-test: all
	@./$(TARGET) tests/interp2.lume > /tmp/lume-interp2-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-interp2-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-interp2-test.out; then \
		echo "==> interp2 单测失败 (rc=$$rc)"; exit 1; fi; \
	if [ $$(grep -c PASS /tmp/lume-interp2-test.out) -ne 54 ]; then \
		echo "==> interp2 断言数不符 (期望 54)"; exit 1; fi; \
	check_bad() { f=$$1; key=$$2; \
	  rc=0; ./$(TARGET) --check tests/interp2-bad/$$f.lume > /tmp/lume-interp2-bad.out 2>&1 || rc=$$?; \
	  if [ $$rc -eq 0 ] || ! grep -q "$$key" /tmp/lume-interp2-bad.out; then \
	    echo "==> interp2 负例 $$f 未被拒或诊断不符 (rc=$$rc):"; cat /tmp/lume-interp2-bad.out; exit 1; fi; \
	  echo "ok   interp2-bad $$f: 拒绝且诊断准确"; }; \
	check_bad istr-bad-expr "unexpected token 'end-of-file' in expression"; \
	check_bad istr-noquote "unterminated interpolated string literal"; \
	check_bad istr-unclosed "unterminated interpolation segment (missing '}')"; \
	check_bad with-dup-field "duplicate field 'x' in with update"; \
	check_bad with-empty "with update needs at least one field"; \
	check_bad with-scalar "'with' update requires a map or struct base"; \
	check_bad with-type-mismatch "struct field: cannot use a value of type string where int is expected"; \
	check_bad with-unknown-field "unknown field 'z' in struct update"; \
	echo "==> interp2 单测通过"


# NO_PROXY 绕过代理、直连内网地址。起一个本地 server(127.0.0.1:18891),
# 把 HTTP_PROXY 指到死端口 127.0.0.1:9 作对照:绕过失效会撞死代理 => FAIL。
http-noproxy-test: all
	@PORT=18891; \
	 python3 tests/http_noproxy_srv.py $$PORT > /tmp/lume-http-noproxy-srv.log 2>&1 & \
	 SRV=$$!; \
	 sleep 1; \
	 HTTP_PROXY=http://127.0.0.1:9 LUME_HTTP_PROXY= NO_PROXY=127.0.0.1 \
	   ./$(TARGET) tests/builtins-http-noproxy.lume > /tmp/lume-http-noproxy.out 2>&1; \
	 rc=$$?; \
	 kill $$SRV 2>/dev/null; \
	 cat /tmp/lume-http-noproxy.out; \
	 if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-http-noproxy.out; then \
	   echo "==> http no_proxy 单测失败 (rc=$$rc)"; exit 1; fi; \
	 echo "==> http no_proxy 单测通过"

# map 下标标量读: 合规路径(get()/标注/可解析字面量)三后端一致; 未标注的
# 「变量 map」标量读在原生后端给出准确、可操作的错误(不再是误导性的
# "no such key")。见 tests/map-index-scalar*.lume 与 src/rt.c 的
# g_index_fail_kind。
map-index-test: all
	@printf '7\nlume\n2\n' > /tmp/mi_expected; \
	 for be in text llvm; do \
	   if [ "$$be" = llvm ]; then \
	     flag=--compile-llvm; \
	     if ./$(TARGET) --compile-llvm tests/map-index-scalar.lume -o /tmp/mi_p 2>&1 | grep -q "needs a build with libLLVM"; then \
	       echo "ok   map-index: libLLVM 后端跳过(无 libLLVM)"; continue; fi; \
	   else flag=--compile-text; fi; \
	   ./$(TARGET) $$flag tests/map-index-scalar.lume -o /tmp/mi_$$be > /dev/null 2>&1 || { echo "==> map-index $$be 合规路径编译失败"; exit 1; }; \
	   /tmp/mi_$$be > /tmp/mi_$$be.out 2>&1; rc=$$?; \
	   if [ $$rc -ne 0 ] || ! diff -q /tmp/mi_expected /tmp/mi_$$be.out > /dev/null; then \
	     echo "==> map-index $$be 合规路径输出不符 (rc=$$rc):"; cat /tmp/mi_$$be.out; exit 1; fi; \
	   ./$(TARGET) $$flag tests/map-index-scalar-bad.lume -o /tmp/mib_$$be > /dev/null 2>&1 || true; \
	   /tmp/mib_$$be > /tmp/mib_$$be.out 2>&1; rc=$$?; \
	   if [ $$rc -eq 0 ] || ! grep -q "map value at this key is a scalar" /tmp/mib_$$be.out; then \
	     echo "==> map-index $$be 未标注标量读未给出准确错误 (rc=$$rc):"; cat /tmp/mib_$$be.out; exit 1; fi; \
	   echo "ok   map-index: $$be 合规路径正确 + 标量读报错准确"; \
	 done; \
	 echo "==> map-index 单测通过"

# SQLite (2026-10-12): tests/sqlite.lume 自带可用性探针, stub 构建整段 SKIP。
sql-test: all
	@./$(TARGET) tests/sqlite.lume > /tmp/lume-sql-test.out 2>&1; \
	rc=$$?; cat /tmp/lume-sql-test.out; \
	if [ $$rc -ne 0 ] || grep -q FAIL /tmp/lume-sql-test.out; then \
		echo "==> sqlite 单测失败 (rc=$$rc)"; exit 1; fi; \
	echo "==> sqlite 单测通过"

# docroot 静态服务 (2026-10-12): 路由优先; 静态 200/404; 穿越路径(含
# URL 编码 %2e%2e)一律 404。--path-as-is 防 curl 把 ../ 归一化掉。
docroot-test: all
	@./$(TARGET) tests/docroot-srv.lume > /tmp/lume-docroot-srv.log 2>&1 & \
	 SRV=$$!; sleep 1; \
	 fail=0; \
	 probe() { url="$$1"; want_code="$$2"; want_sub="$$3"; \
	   out=$$(curl -s --noproxy '*' --path-as-is -w '\n%{http_code}' "http://127.0.0.1:18892$$url"); \
	   code=$$(printf '%s\n' "$$out" | tail -1); \
	   body=$$(printf '%s\n' "$$out" | sed '$$d'); \
	   if [ "$$code" != "$$want_code" ]; then echo "FAIL docroot $$url: code=$$code want=$$want_code"; fail=1; \
	   elif [ -n "$$want_sub" ] && printf '%s' "$$body" | grep -q "$$want_sub"; then echo "ok   docroot $$url"; \
	   elif [ -n "$$want_sub" ]; then echo "FAIL docroot $$url: body missing '$$want_sub'"; printf '%s\n' "$$body"; fail=1; \
	   else echo "ok   docroot $$url ($$code)"; fi; }; \
	 probe "/" 200 docroot-ok; \
	 probe "/index.html" 200 docroot-ok; \
	 probe "/app.css" 200 "color: #333"; \
	 probe "/sub/" 200 sub-index-ok; \
	 probe "/api" 200 '"src":"route"'; \
	 probe "/api/item/42" 200 '"id":"42"'; \
	 probe "/missing.txt" 404 ""; \
	 probe "/../secret.txt" 404 ""; \
	 probe "/%2e%2e/secret.txt" 404 ""; \
	 probe "/sub/../../secret.txt" 404 ""; \
	 kill $$SRV 2>/dev/null; \
	 if [ $$fail -ne 0 ]; then echo "==> docroot 单测失败"; exit 1; fi; \
	 echo "==> docroot 单测通过"

# 静态检查：两个原生后端 (text / libLLVM) 必须覆盖同一组 AST 节点类型，
# 否则单边新增节点会静默漂移（另一端掉进 default:）。见脚本内注释。
backend-parity:
	@chmod +x scripts/check-backend-parity.sh
	@./scripts/check-backend-parity.sh
