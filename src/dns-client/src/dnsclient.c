/*
 * dnsclient.c —— 手写 DNS 查询报文并解析应答（UDP，系统调用层）
 *
 * 来源：D:\6. DNS协议与请求\code\dns.c（2019 年的课程示例）。
 * 2026-11-23 作为"入库态"进仓库，2026-11-27 拆成本文件 + include/dnsclient.h +
 * 薄驱动 src/main.c。
 *
 * 2026-11-28 起进入"逐条修缺陷"：当天只改解析侧（长度形参 + 边界核对 +
 * 压缩指针必须回退，README 第 8 节第 4、5 条）。报文构造与连接管理的函数体
 * 仍未改动，第 1、2、3、6、7、8…条照旧。
 *
 * 入库时为了零警告做过五处编译层改动（语义不变），逐条写在 README 的
 * "实现记录"里；这里不再重复。
 *
 * 内部与对外的分界：is_pointer / dns_name_read / read_u16 仍在文件内 static，
 * 它们没有独立意义（一个判压缩指针的高 2 位，一个带边界地读域名，一个带边界地
 * 读 16 位字段）；其余五个函数在 include/dnsclient.h 里有声明。
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

/* 线上编码的域名最长 255 字节：每个 label 一个长度字节 + 内容，末尾一个根标签
 * （RFC 1035 §2.3.4）。
 *
 * 换成点分文本后长度恒为 253：文本长度 = 内容字节之和 + 分隔点个数，而
 * 线上长度 = 内容字节之和 + label 个数（长度字节）+ 1（根标签）；两个 label
 * 之间的分隔点有 "label 个数 - 1" 个，一加一减正好抵消，所以总是少 2。 */
#define DNS_MAX_WIRE_NAME	255
#define DNS_MAX_TEXT_NAME	253

/* 单个 label 的内容上限（RFC 1035 §2.3.4）。长度字节取值 1..63；
 * 64..191 高 2 位是 01 / 10，属于"扩展 label 类型"，本模块不认。 */
#define DNS_MAX_LABEL		63

/* 从 msg 的 off 处原样取出 2 字节，按本机字节序解释成 16 位值，**不做网络序
 * 转换**——转换留在调用点，这样"这一处该用 ntohs 还是 htons"的缺陷仍然原样
 * 留在原来那行（第 8 节第 2 条，今天没修）。
 *
 * 用 memcpy 而不是 *(unsigned short *)(msg + off)：按 C11 6.3.2.3p7，指针转换
 * 的结果未按目标类型对齐时行为未定义。报文里的字段从任意偏移开始，而缓冲区
 * 本身是 char 数组（对齐要求 1），除偏移 0 外都不保证 2 字节对齐。
 *
 * 越界返回 -1，不读。比较写成减法形式，避免 off + 2 在 off 接近 SIZE_MAX 时回绕。 */
static int read_u16(const unsigned char *msg, size_t len, size_t off,
                    unsigned short *out) {
	if (off > len || len - off < 2) return -1;
	memcpy(out, msg + off, 2);
	return 0;
}

/* 从 msg 的 off 处读一个域名，写成点分文本放进 out（容量 outsz，含结尾 NUL）。
 *
 * 返回值是这个域名在报文里**实际占用的字节数**（不是文本长度）：不带压缩指针
 * 时是各 label 的长度字节 + 内容 + 根标签；带指针时只算到指针为止，指针指向
 * 的那一段不重复计入。失败返回 -1。
 *
 * 与 2026-11-27 之前的 dns_parse_name 有三处差别，都是第 4、5 条缺陷的修法：
 *   1. 每一步都对照 len，越出 len 一律返回 -1，不再顺着报文自称的长度读下去；
 *   2. 压缩指针必须**回退**（目标偏移严格小于当前偏移）。RFC 1035 §4.1.4 说
 *      指针指向"报文里先前出现过的"域名，所以回退是协议本身的要求，不只是
 *      防环手段；回退保证每跳一次 pos 严格减小，跳转次数因此有上界，
 *      指向自己或指向后面的指针在第一跳就被拒掉（不再栈溢出）；
 *   3. 不再递归。原来跳指针时递归调用自己，深度由报文内容决定；这里用循环，
 *      跳转次数另有上限（DNS_MAX_WIRE_NAME，远超合法报文可能出现的次数）。
 */
