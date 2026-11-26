# ============================================================
# epoll-server 模块（入库态）
#
# 一个模块两个可执行文件，因为源素材就是两个配套的程序：
#   bin/epoll-server     多端口监听的 epoll 服务端（src/tcp_server.c）
#   bin/mul-port-client  多连接压测客户端（src/mul_port_client_epoll.c）
#
# 目标：
#   make            构建上面两个二进制
#   make test       跑离线可判定的那一部分（见 tests/smoke.sh）
#   make sanitize   ASan + UBSan 重新构建并跑同一套检查（产物在 bin/sanitize/）
#   make clean      删除 bin/
#
# 与仓库里五个成型的模块相比，这里少了 include/ 与 lib*.a，也少了
# check / bench / tsan：两个源文件都是"全部逻辑写在 main 里"的单文件程序，
# 没有对外接口可拆。拆分属于第二阶段（2026-12-20 ~ 2026-12-26），见 README 第 2 节。
# ============================================================

CC       ?= cc

CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g

OUT      ?= bin
SERVER   := $(OUT)/epoll-server
CLIENT   := $(OUT)/mul-port-client

# 两个目标给不同的 feature test macro，是分别实测的结果而不是照搬：
#
#   服务端用到的 socket/bind/listen/accept/setsockopt/recv（<sys/socket.h>）、
#   close（<unistd.h>）、epoll_*（<sys/epoll.h>）、htons/INADDR_ANY
#   （<arpa/inet.h>）在 glibc 2.35 上都是无条件声明，三种组合（不给宏 /
#   _POSIX_C_SOURCE=200809L / _DEFAULT_SOURCE）都是零警告 → 不给宏。
#
#   客户端用到 usleep，它在 <unistd.h> 里藏在 __USE_MISC 之后（BSD/XSI 遗留
#   接口），而 -std=c11 定义的 __STRICT_ANSI__ 会把 __USE_MISC 关掉 → 不给
#   -D_DEFAULT_SOURCE 就是隐式函数声明（C11 6.5.2.2p6 起不允许）。
SERVER_CPPFLAGS ?=
CLIENT_CPPFLAGS ?= -D_DEFAULT_SOURCE

.PHONY: all test sanitize sanitize-run clean

all: $(SERVER) $(CLIENT)

$(OUT):
	@mkdir -p $(OUT)

$(SERVER): src/tcp_server.c | $(OUT)
	$(CC) $(CFLAGS) $(SERVER_CPPFLAGS) $< -o $@

$(CLIENT): src/mul_port_client_epoll.c | $(OUT)
	$(CC) $(CFLAGS) $(CLIENT_CPPFLAGS) $< -o $@

test: $(SERVER) $(CLIENT)
	@echo
	@sh tests/smoke.sh $(SERVER) $(CLIENT)

SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1

# 变体构建通过命令行覆盖 OUT。原因与其它模块相同：make 只比较时间戳、
# 不比较编译选项，共用 bin/ 会留下插桩过的二进制。
sanitize:
	@$(MAKE) --no-print-directory OUT=$(OUT)/sanitize \
		CFLAGS='$(CFLAGS) $(SAN_FLAGS)' sanitize-run

sanitize-run: $(SERVER) $(CLIENT)
	@echo "\n--- 参数与启动行为检查（ASan + UBSan）---"
	@sh tests/smoke.sh $(SERVER) $(CLIENT)

clean:
	rm -rf $(OUT)
