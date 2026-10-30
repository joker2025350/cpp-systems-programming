/*
 * bench_contacts.c —— 量一下"链表 + 哈希索引"的代价
 *
 * 测五件事：
 *
 *   build  每加一条都要查重；有索引时是 O(1) 均摊，因此整批是 O(n)
 *   find   按姓名定位桶链，链长在装载因子 <= 1.0 时平均 1 条多一点
 *   order  按姓名排序：取 n 个指针 + qsort，O(n log n) 次比较
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

/*
 * 排序：取 n 个指针再 qsort。
 *
 * 计时包含调用方的 malloc/free，因为调用方实际付出的就是从"一本书"
 * 得到"有序结果"的总代价，而分配数组本来就是这一步的一部分——
 * 只计 contacts_order 里的时间，会漏掉 n*8 字节分配的实际开销。
 *
 * 结果顺手校验一次严格递增：基准输出常被当数据读，排错的表比没有表更糟。
 */
static double bench_order(contact_book *book, long n) {
	const contact **buf;
	double          t0;
	int             ok = 1;

	buf = (const contact **)malloc((size_t)n * sizeof *buf);
	if (buf == NULL) {
		(void)fprintf(stderr, "排序基准：分配 %ld 个指针失败\n", n);
		return -1.0;
	}

	t0 = now_ms();
	if (contacts_order(book, buf, (size_t)n) != CONTACTS_OK) {
		(void)fprintf(stderr, "排序基准：contacts_order 失败\n");
		free(buf);
		return -1.0;
	}
	t0 = now_ms() - t0;

	for (long i = 0; i + 1 < n; i++) {
		if (strcmp(buf[i]->name, buf[i + 1]->name) >= 0) {
			ok = 0;
		}
	}
	if (!ok) {
		(void)fprintf(stderr, "排序基准：结果不是严格递增\n");
	}

	free(buf);
	return t0;
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
	double               order;
	double               save;
	double               load;
	contacts_index_stats stats;
	size_t               heap;      /* 建完这本书之后堆上占用的字节数 */
} bench_row;

/*
 * 收缩的代价与收益。
 *
 * 与上面四列回答的问题不同：上面问"操作多快"，这里问"删掉大部分记录之后，
 * 内存有没有还回去"。三个数字：桶数、堆占用、删除耗时。
 *
 * 抖动那一段专门跑"删一条、加一条"十万次，落点选在装载因子 0.5——
 * 也就是收缩阈值若取 1/2 就会反复收缩膨胀的那个位置。桶表重分配的**次数**
 * 取自 contacts_index_stats.rehashes，是确定值；耗时只作参考。
 */
static void bench_shrink(long n) {
	contact_book         book;
	contacts_index_stats before;
	contacts_index_stats mid;
	contacts_index_stats final;
	size_t               heap_before;
	size_t               heap_after;
	double               t_remove;
	double               t_churn;
	size_t               rehash_base;
	const long           keep = 1024;
	char                 name[CONTACT_NAME_MAX];

	contacts_init(&book);
	for (long i = 0; i < n; i++) {
		make_name(name, sizeof name, i);
		if (contacts_add(&book, name, "13800000000") != CONTACTS_OK) {
			(void)fprintf(stderr, "收缩基准：构建失败于 i=%ld\n", i);
			contacts_destroy(&book);
			return;
		}
	}
	(void)contacts_index_get(&book, &before);
	heap_before = heap_used();

	t_remove = now_ms();
	for (long i = n - 1; i >= keep; i--) {
		make_name(name, sizeof name, i);
		if (contacts_remove(&book, name) != CONTACTS_OK) {
			(void)fprintf(stderr, "收缩基准：删除失败于 i=%ld\n", i);
			contacts_destroy(&book);
			return;
		}
	}
	t_remove = now_ms() - t_remove;

	(void)contacts_index_get(&book, &mid);
	heap_after = heap_used();

	/* 抖动：删掉一条再加回来，十万次，count 与桶数都应当纹丝不动 */
	rehash_base = mid.rehashes;
	t_churn = now_ms();
	for (long k = 0; k < 100000; k++) {
		make_name(name, sizeof name, 0);
		if (contacts_remove(&book, name) != CONTACTS_OK) {
			(void)fprintf(stderr, "收缩基准：抖动删除失败\n");
			break;
		}
		if (contacts_add(&book, name, "13800000000") != CONTACTS_OK) {
			(void)fprintf(stderr, "收缩基准：抖动插入失败\n");
			break;
		}
	}
	t_churn = now_ms() - t_churn;
	(void)contacts_index_get(&book, &final);
	rehash_base = final.rehashes - rehash_base;

	(void)printf("\n收缩（n = %ld，删到 %ld 条，再抖动 %d 轮）\n\n", n, keep, 100000);
	(void)printf("  桶数         建完 %lu -> 删完 %lu -> 抖动后 %lu\n",
	             (unsigned long)before.buckets, (unsigned long)mid.buckets,
	             (unsigned long)final.buckets);
	(void)printf("  装载因子     %.2f -> %.2f -> %.2f\n",
	             (before.buckets > 0)
	                 ? (double)before.entries / (double)before.buckets : 0.0,
	             (mid.buckets > 0)
	                 ? (double)mid.entries / (double)mid.buckets : 0.0,
	             (final.buckets > 0)
	                 ? (double)final.entries / (double)final.buckets : 0.0);
	(void)printf("  堆占用       %zu KiB -> %zu KiB\n",
	             heap_before / 1024u, heap_after / 1024u);
	(void)printf("  删除耗时     %ld 次 contacts_remove 共 %.1f ms（单次 %.2f µs）\n",
	             n - keep, t_remove, t_remove * 1000.0 / (double)(n - keep));
	(void)printf("  抖动         %d 次删一条加一条共 %.1f ms，桶表重分配 %zu 次（本轮）\n",
	             100000, t_churn, rehash_base);

	contacts_destroy(&book);
}

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
		r->order = bench_order(&book, r->n);
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
	(void)printf("%8s %12s %12s %12s %12s %12s %10s\n",
	             "n", "build(ms)", "find(ms)", "order(ms)", "save(ms)", "load(ms)",
	             "loaded");
	(void)printf("-----------------------------------------------------------------------------\n");
	for (size_t k = 0; k < n_sizes; k++) {
		const bench_row *r = &rows[k];

		(void)printf("%8ld %12.1f %12.1f %12.1f %12.1f %12.1f %10ld\n",
		             r->n, r->build, r->find, r->order, r->save, r->load,
		             r->loaded);
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

	bench_shrink(sizes[n_sizes - 1]);

	(void)printf("\nfind 一列做的是 2n 次查找（n 次命中 + n 次落空）。\n");
	(void)printf("find(ns) 是单次查找平均耗时，包含命中的一次 strcmp。\n");
	(void)printf("order 是 contacts_order 取 n 个指针并 qsort 的总耗时，"
	             "含调用方的 malloc/free。\n");
	(void)printf("load 是 entries/buckets 的装载因子；超过 1.0 说明扩容分配失败，处于降级状态。\n");
	(void)printf("heap 是 glibc mallinfo2 的 uordblks，即已分配未释放的堆字节数；"
	             "非 glibc 平台为 0。\n");
	return 0;
}
