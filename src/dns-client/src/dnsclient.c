/*
 * dnsclient.c —— 手写 DNS 查询报文并解析应答（UDP，系统调用层）
 *
 * 来源：D:\6. DNS协议与请求\code\dns.c（2019 年的课程示例）。
 * 2026-11-23 作为"入库态"进仓库，2026-11-27 拆成本文件 + include/dnsclient.h +
 * 薄驱动 src/main.c。**拆分只改链接属性与文件位置，函数体一行没动，
 * 缺陷一条没修**——已知缺陷在 README 第 9 节，按第 10 节的顺序逐日处理。
 *
 * 入库时为了零警告做过五处编译层改动（语义不变），逐条写在 README 的
 * "实现记录"里；这里不再重复。
 *
 * 内部与对外的分界：is_pointer / dns_parse_name 仍在文件内 static，
 * 它们没有独立意义（一个判压缩指针的高 2 位，一个是 dns_parse_response 的
 * 递归辅助）；其余五个函数在 include/dnsclient.h 里有声明。
 */

#include "dnsclient.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define DNS_SERVER_PORT		53
#define DNS_SERVER_IP		"114.114.114.114"

#define DNS_HOST			0x01
#define DNS_CNAME			0x05

/* client sendto dns server */
int dns_create_header(struct dns_header *header) {

	if (header == NULL) return -1;
	memset(header, 0, sizeof(struct dns_header));

	/* random */
	srandom((unsigned int)time(NULL));
	header->id = (unsigned short)random();

	header->flags = htons(DNS_FLAGS_RD);
	header->questions = htons(1);

	return 0;
}

/* hostname: www.0voice.com
 * www
 * 0voice
 * com
 *
 * name: 3www60voice3com0（每个 label 前一个长度字节，末尾一个 0x00）
 */
int dns_create_question(struct dns_question *question, const char *hostname) {

	if (question == NULL || hostname == NULL) return -1;
	memset(question, 0, sizeof(struct dns_question));

	question->name = (unsigned char *)malloc(strlen(hostname) + 2);
	if (question->name == NULL) {
		return -2;
	}

	question->length = (int)(strlen(hostname) + 2);

	question->qtype = htons(1); /* A */
	question->qclass = htons(1); /* IN */

	/* name */
	const char delim[2] = ".";
	unsigned char *qname = question->name;

	char *hostname_dup = strdup(hostname); /* strdup --> malloc */
	char *token = strtok(hostname_dup, delim); /* www.0voice.com */

	while (token != NULL) {

		size_t len = strlen(token);

		*qname = (unsigned char)len;
		qname ++;

		memcpy(qname, token, len + 1);
		qname += len;

		token = strtok(NULL, delim); /* 0voice.com , com */

	}

	free(hostname_dup);

	return 0;
}

/* struct dns_header   *header
 * struct dns_question *question
 * char                *request
 */
int dns_build_request(struct dns_header *header, struct dns_question *question,
                      char *request, int rlen) {

	if (header == NULL || question == NULL || request == NULL) return -1;
	memset(request, 0, (size_t)rlen);

	/* header --> request */

	memcpy(request, header, sizeof(struct dns_header));
	int offset = (int)sizeof(struct dns_header);

	/* question --> request */
	memcpy(request + offset, question->name, (size_t)question->length);
	offset += question->length;

	memcpy(request + offset, &question->qtype, sizeof(question->qtype));
	offset += (int)sizeof(question->qtype);

	memcpy(request + offset, &question->qclass, sizeof(question->qclass));
	offset += (int)sizeof(question->qclass);

	return offset;
}

/* 域名压缩（RFC 1035 §4.1.4）：一个 2 字节指针，高 2 位是 11，
 * 低 14 位是从报文起点算的偏移。这个判断只看高 2 位。 */
static int is_pointer(int in) {
	return ((in & 0xC0) == 0xC0);
}

