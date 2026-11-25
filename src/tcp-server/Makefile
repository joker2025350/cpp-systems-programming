# ============================================================
# tcp-server 模块（入库态）
#
# 目标：
#   make            构建 bin/tcp-server
#   make test       跑离线可判定的那一部分（见 tests/smoke.sh）
#   make sanitize   ASan + UBSan 重新构建并跑同一套检查（产物在 bin/sanitize/）
#   make clean      删除 bin/
#
# 与仓库里五个成型的模块相比，这里少了 lib / cli 的拆分、check / bench / tsan：
# 源码目前还是单文件程序，accept 循环、连接注册、读与关全在 main 所在的文件里，
# 没有对外接口可拆。拆分与补测试属于第二阶段，见 README 第 2、10 节。
# ============================================================

CC       ?= cc

CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g

# 这里同样**没有** feature test macro，是实测的结果而不是照搬：
# 本文件用到的 socket / bind / listen / accept / recv（<sys/socket.h>）、
# close（<unistd.h>）、epoll_*（<sys/epoll.h>）、htons / inet_addr（<arpa/inet.h>）
# 在 glibc 2.35 的头文件里都是无条件声明，不受 __USE_MISC 控制。
# gcc 11.4.0 上实测三种组合（不给宏 / 只给 _POSIX_C_SOURCE=200809L / 给
# _DEFAULT_SOURCE）都是零警告。dns-client 必须给 -D_DEFAULT_SOURCE，是因为它用的
# random / srandom / bzero 确实藏在 __USE_MISC 之后；那条经验不能整体照搬。

# 变体构建通过命令行覆盖它。原因与其它模块相同：make 只比较时间戳、
# 不比较编译选项，共用 bin/ 会留下插桩过的二进制。
OUT      ?= bin
BIN      := $(OUT)/tcp-server

.PHONY: all test sanitize sanitize-run clean

all: $(BIN)

$(OUT):
	@mkdir -p $(OUT)

$(BIN): src/tcp_server.c | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -o $@

test: $(BIN)
	@echo
	@sh tests/smoke.sh $(BIN)

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: $(BIN)
	@echo "\n--- 参数与启动行为检查（ASan + UBSan）---"
	@sh tests/smoke.sh $(BIN)

clean:
	rm -rf $(OUT)
