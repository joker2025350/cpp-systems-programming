/*
 * bench_contacts.c —— 量一下"链表 + 哈希索引"的代价
 *
 * 测四件事：
 *
 *   build  每加一条都要查重；有索引时是 O(1) 均摊，因此整批是 O(n)
 *   find   按姓名定位桶链，链长在装载因子 <= 1.0 时平均 1 条多一点
 *   save   顺序遍历链表，O(n)；与索引无关，因为顺序保存在链表里
 *   load   逐行解析 + 插入，O(n)
 *
 * 第二张表给出索引自身的形态：桶数、装载因子、最长链、单次查找耗时。
 * 这四项是解释第一张表的依据——只看"变快了"，无法判断快是因为索引，
 * 还是因为换了机器。
 *
 * 用法：make bench
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE     /* mallinfo2：只在 glibc 下用来量堆内存，见下 */

#include "contacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#define BENCH_FILE "bin/bench_contacts.txt"
#define MAX_SIZES  8

/*
 * 当前进程从堆上分配出去的字节数。
 *
 * 用 mallinfo2 而不是 getrusage 的 maxrss：maxrss 是进程峰值、含代码段与
 * 栈，而且只增不减，同一进程里连续测四个规模时后面的数字会被前面的峰值
 * 抬高。uordblks 是"用户已分配、未释放"的字节数，正好对应"这本书占了
 * 多少堆"。它是 glibc 扩展，其他 libc 下返回 0，由调用方标成 n/a。
 */
static size_t heap_used(void) {
#if defined(__GLIBC__)
	struct mallinfo2 mi = mallinfo2();

	return mi.uordblks;
#else
	return 0;
#endif
}

