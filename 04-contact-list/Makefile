# ============================================================
# 04 通讯录模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/libcontacts.a
#   make cli        只构建命令行工具 bin/contacts
#   make test       构建并运行单元测试
#   make check      用命令行做端到端验证
#   make sanitize   用 ASan + UBSan 重新构建并跑测试
#   make bench      测量不同规模下的构建 / 查找 / 存取耗时
#   make clean      删除 bin/
# ============================================================

CC       ?= cc
AR       ?= ar

# -Wconversion 用来捕捉隐式窄化转换。本模块里 size_t 与 int 混用处不少
# （fgets 的长度参数、snprintf 的缓冲区大小），打开它能防住静默截断。
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g
CPPFLAGS += -Iinclude

TESTDATA ?= testdata

BIN      := bin
LIB      := $(BIN)/libcontacts.a
CLI      := $(BIN)/contacts
TESTBIN  := $(BIN)/test_contacts
BENCHBIN := $(BIN)/bench_contacts

LIB_OBJ  := $(BIN)/contacts.o
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
# 这样"模块能被独立链接"这件事在每次构建时都被验证一遍。
$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) $^ -o $@

$(TESTBIN): tests/test_contacts.c $(LIB) | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

$(BENCHBIN): tests/bench_contacts.c $(LIB) | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

test: $(TESTBIN)
	@echo
	@$(TESTBIN) $(TESTDATA)

check: $(CLI)
	@sh tests/check_cli.sh $(CLI) $(TESTDATA)

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
sanitize: clean all $(TESTBIN)
	@echo "\n--- 单元测试（ASan + UBSan）---"
	@$(TESTBIN) $(TESTDATA)
	@echo "\n--- 命令行（ASan + UBSan）---"
	@$(CLI) -f $(BIN)/asan_book.txt add alice 13900001111
	@$(CLI) -f $(BIN)/asan_book.txt add bob 13800002222
	@$(CLI) -f $(BIN)/asan_book.txt list
	@$(CLI) -f $(BIN)/asan_book.txt del alice
	@$(CLI) -f $(BIN)/asan_book.txt load $(TESTDATA)/c.txt -r
	@$(CLI) -f $(BIN)/asan_book.txt list
	@echo "\n--- 错误路径 ---"
	@$(CLI) -f $(BIN)/asan_book.txt load $(BIN)/nonexistent.txt || true
	@rm -f $(BIN)/asan_book.txt

bench: $(BENCHBIN)
	@echo
	@$(BENCHBIN)

clean:
	rm -rf $(BIN)
