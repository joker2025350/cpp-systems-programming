# ============================================================
# dns-client 模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/libdnsclient.a
#   make cli        只构建命令行工具 bin/dns-client
#   make test       构建并运行报文构造的离线用例（链接 libdnsclient.a）
#   make check      跑命令行行为检查（缺参数、退出码、输出）
#   make sanitize   ASan + UBSan 重新构建并跑 test 与 check（产物在 bin/sanitize/）
#   make bench      暂无吞吐可量，打印原因后正常退出
#   make clean      删除 bin/（含全部变体）
#
# 2026-11-27 拆库：报文构造、应答解析、连接管理从原来的单文件程序里搬进
# libdnsclient.a，src/main.c 退化成薄驱动。驱动只链接静态库、不重复编译模块
# 源码——"库能被独立链接"因此每构建一次就被验证一次。
#
# test 与 check 的分工：test 是库的单元用例（进程内，不碰网络）；
# check 是命令行这个交付物的端到端行为。入库阶段两者混在一个 smoke.sh 里，
# 拆库之后各归各位。
# ============================================================

CC       ?= cc
AR       ?= ar

CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g

# -Iinclude：dnsclient.c 与 main.c 都用 #include "dnsclient.h"，
# 不给这个目录就得写相对路径，两处写法迟早不一致。
#
# -D_POSIX_C_SOURCE=200809L：-std=c11 定义 __STRICT_ANSI__，关掉 glibc 的默认扩展，
# 不给这个宏就看不到 strdup 这些 POSIX 声明。
#
# -D_DEFAULT_SOURCE 必须一起给：random / srandom / bzero 属于 BSD/SVID 遗留接口，
# glibc 把它们藏在 __USE_MISC 之后，而只开 _POSIX_C_SOURCE 时 __USE_MISC 是关的
# （features.h 的规则：_POSIX_C_SOURCE 与 _DEFAULT_SOURCE 互斥，只留更严的那个）。
# 结果是这两个函数变成隐式声明——按 C11 6.5.2.2p6 隐式声明已被移除，
# 函数返回类型不是 int 时行为未定义。原始代码正是这么编过去的。
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

# 变体构建通过命令行覆盖它。原因与其它模块相同：make 只比较时间戳、
# 不比较编译选项，共用 bin/ 会留下插桩过的二进制。
OUT      ?= bin

LIB      := $(OUT)/libdnsclient.a
CLI      := $(OUT)/dns-client
TESTMSG  := $(OUT)/test_message

LIB_OBJ  := $(OUT)/dnsclient.o
CLI_OBJ  := $(OUT)/main.o

.PHONY: all lib cli test check sanitize sanitize-run bench clean

all: lib cli

lib: $(LIB)

cli: $(CLI)

$(OUT):
	@mkdir -p $(OUT)

# 模式规则：src/*.c -> $(OUT)/*.o
$(OUT)/%.o: src/%.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

# 命令行工具只链接静态库，不重复编译模块源码。
$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTMSG): tests/test_message.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

test: $(TESTMSG)
	@echo
	@$(TESTMSG)

check: $(CLI)
	@sh tests/smoke.sh $(CLI)

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: all $(TESTMSG)
	@echo "\n--- 报文构造用例（ASan + UBSan）---"
	@$(TESTMSG)
	@echo "\n--- 命令行行为（ASan + UBSan）---"
	@sh tests/smoke.sh $(CLI)

# 现在没有可量的东西：单次查询的墙钟时间几乎全是到 DNS 服务器的 RTT，
# 程序本身只有一次 malloc 和两次 memcpy，量出来的是网络噪声。有意义的是
# 批量 + 并发在飞的吞吐，那要先有复用 socket 的结构（README 第 9 节第 12 条）。
# 留这个目标是为了让根 Makefile 的 make bench 不必按模块写例外。
bench:
	@printf 'dns-client：暂无可量的吞吐——单次查询的耗时由 RTT 主导，\n'
	@printf '  批量并发在飞的结构尚未实现（README 第 9 节第 12 条）。\n'

clean:
	rm -rf $(OUT)

