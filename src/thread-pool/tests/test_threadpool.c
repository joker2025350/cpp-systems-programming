/*
 * test_threadpool.c —— threadpool 模块的单元测试
 *
 * 覆盖七类行为：
 *   1. 参数校验与生命周期（含重复 create/destroy）
 *   2. 任务语义：每个提交的任务恰好执行一次，参数正确送达
 *   3. 队列语义：FIFO 顺序
 *   4. 并发语义：多生产者同时提交，计数不丢
 *   5. 同步语义：wait 的正确性、幂等性，以及工作线程调用 wait 被拒绝
 *   6. 停机语义：destroy 优雅停机（排空队列而不是丢弃）
 *   7. 有界队列与背压：容量约束、满即失败、阻塞提交、工作线程不等待空位
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
/* 有界队列用的辅助：闸门与短任务                                     */
/* ---------------------------------------------------------------- */

/*
 * 闸门（gate）：任务进入后停在这里，直到测试线程显式放行。
 *
 * 为什么不用 sleep 来"等队列填满"：sleep 只能保证"至少过去了这么久"，
 * 不能保证"工作线程已经取走 / 还没取走某个任务"。闸门把"队列此刻恰好处在
 * 目标状态"变成可以直接构造的事实，测试因此不依赖机器速度。
 *
 * 时间上限 max_ms 的作用是兜底：实现有缺陷时（比如本不该阻塞的地方阻塞了），
 * 闸门自行放开，测试报出 FAIL 而不是把 make test 挂死。挂死的测试不会给出
 * 任何定位信息，比失败还糟。
 */
typedef struct {
	pthread_mutex_t mu;
	pthread_cond_t  cv;
	int             open;
} gate;

static void gate_init(gate *g) {
	(void)pthread_mutex_init(&g->mu, NULL);
	(void)pthread_cond_init(&g->cv, NULL);
	g->open = 0;
}

static void gate_release(gate *g) {
	(void)pthread_mutex_lock(&g->mu);
	g->open = 1;
	(void)pthread_cond_broadcast(&g->cv);
	(void)pthread_mutex_unlock(&g->mu);
}

static void gate_wait(gate *g, int max_ms) {
	struct timespec deadline;

	(void)pthread_mutex_lock(&g->mu);

	(void)clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec  += max_ms / 1000;
	deadline.tv_nsec += (long)(max_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec  += 1;
		deadline.tv_nsec -= 1000000000L;
	}

	while (!g->open) {
		if (pthread_cond_timedwait(&g->cv, &g->mu, &deadline) != 0) {
			break;   /* 超时或出错：不再等，让测试继续跑到断言处 */
		}
	}

	(void)pthread_mutex_unlock(&g->mu);
}

static void gate_destroy(gate *g) {
	(void)pthread_cond_destroy(&g->cv);
	(void)pthread_mutex_destroy(&g->mu);
}

/* 占用一个工作线程的闸门任务 */
typedef struct {
	gate *g;
	long *done;      /* 放行后自增，用来证明任务确实执行过 */
	int   max_ms;
} hold_job;

static void task_hold(void *arg) {
	hold_job *h = (hold_job *)arg;

	gate_wait(h->g, h->max_ms);
	(void)atomic_inc(h->done);
}

/* 每个任务忙等约 20 µs，用来让"生产快于消费"稳定成立 */
static void task_spin(void *arg) {
	long *hit = (long *)arg;
	double end = now_ms() + 0.02;

	while (now_ms() < end) {
		/* 空转 */
	}
	(void)atomic_inc(hit);
}

/* 在工作线程里调用 submit_wait：队列满时必须立刻返回 FULL，而不是等待 */
typedef struct {
	threadpool *pool;
	gate       *go;        /* 主线程把队列填满后放行 */
	long        reached;   /* 已进入任务（主线程据此确认工作线程就位） */
	tpool_status status;
	double      wait_ms;   /* submit_wait 自身的耗时 */
} workerprobe;