static int dns_name_read(const unsigned char *msg, size_t len, size_t off,
                         char *out, size_t outsz) {
	size_t pos = off;
	size_t olen = 0;		/* 已写入的文本字符数，不含 NUL */
	size_t jumps = 0;
	int consumed = -1;		/* 到根标签或第一个指针为止占用的字节数 */

	if (msg == NULL || out == NULL || outsz == 0) return -1;

	while (1) {

		unsigned char c;
		size_t dot;

		if (pos >= len) return -1;
		c = msg[pos];

		if (c == 0) {			/* 根标签：名字到此结束 */
			if (consumed < 0) consumed = (int)(pos - off) + 1;
			break;
		}

		if (is_pointer((int)c)) {	/* 压缩指针：2 字节 */
			size_t target;

			if (len - pos < 2) return -1;
			target = ((size_t)(c & 0x3F) << 8) | (size_t)msg[pos + 1];
			if (consumed < 0) consumed = (int)(pos - off) + 2;
			if (target >= pos) return -1;
			if (++jumps > DNS_MAX_WIRE_NAME) return -1;
			pos = target;
			continue;
		}

		/* 走到这里说明高 2 位是 00；再排除 01 / 10 两种保留类型。
		 * 它们的长度字节都 > 63，与 label 上限合并成一条检查。 */
		if ((size_t)c > DNS_MAX_LABEL) return -1;
		if (len - pos < (size_t)c + 1) return -1;

		dot = (olen > 0) ? 1U : 0U;
		if (olen + dot + (size_t)c + 1 > outsz) return -1;			/* 留 1 字节给 NUL */
		if (olen + dot + (size_t)c > DNS_MAX_TEXT_NAME) return -1;

		if (dot != 0) out[olen++] = '.';
		memcpy(out + olen, msg + pos + 1, (size_t)c);
		olen += (size_t)c;
		pos += (size_t)c + 1;
	}

	out[olen] = '\0';
	return consumed;
}

