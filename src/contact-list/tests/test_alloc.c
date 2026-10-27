/*
 * test_alloc.c —— 内存分配失败的注入测试
 *
 * 普通单元测试无法覆盖"malloc 返回 NULL"这条路径：它取决于此刻系统的
 * 可用内存，也不能靠写一个巨大的分配请求去逼近——那会让结果依赖
 * overcommit 策略，在容器里与物理机上不一样。
 *
 * 做法是链接期符号替换。GNU ld 的 --wrap 语义：链接时把所有对符号 S 的
 * **未定义引用**改指 __wrap_S，同时把 __real_S 解析成原来的 S。
 * 因此本文件定义 __wrap_malloc / __wrap_calloc 之后，libcontacts.a 里
 * 对 malloc / calloc 的调用全部经过这里。
 *
 * 边界要说清楚：libc 内部对 malloc 的调用不经过这里（那些引用在 libc
 * 内部就已解析，不参与本次链接），所以 fopen 分配的 stdio 缓冲区不受
 * 影响。这正是需要的效果——注入的是模块自己的分配，而不是整个进程的。
 *
 * 检查的不是"函数返回了错误码"，而是"失败之后数据结构是否仍然自洽"：
 * count、遍历长度、索引 entries 三者必须相等，且每个已插入的姓名都能
 * 被找到、每个未插入的姓名都找不到。分配失败最危险的后果不是报错，
 * 而是留下"节点已挂上但索引里没有"这类中间状态——它在后续操作里
 * 表现为随机查不到数据，与分配失败本身毫无关系。
 *
 * 用法：
 *   make test-alloc
 */

#define _POSIX_C_SOURCE 200809L

#include "contacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 注入点
 * ------------------------------------------------------------------ */

static long g_fail_at = -1;     /* 第几次分配返回 NULL；-1 表示不注入 */
static long g_calls   = 0;      /* 注入开启后被调用的次数 */

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t count, size_t size);

static int injecting_now(void) {
	if (g_fail_at < 0) {
		return 0;
	}
	return (g_calls++ == g_fail_at) ? 1 : 0;
}

void *__wrap_malloc(size_t size) {
	return injecting_now() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
	return injecting_now() ? NULL : __real_calloc(count, size);
}

/* 从第 n 次分配开始注入；n 为负数表示关闭 */
static void inject_at(long n) {
	g_fail_at = n;
	g_calls   = 0;
}

static void inject_off(void) {
	inject_at(-1);
}

/* ------------------------------------------------------------------
 * 断言与不变量
 * ------------------------------------------------------------------ */

static long g_checks   = 0;
static long g_failures = 0;

static void expect_long(const char *name, long got, long want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-52s 实际 %ld，期望 %ld\n", name, got, want);
	}
}

static void expect_status(const char *name, contacts_status got,
                          contacts_status want) {
	expect_long(name, (long)got, (long)want);
}

static void make_name(char *buf, size_t size, long i) {
	(void)snprintf(buf, size, "user%05ld", i);
}

static void make_phone(char *buf, size_t size, long i) {
	(void)snprintf(buf, size, "138%08ld", i);
}

/*
 * 核对一本书的全部不变量。已插入的姓名是 0..want_count-1（配合 make_name）。
 *
 * 六项聚合检查，而不是对每个节点各断言一次：失败时输出的信息足够定位
 * （哪一项、期望多少、实际多少），而通过时不要刷几千行 "ok"。
 */