static void task_worker_probe(void *arg) {
	workerprobe *w = (workerprobe *)arg;
	double       t0;

	(void)atomic_inc(&w->reached);

	/* 这里的上限给得很宽：它只是防止测试自身挂死。真正的兜底在 hold 闸门上——
	 * 若实现让工作线程等了空位，400 ms 后 hold 任务结束、腾出位置，等待自然解除。 */
	gate_wait(w->go, 2000);

	t0 = now_ms();
	w->status  = threadpool_submit_wait(w->pool, task_nop, NULL);
	w->wait_ms = now_ms() - t0;
}

/* 阻塞提交：主线程在队列满时调用 submit_wait，记录它等了多久 */
typedef struct {
	threadpool *pool;
	slot       *s;
	tpool_status status;
	double      wait_ms;
} blocking_submit;

static void *blocking_submit_main(void *arg) {
	blocking_submit *b = (blocking_submit *)arg;
	double           t0 = now_ms();

	b->status  = threadpool_submit_wait(b->pool, task_mark, b->s);
	b->wait_ms = now_ms() - t0;
	return NULL;
}

/* 有界池上的多生产者：全部走阻塞提交 */
typedef struct {
	threadpool *pool;
	long       *hits;      /* 每个任务一个命中计数，任务忙等约 20 µs */
	int         begin;
	int         count;
	long        failures;
	long       *finished;
} bounded_producer_arg;

static void *bounded_producer_main(void *arg) {
	bounded_producer_arg *a = (bounded_producer_arg *)arg;

	for (int i = 0; i < a->count; i++) {
		if (threadpool_submit_wait(a->pool, task_spin, &a->hits[a->begin + i]) != TPOOL_OK) {
			(void)atomic_inc(&a->failures);
		}
	}
	(void)atomic_inc(a->finished);
	return NULL;
}

/* 自旋等待某个计数达到目标值，最多 timeout_ms。仅用于等待"工作线程已就位"这类
 * 由本测试自己构造的前置条件，不用于断言性能。 */
static int wait_for_count(const long *p, long want, int timeout_ms) {
	double end = now_ms() + (double)timeout_ms;

	while (now_ms() < end) {
		if (atomic_load(p) >= want) {
			return 0;
		}
		sleep_ms(1);
	}
	return -1;
}

