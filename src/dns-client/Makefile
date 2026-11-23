# ============================================================
# dns-client 模块（入库态）
#
# 目标：
#   make            构建 bin/dns-client
#   make test       跑离线可判定的那一部分（见 tests/smoke.sh）
#   make sanitize   ASan + UBSan 重新构建并跑同一套检查（产物在 bin/sanitize/）
#   make clean      删除 bin/
#
# 与仓库里五个成型的模块相比，这里少了 lib / cli 的拆分、check / bench / tsan：
# 源码目前还是单文件程序，没有对外接口可拆。拆分与补测试属于第二阶段，
# 见 README 第 9、10 节。
# ============================================================

CC       ?= cc

CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g

# -D_POSIX_C_SOURCE=200809L：-std=c11 定义 __STRICT_ANSI__，关掉 glibc 的默认扩展，
# 不给这个宏就看不到 strdup 这些 POSIX 声明。
#
# -D_DEFAULT_SOURCE 必须一起给：random / srandom / bzero 属于 BSD/SVID 遗留接口，
# glibc 把它们藏在 __USE_MISC 之后，而只开 _POSIX_C_SOURCE 时 __USE_MISC 是关的
# （features.h 的规则：_POSIX_C_SOURCE 与 _DEFAULT_SOURCE 互斥，只留更严的那个）。
# 结果是这两个函数变成隐式声明——按 C11 6.5.2.2p6 隐式声明已被移除，
# 函数返回类型不是 int 时行为未定义。原始代码正是这么编过去的。
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

# 变体构建通过命令行覆盖它。原因与其它模块相同：make 只比较时间戳、
# 不比较编译选项，共用 bin/ 会留下插桩过的二进制。
OUT      ?= bin
BIN      := $(OUT)/dns-client

.PHONY: all test sanitize sanitize-run clean

all: $(BIN)

$(OUT):
	@mkdir -p $(OUT)

$(BIN): src/dns.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@

test: $(BIN)
	@echo
	@sh tests/smoke.sh $(BIN)

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: $(BIN)
	@echo "\n--- 参数检查（ASan + UBSan）---"
	@sh tests/smoke.sh $(BIN)

clean:
	rm -rf $(OUT)
