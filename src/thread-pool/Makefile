# ============================================================
# 线程池模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/libthreadpool.a
#   make cli        只构建命令行工具 bin/threadpool
#   make test       构建并运行单元测试
#   make check      命令行端到端检查
#   make bench      三组基准（空任务 / 每任务建线程 / 固定工作量加速比）
#   make sanitize   ASan + UBSan 重新构建并跑测试（产物在 bin/sanitize/）
#   make tsan       TSan 重新构建并跑测试（产物在 bin/tsan/）
#   make clean      删除 bin/（含全部变体）
# ============================================================

CC       ?= cc
AR       ?= ar

# pthread 既是编译期开关（影响可重入相关声明）也是链接期库，放在 CFLAGS 里，
# 编译与链接两边都不会漏。
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g -pthread

# -D_POSIX_C_SOURCE=200809L 是必须的：-std=c11 定义 __STRICT_ANSI__，
# 关掉 glibc 的默认扩展，nanosleep / clock_gettime 的声明就看不到了。
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   += -pthread

# 输出目录。变体构建通过命令行覆盖它。
#
# 为什么必须分目录：make 判断"要不要重建"只比较时间戳，不比较编译选项。
# sanitize / tsan 与普通构建共用 bin/ 时，普通构建会认为目标已是最新，
# 于是留下插桩过的二进制 —— 它一运行就报 sanitizer 的地址映射错误，
# 而报错与"刚刚 make 了一下"之间看不出因果关系。
OUT      ?= bin

LIB      := $(OUT)/libthreadpool.a
CLI      := $(OUT)/threadpool
TESTBIN  := $(OUT)/test_threadpool
BENCHBIN := $(OUT)/bench_threadpool

LIB_OBJ  := $(OUT)/threadpool.o
CLI_OBJ  := $(OUT)/main.o

.PHONY: all lib cli test check bench sanitize sanitize-run tsan tsan-run clean

all: lib cli

lib: $(LIB)

cli: $(CLI)

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/%.o: src/%.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

# 命令行工具只链接静态库，不重复编译模块源码
$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTBIN): tests/test_threadpool.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

$(BENCHBIN): tests/bench_threadpool.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

test: $(TESTBIN)
	@echo
	@$(TESTBIN)

check: $(CLI)
	@echo
	@sh tests/check_cli.sh $(CLI)

bench: $(BENCHBIN)
	@echo
	@$(BENCHBIN)

# 变体构建：覆盖 OUT 与 CFLAGS 递归调用自身。
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: all $(TESTBIN)
	@echo "\n--- 单元测试（ASan + UBSan）---"
	@$(TESTBIN)
	@echo "\n--- 命令行（ASan + UBSan）---"
	@$(CLI) run -t 4 -n 1000 -w 0

# TSan 与 ASan 的运行时插桩互斥，不能同时链接。
# setarch -R 关闭 ASLR：TSan 需要固定的地址布局来映射影子内存，
# 开着 ASLR 时可能一开始就报 "unexpected memory mapping"。
tsan:
	@$(MAKE) --no-print-directory OUT=$(OUT)/tsan \
		CFLAGS='$(CFLAGS) -fsanitize=thread -O1 -g' tsan-run

tsan-run: all $(TESTBIN)
	@echo "\n--- 单元测试（ThreadSanitizer）---"
	@setarch $$(uname -m) -R $(TESTBIN)

clean:
	rm -rf $(OUT)