static void dns_parse_name(unsigned char *chunk, unsigned char *ptr, char *out, int *len) {

	int flag = 0, n = 0;
	char *pos = out + (*len);

	while (1) {

		flag = (int)ptr[0];
		if (flag == 0) break;

		if (is_pointer(flag)) {

			n = (int)ptr[1];
			ptr = chunk + n;
			dns_parse_name(chunk, ptr, out, len);
			break;

		} else {

			ptr ++;
			memcpy(pos, ptr, (size_t)flag);
			pos += flag;
			ptr += flag;

			*len += flag;
			if ((int)ptr[0] != 0) {
				memcpy(pos, ".", 1);
				pos += 1;
				(*len) += 1;
			}
		}

	}

}

int dns_parse_response(char *buffer, struct dns_item **domains) {

	int i = 0;
	unsigned char *ptr = (unsigned char *)buffer;

	ptr += 4;
	int querys = ntohs(*(unsigned short *)ptr);

	ptr += 2;
	int answers = ntohs(*(unsigned short *)ptr);

	ptr += 6;
	for (i = 0; i < querys; i ++) {
		while (1) {
			int flag = (int)ptr[0];
			ptr += (flag + 1);

			if (flag == 0) break;
		}
		ptr += 4;
	}

	char cname[128], aname[128], ip[20], netip[4];
	int len, type, ttl, datalen;

	int cnt = 0;
	struct dns_item *list = (struct dns_item *)calloc((size_t)answers, sizeof(struct dns_item));
	if (list == NULL) {
		return -1;
	}

	for (i = 0; i < answers; i ++) {

		bzero(aname, sizeof(aname));
		len = 0;

		dns_parse_name((unsigned char *)buffer, ptr, aname, &len);
		ptr += 2;

		type = htons(*(unsigned short *)ptr);
		ptr += 4;

		ttl = htons(*(unsigned short *)ptr);
		ptr += 4;

		datalen = ntohs(*(unsigned short *)ptr);
		ptr += 2;

		if (type == DNS_CNAME) {

			bzero(cname, sizeof(cname));
			len = 0;
			dns_parse_name((unsigned char *)buffer, ptr, cname, &len);
			ptr += datalen;

		} else if (type == DNS_HOST) {

			bzero(ip, sizeof(ip));

			if (datalen == 4) {
				memcpy(netip, ptr, (size_t)datalen);
				inet_ntop(AF_INET, netip, ip, sizeof(struct sockaddr));

				printf("%s has address %s\n", aname, ip);
				printf("\tTime to live: %d minutes , %d seconds\n", ttl / 60, ttl % 60);

				list[cnt].domain = (char *)calloc(strlen(aname) + 1, 1);
				memcpy(list[cnt].domain, aname, strlen(aname));

				list[cnt].ip = (char *)calloc(strlen(ip) + 1, 1);
				memcpy(list[cnt].ip, ip, strlen(ip));

				cnt ++;
			}

			ptr += datalen;
		}
	}

	*domains = list;
	ptr += 2;

	return cnt;

}

int dns_client_commit(const char *domain) {

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		return -1;
	}

	struct sockaddr_in servaddr = {0};
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(DNS_SERVER_PORT);
	servaddr.sin_addr.s_addr = inet_addr(DNS_SERVER_IP);

	int ret = connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
	printf("connect : %d\n", ret);

	struct dns_header header = {0};
	dns_create_header(&header);

	struct dns_question question = {0};
	dns_create_question(&question, domain);

	char request[1024] = {0};
	int length = dns_build_request(&header, &question, request, 1024);

	/* request */
	(void)sendto(sockfd, request, (size_t)length, 0, (struct sockaddr *)&servaddr, sizeof(struct sockaddr));

	/* recvfrom */
	char response[1024] = {0};
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(struct sockaddr_in);

	int n = (int)recvfrom(sockfd, response, sizeof(response), 0, (struct sockaddr *)&addr, &addr_len);

	struct dns_item *dns_domain = NULL;
	dns_parse_response(response, &dns_domain);

	free(dns_domain);

	return n;

}
