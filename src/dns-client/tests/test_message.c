/*
 * test_message.c —— 报文构造的离线用例
 *
 * 这是拆出 include/dnsclient.h 之后第一批能写的用例：2026-11-23 入库时
 * dns_build_request 关在 main 所在的同一个文件里、且是 static，"发出去的字节
 * 对不对"只能靠连真实 DNS 服务器 + 人工比对 dig 的输出。现在构造过程能在进程内
 * 直接调用，固定输入 → 逐字节比对期望值，不碰网络。
 *
 * 覆盖的是**当前实现的实际行为**，包括它的缺陷：
 *   - 断言 QNAME 的长度字节与 label 内容（RFC 1035 §4.1.2）；
 *   - 断言报文尾部的 QTYPE/QCLASS 为 00 01 / 00 01（A / IN）；
 *   - 断言返回值等于 12 + QNAME 长度 + 4，即写入的字节数。
 * 没有断言的：TTL（那属于解析）、rlen 不足时的行为（第 8 节第 4 条，
 * 现在会越界写，ASan 下会直接报错，不该写成"通过"）、
 * 带结尾点的域名（长度字段与实际写入字节数不一致，另案处理）。
 *
 * 编译：make test（链接 bin/libdnsclient.a，不重复编译模块源码）
 */

#include "dnsclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static int g_checks   = 0;
static int g_failures = 0;

static void pass(const char *name) {
	(void)printf("ok    %s\n", name);
}

static void fail(const char *name, const char *detail) {
	g_failures++;
	(void)printf("FAIL  %-50s %s\n", name, detail);
}

/* 断言 int 相等 */
static void expect_int(const char *name, int got, int want) {
	g_checks++;
	if (got != want) {
		char buf[96];
		(void)snprintf(buf, sizeof(buf), "得到 %d，期望 %d", got, want);
		fail(name, buf);
		return;
	}
	pass(name);
}

/* 断言两段字节逐字节相同。不一致时只报第一处差异——
 * 报文比对里"从哪一字节开始错"就是全部信息，把十几处差异都打出来反而看不清。 */
static void expect_bytes(const char *name, const unsigned char *got,
                         const unsigned char *want, size_t n) {
	size_t i;

	g_checks++;
	for (i = 0; i < n; i++) {
		if (got[i] != want[i]) {
			char buf[96];
			(void)snprintf(buf, sizeof(buf), "第 %zu 字节为 %02x，期望 %02x",
			               i, got[i], want[i]);
			fail(name, buf);
			return;
		}
	}
	pass(name);
}

/* ----------------------------------------------------------------
 * 报文头
 * ---------------------------------------------------------------- */

static void test_header(void) {
	struct dns_header h;

	g_checks++;
	if (dns_create_header(NULL) != -1) {
		fail("dns_create_header(NULL) 返回 -1", "返回值不是 -1");
	} else {
		pass("dns_create_header(NULL) 返回 -1");
	}

	(void)memset(&h, 0xA5, sizeof(h));	/* 先弄脏，证明函数确实写了每个字段 */
	expect_int("dns_create_header 返回 0", dns_create_header(&h), 0);

	/* flags 存的是网络字节序，比较前转回主机序；不这样写就变成
	 * "在小端上断言 0x0001"，把实现细节当成了契约。 */
	expect_int("Flags 的 RD 位为 1（0x0100）", (int)ntohs(h.flags), 0x0100);
	expect_int("QDCOUNT 为 1", (int)ntohs(h.questions), 1);
	expect_int("ANCOUNT 为 0", (int)ntohs(h.answer), 0);
	expect_int("NSCOUNT 为 0", (int)ntohs(h.authority), 0);
	expect_int("ARCOUNT 为 0", (int)ntohs(h.additional), 0);
}

/* ----------------------------------------------------------------
 * QNAME 编码
 * ---------------------------------------------------------------- */

/* www.0voice.com 的线上编码，16 字节：
 *   03 77 77 77   label "www"
 *   06 30 76 6f 69 63 65   label "0voice"
 *   03 63 6f 6d   label "com"
 *   00            根标签
 * 长度 16 = strlen("www.0voice.com") + 2。 */
static const unsigned char k_qname_www[] = {
	0x03, 'w', 'w', 'w',
	0x06, '0', 'v', 'o', 'i', 'c', 'e',
	0x03, 'c', 'o', 'm',
	0x00
};

