/*
 * test_response.c —— 应答解析的离线用例（2026-11-28）
 *
 * 输入不是"连一次真实 DNS 服务器"的结果，而是 testdata/ 下七个固定字节文件：
 * 手工按 RFC 1035 §4.1 编码出来的完整应答、截断应答、坏压缩指针。这样断言
 * 的输入是常量，"服务器今天回什么"不再是测试的一部分。
 *
 * 期望值同样是手写的（域名文本、点分地址、记录条数），不是从本程序的输出
 * 抄回来的——抄输出只能证明"改动前后一样"，不能证明"对"。
 *
 * 这批用例是 2026-11-28 那次改动的验收条件：dns_parse_response 从
 * (char *, struct dns_item **) 变成 (const char *, size_t, struct dns_item **)，
 * 全程按收到的字节数核对游标。三条截断/坏指针用例在旧实现上会读越界或
 * 栈溢出——它们不是"顺便加的测试"，是这次改动唯一能证明自己的地方。
 *
 * 运行时会看到夹杂的 "xxx has address ..." 行：库仍在解析里 printf
 * （README 第 8 节第 13 条，尚未搬回驱动）。它不影响下面的计数，
 * 但正是待办里要搬走的东西——这里不掩盖它。
 *
 * 编译：make test（链接 bin/libdnsclient.a，不重复编译模块源码）
 * 用法：./test_response [testdata 目录]   默认 testdata
 */

#include "dnsclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks   = 0;
static int g_failures = 0;

static const char *g_datadir = "testdata";

static void pass(const char *name) {
	(void)printf("ok    %s\n", name);
}

static void fail(const char *name, const char *detail) {
	g_failures++;
	(void)printf("FAIL  %-52s %s\n", name, detail);
}

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

static void expect_size(const char *name, size_t got, size_t want) {
	g_checks++;
	if (got != want) {
		char buf[96];
		(void)snprintf(buf, sizeof(buf), "得到 %zu 字节，期望 %zu 字节", got, want);
		fail(name, buf);
		return;
	}
	pass(name);
}

static void expect_str(const char *name, const char *got, const char *want) {
	g_checks++;
	if (got == NULL || strcmp(got, want) != 0) {
		char buf[256];
		(void)snprintf(buf, sizeof(buf), "得到 \"%s\"，期望 \"%s\"",
		               got == NULL ? "(NULL)" : got, want);
		fail(name, buf);
		return;
	}
	pass(name);
}

static void expect_null(const char *name, const struct dns_item *got) {
	g_checks++;
	if (got != NULL) {
		fail(name, "*domains 不是 NULL");
		return;
	}
	pass(name);
}

/* 读一个 testdata 文件，返回字节数；读不到返回 0。
 * 这里用 0 当"失败"是安全的：没有任何用例的文件是 0 字节，而 0 字节本身
 * 也不是合法的 DNS 应答（连 12 字节报文头都不够）。 */
static size_t load(const char *name, unsigned char *buf, size_t cap) {
	char path[512];
	FILE *f;
	size_t n;

	(void)snprintf(path, sizeof(path), "%s/%s", g_datadir, name);

	f = fopen(path, "rb");
	if (f == NULL) return 0;

	n = fread(buf, 1, cap, f);
	(void)fclose(f);

	return n;
}

/* 跑一份固定应答：先钉住文件字节数（字节数本身就是文档语义，见 README 的
 * 偏移表），再断言解析返回的条数、第一条的域名与地址。
 *
 * want_cnt 为 -1 表示"这份报文该被拒绝"：返回 -1，且 *domains 被置 NULL。
 * 传入前先把 *domains 指向一个栈上的假结构，这样"函数有没有真的把它清掉"
 * 才是被验证的事情——如果它没清，我们看到的会是这个假地址而不是 NULL。 */
