/*
 * mul_port_client_epoll.c —— 多端口、多连接的压测客户端（入库态）
 *
 * 来源：D:\9. 百万并发的服务器\mul_port_client_epoll.c（2019 年课程示例）
 *
 * 与服务端配套：按 ip/port 连上去，端口号在 port..port+99 之间轮转，每个连接
 * 建好之后立刻发一行 "Hello Server: client --> N\n"，然后把 fd 注册进自己的
 * epoll 集合，每 1000 个连接 poll 一次事件。目标连接数是 340000。
 *
 * 本轮只做编译层改动，**逻辑与原始示例一致，缺陷一条没修**，清单见 README
 * 「已知局限」。这个文件里的缺陷比服务端严重：EPOLLOUT/EPOLLIN 两个分支都用
 * 循环里那个"最后新建的" sockfd 而不是 events[i].data.fd，所以读写都落不到
 * 真正就绪的连接上；详见 README 第 1、2 条。
 *
 * 编译层改动逐条：
 *
 * 1. 补 <unistd.h>（close、usleep 原本无声明；-std=c11 下隐式函数声明是 C11
 *    6.5.2.2p6 的错误）与 <sys/time.h>（gettimeofday 原本无声明，同一个理由）。
 *
 * 2. 三处显式转换：htons((uint16_t)(port + index))、(socklen_t)sizeof(...)
 *   （connect 与 setsockopt 各一处）、(int)TIME_SUB_MS(...)（long → int）、
 *   以及 usleep((useconds_t)(1 * 1000))（int → useconds_t，-Wsign-conversion）。
 *
 * 3. 本文件需要 -D_DEFAULT_SOURCE 才能看到 usleep 的声明：它在 glibc 的
 *    <unistd.h> 里藏在 __USE_MISC 之后（BSD/XSI 遗留接口），而 -std=c11 定义
 *    __STRICT_ANSI__ 会把 __USE_MISC 关掉。这一点与服务端不同——服务端实测
 *    三种 feature macro 组合都是零警告，所以 Makefile 给两个目标不同的宏。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>


#define MAX_BUFFER		128
#define MAX_EPOLLSIZE	(384*1024)
#define MAX_PORT		100

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

int isContinue = 0;

static int ntySetNonblock(int fd) {
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) return flags;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0) return -1;
	return 0;
}

static int ntySetReUseAddr(int fd) {
	int reuse = 1;
	return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, (socklen_t)sizeof(reuse));
}



int main(int argc, char **argv) {
	if (argc <= 2) {
		printf("Usage: %s ip port\n", argv[0]);
		exit(0);
	}

	const char *ip = argv[1];
	int port = atoi(argv[2]);
	int connections = 0;
	char buffer[128] = {0};
	int i = 0, index = 0;

	struct epoll_event events[MAX_EPOLLSIZE];

	int epoll_fd = epoll_create(MAX_EPOLLSIZE);

	strcpy(buffer, " Data From MulClient\n");

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(struct sockaddr_in));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(ip);

	struct timeval tv_begin;
	gettimeofday(&tv_begin, NULL);

	while (1) {
		if (++index >= MAX_PORT) index = 0;

		struct epoll_event ev;
		int sockfd = 0;

		if (connections < 340000 && !isContinue) {
			sockfd = socket(AF_INET, SOCK_STREAM, 0);
			if (sockfd == -1) {
				perror("socket");
				goto err;
			}

			//ntySetReUseAddr(sockfd);
			addr.sin_port = htons((uint16_t)(port + index));

			if (connect(sockfd, (struct sockaddr*)&addr, (socklen_t)sizeof(struct sockaddr_in)) < 0) {
				perror("connect");
				goto err;
			}
			ntySetNonblock(sockfd);
			ntySetReUseAddr(sockfd);

			sprintf(buffer, "Hello Server: client --> %d\n", connections);
			send(sockfd, buffer, strlen(buffer), 0);

			ev.data.fd = sockfd;
			ev.events = EPOLLIN | EPOLLOUT;
			epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sockfd, &ev);

			connections ++;
		}
		//connections ++;
		if (connections % 1000 == 999 || connections >= 340000) {
			struct timeval tv_cur;
			memcpy(&tv_cur, &tv_begin, sizeof(struct timeval));

			gettimeofday(&tv_begin, NULL);

			int time_used = (int)TIME_SUB_MS(tv_begin, tv_cur);
			printf("connections: %d, sockfd:%d, time_used:%d\n", connections, sockfd, time_used);

			int nfds = epoll_wait(epoll_fd, events, connections, 100);
			for (i = 0;i < nfds;i ++) {
				int clientfd = events[i].data.fd;

				if (events[i].events & EPOLLOUT) {
					sprintf(buffer, "data from %d\n", clientfd);
					send(sockfd, buffer, strlen(buffer), 0);
				} else if (events[i].events & EPOLLIN) {
					char rBuffer[MAX_BUFFER] = {0};
					ssize_t length = recv(sockfd, rBuffer, MAX_BUFFER, 0);
					if (length > 0) {
						printf(" RecvBuffer:%s\n", rBuffer);

						if (!strcmp(rBuffer, "quit")) {
							isContinue = 0;
						}

					} else if (length == 0) {
						printf(" Disconnect clientfd:%d\n", clientfd);
						connections --;
						close(clientfd);
					} else {
						if (errno == EINTR) continue;

						printf(" Error clientfd:%d, errno:%d\n", clientfd, errno);
						close(clientfd);
					}
				} else {
					printf(" clientfd:%d, errno:%d\n", clientfd, errno);
					close(clientfd);
				}
			}
		}

		usleep((useconds_t)(1 * 1000));
	}

	return 0;

err:
	printf("error : %s\n", strerror(errno));
	return 0;

}