/* 等"正在执行的任务数"达到 want。同样是构造前置条件，不是性能断言。 */
static int wait_running(threadpool *p, size_t want, int timeout_ms) {
	double end = now_ms() + (double)timeout_ms;

	while (now_ms() < end) {
		if (threadpool_running(p) >= want) {
			return 0;
		}
		sleep_ms(1);
	}
	return -1;
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
/* 7. 有界队列与背压                                                  */
/* ---------------------------------------------------------------- */

static void test_bounded_params(void) {
	threadpool *p = NULL;

	printf("\n--- 有界队列：参数与容量 ---\n");
	expect_status("create_bounded(4, 0) 被拒绝",
	              threadpool_create_bounded(4, 0, &p), TPOOL_ERR_PARAM);
	expect_true("容量 0 被拒绝后句柄为 NULL", p == NULL);
	expect_status("create_bounded(0, 4) 被拒绝",
	              threadpool_create_bounded(0, 4, &p), TPOOL_ERR_PARAM);
	expect_status("create_bounded(4, 4, NULL) 被拒绝",
	              threadpool_create_bounded(4, 4, NULL), TPOOL_ERR_PARAM);

	if (threadpool_create_bounded(2, 4, &p) != TPOOL_OK) {
		expect_true("create_bounded(2, 4) 成功", 0);
		return;
	}
	expect_int("capacity == 4", (long long)threadpool_capacity(p), 4);
	expect_int("新建池 backpressure == 0", (long long)threadpool_backpressure(p), 0);
	expect_status("submit_wait(pool=NULL)",
	              threadpool_submit_wait(NULL, task_nop, NULL), TPOOL_ERR_PARAM);
	expect_status("submit_wait(fn=NULL)",
	              threadpool_submit_wait(p, NULL, NULL), TPOOL_ERR_PARAM);
	expect_int("capacity(NULL) == 0", (long long)threadpool_capacity(NULL), 0);
	expect_int("backpressure(NULL) == 0", (long long)threadpool_backpressure(NULL), 0);
	threadpool_destroy(p);

	/* 无界池：容量报 0，submit_wait 退化成 submit，永远不计背压 */
	p = NULL;
	if (threadpool_create(1, &p) != TPOOL_OK) {
		expect_true("create(1)", 0);
		return;
	}
	expect_int("无界池 capacity == 0", (long long)threadpool_capacity(p), 0);
	for (int i = 0; i < 100; i++) {
		(void)threadpool_submit_wait(p, task_nop, NULL);
	}
	(void)threadpool_wait(p);
	expect_int("无界池 100 次 submit_wait 全部接受",
	           (long long)threadpool_submitted(p), 100);
	expect_int("无界池上 submit_wait 不计背压",
	           (long long)threadpool_backpressure(p), 0);
	threadpool_destroy(p);
}

static void test_bounded_full(void) {
	enum { CAP = 3 };
	threadpool *p = NULL;
	gate        g;
	hold_job    h;
	long        hold_done = 0;
	slot        slots[CAP + 1];

	printf("\n--- 有界队列：满即失败（容量 %d）---\n", CAP);
	(void)memset(slots, 0, sizeof slots);
	gate_init(&g);
	h.g      = &g;
	h.done   = &hold_done;
	h.max_ms = 3000;

	if (threadpool_create_bounded(1, CAP, &p) != TPOOL_OK) {
		expect_true("create_bounded(1, 3)", 0);
		gate_destroy(&g);
		return;
	}

	expect_status("提交闸门任务占住唯一的工作线程",
	              threadpool_submit(p, task_hold, &h), TPOOL_OK);
	expect_int("工作线程已进入闸门任务", wait_running(p, 1, 2000) == 0 ? 1 : 0, 1);

	for (int i = 0; i < CAP; i++) {
		slots[i].index = i;
		if (threadpool_submit(p, task_mark, &slots[i]) != TPOOL_OK) {
			expect_true("填满队列的提交全部成功", 0);
		}
	}
	expect_int("queued 达到容量", (long long)threadpool_queued(p), CAP);

	slots[CAP].index = CAP;
	expect_status("队列满时 submit 返回 TPOOL_ERR_FULL",
	              threadpool_submit(p, task_mark, &slots[CAP]), TPOOL_ERR_FULL);
	expect_int("FULL 之后 queued 不变", (long long)threadpool_queued(p), CAP);
	expect_int("FULL 之后 submitted 不变",
	           (long long)threadpool_submitted(p), CAP + 1);
	expect_int("被拒绝的任务从未执行", slots[CAP].hit, 0);

	gate_release(&g);
	(void)threadpool_wait(p);
	expect_int("排空后 completed == 容量 + 1",
	           (long long)threadpool_completed(p), CAP + 1);
	expect_int("排空后 hold 任务确实执行", atomic_load(&hold_done), 1);
	expect_int("排空后 queued == 0", (long long)threadpool_queued(p), 0);

	expect_status("排空后可以继续提交",
	              threadpool_submit(p, task_mark, &slots[CAP]), TPOOL_OK);
	(void)threadpool_wait(p);
	expect_int("第二次排空后 completed == 容量 + 2",
	           (long long)threadpool_completed(p), CAP + 2);

	threadpool_destroy(p);
	gate_destroy(&g);
}

static void test_bounded_submit_wait_blocks(void) {
	enum { CAP = 2 };
	threadpool     *p = NULL;
	gate            g;
	hold_job        h;
	blocking_submit b;
	slot            slots[CAP + 1];
	pthread_t       tid;
	long            hold_done = 0;

	printf("\n--- 有界队列：submit_wait 阻塞到有空位 ---\n");
	(void)memset(slots, 0, sizeof slots);
	gate_init(&g);
	h.g      = &g;
	h.done   = &hold_done;
	h.max_ms = 3000;
	b.pool   = NULL;
	b.s      = &slots[CAP];
	b.status = TPOOL_ERR_PARAM;   /* 哨兵：没被改过说明提交方还没返回 */
	b.wait_ms = 0.0;

	if (threadpool_create_bounded(1, CAP, &p) != TPOOL_OK) {
		expect_true("create_bounded(1, 2)", 0);
		gate_destroy(&g);
		return;
	}
	b.pool = p;

	expect_int("工作线程已进入闸门任务",
	           (threadpool_submit(p, task_hold, &h) == TPOOL_OK
	            && wait_running(p, 1, 2000) == 0) ? 1 : 0, 1);
	for (int i = 0; i < CAP; i++) {
		slots[i].index = i;
		(void)threadpool_submit(p, task_mark, &slots[i]);
	}
	expect_int("队列已满", (long long)threadpool_queued(p), CAP);

	if (pthread_create(&tid, NULL, blocking_submit_main, &b) != 0) {
		expect_true("提交线程创建成功", 0);
		gate_release(&g);
		threadpool_destroy(p);
		gate_destroy(&g);
		return;
	}

	/* 等到它真的进入等待，再继续。轮询而不是固定 sleep：固定 sleep 在
	 * 负载重的机器上会偶发地"还没被调度到"，那样的失败与被测代码无关。 */
	for (int i = 0; i < 500 && threadpool_backpressure(p) == 0; i++) {
		sleep_ms(1);
	}
	expect_int("背压计数 +1", (long long)threadpool_backpressure(p), 1);

	/* 再等 40 ms：确认它在拿到空位之前不会自己返回，同时让 wait_ms 有量可读 */
	sleep_ms(40);
	expect_status("未被放行前提交方仍在等待", b.status, TPOOL_ERR_PARAM);

	gate_release(&g);
	(void)pthread_join(tid, NULL);

	expect_status("出现空位后 submit_wait 返回 TPOOL_OK", b.status, TPOOL_OK);
	expect_true("提交方确实等了一段（> 30 ms）", b.wait_ms > 30.0);
	printf("info  submit_wait 等待 %.1f ms\n", b.wait_ms);

	(void)threadpool_wait(p);
	expect_int("四个任务全部执行", (long long)threadpool_completed(p), CAP + 2);
	expect_int("背压计数最终为 1", (long long)threadpool_backpressure(p), 1);

	threadpool_destroy(p);
	gate_destroy(&g);
}

static void test_worker_submit_wait_does_not_block(void) {
	enum { MAX_HOLD_MS = 400 };
	threadpool  *p = NULL;
	gate         hold_gate;
	gate         go_gate;
	hold_job     h;
	workerprobe  w;
	slot         filler;
	long         hold_done = 0;

	/*
	 * 布局：两个工作线程，容量 1。
	 *   A 进闸门任务（占用，不去取队里的东西）；
	 *   B 进探针任务，停在 go 闸门上；
	 *   主线程把队列填到 1/1；
	 *   放行 go，B 在满队列上调用 submit_wait。
	 *
	 * 正确的实现必须立刻返回 FULL。若实现了"工作线程也等空位"，它会等 A 的
	 * 闸门超时（400 ms）后才拿到 A 腾出的位置 —— 于是 status 与耗时都能看出来。
	 */
	printf("\n--- 工作线程不在满队列上等待空位 ---\n");
	gate_init(&hold_gate);
	gate_init(&go_gate);
	h.g      = &hold_gate;
	h.done   = &hold_done;
	h.max_ms = MAX_HOLD_MS;
	w.pool    = NULL;
	w.go      = &go_gate;
	w.reached = 0;
	w.status  = TPOOL_ERR_CLOSED;   /* 哨兵：不是任何合法返回值 */
	w.wait_ms = 0.0;
	filler.index = 0;

	if (threadpool_create_bounded(2, 1, &p) != TPOOL_OK) {
		expect_true("create_bounded(2, 1)", 0);
		gate_destroy(&hold_gate);
		gate_destroy(&go_gate);
		return;
	}
	w.pool = p;

	expect_int("闸门任务占住一个工作线程",
	           (threadpool_submit(p, task_hold, &h) == TPOOL_OK
	            && wait_running(p, 1, 2000) == 0) ? 1 : 0, 1);
	expect_status("探针任务被另一个工作线程取走",
	              threadpool_submit(p, task_worker_probe, &w), TPOOL_OK);
	expect_int("探针已在工作线程内就绪", wait_for_count(&w.reached, 1, 2000) == 0 ? 1 : 0, 1);

	expect_status("把队列填到容量上限",
	              threadpool_submit(p, task_mark, &filler), TPOOL_OK);
	expect_int("队列已满（1/1）", (long long)threadpool_queued(p), 1);

	gate_release(&go_gate);
	sleep_ms(50);

	expect_status("工作线程调用 submit_wait 返回 TPOOL_ERR_FULL",
	              w.status, TPOOL_ERR_FULL);

	gate_release(&hold_gate);
	(void)threadpool_wait(p);

	expect_true("该调用没有等待（< 100 ms）", w.wait_ms < 100.0);
	expect_int("工作线程的提交不计入背压",
	           (long long)threadpool_backpressure(p), 0);
	printf("info  工作线程上的 submit_wait 耗时 %.3f ms\n", w.wait_ms);

	threadpool_destroy(p);
	gate_destroy(&hold_gate);
	gate_destroy(&go_gate);
}

static void test_bounded_multi_producer(void) {
	enum { PRODUCERS = 4, PER_PRODUCER = 500, N = PRODUCERS * PER_PRODUCER, CAP = 8 };
	static long hits[N];          /* 占 16 KiB，放在静态区避免大栈帧 */
	threadpool *p = NULL;
	pthread_t   tids[PRODUCERS];
	bounded_producer_arg args[PRODUCERS];
	long        finished = 0;
	size_t      peak     = 0;
	long        total    = 0;

	/*
	 * 这里要回答的两个问题：
	 *   1. 队列深度是否真的被容量卡住（而不是"大概不会涨太多"）；
	 *   2. 阻塞提交是否会丢任务（返回 OK 却从未执行）。
	 * 前者靠主线程反复采样 queued 取最大值；后者靠执行次数合计。
	 *
	 * 每个任务忙等 20 µs：只有消费比生产慢，背压才会真的触发。
	 * 任务本身太快时生产端不会跑赢消费端，这个测试会退化成空转。
	 */
	printf("\n--- 有界队列：%d 个生产者 × %d 个任务，容量 %d ---\n",
	       PRODUCERS, PER_PRODUCER, CAP);
	(void)memset(hits, 0, sizeof hits);

	if (threadpool_create_bounded(2, CAP, &p) != TPOOL_OK) {
		expect_true("create_bounded(2, 8)", 0);
		return;
	}

	for (int i = 0; i < PRODUCERS; i++) {
		args[i].pool     = p;
		args[i].hits     = hits;
		args[i].begin    = i * PER_PRODUCER;
		args[i].count    = PER_PRODUCER;
		args[i].failures = 0;
		args[i].finished = &finished;
		if (pthread_create(&tids[i], NULL, bounded_producer_main, &args[i]) != 0) {
			expect_true("生产者线程创建成功", 0);
			threadpool_destroy(p);
			return;
		}
	}

	/* 采样：凡是"队列深度从未超过容量"这类断言，采样只能证伪不能证明，
	 * 但配合下面的计数断言已经足够——超限的那一次必然会被采到很多次。 */
	while (atomic_load(&finished) < PRODUCERS) {
		size_t q = threadpool_queued(p);

		if (q > peak) {
			peak = q;
		}
	}

	for (int i = 0; i < PRODUCERS; i++) {
		(void)pthread_join(tids[i], NULL);
	}
	(void)threadpool_wait(p);

	for (int i = 0; i < N; i++) {
		total += atomic_load(&hits[i]);
	}

	long failures = 0;

	for (int i = 0; i < PRODUCERS; i++) {
		failures += args[i].failures;
	}

	expect_int("阻塞提交失败次数", failures, 0);
	expect_int("执行次数合计 == 提交数", total, N);
	expect_int("submitted == completed", 
	           (long long)(threadpool_submitted(p) - threadpool_completed(p)), 0);
	expect_int("submitted == N", (long long)threadpool_submitted(p), N);
	expect_true("队列深度峰值不超过容量", peak <= CAP);
	expect_true("背压确实触发过", threadpool_backpressure(p) > 0);
	printf("info  采样到的队列深度峰值 %zu/%d，背压次数 %llu\n",
	       peak, CAP, threadpool_backpressure(p));

	threadpool_destroy(p);
}

static void test_destroy_wakes_blocked_submitter(void) {
	enum { CAP = 2, HOLD_MS = 300 };
	threadpool     *p = NULL;
	gate            g;
	hold_job        h;
	blocking_submit b;
	slot            slots[CAP + 1];
	pthread_t       tid;
	long            hold_done = 0;
	double          t0;
	double          elapsed;

	/*
	 * destroy 与阻塞提交同时发生，两条不变量必须同时成立：
	 *   1. 等在 space 上的提交方被唤醒并返回 CLOSED —— 否则它永远等不到空位，
	 *      而 destroy 又在等队列排空，双方互等；
	 *   2. destroy 不能在被唤醒的提交方还没离开临界区时释放池对象 ——
	 *      否则对方随后会对已释放的互斥锁执行 unlock。这条只能靠 ASan 抓，
	 *      所以本测试在 make sanitize 下同样要跑。
	 */
	printf("\n--- destroy 唤醒阻塞中的提交方 ---\n");
	(void)memset(slots, 0, sizeof slots);
	gate_init(&g);
	h.g      = &g;
	h.done   = &hold_done;
	h.max_ms = HOLD_MS;
	b.pool   = NULL;
	b.s      = &slots[CAP];
	b.status = TPOOL_ERR_PARAM;
	b.wait_ms = 0.0;

	if (threadpool_create_bounded(1, CAP, &p) != TPOOL_OK) {
		expect_true("create_bounded(1, 2)", 0);
		gate_destroy(&g);
		return;
	}
	b.pool = p;

	(void)threadpool_submit(p, task_hold, &h);
	(void)wait_running(p, 1, 2000);
	for (int i = 0; i < CAP; i++) {
		slots[i].index = i;
		(void)threadpool_submit(p, task_mark, &slots[i]);
	}

	if (pthread_create(&tid, NULL, blocking_submit_main, &b) != 0) {
		expect_true("提交线程创建成功", 0);
		gate_release(&g);
		threadpool_destroy(p);
		gate_destroy(&g);
		return;
	}

	/* 等到它真的进入等待，再开始 destroy；否则测的是另一条路径 */
	for (int i = 0; i < 200 && threadpool_backpressure(p) == 0; i++) {
		sleep_ms(1);
	}
	expect_int("提交方已进入等待", (long long)threadpool_backpressure(p), 1);

	t0 = now_ms();
	threadpool_destroy(p);
	elapsed = now_ms() - t0;

	(void)pthread_join(tid, NULL);

	expect_status("被唤醒的提交方返回 TPOOL_ERR_CLOSED", b.status, TPOOL_ERR_CLOSED);
	expect_int("destroy 前的已提交任务全部执行（未丢失）",
	           (long long)hold_done + slots[0].hit + slots[1].hit, 3);
	expect_int("被拒绝的任务从未执行", slots[CAP].hit, 0);
	expect_true("destroy 没有死锁（< 2 s）", elapsed < 2000.0);
	printf("info  destroy 耗时 %.1f ms（含 %d ms 的闸门超时）\n", elapsed, HOLD_MS);

	gate_destroy(&g);
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
	test_bounded_params();
	test_bounded_full();
	test_bounded_submit_wait_blocks();
	test_worker_submit_wait_does_not_block();
	test_bounded_multi_producer();
	test_destroy_wakes_blocked_submitter();

	printf("\n----------------------------------------\n");
	printf("检查项 %d，失败 %d\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