static void check_case(const char *file, size_t want_size, int want_cnt,
                       const char *want_domain, const char *want_ip) {
	unsigned char buf[512];
	char label[160];
	struct dns_item sentinel;
	struct dns_item *items = &sentinel;
	size_t n;
	int rc;

	(void)snprintf(label, sizeof(label), "%s：文件字节数", file);
	n = load(file, buf, sizeof(buf));
	if (n == 0) {
		g_checks++;
		fail(label, "读不到文件（先跑 make test 的 cwd 是模块根目录）");
		return;
	}
	expect_size(label, n, want_size);

	(void)snprintf(label, sizeof(label), "%s：解析返回 %d", file, want_cnt);
	rc = dns_parse_response((const char *)buf, n, &items);
	expect_int(label, rc, want_cnt);

	if (want_cnt > 0) {
		if (rc == want_cnt && items != NULL) {
			(void)snprintf(label, sizeof(label), "%s：第 1 条域名", file);
			expect_str(label, items[0].domain, want_domain);
			(void)snprintf(label, sizeof(label), "%s：第 1 条地址", file);
			expect_str(label, items[0].ip, want_ip);
		}
	} else {
		(void)snprintf(label, sizeof(label), "%s：*domains 为 NULL", file);
		expect_null(label, items);
	}

	/* 库的契约：数组本身与每条的 domain / ip 都由调用方 free。
	 * 只有解析成功且真的写出数组时才 free，避免 free 掉那个栈上的假结构。 */
	if (rc > 0 && items != NULL && items != &sentinel) {
		int i;
		for (i = 0; i < rc; i++) {
			free(items[i].domain);
			free(items[i].ip);
		}
		free(items);
	}
}

/* 长度形参这条边界本身：报文自称 ANCOUNT=1 但 len 只到报文头，必须拒绝。 */
static void test_length_edges(void) {
	unsigned char buf[512];
	struct dns_item sentinel;
	struct dns_item *items;
	size_t n;
	int rc;

	n = load("a-simple.bin", buf, sizeof(buf));
	if (n == 0) {
		g_checks++;
		fail("a-simple.bin 可读（长度用例的前置条件）", "读不到文件");
		return;
	}

	/* 11 字节：连报文头都不完整（DNS_HEADER_SIZE 是 12） */
	items = &sentinel;
	rc = dns_parse_response((const char *)buf, 11, &items);
	expect_int("len = 11（报文头不完整）返回 -1", rc, -1);
	expect_null("len = 11 时 *domains 为 NULL", items);

	/* 12 字节：报文头完整、自称 ANCOUNT=1，但后面一个字节都没有 */
	items = &sentinel;
	rc = dns_parse_response((const char *)buf, 12, &items);
	expect_int("len = 12（自称 1 条记录但没数据）返回 -1", rc, -1);
	expect_null("len = 12 时 *domains 为 NULL", items);

	/* 参数检查 */
	items = &sentinel;
	expect_int("buffer 为 NULL 返回 -1",
	           dns_parse_response(NULL, 0, &items), -1);

	expect_int("domains 为 NULL 返回 -1",
	           dns_parse_response((const char *)buf, n, NULL), -1);
}

int main(int argc, char *argv[]) {

	if (argc > 1) g_datadir = argv[1];

	(void)printf("dns-client 应答解析离线用例（输入是 testdata/ 的固定字节）\n\n");

	/* 正常应答：压缩指针形式的 NAME */
	check_case("a-simple.bin", 48, 1, "www.0voice.com", "93.184.216.34");

	/* CNAME 链 + A：第二条记录的 NAME 用指针指向第一条 RDATA 里的域名，
	 * 这是 RFC 1035 §4.1.4 允许的"指向先前出现过的名字"。 */
	check_case("cname-chain.bin", 74, 1, "example.com", "93.184.216.34");

	/* NAME 完整写出（16 字节）而不是 2 字节指针：游标必须按实际消耗推进，
	 * 旧实现无条件 ptr += 2，在这一份上会从 TTL 中间开始读。 */
	check_case("uncompressed-name.bin", 62, 1, "www.0voice.com", "192.0.2.1");

	/* ANCOUNT=0（RCODE=3，NXDOMAIN）：0 条记录不是错误，*domains 为 NULL */
	check_case("nxdomain.bin", 33, 0, NULL, NULL);

	/* 坏报文：三条都必须被拒，而不是读越界之后"碰巧"给出结果 */
	check_case("truncated-question.bin", 40, -1, NULL, NULL);
	check_case("truncated-rdata.bin", 46, -1, NULL, NULL);
	check_case("bad-pointer.bin", 28, -1, NULL, NULL);

	test_length_edges();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
