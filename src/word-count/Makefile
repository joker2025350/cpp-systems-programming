# ============================================================
# 单词统计模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/libwordcount.a
#   make cli        只构建命令行工具 bin/wordcount
#   make test       构建并运行单元测试
#   make check      与 wc 交叉验证 + 并行/顺序输出一致性检查
#   make sanitize   ASan + UBSan 重新构建并跑测试（产物在 bin/sanitize/）
#   make tsan       TSan 重新构建并跑测试（产物在 bin/tsan/）
#   make bench      缓冲区大小与并行度的吞吐测量
#   make clean      删除 bin/（含全部变体）
# ============================================================

CC       ?= cc
AR       ?= ar

# -Wconversion 用于捕捉隐式窄化转换。本模块对字节的读写都要求显式类型转换，
# 打开它可以防止将来改动时引入静默截断。
# -pthread 是必须的：驱动层（src/main.c）链接线程池，需要 pthread 的编译期宏
# 与链接期库。库本身不引用 pthread，但编译选项是全模块共用的。
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g -pthread

# 输出目录。变体构建（sanitize / tsan）通过命令行覆盖它，与普通构建分开存放：
# make 只比较时间戳，不比较编译选项，共用目录会让普通构建把插桩过的
# 可执行文件当成"已是最新"，于是运行时报 ASan/UBSan 的链接期符号错误。
OUT      ?= bin

# ------------------------------------------------------------------
# 跨模块依赖：并行驱动
#
# 只有 src/main.c 用 thread-pool 提交任务；libwordcount.a 里没有任何对它的
# 引用，因此库的使用者不必链接 pthread。依赖方向是单向的：
#   驱动层 -> libthreadpool.a
#   库层   -> 无外部依赖
#
# 不直接链接 ../thread-pool/bin/libthreadpool.a，原因有三条：
#   1. 那样构建本模块就要求 thread-pool 事先被单独 make 过，
#      从干净的工作树执行 make -C src/word-count 会直接失败；
#   2. sanitize / tsan 变体需要的是插桩过的池，
#      共用 thread-pool 的 bin/ 会拿到普通构建的那一份，
#      结果是"测试跑过了，但池里发生的错误从没被 ASan 检查过"；
#   3. 变体产物要落在本模块自己的输出目录下，clean 才能一次清干净。
# 代价是根 make 会把 thread-pool 编译两遍（各自一份 bin/）。编译它只要
# 几百毫秒，换来的是每个模块都能独立构建，这笔换算是划算的。
# ------------------------------------------------------------------
POOLDIR  := ../thread-pool
POOLINC  := $(POOLDIR)/include
POOLOUT  := $(OUT)/thread-pool
POOLLIB  := $(POOLOUT)/libthreadpool.a

# -D_POSIX_C_SOURCE=200809L 是必须的：-std=c11 定义 __STRICT_ANSI__，
# 关掉 glibc 的默认扩展，getopt（驱动层解析 -j / -h）与 nanosleep 的声明
# 就看不到了。不定义它时 getopt 会退化成隐式声明。
CPPFLAGS += -Iinclude -I$(POOLINC) -D_POSIX_C_SOURCE=200809L
LDLIBS   += -pthread

LIB      := $(OUT)/libwordcount.a
CLI      := $(OUT)/wordcount
TESTBIN  := $(OUT)/test_wordcount

LIB_OBJ  := $(OUT)/wordcount.o
CLI_OBJ  := $(OUT)/main.o

.PHONY: all lib cli test check sanitize sanitize-run tsan tsan-run bench clean

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
# OUT 必须传绝对路径：子 make 的工作目录是 $(POOLDIR)，
# 相对路径会落到 ../thread-pool/ 之下而不是本模块的 $(OUT) 里。
# CFLAGS 原样透传，保证 sanitize / tsan 变体连池一起插桩。
$(POOLLIB):
	@$(MAKE) --no-print-directory -C $(POOLDIR) lib \
		OUT=$(abspath $(POOLOUT)) CFLAGS='$(CFLAGS)'

# 命令行工具只链接静态库，不重复编译模块源码，
# 这样能保证"模块能被独立链接"这件事在每次构建时都被验证。
$(CLI): $(CLI_OBJ) $(LIB) $(POOLLIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTBIN): tests/test_wordcount.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

test: $(TESTBIN)
	@echo
	@$(TESTBIN)

