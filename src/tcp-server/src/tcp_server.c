/* ============================================================
 * tcp_server.c —— epoll 边缘触发的 TCP 服务端
 *
 * 来源：D:\8. TCP服务器\tcp_server.c（课程示例，155 行）
 * 入库：2026-11-25，两个月计划第 1 阶段第 3 天
 *
 * 行为与原始示例一致：同一个 epoll 循环、同样用 EPOLLET 注册接入的
 * 连接、同样在每次事件里只 recv 一次、同样的 printf 输出格式。
 * 缺陷一条没修，全部列在 README 第 9 节。
 *
 * 只做了让它在 -Wall -Wextra -Wconversion 与加严旗标下零警告构建所必需的
 * 编译层改动，逐条如下（每一条都不改变运行行为）：
 *
 *   1. 补 <unistd.h> 与 <sys/socket.h>。原文两个都没有：close(2) 完全
 *      没有声明，在 -std=c11 下是隐式函数声明，而 C11 起（6.5.2.2p6）
 *      不允许；socket/bind/listen/accept/recv 原本只是靠
 *      <arpa/inet.h> -> <netinet/in.h> -> <sys/socket.h> 这条间接包含链
 *      碰巧拿到声明，依赖它不属于可移植写法。
 *
 *   2. 用一个宏 TCP_SERVER_PTHREAD（默认 0）同时管理 client_routine 的
 *      定义与 main 里的 pthread 分支，与原文的 #if 0 等价。原文把函数
 *      定义留在块外、调用点关在 #if 0 里，于是这个函数没有任何调用者：
 *      不加 static 触发 -Wmissing-prototypes，加了 static 触发
 *      -Wunused-function，两条路都过不去。用同一个宏把两者绑在一起，
 *      是让"这段代码不参与构建"这件事在源码里显式化的最小改动。
 *
 *   3. 五处显式转换，全部为 -Wconversion 所需：
 *      htons((uint16_t)port)、(socklen_t)sizeof(...) 两处、
 *      (int)recv(..., (size_t)BUFFER_LENGTH, 0) 两处。这些转换不改变
 *      任何一处的运行结果（recv 的返回值域是 [-1, 1024]，落在 int 里），
 *      但它们把窄化这件事写在了源码上，第 3 处的副作用见 README
 *      第 9 节第 12 条。
 *
 * 刻意没动的：main 的 return -1（从 main 返回 -1 的退出状态是 255，
 * POSIX 只取低 8 位，见 README "实现记录"）。
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/socket.h>		/* 补：socket / bind / listen / accept / recv */
#include <unistd.h>			/* 补：close */
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>

#include <sys/epoll.h>

#define BUFFER_LENGTH		1024
#define EPOLL_SIZE			1024

/* 原文这里是裸的 #if 0，client_routine 定义在块外、调用在块内。见文件头第 2 条。 */
#define TCP_SERVER_PTHREAD	0

#if TCP_SERVER_PTHREAD

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

#endif /* TCP_SERVER_PTHREAD */


// ./tcp_server <port>

int main(int argc, char *argv[]) {

	if (argc < 2) {
		printf("Param Error\n");
		return -1;
	}
	int port = atoi(argv[1]);

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sockfd, (struct sockaddr*)&addr, (socklen_t)sizeof(struct sockaddr_in)) < 0) {
		perror("bind");
		return 2;
	}

	if (listen(sockfd, 5) < 0) {
		perror("listen");
		return 3;
	}
	//

#if TCP_SERVER_PTHREAD

	while (1) {

		struct sockaddr_in client_addr;
		memset(&client_addr, 0, sizeof(struct sockaddr_in));
		socklen_t client_len = (socklen_t)sizeof(client_addr);

		int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);

		pthread_t thread_id;
		pthread_create(&thread_id, NULL, client_routine, &clientfd);

	}

#else

	int epfd = epoll_create(1);
	struct epoll_event events[EPOLL_SIZE] = {0};

	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = sockfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);


	while (1) {

		int nready = epoll_wait(epfd, events, EPOLL_SIZE, 5); // -1, 0, 5
		if (nready == -1) continue;

		int i = 0;
		for (i = 0;i < nready;i ++) {

			if (events[i].data.fd == sockfd) { // listen

				struct sockaddr_in client_addr;
				memset(&client_addr, 0, sizeof(struct sockaddr_in));
				socklen_t client_len = (socklen_t)sizeof(client_addr);

				int clientfd = accept(sockfd, (struct sockaddr*)&client_addr, &client_len);

				ev.events = EPOLLIN | EPOLLET;
				ev.data.fd = clientfd;
				epoll_ctl(epfd, EPOLL_CTL_ADD, clientfd, &ev);

			} else {

				int clientfd = events[i].data.fd;

				char buffer[BUFFER_LENGTH] = {0};
				int len = (int)recv(clientfd, buffer, (size_t)BUFFER_LENGTH, 0);
				if (len < 0) {
					close(clientfd);

					ev.events = EPOLLIN;
					ev.data.fd = clientfd;
					epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &ev);

				} else if (len == 0) { // disconnect
					close(clientfd);

					ev.events = EPOLLIN;
					ev.data.fd = clientfd;
					epoll_ctl(epfd, EPOLL_CTL_DEL, clientfd, &ev);

				} else {
					printf("Recv: %s, %d byte(s)\n", buffer, len);
				}


			}

		}

	}


#endif

	return 0;
}
