# ============================================================
# 05 单词统计模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/libwordcount.a
#   make cli        只构建命令行工具 bin/wordcount
#   make test       构建并运行单元测试
#   make check      用 testdata 下的文件与 wc 做交叉验证
#   make sanitize   用 ASan + UBSan 重新构建并跑测试
#   make bench      测量不同缓冲区大小下的吞吐
#   make clean      删除 bin/
# ============================================================

CC       ?= cc
AR       ?= ar

# -Wconversion 用于捕捉隐式窄化转换。本模块对字节的读写都要求显式类型转换，
# 打开它可以防止将来改动时引入静默截断。
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g
CPPFLAGS += -Iinclude

BIN      := bin
LIB      := $(BIN)/libwordcount.a
CLI      := $(BIN)/wordcount
TESTBIN  := $(BIN)/test_wordcount

LIB_OBJ  := $(BIN)/wordcount.o
CLI_OBJ  := $(BIN)/main.o

.PHONY: all lib cli test check sanitize bench clean

all: lib cli

lib: $(LIB)

cli: $(CLI)

$(BIN):
	@mkdir -p $(BIN)

# 模式规则：src/*.c -> bin/*.o
$(BIN)/%.o: src/%.c | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

# 命令行工具只链接静态库，不重复编译模块源码，
# 这样能保证"模块能被独立链接"这件事在每次构建时都被验证。
$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTBIN): tests/test_wordcount.c $(LIB) | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

test: $(TESTBIN)
	@echo
	@$(TESTBIN)

check: $(CLI)
	@sh tests/check_against_wc.sh

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
sanitize: clean all $(TESTBIN)
	@echo "\n--- 单元测试（ASan + UBSan）---"
	@$(TESTBIN)
	@echo "\n--- 命令行工具（ASan + UBSan）---"
	@$(CLI) testdata/a.txt testdata/b.txt
	@echo "\n--- 错误路径 ---"
	@$(CLI) testdata/nonexistent.txt || true

bench: $(CLI) | $(BIN)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=256   src/wordcount.c src/main.c -o $(BIN)/wc_b256
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=1024  src/wordcount.c src/main.c -o $(BIN)/wc_b1k
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=4096  src/wordcount.c src/main.c -o $(BIN)/wc_b4k
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=16384 src/wordcount.c src/main.c -o $(BIN)/wc_b16k
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DWC_BUFFER_SIZE=65536 src/wordcount.c src/main.c -o $(BIN)/wc_b64k
	@# 合成一个较大的输入，使进程启动开销相对可忽略（约 12 MB）
	@rm -f $(BIN)/bench_input.txt
	@i=0; while [ $$i -lt 20 ]; do cat testdata/b.txt >> $(BIN)/bench_input.txt; i=$$((i + 1)); done
	@echo "输入：$(BIN)/bench_input.txt，各跑 20 轮"
	@for tag in b256 b1k b4k b16k b64k; do \
		start=$$(date +%s%N); \
		for i in $$(seq 1 20); do $(BIN)/wc_$$tag $(BIN)/bench_input.txt > /dev/null; done; \
		end=$$(date +%s%N); \
		printf "%-6s %6d ms\n" "$$tag" $$(( (end - start) / 1000000 )); \
	done

clean:
	rm -rf $(BIN)
