# ============================================================
# MySQL 客户端模块
#
# 目标：
#   make            构建静态库与命令行工具（默认）
#   make lib        只构建静态库 bin/mysqlclient 对应的 bin/libmysqlclient.a
#   make cli        只构建命令行工具 bin/mysqlclient
#   make test       构建并运行单元测试（纯逻辑 + 接口参数检查）
#   make check      命令行端到端检查（不需要运行中的数据库）
#   make check-live 需要真实数据库的往返验证，连接参数走环境变量
#   make check-scale 百万行下的峰值内存对照（物化 vs 流式，需要数据库）
#   make sanitize   ASan + UBSan 重新构建并跑测试（产物在 bin/sanitize/）
#   make bench      文件读写的吞吐测量
#   make clean      删除 bin/（含全部变体）
#
# 与其他模块不同，本模块有一个外部依赖：libmysqlclient。
# 位置按下面的顺序确定，三处都可以用命令行变量覆盖：
#   1. mysql_config --cflags --libs（装了 libmysqlclient-dev 就有）
#   2. pkg-config mysqlclient
#   3. make MYSQL_CFLAGS=... MYSQL_LIBS=...
#
# 没有免密 sudo 的机器上，第 3 条是可用的：
#   apt-get download libmysqlclient-dev libmysqlclient21
#   for f in *.deb; do dpkg-deb -x "$f" root; done
#   make MYSQL_CFLAGS="-I$PWD/root/usr/include/mysql" \
#        MYSQL_LIBS="-L$PWD/root/usr/lib/x86_64-linux-gnu -lmysqlclient \
#                    -Wl,-rpath,$PWD/root/usr/lib/x86_64-linux-gnu"
# ============================================================

CC       ?= cc
AR       ?= ar

# -Wconversion：本模块在 size_t / unsigned long / unsigned long long 之间
# 来回转换（MySQL 的 API 用的是后两者），打开它能防住静默窄化。
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g

# -D_POSIX_C_SOURCE=200809L：-std=c11 定义 __STRICT_ANSI__，会关掉 glibc 的
# 默认扩展，驱动层用到的 stat / S_ISREG 因此需要显式声明特性宏。
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L

# 本机凭据。config.h 不入库（见 .gitignore），存在时才把它编进去；
# 不存在时 main.c 用一组"只连本机、无密码"的默认值，
# 因此从干净检出执行 make 不会因为找不到 config.h 而失败。
ifneq ($(wildcard config.h),)
CPPFLAGS += -I. -DMC_HAVE_CONFIG_H
endif

# 输出目录。变体构建通过命令行覆盖它，与普通构建分开存放：
# make 只比较时间戳、不比较编译选项，共用 bin/ 会让普通构建把插桩过的
# 可执行文件当成"已是最新"，运行时报 sanitizer 的符号错误。
OUT      ?= bin

# ------------------------------------------------------------
# 外部依赖：MySQL 客户端库
# ------------------------------------------------------------
MYSQL_CFLAGS ?= $(shell mysql_config --cflags 2>/dev/null)
MYSQL_LIBS   ?= $(shell mysql_config --libs 2>/dev/null)

ifeq ($(strip $(MYSQL_CFLAGS)),)
MYSQL_CFLAGS := $(shell pkg-config --cflags mysqlclient 2>/dev/null)
endif
ifeq ($(strip $(MYSQL_LIBS)),)
MYSQL_LIBS := $(shell pkg-config --libs mysqlclient 2>/dev/null)
endif

ifeq ($(strip $(MYSQL_CFLAGS)),)
$(warning 未找到 mysql.h 的位置：装 libmysqlclient-dev，或用 MYSQL_CFLAGS / MYSQL_LIBS 指定（见本文件顶部注释）。)
endif

LIB      := $(OUT)/libmysqlclient.a
CLI      := $(OUT)/mysqlclient
TESTPURE := $(OUT)/test_pure
TESTDB   := $(OUT)/test_db
TESTSTRM := $(OUT)/test_stream
BENCHBIN := $(OUT)/bench_file

