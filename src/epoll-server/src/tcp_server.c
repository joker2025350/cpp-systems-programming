/*
 * tcp_server.c —— 多端口监听的 epoll 服务端（入库态）
 *
 * 来源：D:\9. 百万并发的服务器\tcp_server.c（2019 年课程示例）
 *
 * 本轮只做编译层改动：让它在 -Wall -Wextra -Wconversion 与加严旗标下零警告
 * 构建。**逻辑与原始示例一致，缺陷一条没修**，清单见 README「已知局限」。
 *
 * 编译层改动逐条：
 *
 * 1. 补 <unistd.h>（close 原本完全没有声明；-std=c11 下隐式函数声明从 C11
 *    6.5.2.2p6 起不被允许）与 <sys/socket.h>（socket/bind/listen/accept/
 *    setsockopt/recv 的声明原先是靠 <arpa/inet.h> → <netinet/in.h> →
 *    <sys/socket.h> 这条间接包含链碰巧拿到的）。
 *
 * 2. 用宏 EPOLL_SERVER_PTHREAD（默认 0）同时管住 client_routine 的定义与 main
 *    里的 pthread 分支。原文把函数定义留在 #if 0 块外、调用关在块内，函数没有
 *    调用点：不加 static 触发 -Wmissing-prototypes，加了 static 触发
 *    -Wunused-function，两条路都过不去。用宏把"这段不参与构建"显式化，语义与
 *    原文的 #if 0 等价。注意该分支引用的是端口循环里的 sockfd，已出作用域，
 *    所以把宏置 1 之后是编译不过的——这是原文遗留的缺陷，见 README「已知局限」
 *    末尾那段。
 *
 * 3. islistenfd 加 static：同上，非 static 且无原型声明会触发
 *    -Wmissing-prototypes。
 *
 * 4. 内层事件循环的计数变量由 i 改名 k：它与端口循环的 i 同名，构成变量遮蔽，
 *    触发 -Wshadow。只是改名，作用域与取值不变。
 *
 * 5. 显式转换七处：
 *      htons((uint16_t)(port + i))                        int → uint16_t
 *      (socklen_t)sizeof(struct sockaddr_in)   [bind]      size_t → socklen_t
 *      (socklen_t)sizeof(client_addr)          [声明]      size_t → socklen_t
 *      (socklen_t)sizeof(reuse)                [setsockopt] size_t → socklen_t
 *      (int)recv(...)                                      ssize_t → int
 *      (size_t)BUFFER_LENGTH                               int → size_t
 *    四处是 -Wconversion，两处是 -Wsign-conversion。都不改变结果：recv 的返回值
 *    域是 [-1, 1024]，落在 int 里；sizeof 的结果域远小于 socklen_t。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

#include <errno.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>

#define BUFFER_LENGTH		1024
#define EPOLL_SIZE			1024

#define MAX_PORT			100

#ifndef EPOLL_SERVER_PTHREAD
#define EPOLL_SERVER_PTHREAD 0
#endif

#if EPOLL_SERVER_PTHREAD
void *client_routine(void *arg) {

	int clientfd = *(int *)arg;

	while (1) {

		char buffer[BUFFER_LENGTH] = {0};
		int len = (int)recv(clientfd, buffer, (size_t)BUFFER_LENGTH, 0);
		if (len < 0) {
			close(clientfd);
			break;
		} else if (len == 0) { // disconnect
			close(clientfd);
			break;
		} else {
			printf("Recv: %s, %d byte(s)\n", buffer, len);
		}

	}

}
#endif /* EPOLL_SERVER_PTHREAD */


static int islistenfd(int fd, int *fds) {

	int i = 0;
	for (i = 0;i < MAX_PORT;i ++) {
		if (fd == *(fds+i)) return fd;
	}

	return 0;
}

// ./epoll-server 8888

int main(int argc, char *argv[]) {

	if (argc < 2) {
		printf("Param Error\n");
		return -1;
	}

	int port = atoi(argv[1]); // start
	int sockfds[MAX_PORT] = {0}; // listen fd
	int epfd = epoll_create(1);

	int i = 0;
	for (i = 0;i < MAX_PORT;i ++) {

		int sockfd = socket(AF_INET, SOCK_STREAM, 0);

		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(struct sockaddr_in));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((uint16_t)(port + i)); // 8888 8889 8890 8891 .... 8987
		addr.sin_addr.s_addr = INADDR_ANY;

		if (bind(sockfd, (struct sockaddr*)&addr, (socklen_t)sizeof(struct sockaddr_in)) < 0) {
			perror("bind");
			return 2;
		}

		if (listen(sockfd, 5) < 0) {
			perror("listen");
			return 3;
		}
		printf("tcp server listen on port : %d\n", port + i);

		struct epoll_event ev;
		ev.events = EPOLLIN;
		ev.data.fd = sockfd;
		epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

		sockfds[i] = sockfd;
	}
	//

#if EPOLL_SERVER_PTHREAD

	while (1) {

		struct sockaddr_in client_addr;
		memset(&client_addr, 0, sizeof(struct sockaddr_in));
		socklen_t client_len = (socklen_t)sizeof(client_addr);

		int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);

		pthread_t thread_id;
		pthread_create(&thread_id, NULL, client_routine, &clientfd);

	}

#else


	struct epoll_event events[EPOLL_SIZE] = {0};

	while (1) {

		int nready = epoll_wait(epfd, events, EPOLL_SIZE, 5); // -1, 0, 5
		if (nready == -1) continue;

		int k = 0;
		for (k = 0;k < nready;k ++) {

			int sockfd = islistenfd(events[k].data.fd, sockfds);
			if (sockfd) { // listen 2

				struct sockaddr_in client_addr;
				memset(&client_addr, 0, sizeof(struct sockaddr_in));
				socklen_t client_len = (socklen_t)sizeof(client_addr);

				int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);

				fcntl(clientfd, F_SETFL, O_NONBLOCK);

				int reuse = 1;
				setsockopt(clientfd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, (socklen_t)sizeof(reuse));

				struct epoll_event ev;
				ev.events = EPOLLIN | EPOLLET;
				ev.data.fd = clientfd;
				epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);

			} else {

				int clientfd = events[k].data.fd;

				char buffer[BUFFER_LENGTH] = {0};
				int len = (int)recv(clientfd, buffer, (size_t)BUFFER_LENGTH, 0);
				if (len < 0) {
					close(clientfd);

					struct epoll_event ev;
					ev.events = EPOLLIN;
					ev.data.fd = clientfd;
					epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &ev);

				} else if (len == 0) { // disconnect
					close(clientfd);

					struct epoll_event ev;
					ev.events = EPOLLIN;
					ev.data.fd = clientfd;
					epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &ev);

				} else {
					printf("Recv: %s, %d byte(s), clientfd: %d\n", buffer, len, clientfd);
				}


			}

		}

	}


#endif

	return 0;
}