static void verify(const char *label, contact_book *book, long want_count) {
	contacts_index_stats st;
	const contact       *item;
	char                 name[CONTACT_NAME_MAX];
	char                 phone[CONTACT_PHONE_MAX];
	char                 buf[128];
	long                 traversed  = 0;
	long                 unfindable = 0;
	long                 wrong      = 0;

	for (item = contacts_first(book); item != NULL; item = contacts_next(item)) {
		traversed++;
	}

	(void)snprintf(buf, sizeof buf, "%s: count", label);
	expect_long(buf, (long)contacts_count(book), want_count);
	(void)snprintf(buf, sizeof buf, "%s: 遍历长度等于 count", label);
	expect_long(buf, traversed, want_count);

	if (contacts_index_get(book, &st) == CONTACTS_OK) {
		(void)snprintf(buf, sizeof buf, "%s: 索引 entries 等于 count", label);
		expect_long(buf, (long)st.entries, want_count);
	}

	for (long i = 0; i < want_count; i++) {
		make_name(name, sizeof name, i);
		make_phone(phone, sizeof phone, i);
		item = contacts_find(book, name);
		if (item == NULL) {
			unfindable++;
		} else if (strcmp(item->phone, phone) != 0) {
			wrong++;
		}
	}
	(void)snprintf(buf, sizeof buf, "%s: 已插入的姓名全部可查", label);
	expect_long(buf, unfindable, 0);
	(void)snprintf(buf, sizeof buf, "%s: 查到的电话全部正确", label);
	expect_long(buf, wrong, 0);

	/* 紧接着范围之后的名字必须落空 */
	make_name(name, sizeof name, want_count);
	(void)snprintf(buf, sizeof buf, "%s: 未插入的姓名不可查", label);
	expect_long(buf, (long)(contacts_find(book, name) == NULL), 1);
}

