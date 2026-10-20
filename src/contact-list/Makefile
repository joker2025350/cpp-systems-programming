# ============================================================
# 通讯录模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/libcontacts.a
#   make cli        只构建命令行工具 bin/contacts
#   make test       构建并运行单元测试
#   make check      用命令行做端到端验证
#   make sanitize   用 ASan + UBSan 重新构建并跑测试（产物在 bin/sanitize/）
#   make bench      测量不同规模下的构建 / 查找 / 存取耗时
#   make clean      删除 bin/（含全部变体）
# ============================================================

CC       ?= cc
AR       ?= ar

# -Wconversion 用来捕捉隐式窄化转换。本模块里 size_t 与 int 混用处不少
# （fgets 的长度参数、snprintf 的缓冲区大小），打开它能防住静默截断。
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g
CPPFLAGS += -Iinclude

TESTDATA ?= testdata

# 输出目录。变体构建（sanitize）通过命令行覆盖它，与普通构建分开存放：
# make 只比较时间戳，不比较编译选项，共用目录会让普通构建把插桩过的
# 可执行文件当成"已是最新"，于是运行时报 ASan/UBSan 的链接期符号错误。
OUT      ?= bin

LIB      := $(OUT)/libcontacts.a
CLI      := $(OUT)/contacts
TESTBIN  := $(OUT)/test_contacts
BENCHBIN := $(OUT)/bench_contacts

LIB_OBJ  := $(OUT)/contacts.o
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

# 命令行工具只链接静态库，不重复编译模块源码，
# 这样"模块能被独立链接"这件事在每次构建时都被验证一遍。
$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTBIN): tests/test_contacts.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

$(BENCHBIN): tests/bench_contacts.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

test: $(TESTBIN)
	@echo
	@$(TESTBIN) $(TESTDATA)

check: $(CLI)
	@sh tests/check_cli.sh $(CLI) $(TESTDATA)

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: all $(TESTBIN)
	@echo "\n--- 单元测试（ASan + UBSan）---"
	@$(TESTBIN) $(TESTDATA)
	@echo "\n--- 命令行（ASan + UBSan）---"
	@$(CLI) -f $(OUT)/asan_book.txt add alice 13900001111
	@$(CLI) -f $(OUT)/asan_book.txt add bob 13800002222
	@$(CLI) -f $(OUT)/asan_book.txt list
	@$(CLI) -f $(OUT)/asan_book.txt del alice
	@$(CLI) -f $(OUT)/asan_book.txt load $(TESTDATA)/c.txt -r
	@$(CLI) -f $(OUT)/asan_book.txt list
	@echo "\n--- 错误路径 ---"
	@$(CLI) -f $(OUT)/asan_book.txt load $(OUT)/nonexistent.txt || true
	@rm -f $(OUT)/asan_book.txt

bench: $(BENCHBIN)
	@echo
	@$(BENCHBIN)

clean:
	rm -rf $(OUT)
