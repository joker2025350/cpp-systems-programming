/*
 * httprequest.c —— 手写 HTTP/1.1 请求、把应答原始字节取回（TCP 阻塞 socket + select）
 *
 * 来源：D:\7. http客户端请求\code\httprequest.c（2019 年的课程示例）。
 * 本文件是"入库态"：程序能编译、能运行，但**逻辑与原始示例一致，缺陷一个都没修**。
 * 已知缺陷清单见 README 第 9 节，工程化与修缺陷放在第二阶段逐日推进。
 *
 * 入库时只做了"让它在 -Wall -Wextra -Wconversion 与加严旗标下零警告"所必需的改动，
 * 都是编译层面的事，输出字节与原始示例完全相同：
 *   1. 补上缺失的头文件 <sys/select.h> 与 <stdint.h>。select / fd_set / FD_ZERO /
 *      FD_ISSET / struct timeval 按 POSIX 声明在 <sys/select.h>，原文只包了
 *      <sys/socket.h>，于是 fd_set 是未知类型、timeval 的 storage size 未知——
 *      这两条是编译错误，不是警告。gethostbyname / inet_addr / inet_ntoa 在
 *      glibc 2.35 上是无条件声明的，所以**不需要** -D_DEFAULT_SOURCE，
 *      这一点与 dns-client 不同（那边 random/srandom/bzero 藏在 __USE_MISC 之后）。
 *      <stdint.h> 只为下面的 (uint16_t) 显式转换。
 *   2. 三个函数改成 static：它们只在本文件内使用，且没有对外声明，
 *      留着外部链接会触发 -Wmissing-prototypes。
 *   3. 显式转换，消掉 -Wconversion / -Wsign-conversion：htons((uint16_t)80)、
 *      (socklen_t)sizeof(struct sockaddr_in)、(size_t)BUFFER_SIZE、
 *      (int)recv(...)，以及 strncat 的第三参数 (size_t)len。
 *      **转换不改变任何行为**，但把 len 可能是 -1 这件事显式化了，见 README 第 9 节第 4 条。
 *   4. 请求格式串从"行尾反斜杠续行"改成"相邻字符串字面量"，逐字节相同。
 *      继续用反斜杠时，行尾多一个空格就会被悄悄拼进请求里，肉眼看不出来。
 *
 * 刻意没有动的两处：
 *   - main 末尾没有 return。按 C11 5.1.2.2.3p1，从 main 掉出去等价于 return 0，
 *     不是缺陷，也不产生警告。
 *   - 宏名 CONNETION_TYPE 的拼写沿用原文（少一个 C）。它是本文件内的标识符，
 *     改名只会让"与原始示例对照"变难。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <netdb.h>
#include <fcntl.h>


#define HTTP_VERSION		"HTTP/1.1"
#define CONNETION_TYPE		"Connection: close\r\n"

#define BUFFER_SIZE		4096


/*
 * 域名 -> 点分十进制 IPv4 字符串。
 * gethostbyname 返回的 hostent 与 inet_ntoa 返回的字符串都在静态区里，
 * 下一次调用会覆盖（见 README 第 9 节第 14 条）。
 */
static char *host_to_ip(const char *hostname) {

	struct hostent *host_entry = gethostbyname(hostname); //dns

	// 14.215.177.39 -->
	//inet_ntoa ( unsigned int --> char *
	// 0x12121212 --> "18.18.18.18"
	if (host_entry) {
		return inet_ntoa(*(struct in_addr*)*host_entry->h_addr_list);
	}

	return NULL;
}

/*
 * 建一条到 ip:80 的 TCP 连接，成功后切成非阻塞并返回描述符。
 * socket() 的返回值没有检查，connect() 失败时描述符也不关（README 第 9 节 1、10 条）。
 */
static int http_create_socket(char *ip) {

	int sockfd = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in sin = {0};
	sin.sin_family = AF_INET;
	sin.sin_port = htons((uint16_t)80); //
	sin.sin_addr.s_addr = inet_addr(ip);

	if (0 != connect(sockfd, (struct sockaddr*)&sin, (socklen_t)sizeof(struct sockaddr_in))) {
		return -1;
	}

	fcntl(sockfd, F_SETFL, O_NONBLOCK);

	return sockfd;

}

/*
 * 发一个 GET 请求，把应答原样收进堆上的字符串返回。
 * 调用方负责 free；没有 close(sockfd)，描述符在这里泄漏（README 第 9 节第 10 条）。
 */
static char * http_send_request(const char *hostname, const char *resource) {

	char *ip = host_to_ip(hostname); //
	int sockfd = http_create_socket(ip);

	char buffer[BUFFER_SIZE] = {0};
	sprintf(buffer,
"GET %s %s\r\n"
"Host: %s\r\n"
"%s\r\n"
"\r\n",

		resource, HTTP_VERSION,
		hostname,
		CONNETION_TYPE
	);

	send(sockfd, buffer, strlen(buffer), 0);



	//select

	fd_set fdread;

	FD_ZERO(&fdread);
	FD_SET(sockfd, &fdread);

	struct timeval tv;
	tv.tv_sec = 5;
	tv.tv_usec = 0;

	char *result = malloc(sizeof(int));
	memset(result, 0, sizeof(int));

	while (1) {

		int selection = select(sockfd+1, &fdread, NULL, NULL, &tv);
		if (!selection || !FD_ISSET(sockfd, &fdread)) {
			break;
		} else {

			memset(buffer, 0, (size_t)BUFFER_SIZE);
			int len = (int)recv(sockfd, buffer, (size_t)BUFFER_SIZE, 0);
			if (len == 0) { // disconnect
				break;
			}

			result = realloc(result, (strlen(result) + (size_t)len + 1) * sizeof(char));
			strncat(result, buffer, (size_t)len);
		}

	}

	return result;
}

int main(int argc, char *argv[]) {

	if (argc < 3) return -1;

	char *response = http_send_request(argv[1], argv[2]);
	printf("response : %s\n", response);

	free(response);

}
