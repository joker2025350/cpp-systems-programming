/*
 * test_counter.c —— counter 模块的单元测试
 *
 * 重点是三件事：
 *   1. 单线程下五种实现的结果都必须正确（这是基线）
 *   2. 多线程下四种同步实现的结果必须精确等于 线程数 × 每线程次数
 *   3. raw 在同样条件下会丢更新——这里只记录、不断言，
 *      因为它按定义是数据竞争，结果没有任何保证
 *
 * 编译：make test
 */

#include "counter.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks   = 0;
static int g_failures = 0;

/* ---------------------------------------------------------------- */

static void expect_int(const char *name, long long got, long long want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-46s 实际 %lld，期望 %lld\n", name, got, want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

static void expect_true(const char *name, int cond) {
	expect_int(name, cond ? 1 : 0, 1);
}

static void expect_status(const char *name, counter_status got, counter_status want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-46s 实际 %d，期望 %d\n", name, (int)got, (int)want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

/* 报告一个值，不判定对错（用于 raw 的结果） */
static void report_value(const char *name, long long got, long long want) {
	(void)printf("info  %-46s 实际 %lld（期望 %lld，差 %lld）\n",
	             name, got, want, want - got);
}

/* ---------------------------------------------------------------- */

typedef struct {
	counter  *c;
	long long iterations;
} worker_arg;

static void *worker(void *arg) {
	worker_arg *a = (worker_arg *)arg;

	for (long long i = 0; i < a->iterations; i++) {
		(void)counter_add(a->c, 1);
	}
	return NULL;
}

/* 起 threads 个线程各做 per_thread 次自增，返回最终值 */
static long long hammer(counter *c, int threads, long long per_thread) {
	pthread_t  *tids;
	worker_arg *args;
	long long   final_value;

	tids = (pthread_t *)calloc((size_t)threads, sizeof *tids);
	args = (worker_arg *)calloc((size_t)threads, sizeof *args);
	if (tids == NULL || args == NULL) {
		free(tids);
		free(args);
		return -1;
	}

	for (int i = 0; i < threads; i++) {
		args[i].c          = c;
		args[i].iterations = per_thread;
		if (pthread_create(&tids[i], NULL, worker, &args[i]) != 0) {
			/* 创建失败时把已经起来的线程收掉，避免留下半截状态 */
			for (int j = 0; j < i; j++) {
				(void)pthread_join(tids[j], NULL);
			}
			free(tids);
			free(args);
			return -1;
		}
	}

	for (int i = 0; i < threads; i++) {
		(void)pthread_join(tids[i], NULL);
	}

	final_value = counter_get(c);
	free(tids);
	free(args);
	return final_value;
}

/* ---------------------------------------------------------------- */

static void test_create_and_destroy(void) {
	for (int k = 0; k < COUNTER_KIND_COUNT; k++) {
		counter_kind kind = (counter_kind)k;
		counter     *c    = NULL;
		char         label[64];

		if (!counter_kind_supported(kind)) {
			(void)snprintf(label, sizeof label, "%s 在此平台不可用",
			               counter_kind_name(kind));
			expect_status(label, counter_create(kind, 0, &c),
			              COUNTER_ERR_UNSUPPORTED);
			expect_true("不可用时 out 被置为 NULL", c == NULL);
			continue;
		}

		(void)snprintf(label, sizeof label, "创建 %s", counter_kind_name(kind));
		expect_status(label, counter_create(kind, 0, &c), COUNTER_OK);
		expect_true("创建后指针非空", c != NULL);
		expect_int("初值为 0", counter_get(c), 0);

		counter_destroy(c);
	}

	/* NULL 与非法参数不能崩 */
	counter_destroy(NULL);
	expect_true("counter_destroy(NULL) 不崩溃", 1);

	{
		counter *c = NULL;

		expect_status("out 为 NULL", counter_create(COUNTER_KIND_MUTEX, 0, NULL),
		              COUNTER_ERR_PARAM);
		expect_status("非法 kind", counter_create((counter_kind)99, 0, &c),
		              COUNTER_ERR_UNSUPPORTED);
		expect_true("失败后 out 仍为 NULL", c == NULL);
	}
}

static void test_initial_value(void) {
	for (int k = 0; k < COUNTER_KIND_COUNT; k++) {
		counter_kind kind = (counter_kind)k;
		counter     *c    = NULL;
		char         label[64];

		if (!counter_kind_supported(kind)) {
			continue;
		}

		(void)snprintf(label, sizeof label, "%s 支持非零初值",
		               counter_kind_name(kind));
		expect_status("创建带初值的计数器",
		              counter_create(kind, 12345, &c), COUNTER_OK);
		expect_int(label, counter_get(c), 12345);
		counter_destroy(c);
	}
}

/* 单线程：五种实现都必须给出正确结果，这是基线 */
static void test_single_thread(void) {
	for (int k = 0; k < COUNTER_KIND_COUNT; k++) {
		counter_kind kind = (counter_kind)k;
		counter     *c    = NULL;
		char         label[64];

		if (!counter_kind_supported(kind)) {
			continue;
		}

		(void)counter_create(kind, 0, &c);
		if (c == NULL) {
			continue;
		}

		for (int i = 0; i < 1000; i++) {
			(void)counter_add(c, 1);
		}
		(void)snprintf(label, sizeof label, "%s 单线程累加 1000",
		               counter_kind_name(kind));
		expect_int(label, counter_get(c), 1000);

		/* counter_add 的返回值应当是加之后的新值 */
		(void)snprintf(label, sizeof label, "%s add 返回新值",
		               counter_kind_name(kind));
		expect_int(label, counter_add(c, 5), 1005);

		/* 负增量也要支持 */
		(void)snprintf(label, sizeof label, "%s 支持负增量",
		               counter_kind_name(kind));
		expect_int(label, counter_add(c, -6), 999);

		counter_destroy(c);
	}
}

/*
 * 多线程：同步实现必须一条不丢。
 * 每次跑 8 线程 × 50000 次，期望 400000。
 */
static void test_multi_thread_exact(void) {
	const int       threads    = 8;
	const long long per_thread = 50000;
	const long long expected   = (long long)threads * per_thread;

	for (int k = 0; k < COUNTER_KIND_COUNT; k++) {
		counter_kind kind = (counter_kind)k;
		counter     *c    = NULL;
		long long    got;
		char         label[64];

		if (!counter_kind_supported(kind) || !counter_kind_is_atomic(kind)) {
			continue;
		}

		(void)counter_create(kind, 0, &c);
		if (c == NULL) {
			continue;
		}

		got = hammer(c, threads, per_thread);

		(void)snprintf(label, sizeof label,
		               "%s 8 线程 × 5 万次不丢", counter_kind_name(kind));
		expect_int(label, got, expected);

		/* 跑完之后读数必须与最后一次写入一致 */
		(void)snprintf(label, sizeof label, "%s 结束后的读数一致",
		               counter_kind_name(kind));
		expect_int(label, counter_get(c), expected);

		counter_destroy(c);
	}
}

/*
 * raw：不断言结果，只把差值打出来。
 *
 * 为什么不断言"必然小于期望值"：raw 是数据竞争，按 C11 5.1.2.4
 * 属于未定义行为，理论上结果可以是任何东西。要求它"一定丢更新"
 * 本身就是一条没有标准依据的断言。这个用例的作用是留下观测记录，
 * 不是判定对错。
 */
static void test_raw_demonstrates_loss(void) {
	const int       threads    = 8;
	const long long per_thread = 200000;
	const long long expected   = (long long)threads * per_thread;
	counter        *c          = NULL;
	long long       got;

	(void)counter_create(COUNTER_KIND_RAW, 0, &c);
	if (c == NULL) {
		return;
	}

	got = hammer(c, threads, per_thread);

	report_value("raw 8 线程 × 20 万次（会丢更新）", got, expected);

	/* 能断言的只有：结果落在 [0, expected] 这个合理区间内。
	 * 严格说这也不由标准保证，只是实践经验。 */
	g_checks++;
	if (got < 0 || got > expected) {
		g_failures++;
		(void)printf("FAIL  %-46s 结果 %lld 落在 [0, %lld] 之外\n",
		             "raw 结果在合理区间内", got, expected);
	} else {
		(void)printf("ok    %s\n", "raw 结果在合理区间内");
	}

	counter_destroy(c);
}

static void test_kind_helpers(void) {
	expect_true("raw 不是原子的", counter_kind_is_atomic(COUNTER_KIND_RAW) == 0);
	expect_true("mutex 是原子的", counter_kind_is_atomic(COUNTER_KIND_MUTEX) != 0);
	expect_true("spin 是原子的", counter_kind_is_atomic(COUNTER_KIND_SPIN) != 0);
	expect_true("atomic 是原子的", counter_kind_is_atomic(COUNTER_KIND_ATOMIC) != 0);
	expect_true("asm 是原子的", counter_kind_is_atomic(COUNTER_KIND_ASM) != 0);

	expect_true("kind_name(raw) == raw",
	            strcmp(counter_kind_name(COUNTER_KIND_RAW), "raw") == 0);
	expect_true("kind_name(atomic) == atomic",
	            strcmp(counter_kind_name(COUNTER_KIND_ATOMIC), "atomic") == 0);
	expect_true("未知 kind 有名字",
	            counter_kind_name((counter_kind)99) != NULL);

	expect_true("raw 总是可用", counter_kind_supported(COUNTER_KIND_RAW) != 0);
	expect_true("未知 kind 不可用", counter_kind_supported((counter_kind)99) == 0);
}

static void test_status_string(void) {
	const char *s;

	for (int i = 0; i <= 3; i++) {
		g_checks++;
		s = counter_status_string((counter_status)i);
		if (s == NULL || s[0] == '\0') {
			g_failures++;
			(void)printf("FAIL  %-46s 返回空描述\n", "状态码描述非空");
		}
	}
	(void)printf("ok    %s\n", "状态码描述非空");

	g_checks++;
	s = counter_status_string((counter_status)999);
	if (s == NULL || strcmp(s, "unknown status") != 0) {
		g_failures++;
		(void)printf("FAIL  %-46s 期望 \"unknown status\"\n", "未知状态码");
	} else {
		(void)printf("ok    %s\n", "未知状态码");
	}
}

/* ---------------------------------------------------------------- */

int main(void) {
	(void)printf("counter %s 单元测试\n\n", COUNTER_VERSION_STRING);

	test_create_and_destroy();
	test_initial_value();
	test_single_thread();
	test_multi_thread_exact();

	/*
	 * raw 的用例在 TSan 下必然报 data race——这正是它存在的意义。
	 * 但 make tsan 的目的是"验证四种同步实现确实没有竞争"，
	 * 所以 TSan 构建时把它关掉；想亲眼看竞争报告，
	 * 用不定义 COUNTER_TEST_SKIP_RAW 的构建跑一次即可。
	 */
#ifndef COUNTER_TEST_SKIP_RAW
	test_raw_demonstrates_loss();
#endif

	test_kind_helpers();
	test_status_string();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