/* 单 label：没有点可切，strtok 返回整串 */
static const unsigned char k_qname_localhost[] = {
	0x09, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't',
	0x00
};

static void test_question(void) {
	struct dns_question q;

	g_checks++;
	if (dns_create_question(NULL, "a") == -1 && dns_create_question(&q, NULL) == -1) {
		pass("dns_create_question 参数为 NULL 返回 -1");
	} else {
		fail("dns_create_question 参数为 NULL 返回 -1", "返回值不是 -1");
	}

	(void)memset(&q, 0, sizeof(q));
	expect_int("dns_create_question 返回 0",
	           dns_create_question(&q, "www.0voice.com"), 0);
	expect_int("QNAME 长度为 strlen+2 = 16", q.length, 16);
	expect_int("QTYPE 为 1（A）", (int)ntohs(q.qtype), 1);
	expect_int("QCLASS 为 1（IN）", (int)ntohs(q.qclass), 1);

	if (q.name == NULL) {
		g_checks++;
		fail("QNAME 已分配", "name 为 NULL");
		return;
	}
	expect_bytes("QNAME 逐字节为 03 www 06 0voice 03 com 00", q.name, k_qname_www, 16);
	free(q.name);

	(void)memset(&q, 0, sizeof(q));
	expect_int("单 label 域名也能编码",
	           dns_create_question(&q, "localhost"), 0);
	expect_int("单 label 的长度为 strlen+2 = 11", q.length, 11);
	if (q.name == NULL) {
		g_checks++;
		fail("单 label 的 QNAME 已分配", "name 为 NULL");
		return;
	}
	expect_bytes("单 label 的 QNAME 逐字节为 09 localhost 00",
	             q.name, k_qname_localhost, 11);
	free(q.name);
}

/* ----------------------------------------------------------------
 * 整条查询报文
 * ---------------------------------------------------------------- */

static void test_request(void) {
	struct dns_header h;
	struct dns_question q;
	unsigned char req[64];
	int n;

	g_checks++;
	if (dns_build_request(NULL, &q, (char *)req, (int)sizeof(req)) == -1 &&
	    dns_build_request(&h, NULL, (char *)req, (int)sizeof(req)) == -1 &&
	    dns_build_request(&h, &q, NULL, (int)sizeof(req)) == -1) {
		pass("dns_build_request 参数为 NULL 返回 -1");
	} else {
		fail("dns_build_request 参数为 NULL 返回 -1", "返回值不是 -1");
	}

	(void)memset(req, 0xA5, sizeof(req));	/* 弄脏，验证 memset 与写入范围 */
	(void)dns_create_header(&h);
	(void)dns_create_question(&q, "www.0voice.com");

	n = dns_build_request(&h, &q, (char *)req, (int)sizeof(req));
	expect_int("返回值为 12 + 16 + 4 = 32", n, 32);

	expect_bytes("报文头 12 字节与 struct dns_header 一致",
	             req, (const unsigned char *)&h, (size_t)DNS_HEADER_SIZE);
	expect_bytes("QNAME 紧跟在报文头之后",
	             req + DNS_HEADER_SIZE, k_qname_www, 16);

	{
		static const unsigned char tail[4] = { 0x00, 0x01, 0x00, 0x01 };
		expect_bytes("QTYPE/QCLASS 为 00 01 00 01",
		             req + DNS_HEADER_SIZE + 16, tail, 4);
	}

	/* 写入范围之外必须是 memset 过的 0，否则整条报文的有效长度就不是 32 了 */
	g_checks++;
	{
		int i;
		int bad = -1;
		for (i = n; i < (int)sizeof(req); i++) {
			if (req[i] != 0) { bad = i; break; }
		}
		if (bad >= 0) {
			char buf[64];
			(void)snprintf(buf, sizeof(buf), "第 %d 字节起非 0", bad);
			fail("返回值之后到缓冲区末尾全为 0", buf);
		} else {
			pass("返回值之后到缓冲区末尾全为 0");
		}
	}

	free(q.name);
}

/* ---------------------------------------------------------------- */

int main(void) {
	(void)printf("dns-client 报文构造离线用例\n\n");

	test_header();
	test_question();
	test_request();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
