/*
 * bench_contacts.c —— 量一下链表方案的代价
 *
 * 只测三件事，因为这三件事的复杂度不一样：
 *
 *   add    每次都要全表查重，所以批量构建是 O(n²)
 *   find   线性扫描，命中位置平均在中间，O(n)
 *   save   顺序遍历，O(n)；load 又退回 O(n²)，因为每条都要查重
 *
 * 这张表的作用不是"证明快"，而是把"什么时候该换数据结构"说清楚：
 * n 到几千之后，载入和批量添加的耗时开始按平方增长，
 * 而查找本身始终是线性扫描。
 *
 * 用法：make bench
 */

#define _POSIX_C_SOURCE 200809L

#include "contacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_FILE "bin/bench_contacts.txt"

static double now_ms(void) {
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void make_name(char *buf, size_t size, long i) {
	(void)snprintf(buf, size, "user%06ld", i);
}

/* 构建 n 条，返回耗时（毫秒） */
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

/* 每组 n 次查找，一半命中、一半落空，返回耗时 */
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
		/* 落空：这个前缀不可能存在 */
		make_name(name, sizeof name, n + i);
		if (contacts_find(book, name) != NULL) {
			(void)fprintf(stderr, "不该命中\n");
		}
	}
	(void)hits;
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

int main(void) {
	static const long sizes[] = {1000, 4000, 16000, 64000};
	const size_t      n_sizes = sizeof sizes / sizeof sizes[0];

	(void)printf("contacts 基准\n\n");
	(void)printf("%8s %12s %12s %12s %12s %12s\n",
	             "n", "build(ms)", "find(ms)", "save(ms)", "load(ms)", "loaded");
	(void)printf("--------------------------------------------------------------------------\n");

	for (size_t k = 0; k < n_sizes; k++) {
		long         n = sizes[k];
		contact_book book;
		double       t_build;
		double       t_find;
		double       t_save;
		double       t_load;
		long         loaded = 0;

		t_build = bench_build(&book, n);
		if (t_build < 0) {
			contacts_destroy(&book);
			return 1;
		}

		t_find = bench_find(&book, n);
		t_save = bench_save(&book);
		t_load = bench_load(&loaded);

		(void)printf("%8ld %12.1f %12.1f %12.1f %12.1f %12ld\n",
		             n, t_build, t_find, t_save, t_load, loaded);

		contacts_destroy(&book);
	}

	(void)remove(BENCH_FILE);
	(void)printf("\nfind 一列做的是 2n 次查找（n 次命中 + n 次落空）。\n");
	(void)printf("build 与 load 都是 O(n^2)：每加一条都要全表查重。\n");
	return 0;
}
