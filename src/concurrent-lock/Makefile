# ============================================================
# 并发计数器模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/libcounter.a
#   make cli        只构建命令行工具 bin/counter
#   make test       构建并运行单元测试
#   make check      池驱动的端到端检查（tests/check_pool.sh）
#   make bench      五种实现的吞吐对比，以及两种驱动方式的对照
#   make demo       两组对照，观察 raw 实现的更新丢失
#   make sanitize   ASan + UBSan 重新构建并跑测试（产物在 bin/sanitize/）
#   make tsan       TSan 重新构建并跑测试（跳过 raw，产物在 bin/tsan/）
#   make clean      删除 bin/（含全部变体）
# ============================================================

CC       ?= cc
AR       ?= ar

# pthread 既是编译期宏（定义线程安全相关的重入属性）也是链接期库，
# 因此放进 CFLAGS 而不是 LDLIBS，两边都不会漏。
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g -pthread

# 输出目录。所有编译产物都落在这里，变体构建通过命令行覆盖它。
#
# 为什么变体要分目录：make 判断"要不要重建"只比较目标与依赖的修改时间，
# 不比较编译选项。如果 sanitize / tsan 与普通构建共用 bin/，
# 那么先 make tsan 再 make 时，普通构建会认为 bin/counter 已经是最新的，
# 于是留下一个 TSan 插桩过的可执行文件 —— 它一运行就报
# "FATAL: ThreadSanitizer: unexpected memory mapping"，而且报错信息
# 和"你刚刚 make 了一下"之间看不出任何因果关系。
OUT      ?= bin

# ------------------------------------------------------------------
# 跨模块依赖：池驱动的任务提交
#
# 只有驱动（src/main.c）用 thread-pool；libcounter.a 里没有任何对它的引用，
# 因此把本模块当库用的调用方不必链接线程池。依赖方向单向：
#   驱动层 -> libthreadpool.a
#   库层   -> 无外部依赖
#
# 做法与 word-count 一致：本模块自己把 thread-pool 的静态库建到自己的 $(OUT)
# 下，而不是直接链 ../thread-pool/bin/libthreadpool.a。原因有三条：
#   1. 直接链别人的 bin/ 会让"从干净树 make -C src/concurrent-lock"失败；
#   2. sanitize / tsan 变体需要的是插桩过的池，共用普通构建那份等于
#      "测试跑过了，但池里发生的事从没被 ASan / TSan 检查过"；
#   3. clean 要能一次清干净本模块的所有产物。
# 代价是根 make 会把 thread-pool 编译两遍（word-count 那边已经有一份），
# 每次几百毫秒。
#
# POOLOUT 必须写在 OUT 之后：:= 是立即展开，OUT 还没定义时它会变成
# "/thread-pool"，然后 make 会真的去创建根目录下的 /thread-pool。
# ------------------------------------------------------------------
POOLDIR  := ../thread-pool
POOLINC  := $(POOLDIR)/include
POOLOUT  := $(OUT)/thread-pool
POOLLIB  := $(POOLOUT)/libthreadpool.a

# -D_POSIX_C_SOURCE=200809L 是必须的：-std=c11 会定义 __STRICT_ANSI__，
# 关掉 glibc 的默认扩展，pthread_spin_lock 与 nanosleep 的声明就看不到了。
# 驱动层还要用 getopt（解析 -k / -t / -p / -b），同样依赖这个宏。
CPPFLAGS += -Iinclude -I$(POOLINC) -D_POSIX_C_SOURCE=200809L
LDLIBS   += -pthread

LIB      := $(OUT)/libcounter.a
CLI      := $(OUT)/counter
TESTBIN  := $(OUT)/test_counter

LIB_OBJ  := $(OUT)/counter.o
CLI_OBJ  := $(OUT)/main.o

.PHONY: all lib cli test check bench demo sanitize sanitize-run tsan tsan-run clean

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

# 把 thread-pool 的静态库建到本模块的输出目录里。
# OUT 必须传绝对路径：子 make 的工作目录是 $(POOLDIR)，相对路径会落到
# ../thread-pool/ 之下而不是本模块的 $(OUT) 里。CFLAGS 原样透传，
# 保证 sanitize / tsan 变体连池一起插桩。
$(POOLLIB):
	@$(MAKE) --no-print-directory -C $(POOLDIR) lib \
		OUT=$(abspath $(POOLOUT)) CFLAGS='$(CFLAGS)'

# 命令行工具只链接静态库，不重复编译模块源码
$(CLI): $(CLI_OBJ) $(LIB) $(POOLLIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTBIN): tests/test_counter.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

test: $(TESTBIN)
	@echo
	@$(TESTBIN)

check: $(CLI)
	@sh tests/check_pool.sh $(CLI)

bench: $(CLI)
	@echo
	@$(CLI) -k all -t 10 -n 1000000 -r 3
	@echo
	@echo "===== 池驱动：任务粒度扫描（8 个工作线程，总自增次数固定 2^20）====="
	@for b in 0 1 8 64 512; do \
		if [ $$b -eq 0 ]; then n=1048576; else n=$$((1048576 / b)); fi; \
		$(CLI) -k atomic -p -t 8 -n $$n -b $$b -r 3 | grep -E '^(driver|atomic|池统计)'; \
	done
	@echo
	@echo "===== 池驱动：batch=1 时五种实现（任务粒度 = 一次自增）====="
	@$(CLI) -k all -p -t 8 -n 1048576 -b 1 -r 3

demo: $(CLI)
	@echo "===== 高竞争：10 线程各 100 万次，不加 sleep ====="
	@echo
	@$(CLI) -k raw -t 10 -n 1000000 -r 1
	@echo
	@echo "===== 同样的线程数，每次自增后睡 1ms ====="
	@echo
	@$(CLI) -k raw -t 10 -n 200 -s -r 1

# 变体构建：覆盖 OUT 与 CFLAGS 递归调用自身。
# 命令行传入的变量优先级最高，所以覆盖面不会被文件里的 ?= / += 顶掉。
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: all $(TESTBIN)
	@echo "\n--- 单元测试（ASan + UBSan）---"
	@$(TESTBIN)
	@echo "\n--- 命令行（ASan + UBSan）---"
	@$(CLI) -k atomic -t 4 -n 100000 -r 1
	@echo "\n--- 池驱动（ASan + UBSan）---"
	@$(CLI) -k all -p -t 4 -n 20000 -b 8 -r 1

# TSan 只能单独用：它和 ASan 的运行时插桩互斥，不能同时链接。
# setarch -R 关闭 ASLR —— TSan 依赖固定的地址布局来映射影子内存，
# 开着 ASLR 时可能一开始就报 "unexpected memory mapping"。
tsan:
	@$(MAKE) --no-print-directory OUT=$(OUT)/tsan \
		CFLAGS='$(CFLAGS) -fsanitize=thread -O1 -g' \
		CPPFLAGS='$(CPPFLAGS) -DCOUNTER_TEST_SKIP_RAW' tsan-run

tsan-run: all $(TESTBIN)
	@echo "\n--- 单元测试（ThreadSanitizer，跳过 raw）---"
	@setarch $$(uname -m) -R $(TESTBIN)
	@
	@# 池驱动也过一遍 TSan：池自己的队列与计数器内部的锁同时被检查，
	@# 这正是"两个模块接起来"之后新增的那部分代码路径。
	@echo "\n--- 池驱动（ThreadSanitizer，只跑 atomic）---"
	@setarch $$(uname -m) -R $(CLI) -k atomic -p -t 4 -n 2000 -b 4 -r 1

clean:
	rm -rf $(OUT)
