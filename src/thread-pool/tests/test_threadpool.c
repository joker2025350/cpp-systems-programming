/*
 * test_threadpool.c —— threadpool 模块的单元测试
 *
 * 覆盖六类行为：
 *   1. 参数校验与生命周期（含重复 create/destroy）
 *   2. 任务语义：每个提交的任务恰好执行一次，参数正确送达
 *   3. 队列语义：FIFO 顺序
 *   4. 并发语义：多生产者同时提交，计数不丢
 *   5. 同步语义：wait 的正确性、幂等性，以及工作线程调用 wait 被拒绝
 *   6. 停机语义：destroy 优雅停机（排空队列而不是丢弃）
 *
 * 计数一律用 __atomic 内建函数。这些调用会被 ThreadSanitizer 识别为原子访问，
 * 因此测试本身在 TSan 下必须是干净的 —— 如果测试自己有数据竞争，
 * sanitizer 的报告就无法定位到被测代码。
 *
 * 编译：make test
 */

#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_checks   = 0;
static int g_failures = 0;

/* ---------------------------------------------------------------- */

static void expect_int(const char *name, long long got, long long want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-52s 实际 %lld，期望 %lld\n", name, got, want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

static void expect_true(const char *name, int cond) {
	expect_int(name, cond ? 1 : 0, 1);
}

static void expect_status(const char *name, tpool_status got, tpool_status want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-52s 实际 %s，期望 %s\n",
		             name,
		             threadpool_status_string(got),
		             threadpool_status_string(want));
		return;
	}
	(void)printf("ok    %s\n", name);
}