# 库的成员。file.o / options.o / status.o 不引用任何 mysql_* 符号，
# 因此只用到它们的测试程序不需要链接 libmysqlclient —— 链接器按需抽取
# 静态库成员，没被引用到的 mysqlclient.o 根本不会被拉进来。
LIB_OBJ  := $(OUT)/mysqlclient.o $(OUT)/options.o $(OUT)/status.o $(OUT)/file.o
CLI_OBJ  := $(OUT)/main.o

.PHONY: all lib cli test check check-live check-scale sanitize sanitize-run bench clean

all: lib cli

lib: $(LIB)

cli: $(CLI)

$(OUT):
	@mkdir -p $(OUT)

$(OUT)/%.o: src/%.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(MYSQL_CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

# 命令行工具只链接静态库，不重复编译模块源码。
# 链接顺序有意义：静态库要放在引用它的目标文件之后，
# 外部库要放在静态库之后（gcc 从左到右解析，后面的库才能填前面的未定义符号）。
$(CLI): $(CLI_OBJ) $(LIB)
	$(CC) $(CFLAGS) $(CLI_OBJ) $(LIB) $(MYSQL_LIBS) -o $@

# 纯逻辑测试：不链接 libmysqlclient，因此在没有数据库驱动的机器上也能跑
$(TESTPURE): tests/test_pure.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

# 接口层测试：需要链接库，但不需要运行中的服务器（只验证参数检查与失败路径）
$(TESTDB): tests/test_db.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(MYSQL_CFLAGS) $< $(LIB) $(MYSQL_LIBS) -o $@

$(BENCHBIN): tests/bench_file.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(LIB) -o $@

# 流式取数的测试：分"正确性"与"内存测量"两种用法（见文件顶部说明）。
# 两者都需要运行中的服务器，因此不参与 make test。
$(TESTSTRM): tests/test_stream.c $(LIB) | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(MYSQL_CFLAGS) $< $(LIB) $(MYSQL_LIBS) -o $@

test: $(TESTPURE) $(TESTDB)
	@echo
	@$(TESTPURE)
	@echo
	@$(TESTDB)

check: $(CLI)
	@echo
	@sh tests/check_cli.sh $(CLI)

# 需要一台连得上的数据库。连接参数走环境变量，与驱动层的默认值来源一致：
#   MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root make check-live
# 不并进 make check：没有数据库的机器上它必然全红，那样的目标没人会跑。
check-live: $(CLI) $(TESTSTRM)
	@echo
	@sh tests/check_live.sh $(CLI)
	@echo "\n--- 流式取数：与物化逐格比对 ---"
	@$(TESTSTRM)

# 百万行下的峰值内存对照。单独一个目标是因为它要造 100 万行数据、
# 占用几十秒和几百 MB 磁盘，不适合每次改代码都跑。
check-scale: $(CLI) $(TESTSTRM)
	@echo
	@sh tests/check_scale.sh $(CLI) $(TESTSTRM)

bench: $(BENCHBIN)
	@echo
	@$(BENCHBIN)

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

# 变体构建：覆盖 OUT 与 CFLAGS 递归调用自身。
# MYSQL_CFLAGS / MYSQL_LIBS 必须一起透传，否则子 make 会重新探测一遍，
# 在"用命令行指定解包目录"的场景下探测不到任何东西。
sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' \
		MYSQL_CFLAGS='$(MYSQL_CFLAGS)' MYSQL_LIBS='$(MYSQL_LIBS)' sanitize-run

sanitize-run: all $(TESTPURE) $(TESTDB)
	@echo "\n--- 纯逻辑测试（ASan + UBSan）---"
	@$(TESTPURE)
	@echo "\n--- 接口测试（ASan + UBSan）---"
	@$(TESTDB)
	@echo "\n--- 用法与错误路径（ASan + UBSan）---"
	@$(CLI) -h > /dev/null
	@$(CLI) -H 127.0.0.1 -P 1 ping || true

clean:
	rm -rf $(OUT)