/* 构造 n 条，成功返回 0 */
static int build(contact_book *book, long n) {
	char name[CONTACT_NAME_MAX];
	char phone[CONTACT_PHONE_MAX];

	contacts_init(book);
	for (long i = 0; i < n; i++) {
		make_name(name, sizeof name, i);
		make_phone(phone, sizeof phone, i);
		if (contacts_add(book, name, phone) != CONTACTS_OK) {
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------ */

/* 空表：桶数组分配失败 */
static void test_first_bucket_alloc(void) {
	contact_book book;

	contacts_init(&book);

	inject_at(0);    /* 第一次分配就是桶数组的 calloc */
	expect_status("空表首次插入：桶数组分配失败返回 NOMEM",
	              contacts_add(&book, "alice", "1"), CONTACTS_ERR_NOMEM);
	inject_off();

	verify("桶数组分配失败后", &book, 0);

	/* 失败之后这本书必须还能用 */
	expect_status("关闭注入后重新插入成功",
	              contacts_add(&book, "alice", "1"), CONTACTS_OK);
	expect_long("重新插入后数量为 1", (long)contacts_count(&book), 1);

	contacts_destroy(&book);
}

/* 空表：节点分配失败（桶已建好） */
static void test_first_node_alloc(void) {
	contact_book book;

	contacts_init(&book);

	inject_at(1);    /* 第 0 次是桶数组，第 1 次是节点 */
	expect_status("空表首次插入：节点分配失败返回 NOMEM",
	              contacts_add(&book, "alice", "1"), CONTACTS_ERR_NOMEM);
	inject_off();

	verify("节点分配失败后", &book, 0);
	expect_long("节点分配失败后 find 返回 NULL",
	            (long)(contacts_find(&book, "alice") == NULL), 1);

	expect_status("关闭注入后重新插入成功",
	              contacts_add(&book, "alice", "1"), CONTACTS_OK);
	expect_long("重新插入后数量为 1", (long)contacts_count(&book), 1);

	contacts_destroy(&book);
}

/*
 * 扩容点上的两次分配，后果不同，必须分开验：
 *   第 0 次（扩容用桶数组）失败 -> 放弃扩容，仍用 16 个桶插入，返回成功
 *   节点分配失败               -> 整次插入失败，返回 NOMEM
 * 前者是"只降级不失败"，后者是"彻底失败"，都要求结构自洽。
 */
static void test_growth_alloc(void) {
	contact_book         book;
	contacts_index_stats st;
	char                 name[CONTACT_NAME_MAX];
	char                 phone[CONTACT_PHONE_MAX];

	if (build(&book, 16) != 0) {
		expect_long("构造 16 条", 0, 1);
		contacts_destroy(&book);
		return;
	}

	make_name(name, sizeof name, 16);
	make_phone(phone, sizeof phone, 16);

	/* 第 0 次分配（扩容用的桶数组）失败：插入仍应成功，桶数不涨 */
	inject_at(0);
	expect_status("扩容失败不否决插入",
	              contacts_add(&book, name, phone), CONTACTS_OK);
	inject_off();

	(void)contacts_index_get(&book, &st);
	expect_long("扩容失败后桶数仍为 16", (long)st.buckets, 16);
	expect_long("扩容失败后装载因子超过 1.0",
	            (long)(st.entries > st.buckets), 1);
	verify("扩容失败后", &book, 17);

	/* 再插一条：又一次尝试扩容，这次成功，桶数翻倍 */
	make_name(name, sizeof name, 17);
	make_phone(phone, sizeof phone, 17);
	expect_status("第二次尝试扩容成功",
	              contacts_add(&book, name, phone), CONTACTS_OK);
	(void)contacts_index_get(&book, &st);
	expect_long("扩容成功后桶数为 32", (long)st.buckets, 32);
	verify("扩容成功后", &book, 18);

	/* 18 条 / 32 桶，本次插入不扩容，第 0 次分配就是节点 */
	make_name(name, sizeof name, 18);
	make_phone(phone, sizeof phone, 18);
	inject_at(0);
	expect_status("节点分配失败返回 NOMEM",
	              contacts_add(&book, name, phone), CONTACTS_ERR_NOMEM);
	inject_off();
	verify("节点分配失败后", &book, 18);

	contacts_destroy(&book);
}

/*
 * 在几个扩容边界上逐个分配点扫一遍：无论第几次分配失败，
 * 数据结构都必须自洽。这是对 contacts_add / index_prepare 里分配顺序的
 * 回归——将来若有人在节点分配与入索引之间插入一次新的分配，
 * "失败时只做了半步"会立刻在这里暴露。
 *
 * 规模特意取 2 的幂：此时 count 恰好等于桶数，下一条插入必然触发扩容，
 * 于是这条插入路径上有两次分配（扩容的桶数组、节点），第 0 与第 1 个
 * 注入点都会真的失败。若规模随便取，插入只要一次分配，扫描就退化成
 * "只有第 0 个点有意义"，看起来扫了 4 个点，实际只覆盖了 1 个。
 */
static void test_alloc_sweep(void) {
	static const long sizes[] = {16, 128, 1024};
	char              name[CONTACT_NAME_MAX];
	long              points = 0;

	for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
		long n     = sizes[k];
		long fails = 0;
		char buf[96];

		for (long attempt = 0; attempt < 3; attempt++) {
			contact_book   book;
			contacts_status status;
			char           label[96];
			char           phone[CONTACT_PHONE_MAX];

			if (build(&book, n) != 0) {
				expect_long("扫描前构造数据", 0, 1);
				contacts_destroy(&book);
				return;
			}

			inject_at(attempt);
			make_name(name, sizeof name, n);
			make_phone(phone, sizeof phone, n);
			status = contacts_add(&book, name, phone);
			inject_off();
			points++;
			if (status != CONTACTS_OK) {
				fails++;
			}

			/*
			 * 该次插入可能成功也可能失败，取决于这个规模下它需要
			 * 几次分配；两种都合法。不能接受的是"加进去了但索引里
			 * 没有"——那会让 verify 的 entries 与 count 对不上。
			 */
			(void)snprintf(label, sizeof label, "扫描 n=%ld 分配点 %ld",
			               n, attempt);
			verify(label, &book, (status == CONTACTS_OK) ? n + 1 : n);

			contacts_destroy(&book);
		}

		/*
		 * 非空性检查：至少有一个注入点真的让插入失败。
		 * 没有这一条，扫描可能因为"根本没注入到"而全绿——
		 * 一个从来没生效过的测试比没有测试更危险。
		 */
		(void)snprintf(buf, sizeof buf, "n=%ld 的扩容路径确实被注入到", n);
		expect_long(buf, (long)(fails >= 1), 1);
	}

	(void)printf("      扫描了 %ld 个分配点\n", points);
}

/* 从尾部删掉 make_name 的 [to, from) 这一段，返回实际删除条数 */
static long remove_range(contact_book *book, long from, long to) {
	char name[CONTACT_NAME_MAX];
	long removed = 0;

	for (long i = from - 1; i >= to; i--) {
		make_name(name, sizeof name, i);
		if (contacts_remove(book, name) == CONTACTS_OK) {
			removed++;
		}
	}
	return removed;
}

/*
 * 收缩路径上的分配失败。
 *
 * 收缩与扩容共用 index_rehash，因此失败语义也应当一致：**只降级，不失败**。
 * 删除已经完成、count 已经减一，此时返回 NOMEM 等于让调用方以为"没删掉"，
 * 而实际上记录已经没了——这比桶表偏大严重得多。
 *
 * 因此这里断言的不是"返回了 OK"，而是"返回 OK 之后结构仍然自洽"：
 * verify 会核对 count、遍历长度、索引 entries 三者相等，且剩余姓名全部可查。
 */
static void test_shrink_alloc(void) {
	contact_book         book;
	contacts_index_stats st;
	char                 name[CONTACT_NAME_MAX];
	long                 base;

	if (build(&book, 1024) != 0) {
		expect_long("构造 1024 条", 0, 1);
		contacts_destroy(&book);
		return;
	}

	(void)remove_range(&book, 1024, 256);
	(void)contacts_index_get(&book, &st);
	expect_long("收缩点之前桶数 1024", (long)st.buckets, 1024);

	/* 第 769 次删除恰好让装载因子低于 1/4，收缩要用的桶数组分配失败 */
	base = (long)st.rehashes;
	make_name(name, sizeof name, 255);
	inject_at(0);
	expect_status("收缩的桶数组分配失败，删除仍然成功",
	              contacts_remove(&book, name), CONTACTS_OK);
	inject_off();

	(void)contacts_index_get(&book, &st);
	expect_long("收缩失败后桶数仍是 1024", (long)st.buckets, 1024);
	expect_long("收缩失败不计入重分配次数",
	           (long)(st.rehashes - (size_t)base), 0);
	verify("收缩失败后", &book, 255);

	/* 再删一条会重试收缩，这次分配成功 */
	base = (long)st.rehashes;
	make_name(name, sizeof name, 254);
	expect_status("下一次删除重试收缩", contacts_remove(&book, name), CONTACTS_OK);
	(void)contacts_index_get(&book, &st);
	expect_long("重试成功后桶数减半到 512", (long)st.buckets, 512);
	expect_long("重试成功计入一次重分配",
	            (long)(st.rehashes - (size_t)base), 1);
	verify("收缩成功后", &book, 254);

	contacts_destroy(&book);
}

/*
 * 收缩的积压：连续多次失败之后，一次删除要补做多档。
 *
 * 这条用例专门覆盖 index_shrink 里的 while 而不是 if。若那里写成 if，
 * 桶表停在 1024 而 count 已经降到 127，装载因子 0.12——功能仍然正确，
 * 但"收缩"这件事实际上没做成，内存白占。
 *
 * 这也是唯一能把"一次删除跨两档"打出来的路径：正常运行中每次删除只让
 * count 减一，装载因子不会一次跨过两个阈值。
 */
static void test_shrink_backlog(void) {
	contact_book         book;
	contacts_index_stats st;
	char                 name[CONTACT_NAME_MAX];
	long                 base;

	if (build(&book, 1024) != 0) {
		expect_long("构造 1024 条（积压用例）", 0, 1);
		contacts_destroy(&book);
		return;
	}

	/* 每次删除都让收缩失败：桶表停在 1024，count 一路降到 128 */
	for (long i = 1023; i >= 128; i--) {
		make_name(name, sizeof name, i);
		inject_at(0);
		(void)contacts_remove(&book, name);
		inject_off();
	}

	(void)contacts_index_get(&book, &st);
	expect_long("连续收缩失败后 count 为 128", (long)contacts_count(&book), 128);
	expect_long("连续收缩失败后桶数仍是 1024", (long)st.buckets, 1024);
	verify("连续收缩失败后", &book, 128);

	/* 分配恢复：127 < 1024/4 且 127 < 512/4，一次删除应当连减两档 */
	base = (long)st.rehashes;
	make_name(name, sizeof name, 127);
	expect_status("积压的收缩在分配恢复后补做",
	              contacts_remove(&book, name), CONTACTS_OK);
	(void)contacts_index_get(&book, &st);
	expect_long("一次删除连减两档到 256", (long)st.buckets, 256);
	expect_long("连减两档计入两次重分配",
	            (long)(st.rehashes - (size_t)base), 2);
	verify("积压收缩补做后", &book, 127);

	contacts_destroy(&book);
}

/*
 * 滞回：同一个名字在收缩阈值上反复增删，桶表不该反复重分配。
 *
 * 场景特意落在装载因子**正好 0.25**：count = 256、桶数 1024。这是收缩判据
 * 只差一条的位置：删到 255 收缩一次，桶数变 512，装载因子回到 0.5；
 * 之后无论怎么增删都停在 0.5 到 1.0 之间，不再重分配。
 *
 * 判据用 contacts_index_stats 的 rehashes 增量，而不是耗时：重分配次数是
 * 确定值，耗时受调度与机器频率影响。
 *
 * 这一组用例在把阈值改成 1/2 时会失败，但失败的不是"抖动"——
 * 实测 2000 轮"删一条加一条"在两种阈值下都只有 1 次重分配（扩容判据是
 * "插入后条数 > 桶数"，减半后 count <= nbuckets-1，要连插两条才会再扩容）。
 * 真正被抓的是"抖动后装载因子回到 0.5"这一条：取 1/2 时收缩后装载因子是
 * 1.0，表是满的。教训是别把教科书结论当成本实现的性质，先量再说。
 *
 * 为什么不用 "--wrap=calloc 数调用次数"：`-O2` 下 GCC 会把
 * `malloc(n)` 紧跟 `memset(p, 0, n)` 合并成 `calloc(1, n)`（两者可观察行为
 * 相同，变换合法），而 contacts_add 正好是"malloc 节点 + 全零"这种写法。
 * 于是节点分配也走了 calloc，计数里混进了每条记录的分配，实测每轮恒为 1，
 * 看不出桶表到底有没有被重新分配。计数器放进模块里才可靠。
 */
static void test_shrink_no_thrash(void) {
	contact_book         book;
	contacts_index_stats st;
	char                 name[CONTACT_NAME_MAX];
	char                 phone[CONTACT_PHONE_MAX];
	long                 base;
	long                 reallocs;

	if (build(&book, 1024) != 0) {
		expect_long("构造 1024 条（滞回用例）", 0, 1);
		contacts_destroy(&book);
		return;
	}

	/* 删到 256 条：装载因子正好 1/4，桶数仍是 1024 */
	(void)remove_range(&book, 1024, 256);
	(void)contacts_index_get(&book, &st);
	expect_long("抖动起点 count 为 256", (long)contacts_count(&book), 256);
	expect_long("抖动起点桶数为 1024", (long)st.buckets, 1024);

	/* 第一轮：删一条越过阈值，收缩一次；加回去之后不该再动 */
	base = (long)st.rehashes;
	for (long k = 0; k < 2000; k++) {
		make_name(name, sizeof name, 255);
		(void)contacts_remove(&book, name);

		make_name(name, sizeof name, 255);
		make_phone(phone, sizeof phone, 255);
		(void)contacts_add(&book, name, phone);
	}
	(void)contacts_index_get(&book, &st);
	reallocs = (long)st.rehashes - base;

	expect_long("2000 次跨阈值增删只重分配 1 次", reallocs, 1);
	expect_long("抖动后 count 仍为 256", (long)contacts_count(&book), 256);
	expect_long("抖动后桶数收缩到 512", (long)st.buckets, 512);
	expect_long("抖动后装载因子回到 0.5",
	           (long)(st.entries * 2u == st.buckets), 1);
	verify("抖动后", &book, 256);

	/* 第二轮：这时的状态已经在滞回带里面，一次都不该再重分配 */
	base = (long)st.rehashes;
	for (long k = 0; k < 1000; k++) {
		make_name(name, sizeof name, 255);
		(void)contacts_remove(&book, name);

		make_name(name, sizeof name, 255);
		make_phone(phone, sizeof phone, 255);
		(void)contacts_add(&book, name, phone);
	}
	(void)contacts_index_get(&book, &st);
	reallocs = (long)st.rehashes - base;

	expect_long("稳定后再抖 1000 次没有重分配", reallocs, 0);
	expect_long("稳定后桶数仍为 512", (long)st.buckets, 512);
	verify("稳定后", &book, 256);

	contacts_destroy(&book);
}

/*
 * load 中途内存不足：必须返回 NOMEM，而不是把剩下的行全部记成
 * "格式坏行"再返回 CONTACTS_OK——后者让调用方无法区分"文件里有坏行"
 * 与"进程快没内存了"，而两者的处置完全不同。
 */static void test_load_alloc(void) {
	char                 path[] = "bin/tmp_alloc_load.txt";
	char                 name[CONTACT_NAME_MAX];
	FILE                *fp;
	long                 aborted = 0;
	long                 full    = 0;

	fp = fopen(path, "w");
	if (fp == NULL) {
		expect_long("创建载入用的临时文件", 0, 1);
		return;
	}
	for (long i = 0; i < 200; i++) {
		char phone[CONTACT_PHONE_MAX];

		make_name(name, sizeof name, i);
		make_phone(phone, sizeof phone, i);
		(void)fprintf(fp, "name: %s, phone: %s\n", name, phone);
	}
	(void)fclose(fp);

	for (long k = 0; k < 40; k++) {
		contact_book         book;
		contacts_load_result result;
		contacts_status      status;

		contacts_init(&book);
		(void)memset(&result, 0x00, sizeof result);

		inject_at(k);
		status = contacts_load(&book, path, CONTACTS_LOAD_REPLACE, &result);
		inject_off();

		if (status == CONTACTS_ERR_NOMEM) {
			aborted++;
			/* 中止时要如实报告，不能把没读到的行算成 skipped */
			expect_long("NOMEM 时 skipped 为 0", (long)result.skipped, 0);
			expect_long("NOMEM 时 count 等于 loaded",
			            (long)contacts_count(&book), (long)result.loaded);
			verify("载入被内存不足中止后", &book, (long)result.loaded);
		} else {
			full++;
			expect_status("未触发 NOMEM 时载入成功", status, CONTACTS_OK);
			expect_long("完整载入 200 条", (long)contacts_count(&book), 200);
		}

		contacts_destroy(&book);
	}

	expect_long("存在触发 NOMEM 中止的分配点", (long)(aborted > 0), 1);
	expect_long("也存在不触发失败的分配点", (long)(full > 0), 1);
	(void)remove(path);
}

/* ------------------------------------------------------------------ */

int main(void) {
	(void)printf("contacts 分配失败注入测试\n\n");

	test_first_bucket_alloc();
	test_first_node_alloc();
	test_growth_alloc();
	test_alloc_sweep();
	test_shrink_alloc();
	test_shrink_backlog();
	test_shrink_no_thrash();
	test_load_alloc();

	(void)printf("\n%ld 项检查，%ld 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