int dns_parse_response(const char *buffer, size_t len, struct dns_item **domains) {

	const unsigned char *msg = (const unsigned char *)buffer;

	unsigned short field;
	int querys, answers;
	int i, cnt = 0;
	int type, datalen, ttl;
	size_t off;
	char aname[DNS_MAX_TEXT_NAME + 1];
	char cname[DNS_MAX_TEXT_NAME + 1];
	char ip[20];
	char netip[4];
	struct dns_item *list;

	if (buffer == NULL || domains == NULL) return -1;
	*domains = NULL;

	if (len < DNS_HEADER_SIZE) return -1;

	/* 报文头字段布局（RFC 1035 §4.1.1）：ID 0、Flags 2、QDCOUNT 4、
	 * ANCOUNT 6、NSCOUNT 8、ARCOUNT 10。 */
	if (read_u16(msg, len, 4, &field) < 0) return -1;
	querys = (int)ntohs(field);
	if (read_u16(msg, len, 6, &field) < 0) return -1;
	answers = (int)ntohs(field);

	off = DNS_HEADER_SIZE;

	/* 跳过问题段：QNAME + QTYPE(2) + QCLASS(2)。QDCOUNT 通常为 1，
	 * 但报文自称几段就跳几段——它是报文的一部分，不是常量。 */
	for (i = 0; i < querys; i++) {
		int adv = dns_name_read(msg, len, off, aname, sizeof(aname));
		if (adv < 0) return -1;
		off += (size_t)adv;
		if (len - off < 4) return -1;
		off += 4;
	}

	/* ANCOUNT 为 0（NXDOMAIN 等）：没有记录可收，但这不是错误。
	 * 显式返回 0 而不是让 calloc(0) 决定——glibc 上它返回一个可 free 的
	 * 非空指针，别处可能返回 NULL，行为不该依赖这个。 */
	if (answers == 0) return 0;

	/* 一条记录最少 11 字节：NAME 最短是一个根标签（1 字节），加上
	 * TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)。ANCOUNT 是 16 位字段，
	 * 伪造时最大 65535（约 1 MB 的 dns_item），先用这个下限把"自称很多条、
	 * 后面根本没有这么多字节"的报文拒掉，不为它分配内存。 */
	if (len - off < (size_t)answers * 11) return -1;

	list = (struct dns_item *)calloc((size_t)answers, sizeof(struct dns_item));
	if (list == NULL) return -1;

	for (i = 0; i < answers; i++) {

		/* 一条资源记录的布局（RFC 1035 §4.1.3）：
		 * NAME | TYPE(2) | CLASS(2) | TTL(4) | RDLENGTH(2) | RDATA。
		 * NAME 之后的定长部分是 10 字节，先整体核对够不够，再逐字段读。 */
		int adv = dns_name_read(msg, len, off, aname, sizeof(aname));
		unsigned short raw;

		if (adv < 0) goto bad;
		off += (size_t)adv;
		if (len - off < 10) goto bad;

		if (read_u16(msg, len, off, &raw) < 0) goto bad;
		type = htons(raw);			/* 第 2 条缺陷：这里该是 ntohs */

		if (read_u16(msg, len, off + 4, &raw) < 0) goto bad;
		ttl = htons(raw);			/* 第 1 条缺陷：TTL 是 4 字节，这里只读了高 2 字节 */

		if (read_u16(msg, len, off + 8, &raw) < 0) goto bad;
		datalen = (int)ntohs(raw);

		off += 10;

		if (len - off < (size_t)datalen) goto bad;

		if (type == DNS_CNAME) {

			/* CNAME 的 RDATA 是另一个域名。解析它只为确认编码合法
			 * （非法编码会在这里被拒），文本本身不落库——本模块只返回
			 * A 记录。cname 缓冲因此只写不读，用途就是这个检查。 */
			if (dns_name_read(msg, len, off, cname, sizeof(cname)) < 0) goto bad;

		} else if (type == DNS_HOST && datalen == 4) {

			memcpy(netip, msg + off, 4);
			if (inet_ntop(AF_INET, netip, ip, sizeof(ip)) == NULL) goto bad;

			/* 第 13 条：库函数写 stdout，尚未搬回驱动 */
			printf("%s has address %s\n", aname, ip);
			printf("\tTime to live: %d minutes , %d seconds\n", ttl / 60, ttl % 60);

			list[cnt].domain = (char *)calloc(strlen(aname) + 1, 1);
			if (list[cnt].domain == NULL) goto bad;
			memcpy(list[cnt].domain, aname, strlen(aname));

			list[cnt].ip = (char *)calloc(strlen(ip) + 1, 1);
			if (list[cnt].ip == NULL) goto bad;
			memcpy(list[cnt].ip, ip, strlen(ip));

			cnt ++;
		}

		off += (size_t)datalen;
	}

	*domains = list;
	return cnt;

bad:
	/* 本次改动新增了失败路径，它们必须自己收尾：已经收进 list 的字符串
	 * 若不在这里释放，就是这次改动**新造**的一条泄漏——第 6 条讲的是
	 * dns_client_commit 里那条（正常路径），不是这条。 */
	for (i = 0; i < cnt; i++) {
		free(list[i].domain);
		free(list[i].ip);
	}
	free(list);
	return -1;

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

	/* 只有真收到字节才交给解析。recvfrom 失败返回 -1，转成 size_t 是
	 * SIZE_MAX，边界检查会把它当成"报文很长"而放过去——加了长度形参之后，
	 * "传进来的是真实字节数"这条前置条件必须由调用方守住。 */
	if (n > 0) {
		(void)dns_parse_response(response, (size_t)n, &dns_domain);
	}

	free(dns_domain);

	return n;

}