static double now_ms(void) {
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void make_name(char *buf, size_t size, long i) {
	(void)snprintf(buf, size, "user%06ld", i);
}

/* 构建 n 条，返回耗时（毫秒）；失败返回负数 */
static double bench_build(contact_book *book, long n) {
	char   name[CONTACT_NAME_MAX];
	double t0;

	contacts_init(book);
	t0 = now_ms();
	for (long i = 0; i < n; i++) {
		make_name(name, sizeof name, i);
		if (contacts_add(book, name, "13800000000") != CONTACTS_OK) {
			(void)fprintf(stderr, "构建失败于 i=%ld\n", i);
			return -1.0;
		}
	}
	return now_ms() - t0;
}

/* 每组 n 次查找，一半命中、一半落空，返回耗时（毫秒） */
static double bench_find(contact_book *book, long n) {
	char   name[CONTACT_NAME_MAX];
	long   hits = 0;
	double t0;

	t0 = now_ms();
	for (long i = 0; i < n; i++) {
		/* 命中：取表中间的一个名字 */
		make_name(name, sizeof name, n / 2 + (i % 2));
		if (contacts_find(book, name) != NULL) {
			hits++;
		}
		/* 落空：这个序号不在表里（n 条数据只用 0..n-1） */
		make_name(name, sizeof name, n + i);
		if (contacts_find(book, name) != NULL) {
			(void)fprintf(stderr, "不该命中\n");
		}
	}
	if (hits != n) {
		(void)fprintf(stderr, "命中数 %ld，期望 %ld\n", hits, n);
	}
	return now_ms() - t0;
}

static double bench_save(contact_book *book) {
	double t0 = now_ms();

	if (contacts_save(book, BENCH_FILE) != CONTACTS_OK) {
		(void)fprintf(stderr, "保存失败\n");
		return -1.0;
	}
	return now_ms() - t0;
}

static double bench_load(long *count) {
	contact_book         dst;
	contacts_load_result result = {0, 0, 0};
	double               t0;

	contacts_init(&dst);
	t0 = now_ms();
	if (contacts_load(&dst, BENCH_FILE, CONTACTS_LOAD_REPLACE, &result) != CONTACTS_OK) {
		(void)fprintf(stderr, "载入失败\n");
		contacts_destroy(&dst);
		return -1.0;
	}
	t0 = now_ms() - t0;

	*count = (long)result.loaded;
	contacts_destroy(&dst);
	return t0;
}

typedef struct {
	long                 n;
	long                 loaded;
	double               build;
	double               find;
	double               save;
	double               load;
	contacts_index_stats stats;
	size_t               heap;      /* 建完这本书之后堆上占用的字节数 */
} bench_row;

int main(void) {
	static const long sizes[] = {1000, 4000, 16000, 64000};
	const size_t      n_sizes = sizeof sizes / sizeof sizes[0];
	bench_row         rows[MAX_SIZES];

	if (n_sizes > MAX_SIZES) {
		(void)fprintf(stderr, "规模数量超过 MAX_SIZES\n");
		return 1;
	}

	for (size_t k = 0; k < n_sizes; k++) {
		contact_book book;
		bench_row   *r = &rows[k];

		r->n     = sizes[k];
		r->build = bench_build(&book, r->n);
		if (r->build < 0) {
			contacts_destroy(&book);
			return 1;
		}
		r->find = bench_find(&book, r->n);
		r->save = bench_save(&book);
		r->load = bench_load(&r->loaded);

		/* 统计必须在 destroy 之前取，destroy 会释放桶数组 */
		if (contacts_index_get(&book, &r->stats) != CONTACTS_OK) {
			(void)fprintf(stderr, "取索引统计失败\n");
			contacts_destroy(&book);
			return 1;
		}
		r->heap = heap_used();
		contacts_destroy(&book);
	}

	(void)printf("contacts %s 基准\n\n", CONTACTS_VERSION_STRING);
	(void)printf("%8s %12s %12s %12s %12s %10s\n",
	             "n", "build(ms)", "find(ms)", "save(ms)", "load(ms)", "loaded");
	(void)printf("--------------------------------------------------------------------\n");
	for (size_t k = 0; k < n_sizes; k++) {
		const bench_row *r = &rows[k];

		(void)printf("%8ld %12.1f %12.1f %12.1f %12.1f %10ld\n",
		             r->n, r->build, r->find, r->save, r->load, r->loaded);
	}

	(void)printf("\n%8s %9s %8s %9s %14s %9s %11s %8s\n",
	             "n", "buckets", "load", "maxchain", "used/buckets", "find(ns)",
	             "heap(KiB)", "B/entry");
	(void)printf("--------------------------------------------------------------------------------------\n");
	for (size_t k = 0; k < n_sizes; k++) {
		const bench_row *r = &rows[k];
		double           load = 0.0;
		double           used = 0.0;
		double           ns   = 0.0;
		double           per  = 0.0;

		if (r->stats.buckets > 0) {
			load = (double)r->stats.entries / (double)r->stats.buckets;
			used = 100.0 * (double)r->stats.used / (double)r->stats.buckets;
		}
		if (r->find > 0.0) {
			ns = r->find * 1e6 / (2.0 * (double)r->n);
		}
		if (r->heap > 0 && r->n > 0) {
			per = (double)r->heap / (double)r->n;
		}

		(void)printf("%8ld %9lu %8.2f %9lu %13.0f%% %9.0f %11.0f %8.0f\n",
		             r->n, (unsigned long)r->stats.buckets, load,
		             (unsigned long)r->stats.max_chain, used, ns,
		             (double)r->heap / 1024.0, per);
	}

	(void)remove(BENCH_FILE);
	(void)printf("\nfind 一列做的是 2n 次查找（n 次命中 + n 次落空）。\n");
	(void)printf("find(ns) 是单次查找平均耗时，包含命中的一次 strcmp。\n");
	(void)printf("load 是 entries/buckets 的装载因子；超过 1.0 说明扩容分配失败，处于降级状态。\n");
	(void)printf("heap 是 glibc mallinfo2 的 uordblks，即已分配未释放的堆字节数；"
	             "非 glibc 平台为 0。\n");
	return 0;
}