check: $(CLI)
	@sh tests/check_against_wc.sh $(CLI)
	@echo
	@sh tests/check_parallel.sh $(CLI)

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: all $(TESTBIN)
	@echo "\n--- 单元测试（ASan + UBSan）---"
	@$(TESTBIN)
	@echo "\n--- 命令行工具（ASan + UBSan）---"
	@$(CLI) testdata/a.txt testdata/b.txt
	@echo "\n--- 并行路径（ASan + UBSan）---"
	@$(CLI) -j 4 testdata/a.txt testdata/b.txt testdata/a.txt
	@echo "\n--- 错误路径 ---"
	@$(CLI) testdata/nonexistent.txt || true

# TSan 与 ASan 的运行时插桩互斥，不能同时链接。
# setarch -R 关闭 ASLR：TSan 需要固定的地址布局来映射影子内存，
# 开着 ASLR 时可能一开始就报 "unexpected memory mapping"。
tsan:
	@$(MAKE) --no-print-directory OUT=$(OUT)/tsan \
		CFLAGS='$(CFLAGS) -fsanitize=thread -O1 -g' tsan-run

tsan-run: all $(TESTBIN)
	@echo "\n--- 单元测试（ThreadSanitizer）---"
	@setarch $$(uname -m) -R $(TESTBIN)
	@echo "\n--- 并行驱动（ThreadSanitizer）---"
	@setarch $$(uname -m) -R $(CLI) -j 4 testdata/a.txt testdata/b.txt testdata/a.txt testdata/b.txt
	@echo "\n--- 并行驱动 + 不存在的文件（ThreadSanitizer）---"
	@setarch $$(uname -m) -R $(CLI) -j 3 testdata/a.txt testdata/nonexistent.txt testdata/b.txt || true

bench: $(CLI) | $(OUT)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=256   src/wordcount.c src/main.c $(POOLLIB) -o $(OUT)/wc_b256
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=1024  src/wordcount.c src/main.c $(POOLLIB) -o $(OUT)/wc_b1k
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=4096  src/wordcount.c src/main.c $(POOLLIB) -o $(OUT)/wc_b4k
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=16384 src/wordcount.c src/main.c $(POOLLIB) -o $(OUT)/wc_b16k
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=65536 src/wordcount.c src/main.c $(POOLLIB) -o $(OUT)/wc_b64k
	@# 合成一个较大的输入，使进程启动开销相对可忽略（约 12 MB）
	@rm -f $(OUT)/bench_input.txt
	@i=0; while [ $$i -lt 20 ]; do cat testdata/b.txt >> $(OUT)/bench_input.txt; i=$$((i + 1)); done
	@echo "输入：$(OUT)/bench_input.txt，各跑 20 轮"
	@for tag in b256 b1k b4k b16k b64k; do \
		start=$$(date +%s%N); \
		for i in $$(seq 1 20); do $(OUT)/wc_$$tag $(OUT)/bench_input.txt > /dev/null; done; \
		end=$$(date +%s%N); \
		printf "%-6s %6d ms\n" "$$tag" $$(( (end - start) / 1000000 )); \
	done
	@# 并行度：8 个文件，每个 8 份 b.txt，合计约 39 MB。
	@# 文件之间互相独立，正是 -j 的适用场景：扫描是 CPU 密集的，
	@# 而输入在页缓存里，不需要等磁盘。
	@rm -rf $(OUT)/bench_files
	@mkdir -p $(OUT)/bench_files
	@i=0; while [ $$i -lt 8 ]; do \
		n=0; \
		while [ $$n -lt 8 ]; do cat testdata/b.txt >> $(OUT)/bench_files/p$$i.txt; n=$$((n + 1)); done; \
		i=$$((i + 1)); \
	done
	@echo
	@echo "并行度：8 个文件，合计 $$(cat $(OUT)/bench_files/*.txt | wc -c) 字节，各跑 5 轮"
	@for j in 0 1 2 4 8; do \
		start=$$(date +%s%N); \
		for r in $$(seq 1 5); do \
			if [ "$$j" = "0" ]; then \
				$(CLI) $(OUT)/bench_files/*.txt > /dev/null; \
			else \
				$(CLI) -j $$j $(OUT)/bench_files/*.txt > /dev/null; \
			fi; \
		done; \
		end=$$(date +%s%N); \
		if [ "$$j" = "0" ]; then label=seq; else label="-j $$j"; fi; \
		printf "  %-6s %6d ms（5 轮合计）\n" "$$label" $$(( (end - start) / 1000000 )); \
	done

clean:
	rm -rf $(OUT)
