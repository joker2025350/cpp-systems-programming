/*
 * dns.c —— 手写 DNS 查询报文并解析应答（UDP，系统调用层）
 *
 * 来源：D:\6. DNS协议与请求\code\dns.c（2019 年的课程示例）。
 * 本文件是"入库态"：程序能编译、能运行，但**逻辑与原始示例一致，缺陷一个都没修**。
 * 已知缺陷清单见 README 第 9 节，工程化与修缺陷放在第二阶段逐日推进。
 *
 * 入库时只做了"让它在 -Wall -Wextra -Wconversion -Wpedantic 下零警告"所必需的改动，
 * 共五处，都是编译层面的事，不改变任何语义：
 *   1. 补上缺失的头文件（<time.h>、<arpa/inet.h>、<strings.h>）——原文靠隐式声明编译，
 *      按 C11 6.5.2.2p6 隐式函数声明在 C99 之后已被移除，typeof 不匹配时的行为未定义。
 *   2. 四个函数改成 static：它们只在本文件内使用，且没有对外声明，
 *      留着外部链接会触发 -Wmissing-prototypes。
 *   3. 补上 dns_create_question 缺失的 return：函数声明返回 int 却从末尾掉出去，
 *      按 C11 6.9.1p12，返回值未指定，调用方读它就是未定义行为。
 *   4. 显式转换与指针符号一致（strlen 的 size_t → int、char * ↔ unsigned char *）。
 *   5. strncpy(qname, token, len+1) 改成 memcpy(qname, token, len+1)：
 *      这里拷的正好是 len 个字符加结尾 NUL，两者的结果逐字节相同，
 *      但 strncpy 在这种用法下会被静态检查判为"可能没有终止符"。
 *      另删掉一个从头到尾没被用过的变量 alen。
 */

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

struct dns_header {

	unsigned short id;
	unsigned short flags;

	unsigned short questions; // 1
	unsigned short answer;

	unsigned short authority;
	unsigned short additional;

};


struct dns_question {
	int length;
	unsigned short qtype;
	unsigned short qclass;
	unsigned char *name; // 
};

struct dns_item {
	char *domain;
	char *ip;
};


//client sendto dns server

static int dns_create_header(struct dns_header *header) {

	if (header == NULL) return -1;
	memset(header, 0, sizeof(struct dns_header));

	//random
	srandom((unsigned int)time(NULL));
	header->id = (unsigned short)random();

	header->flags = htons(0x0100);
	header->questions = htons(1); 

	return 0;
}

// hostname: www.0voice.com
// www
// 0voice
// com

// name: 3www60voice3com0

static int dns_create_question(struct dns_question *question, const char *hostname) {

	if (question == NULL || hostname == NULL) return -1;
	memset(question, 0, sizeof(struct dns_question));

	question->name = (unsigned char *)malloc(strlen(hostname) + 2);
	if (question->name == NULL) {
		return -2;
	}

	question->length = (int)(strlen(hostname) + 2);

	question->qtype = htons(1); //
	question->qclass = htons(1);

	// name 
	const char delim[2] = ".";
	unsigned char *qname = question->name;
	
	char *hostname_dup = strdup(hostname); // strdup --> malloc
	char *token = strtok(hostname_dup, delim); // www.0voice.com 

	while (token != NULL) {

		size_t len = strlen(token);

		*qname = (unsigned char)len;
		qname ++;

		memcpy(qname, token, len + 1);
		qname += len;

		token = strtok(NULL, delim); //0voice.com ,  com

	}

	free(hostname_dup);

	return 0;
}

// struct dns_header *header
// struct dns_question *question
// char *request

static int dns_build_request(struct dns_header *header, struct dns_question *question, char *request, int rlen) {

	if (header == NULL || question == NULL || request == NULL) return -1;
	memset(request, 0, (size_t)rlen);

	// header --> request
	
	memcpy(request, header, sizeof(struct dns_header));
	int offset = (int)sizeof(struct dns_header);

	// question --> request
	memcpy(request+offset, question->name, (size_t)question->length);
	offset += question->length;

	memcpy(request+offset, &question->qtype, sizeof(question->qtype));
	offset += (int)sizeof(question->qtype);

	memcpy(request+offset, &question->qclass, sizeof(question->qclass));
	offset += (int)sizeof(question->qclass);

	return offset;

}


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




static int dns_parse_response(char *buffer, struct dns_item **domains) {

	int i = 0;
	unsigned char *ptr = (unsigned char *)buffer;

	ptr += 4;
	int querys = ntohs(*(unsigned short *)ptr);

	ptr += 2;
	int answers = ntohs(*(unsigned short *)ptr);

	ptr += 6;
	for (i = 0;i < querys;i ++) {
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

	for (i = 0;i < answers;i ++) {
		
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
				inet_ntop(AF_INET , netip , ip , sizeof(struct sockaddr));

				printf("%s has address %s\n" , aname, ip);
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


static int dns_client_commit(const char *domain) {

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		return  -1;
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

	// request
	(void)sendto(sockfd, request, (size_t)length, 0, (struct sockaddr *)&servaddr, sizeof(struct sockaddr));
	
	//recvfrom 
	char response[1024] = {0};
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(struct sockaddr_in);
	
	
	int n = (int)recvfrom(sockfd, response, sizeof(response), 0, (struct sockaddr *)&addr, &addr_len);

	struct dns_item *dns_domain = NULL;
	dns_parse_response(response, &dns_domain);

	free(dns_domain);
	
	return n;

}


int main(int argc, char *argv[]) {

	if (argc < 2) return -1;

	dns_client_commit(argv[1]);

}
