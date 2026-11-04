/*
 * test_seed.c —— 哈希种子的可复现性与抗洪泛能力
 *
 * 分别验证三件事：
 *   1. 同一个种子能复现同一份桶分布（基准与调试的前提）；
 *   2. 种子为 0 时精确退回 1.3.0 的行为（与历史基准可对照）；
 *   3. 离线构造的同桶姓名集，在随机种子下被打散。
 *
 * 第 3 条是本文件存在的理由。它不作"跑得通"这种断言，而是先在测试里
 * 复现一遍攻击者做的事——用公开的 FNV-1a 常量离线搜出一批哈希低位相同的
 * 姓名——再证明这批姓名在 1.3.0 的哈希下确实退化成一条链，
 * 而在带种子的哈希下不退化。没有前半段，"抗洪泛"三个字就没有依据。
 *
 * 用法：
 *   make test-seed
 *   ./bin/test_seed
 */

#include "contacts.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks   = 0;
static int g_failures = 0;

/* ---------------------------------------------------------------- */

static void expect_int(const char *name, long got, long want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-52s 实际 %ld，期望 %ld\n", name, got, want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

static void expect_true(const char *name, int cond) {
	expect_int(name, cond ? 1 : 0, 1);
}

static void expect_u64(const char *name, uint64_t got, uint64_t want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-52s 实际 0x%016llx，期望 0x%016llx\n", name,
		             (unsigned long long)got, (unsigned long long)want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

/* ---------------------------------------------------------------- */

/*
 * 与模块内部相同的 FNV-1a，独立实现一遍。
 *
 * 故意不调用模块的哈希函数：这里扮演的是"攻击者"，他只有公开的算法常量。
 * 若复用被测代码，测出来的是"它和自己一致"，而不是"固定哈希可以被离线构造"。
 */
static uint64_t fnv1a_plain(const char *s) {
	const unsigned char *p = (const unsigned char *)s;
	uint64_t             h = UINT64_C(0xcbf29ce484222325);

	while (*p != '\0') {
		h ^= (uint64_t)(*p);
		h *= UINT64_C(0x100000001b3);
		p++;
	}
	return h;
}

/*
 * 搜出 want 个"FNV 低 12 位相同"的姓名。
 *
 * 低 12 位相同意味着在桶数不超过 4096 时它们全部落在同一个桶里，
 * 而 64 条记录的桶数只到 64（16 → 32 → 64），所以这条构造是安全余量很大的。
 * 期望尝试次数是 want * 4096 ≈ 26 万次，每次一次 strcmp 级别的计算，
 * 总耗时在 10 ms 量级。
 */
static int build_flood(char (*names)[16], int want) {
	const uint64_t mask   = UINT64_C(0xFFF);
	const uint64_t target = UINT64_C(0x5A5);
	unsigned long  i      = 0;
	int            got    = 0;

	while (got < want && i < 40000000ul) {
		char buf[16];

		(void)snprintf(buf, sizeof buf, "u%lu", i);
		i++;

		if ((fnv1a_plain(buf) & mask) == target) {
			(void)strcpy(names[got], buf);
			got++;
		}
	}
	return got;
}

/* 往书里插 n 条 name0..name(n-1)，电话用固定格式 */
static void fill_sequential(contact_book *book, int n) {
	int i;

	for (i = 0; i < n; i++) {
		char name[32];
		char phone[32];

		(void)snprintf(name, sizeof name, "name%03d", i);
		(void)snprintf(phone, sizeof phone, "139%08d", i);
		if (contacts_add(book, name, phone) != CONTACTS_OK) {
			(void)printf("FAIL  插入 name%03d 失败\n", i);
			g_failures++;
			g_checks++;
			return;
		}
	}
}

/* ---------------------------------------------------------------- */

static void test_seed_accessors(void) {
	contact_book book;

	expect_u64("contacts_seed(NULL) 为 0", contacts_seed(NULL), 0);

	contacts_init_seeded(&book, UINT64_C(0));
	expect_u64("init_seeded(0) 后种子为 0", contacts_seed(&book), 0);

	contacts_init_seeded(&book, UINT64_C(0xdeadbeefcafe1234));
	expect_u64("init_seeded 保留传入的种子",
	           contacts_seed(&book), UINT64_C(0xdeadbeefcafe1234));

	contacts_destroy(&book);

	/* NULL 不能崩 */
	contacts_init_seeded(NULL, UINT64_C(1));
	expect_true("contacts_init_seeded(NULL) 不崩", 1);
}

static void test_init_is_random(void) {
	contact_book a;
	contact_book b;

	contacts_init(&a);
	contacts_init(&b);

	/*
	 * seed_entropy 保证返回值非 0（降级路径也是），所以这一条是确定性的。
	 * 两次取值不同则是概率断言：碰撞概率 2^-64，可以按"不会发生"处理。
	 */
	expect_true("contacts_init 取的种子非 0", contacts_seed(&a) != 0);
	expect_true("两次 contacts_init 得到不同种子",
	            contacts_seed(&a) != contacts_seed(&b));

	contacts_destroy(&a);
	contacts_destroy(&b);
}

/* 同一个种子必须逐字段复现同一份索引状态 */
static void test_same_seed_is_reproducible(void) {
	const uint64_t seed = UINT64_C(0x0123456789abcdef);
	contacts_index_stats s1;
	contacts_index_stats s2;
	contact_book a;
	contact_book b;

	contacts_init_seeded(&a, seed);
	contacts_init_seeded(&b, seed);

	fill_sequential(&a, 500);
	fill_sequential(&b, 500);

	expect_int("同种子：第一本书的桶数", (long)contacts_index_get(&a, &s1), 0);
	expect_int("同种子：第二本书的桶数", (long)contacts_index_get(&b, &s2), 0);

	expect_int("同种子：buckets 相同", (long)s1.buckets, (long)s2.buckets);
	expect_int("同种子：used 相同", (long)s1.used, (long)s2.used);
	expect_int("同种子：max_chain 相同", (long)s1.max_chain, (long)s2.max_chain);
	expect_int("同种子：rehashes 相同", (long)s1.rehashes, (long)s2.rehashes);

	/* 复现的必须是"有内容"的状态，而不是两边都空 */
	expect_int("同种子：500 条的桶数为 512", (long)s1.buckets, 512);
	expect_int("同种子：entries 为 500", (long)s1.entries, 500);

	contacts_destroy(&a);
	contacts_destroy(&b);
}

/*
 * 本轮的核心用例。
 *
 * 同一批 64 个姓名：
 *   - 种子 0（= 1.3.0 的哈希）下全部落进一个桶，最长链 64；
 *   - 两个固定非零种子下散布开，最长链远小于 64。
 *
 * 64 条记录对应的桶数是 64，把 64 个球随机放进 64 个桶，最长链超过 16 的
 * 概率在 1e-13 量级，所以"≤ 16"这条上界不是靠运气成立的。
 */
static void test_flood_is_broken_by_seed(void) {
	char     names[64][16];
	int      n;
	int      i;
	contact_book       book;
	contacts_index_stats st;
	const uint64_t seeds[2] = { UINT64_C(1), UINT64_C(0x9e3779b97f4a7c15) };
	int      k;

	n = build_flood(names, 64);
	expect_int("离线构造出 64 个低 12 位相同的姓名", n, 64);
	if (n != 64) {
		return;     /* 构造失败时后面的断言没有意义 */
	}

	/* ① 种子 0：退回 1.3.0，构造有效 */
	contacts_init_seeded(&book, 0);
	for (i = 0; i < 64; i++) {
		(void)contacts_add(&book, names[i], "1");
	}
	(void)contacts_index_get(&book, &st);

	expect_int("seed=0：桶数 64", (long)st.buckets, 64);
	expect_int("seed=0：64 条全部挤在一个桶", (long)st.used, 1);
	expect_int("seed=0：最长链 64（查找退化成线性扫描）", (long)st.max_chain, 64);
	contacts_destroy(&book);

	/* ② 非零种子：同一批姓名散开 */
	for (k = 0; k < 2; k++) {
		contacts_init_seeded(&book, seeds[k]);
		for (i = 0; i < 64; i++) {
			(void)contacts_add(&book, names[i], "1");
		}
		(void)contacts_index_get(&book, &st);

		expect_int("非零种子：entries 仍为 64", (long)st.entries, 64);
		expect_true("非零种子：非空桶多于 1 个", st.used > 1);
		expect_true("非零种子：最长链不超过 16", st.max_chain <= 16);
		contacts_destroy(&book);
	}
}

/*
 * 种子改变后，增删查的结果不能跟着变。
 *
 * 这条是回归测试：哈希值变了，凡是"算下标"的地方都必须用同一个种子。
 * 只要有一处漏传（比如重挂时用了常量 0），插入与查找就会落到不同的桶，
 * 表现为"刚加进去的记录找不到"，下面 300 条逐条回查必然抓到。
 */
static void test_lookup_survives_any_seed(void) {
	static const uint64_t seeds[] = {
		UINT64_C(0),
		UINT64_C(1),
		UINT64_C(0x0123456789abcdef),
		UINT64_C(0xffffffffffffffff)
	};
	size_t k;

	for (k = 0; k < sizeof seeds / sizeof seeds[0]; k++) {
		contact_book         book;
		contacts_index_stats st;
		int                  i;
		int                  bad = 0;

		contacts_init_seeded(&book, seeds[k]);
		fill_sequential(&book, 300);

		for (i = 0; i < 300; i++) {
			char name[32];
			const contact *c;

			(void)snprintf(name, sizeof name, "name%03d", i);
			c = contacts_find(&book, name);
			if (c == NULL) {
				bad++;
			}
		}
		expect_int("任意种子：300 条插入后逐条回查全部命中", bad, 0);

		(void)contacts_index_get(&book, &st);
		expect_int("任意种子：索引 entries 与 count 一致",
		           (long)(st.entries == contacts_count(&book)), 1);

		/* 删一半，剩下的仍要能找到，删掉的不能复活 */
		for (i = 0; i < 300; i += 2) {
			char name[32];

			(void)snprintf(name, sizeof name, "name%03d", i);
			(void)contacts_remove(&book, name);
		}
		expect_int("任意种子：删 150 条后 count 为 150",
		           (long)contacts_count(&book), 150);

		bad = 0;
		for (i = 0; i < 300; i++) {
			char name[32];
			const contact *c;

			(void)snprintf(name, sizeof name, "name%03d", i);
			c = contacts_find(&book, name);
			if ((i % 2) == 0) {
				bad += (c != NULL) ? 1 : 0;    /* 已删除，不该找到 */
			} else {
				bad += (c == NULL) ? 1 : 0;    /* 未删除，应该找到 */
			}
		}
		expect_int("任意种子：删除后命中集合正确", bad, 0);

		contacts_destroy(&book);
	}
}

/* clear 只释放内容，不改这本书的种子 */
static void test_clear_keeps_seed(void) {
	contact_book book;

	contacts_init_seeded(&book, UINT64_C(0xfeedface));

	fill_sequential(&book, 10);
	contacts_clear(&book);

	expect_u64("clear 之后种子不变",
	           contacts_seed(&book), UINT64_C(0xfeedface));
	expect_int("clear 之后数量为 0", (long)contacts_count(&book), 0);

	/* 清空后继续用，桶分布仍由同一个种子决定 */
	fill_sequential(&book, 10);
	expect_int("clear 之后可以继续插入", (long)contacts_count(&book), 10);

	contacts_destroy(&book);
}

/* ---------------------------------------------------------------- */

int main(void) {
	(void)printf("contacts %s 哈希种子测试\n\n", CONTACTS_VERSION_STRING);

	test_seed_accessors();
	test_init_is_random();
	test_same_seed_is_reproducible();
	test_flood_is_broken_by_seed();
	test_lookup_survives_any_seed();
	test_clear_keeps_seed();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
