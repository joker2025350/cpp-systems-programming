# ============================================================
# http-client 模块（入库态）
#
# 目标：
#   make            构建 bin/http-client
#   make test       跑离线可判定的那一部分（见 tests/smoke.sh）
#   make sanitize   ASan + UBSan 重新构建并跑同一套检查（产物在 bin/sanitize/）
#   make clean      删除 bin/
#
# 与仓库里五个成型的模块相比，这里少了 lib / cli 的拆分、check / bench / tsan：
# 源码目前还是单文件程序，报文拼接、连接建立、应答收集全在 main 所在的文件里，
# 没有对外接口可拆。拆分与补测试属于第二阶段，见 README 第 2、10 节。
# ============================================================

CC       ?= cc

CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g

# 这里**没有** feature test macro，与 dns-client 不同，是实测的结果而不是偷懒：
# 本文件只用到 select / fd_set / struct timeval（由 <sys/select.h> 提供）与
# gethostbyname / inet_addr / inet_ntoa（glibc 2.35 的 <netdb.h>、<arpa/inet.h>
# 无条件声明它们，不受 __USE_MISC 控制）。三种组合在 gcc 11.4.0 上都是零警告：
# 不给宏、只给 _POSIX_C_SOURCE=200809L、给 _DEFAULT_SOURCE。
# dns-client 那边必须给 -D_DEFAULT_SOURCE，是因为它用的 random / srandom / bzero
# 确实藏在 __USE_MISC 之后；这条经验不能整体照搬。

# 变体构建通过命令行覆盖它。原因与其它模块相同：make 只比较时间戳、
# 不比较编译选项，共用 bin/ 会留下插桩过的二进制。
OUT      ?= bin
BIN      := $(OUT)/http-client

.PHONY: all test sanitize sanitize-run clean

all: $(BIN)

$(OUT):
	@mkdir -p $(OUT)

$(BIN): src/httprequest.c | $(OUT)
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