/* 单调时钟，毫秒。CLOCK_MONOTONIC 不受系统时间调整影响，适合测间隔 */
static double now_ms(void) {
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void sleep_ms(long ms) {
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

/* 原子自增，返回自增后的值 */
static long atomic_inc(long *p) {
	return __atomic_add_fetch(p, 1, __ATOMIC_RELAXED);
}

static long atomic_load(const long *p) {
	return __atomic_load_n(p, __ATOMIC_RELAXED);
}

/* ---------------------------------------------------------------- */
/* 任务函数                                                          */
/* ---------------------------------------------------------------- */

typedef struct {
	int  index;
	long hit;      /* 被执行的次数 */
	int  result;   /* 任务写入，主线程校验 */
} slot;

/* 把命中计数、计算结果写入自己的 slot。每个任务独占一个 slot，不共享写。 */
static void task_mark(void *arg) {
	slot *s = (slot *)arg;

	s->result = s->index * 2;
	(void)atomic_inc(&s->hit);
}

/* 空任务：只为了测量调度开销 */
static void task_nop(void *arg) {
	(void)arg;
}

/* 记录执行顺序 */
typedef struct {
	int *seq;
	int  capacity;
	int  count;
} order_box;

static void task_record(void *arg) {
	order_box *b = (order_box *)arg;
	int        i;

	/* 单工作线程 + wait 之后才读取，因此这里不需要额外同步 */
	i = b->count;
	if (i < b->capacity) {
		b->seq[i] = i;
	}
	b->count = i + 1;
}

/* 慢任务：让 destroy 有队列可排 */
static void task_slow_1ms(void *arg) {
	long *n = (long *)arg;

	sleep_ms(1);
	(void)atomic_inc(n);
}

/* 嵌套提交：任务内部再次提交 */
typedef struct {
	threadpool *pool;
	long        done;
} nest_box;

static void task_nested_child(void *arg) {
	nest_box *b = (nest_box *)arg;

	(void)atomic_inc(&b->done);
}

static tpool_status g_nested_submit_status = TPOOL_ERR_PARAM;

static void task_nested_parent(void *arg) {
	nest_box *b = (nest_box *)arg;

	g_nested_submit_status = threadpool_submit(b->pool, task_nested_child, b);
	(void)atomic_inc(&b->done);
}

/* 在工作线程内部调用 wait，必须被拒绝而不是永久阻塞 */
typedef struct {
	threadpool *pool;
	tpool_status status;
} waitprobe;

static void task_call_wait(void *arg) {
	waitprobe *w = (waitprobe *)arg;

	w->status = threadpool_wait(w->pool);
}

/* 多生产者：每个生产者提交自己那一段 slot */
typedef struct {
	threadpool *pool;
	slot       *slots;
	int         begin;
	int         count;
} producer_arg;

static long g_producer_failed_submits = 0;

static void *producer_main(void *arg) {
	producer_arg *a = (producer_arg *)arg;

	for (int i = 0; i < a->count; i++) {
		slot *s = &a->slots[a->begin + i];
		if (threadpool_submit(a->pool, task_mark, s) != TPOOL_OK) {
			(void)atomic_inc(&g_producer_failed_submits);
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------- */
/* 1. 参数校验与生命周期                                              */
/* ---------------------------------------------------------------- */

static void test_param_checks(void) {
	threadpool  *p  = NULL;
	tpool_status st = TPOOL_OK;

	printf("\n--- 参数校验 ---\n");
	expect_status("create(out=NULL) 被拒绝", threadpool_create(4, NULL), TPOOL_ERR_PARAM);
	expect_status("create(0) 被拒绝", threadpool_create(0, &p), TPOOL_ERR_PARAM);
	expect_true("create(0) 失败后句柄为 NULL", p == NULL);
	expect_status("create(-3) 被拒绝", threadpool_create(-3, &p), TPOOL_ERR_PARAM);
	expect_true("create(-3) 失败后句柄仍为 NULL", p == NULL);

	st = threadpool_create(2, &p);
	expect_status("create(2) 成功", st, TPOOL_OK);
	expect_true("句柄非 NULL", p != NULL);

	expect_status("submit(pool=NULL)", threadpool_submit(NULL, task_nop, NULL), TPOOL_ERR_PARAM);
	expect_status("submit(fn=NULL)", threadpool_submit(p, NULL, NULL), TPOOL_ERR_PARAM);
	expect_status("wait(pool=NULL)", threadpool_wait(NULL), TPOOL_ERR_PARAM);
	expect_int("queued(pool=NULL) 返回 0", (long long)threadpool_queued(NULL), 0);
	expect_int("running(pool=NULL) 返回 0", (long long)threadpool_running(NULL), 0);
	expect_int("completed(pool=NULL) 返回 0", (long long)threadpool_completed(NULL), 0);
	expect_int("submitted(pool=NULL) 返回 0", (long long)threadpool_submitted(NULL), 0);
	expect_int("thread_count(pool=NULL) 返回 0", threadpool_thread_count(NULL), 0);

	threadpool_destroy(p);
	threadpool_destroy(NULL);   /* 不崩即通过 */

	/* 空池上直接 destroy，重复多轮用来暴露泄漏（LeakSanitizer 会报） */
	for (int i = 0; i < 10; i++) {
		threadpool *q = NULL;
		if (threadpool_create(i + 1, &q) != TPOOL_OK) {
			expect_true("循环 create 成功", 0);
			return;
		}
		threadpool_destroy(q);
	}
	expect_true("连续 10 轮 create/destroy 无线程与内存泄漏", 1);
}

/* ---------------------------------------------------------------- */
/* 2. 任务语义                                                        */
/* ---------------------------------------------------------------- */

static void test_single_task(void) {
	threadpool *p = NULL;
	slot        s;
	tpool_status st;

	printf("\n--- 单个任务 ---\n");
	(void)memset(&s, 0, sizeof s);
	st = threadpool_create(1, &p);
	expect_status("create(1)", st, TPOOL_OK);

	expect_status("submit 一个任务", threadpool_submit(p, task_mark, &s), TPOOL_OK);
	expect_status("wait 返回", threadpool_wait(p), TPOOL_OK);

	expect_int("任务被执行", s.hit, 1);
	expect_int("参数正确送达（index*2）", s.result, 0);
	expect_int("completed == 1", (long long)threadpool_completed(p), 1);
	expect_int("submitted == 1", (long long)threadpool_submitted(p), 1);
	expect_int("queued == 0", (long long)threadpool_queued(p), 0);
	expect_int("running == 0", (long long)threadpool_running(p), 0);

	threadpool_destroy(p);
}

static void test_1000_tasks_once(void) {
	enum { N = 1000 };
	threadpool *p = NULL;
	slot        slots[N];
	long        total = 0;

	printf("\n--- %d 个任务各执行一次 ---\n", N);
	(void)memset(slots, 0, sizeof slots);

	if (threadpool_create(8, &p) != TPOOL_OK) {
		expect_true("create(8)", 0);
		return;
	}

	for (int i = 0; i < N; i++) {
		slots[i].index = i;
		if (threadpool_submit(p, task_mark, &slots[i]) != TPOOL_OK) {
			expect_true("submit 全部成功", 0);
			threadpool_destroy(p);
			return;
		}
	}
	(void)threadpool_wait(p);

	for (int i = 0; i < N; i++) {
		total += slots[i].hit;
	}
	expect_int("1000 个任务的命中次数合计", total, N);
	expect_int("completed == 1000", (long long)threadpool_completed(p), N);
	expect_int("submitted == 1000", (long long)threadpool_submitted(p), N);

	/* 抽查参数：第 0、499、999 个 slot 的 result 是否为 index*2 */
	expect_int("slot[0].result", slots[0].result, 0);
	expect_int("slot[499].result", slots[499].result, 998);
	expect_int("slot[999].result", slots[999].result, 1998);

	threadpool_destroy(p);
}

/* ---------------------------------------------------------------- */
/* 3. 队列语义：FIFO                                                  */
/* ---------------------------------------------------------------- */

static void test_fifo_order(void) {
	enum { N = 32 };
	threadpool *p = NULL;
	order_box   box;
	int         seq[N];
	int         ordered = 1;

	printf("\n--- 单工作线程下的 FIFO 顺序 ---\n");
	(void)memset(seq, 0, sizeof seq);
	box.seq      = seq;
	box.capacity = N;
	box.count    = 0;

	if (threadpool_create(1, &p) != TPOOL_OK) {
		expect_true("create(1)", 0);
		return;
	}

	/*
	 * 单一工作线程 + 阻塞式紧提交，队列里会堆积多个任务。
	 * 若队列是 LIFO（头插头取），执行顺序会完全反过来。
	 */
	for (int i = 0; i < N; i++) {
		(void)threadpool_submit(p, task_record, &box);
	}
	(void)threadpool_wait(p);

	for (int i = 0; i < N; i++) {
		if (seq[i] != i) {
			ordered = 0;
		}
	}
	expect_int("执行了 N 个", (long long)box.count, N);
	expect_true("执行顺序 == 提交顺序（FIFO）", ordered);

	threadpool_destroy(p);
}

/* ---------------------------------------------------------------- */
/* 4. 并发语义：多生产者                                              */
/* ---------------------------------------------------------------- */

static void test_multi_producer(void) {
	enum { PRODUCERS = 4, PER_PRODUCER = 250, N = PRODUCERS * PER_PRODUCER };
	threadpool *p     = NULL;
	slot        slots[N];
	pthread_t   tids[PRODUCERS];
	producer_arg args[PRODUCERS];
	long        total = 0;

	printf("\n--- %d 个生产者各提交 %d 个任务 ---\n", PRODUCERS, PER_PRODUCER);
	(void)memset(slots, 0, sizeof slots);
	g_producer_failed_submits = 0;

	if (threadpool_create(4, &p) != TPOOL_OK) {
		expect_true("create(4)", 0);
		return;
	}

	for (int i = 0; i < PRODUCERS; i++) {
		args[i].pool  = p;
		args[i].slots = slots;
		args[i].begin = i * PER_PRODUCER;
		args[i].count = PER_PRODUCER;
		if (pthread_create(&tids[i], NULL, producer_main, &args[i]) != 0) {
			expect_true("生产者线程创建成功", 0);
			threadpool_destroy(p);
			return;
		}
	}
	for (int i = 0; i < PRODUCERS; i++) {
		(void)pthread_join(tids[i], NULL);
	}
	(void)threadpool_wait(p);

	for (int i = 0; i < N; i++) {
		total += slots[i].hit;
	}
	expect_int("并发提交失败的次数", atomic_load(&g_producer_failed_submits), 0);
	expect_int("执行次数合计", total, N);
	expect_int("completed == 1000", (long long)threadpool_completed(p), N);
	expect_int("submitted == 1000", (long long)threadpool_submitted(p), N);

	threadpool_destroy(p);
}

static void test_nested_submit(void) {
	threadpool *p = NULL;
	nest_box    box;

	printf("\n--- 任务内部再次提交 ---\n");
	box.pool = NULL;
	box.done = 0;
	g_nested_submit_status = TPOOL_ERR_PARAM;

	if (threadpool_create(2, &p) != TPOOL_OK) {
		expect_true("create(2)", 0);
		return;
	}
	box.pool = p;

	expect_status("提交父任务", threadpool_submit(p, task_nested_parent, &box), TPOOL_OK);
	expect_status("wait 到两代任务都完成", threadpool_wait(p), TPOOL_OK);

	expect_status("工作线程内 submit 成功", g_nested_submit_status, TPOOL_OK);
	expect_int("父子任务都执行", atomic_load(&box.done), 2);
	expect_int("completed == 2", (long long)threadpool_completed(p), 2);

	threadpool_destroy(p);
}

/* ---------------------------------------------------------------- */
/* 5. 同步语义                                                        */
/* ---------------------------------------------------------------- */

static void test_wait_idempotent(void) {
	threadpool *p = NULL;
	double      t0;
	double      elapsed;

	printf("\n--- wait 的幂等性与可复用性 ---\n");
	if (threadpool_create(2, &p) != TPOOL_OK) {
		expect_true("create(2)", 0);
		return;
	}

	(void)threadpool_submit(p, task_nop, NULL);
	(void)threadpool_wait(p);

	t0 = now_ms();
	expect_status("第二次 wait 立即返回", threadpool_wait(p), TPOOL_OK);
	elapsed = now_ms() - t0;
	expect_true("第二次 wait 耗时 < 50 ms", elapsed < 50.0);

	/* wait 之后池仍可用 */
	(void)threadpool_submit(p, task_nop, NULL);
	(void)threadpool_submit(p, task_nop, NULL);
	(void)threadpool_wait(p);
	expect_int("wait 之后继续提交，completed == 3",
	           (long long)threadpool_completed(p), 3);

	threadpool_destroy(p);
}

static void test_wait_from_worker_rejected(void) {
	threadpool *p = NULL;
	waitprobe   w;
	double      t0;
	double      elapsed;

	printf("\n--- 工作线程调用 wait 被拒绝 ---\n");
	w.pool   = NULL;
	w.status = TPOOL_OK;

	if (threadpool_create(2, &p) != TPOOL_OK) {
		expect_true("create(2)", 0);
		return;
	}
	w.pool = p;

	(void)threadpool_submit(p, task_call_wait, &w);

	t0      = now_ms();
	(void)threadpool_wait(p);
	elapsed = now_ms() - t0;

	expect_status("任务内 wait 返回 TPOOL_ERR_BLOCKING", w.status, TPOOL_ERR_BLOCKING);
	expect_true("主线程的 wait 正常返回（未死锁，< 2 s）", elapsed < 2000.0);

	threadpool_destroy(p);
}

/* ---------------------------------------------------------------- */
/* 6. 停机语义：优雅停机                                              */
/* ---------------------------------------------------------------- */

static void test_destroy_drains_queue(void) {
	enum { N = 200 };
	threadpool *p     = NULL;
	long        done  = 0;
	double      t0;
	double      elapsed;

	printf("\n--- destroy 排空队列（%d 个 1 ms 任务）---\n", N);
	if (threadpool_create(1, &p) != TPOOL_OK) {
		expect_true("create(1)", 0);
		return;
	}

	for (int i = 0; i < N; i++) {
		if (threadpool_submit(p, task_slow_1ms, &done) != TPOOL_OK) {
			expect_true("submit 全部成功", 0);
			threadpool_destroy(p);
			return;
		}
	}

	/*
	 * 提交完立刻 destroy，不先 wait。这里验证的正是"优雅停机"：
	 * 单线程 + 每个任务 1 ms，200 个任务约 200 ms，destroy 应当等完，
	 * 而不是把队列里的 199 个任务丢掉。
	 */
	t0 = now_ms();
	threadpool_destroy(p);
	elapsed = now_ms() - t0;

	expect_int("destroy 前已提交的 200 个任务全部执行", atomic_load(&done), N);
	expect_true("destroy 确实等待了执行（> 100 ms）", elapsed > 100.0);
	printf("info  destroy 耗时 %.1f ms（1 线程 × 200 × 1 ms）\n", elapsed);
}

static void test_stress(void) {
	enum { N = 100000, EXTRA = 100 };
	threadpool *p = NULL;

	/*
	 * 第二轮每个任务必须独占一个 slot。若 100 个任务共用同一个 slot，
	 * 它们会并发写同一块内存 —— 那是测试自己的数据竞争，
	 * TSan 会如实报告，但报告指向的不是被模块的缺陷。
	 */
	static slot scratch[EXTRA];
	double      t0;

	printf("\n--- 压力：%d 个空任务 ---\n", N);
	if (threadpool_create(4, &p) != TPOOL_OK) {
		expect_true("create(4)", 0);
		return;
	}

	t0 = now_ms();
	for (int i = 0; i < N; i++) {
		if (threadpool_submit(p, task_nop, NULL) != TPOOL_OK) {
			expect_true("submit 全部成功", 0);
			threadpool_destroy(p);
			return;
		}
	}
	(void)threadpool_wait(p);

	expect_int("completed == 100000", (long long)threadpool_completed(p), N);
	expect_int("submitted == completed，无丢失", 
	           (long long)(threadpool_submitted(p) - threadpool_completed(p)), 0);
	printf("info  提交 + 执行 100000 个空任务耗时 %.1f ms\n", now_ms() - t0);

	/* 再提交一轮，确认高水位之后计数与队列都正常 */
	for (int i = 0; i < EXTRA; i++) {
		scratch[i].index = i;
		(void)threadpool_submit(p, task_mark, &scratch[i]);
	}
	(void)threadpool_wait(p);
	expect_int("第二轮 100 个任务后 completed == 100100",
	           (long long)threadpool_completed(p), N + EXTRA);
	expect_int("第二轮第一个任务命中一次", scratch[0].hit, 1);
	expect_int("第二轮最后一个任务结果正确", scratch[EXTRA - 1].result,
	           (EXTRA - 1) * 2);

	threadpool_destroy(p);
}

/* ---------------------------------------------------------------- */

int main(void) {
	printf("threadpool %s —— 单元测试\n", THREADPOOL_VERSION_STRING);

	test_param_checks();
	test_single_task();
	test_1000_tasks_once();
	test_fifo_order();
	test_multi_producer();
	test_nested_submit();
	test_wait_idempotent();
	test_wait_from_worker_rejected();
	test_destroy_drains_queue();
	test_stress();

	printf("\n----------------------------------------\n");
	printf("检查项 %d，失败 %d\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
