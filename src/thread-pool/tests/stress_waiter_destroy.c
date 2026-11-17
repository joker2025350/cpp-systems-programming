/*
 * stress_waiter_destroy.c —— destroy 与屏障等待并发的竞态压力检查
 *
 * 不放进 make test，原因是它检验的东西不满足"一次运行就能得出结论"：
 * 这段竞态的窗口只有几个微秒，单次运行撞到的概率很低（实测单轮 10 次、
 * 每次 32 个等待方，一次都没撞到；300 轮才稳定触发）。放进常规测试会变成
 * 一条"有时候能抓到、有时候抓不到"的用例，而那种用例最容易在真的回归时
 * 报绿。要撞就多撞几轮，见 make stress。
 *
 * 检验的对象是 threadpool_destroy 与 threadpool_wait_until 的时序：
 *
 *   工作线程做完最后一个任务时会 broadcast 一次 idle。此刻 destroy 与屏障
 *   等待方在同一把互斥锁上排队，谁先拿到锁由实现决定。若 destroy 先拿到，
 *   它会看到 queued / running 都是 0，于是往下走：join 工作线程、销毁三个
 *   条件变量、销毁互斥锁、free 池对象。而此时屏障等待方还挂在 idle 上——
 *   它随后要取的那把锁和要读的那几个计数都已经被释放。按 C11 6.2.4p2，
 *   对象生存期结束后任何访问都是未定义行为。
 *
 *   池内部用 waiters 计数把这段窗口堵住：destroy 的等待条件里含 waiters，
 *   屏障方离开临界区时把计数减到 0 并 broadcast 一次 idle。去掉那一项，
 *   本程序在 ASan 下会报 heap-use-after-free，栈顶落在 wait_impl 读计数的
 *   那一行（1.3.0 落地时做过这条注入验证）。
 *
 * 为什么每轮开 W 个等待方而不是 1 个：单个等待方醒来后只做"取锁、读两个
 * 计数、减一、放锁"，比 destroy 的收尾（join + 四次销毁 + 两次 free）快得多，
 * 几乎总是先跑完。多个等待方会让后几个还排在同一个互斥锁上，正好落在
 * "对象已释放、仍在访问"的区间里。
 *
 * 构造方式（1.4.0 改过一次，两处都不是理论问题）：
 *
 *   原来的时序是"提交一个 2 ms 的忙等任务 → 创建 N 个等待线程 → 立刻创建
 *   destroy 线程"。两个毛病都在 2026-11-05 的 20 次重复运行里露出来：
 *
 *   1. 迟到的等待方在池被 free 之后才去取锁，ASan 报 use-after-free，
 *      栈顶与"真正的竞态"一模一样。这是调用方违反了头文件里那条
 *      "destroy 不得与正在进入模块的新调用并发"，20 次里失败 1 次。
 *   2. 更糟的一种是静默的：2 ms 的忙等任务若在等待方进入之前就结束，
 *      每个 wait_until 都会发现 queued 和 running 都是 0，走快速路径
 *      直接返回 TPOOL_OK —— 计数里一次都没有它，程序照样打印
 *      "9600 个等待方里返回非 TPOOL_OK 的有 0 个"，实际什么都没测。
 *
 *   现在改成闸门构造：任务一直占住唯一的工作线程，直到主线程放行。
 *   于是"等待方已经就位"变成可观测量（threadpool_waiters 报到 N），
 *   不再依赖调度延迟。
 *
 * 两种失败形态，必须都能变成"退出码" (2026-11-17 加看门狗)：
 *
 *   同一个缺陷有两种表现。1.3.0 的构造下是 ASan 报 heap-use-after-free，
 *   进程当场中止，原因写在报告里；1.4.0 的闸门构造下变成了**卡死**——
 *   等待方挂在一个已经被销毁的条件变量上，既不返回也不报错，什么都不打印。
 *   后者靠外部 `timeout` 才变成失败，而外部 timeout 拿不到任何上下文：
 *   它只能告诉你"被杀掉了"，说不出卡在第几轮、卡在哪一步。
 *
 *   所以程序自己带一个看门狗线程：最近一次阶段推进超过 N 秒（默认 30 s），
 *   就把轮次与阶段打到 stderr，然后 _Exit(3)。看门狗**只读自己的原子变量**
 *   与静态常量表，绝不碰 threadpool —— 池此刻可能正在被销毁，
 *   而"池里的对象还能不能读"正是被测的那个问题本身。
 *
 *   看门狗本身也要有判据：它是"卡死时的唯一信息来源"，坏了却不会有人知道，
 *   直到真的卡死那一天（那时你会以为已经有人兜底）。所以有 selftest 模式：
 *   故意永卡，期望 N 秒后以退出码 3 结束。`make stress` 每次先跑它。
 *
 * 用法：stress_waiter_destroy [轮数] [每轮等待方个数] [看门狗秒数]
 *       stress_waiter_destroy selftest [看门狗秒数]
 * 退出码：0 全部返回 TPOOL_OK；1 有等待方返回了别的状态，或等待方没能就位；
 *         2 参数错误；3 看门狗判定卡死
 */

#include "threadpool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_WAITERS 64
#define DEADLINE_S  5      /* 屏障的截止时间：远大于放行延迟，正常不该超时 */

/* ---------------------------------------------------------------- */
/* 闸门：让占位任务一直占着工作线程，直到主线程显式放行               */
/* ---------------------------------------------------------------- */

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t  open;
	int             released;
} gate;

static void gate_init(gate *g) {
	(void)pthread_mutex_init(&g->lock, NULL);
	(void)pthread_cond_init(&g->open, NULL);
	g->released = 0;
}

static void gate_release(gate *g) {
	(void)pthread_mutex_lock(&g->lock);
	g->released = 1;
	(void)pthread_cond_broadcast(&g->open);
	(void)pthread_mutex_unlock(&g->lock);
}

static void gate_wait(gate *g) {
	(void)pthread_mutex_lock(&g->lock);
	while (!g->released) {
		(void)pthread_cond_wait(&g->open, &g->lock);
	}
	(void)pthread_mutex_unlock(&g->lock);
}

static void gate_destroy(gate *g) {
	(void)pthread_cond_destroy(&g->open);
	(void)pthread_mutex_destroy(&g->lock);
}

/* ---------------------------------------------------------------- */
/* 线程函数                                                          */
/* ---------------------------------------------------------------- */

static void task_hold(void *arg) {
	gate_wait((gate *)arg);
}

typedef struct {
	threadpool     *pool;
	struct timespec deadline;
	tpool_status    status;
} waiter;

static void *waiter_main(void *arg) {
	waiter *w = (waiter *)arg;

	w->status = threadpool_wait_until(w->pool, &w->deadline);
	return NULL;
}

static void *destroy_main(void *arg) {
	threadpool_destroy((threadpool *)arg);
	return NULL;
}

/* ---------------------------------------------------------------- */
/* 观测                                                              */
/* ---------------------------------------------------------------- */

static void sleep_ms(long ms) {
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

/*
 * 等到 n 个等待方都进了临界区（threadpool_waiters 报出 n）再返回。
 *
 * 这一步不是保险条款，它决定这个程序检验的到底是什么。库的约定是
 * "destroy 不得与正在进入模块的新调用并发"：一个还没取到互斥锁的调用方，
 * 与 destroy 之间没有任何同步点，那个窗口在加锁之前，库内部计数盖不住它。
 * 观测到 N 个之后，destroy 的等待条件里含 waiters，它不可能越过它们收尾，
 * 于是检验的才是"destroy 会不会把还挂在 idle 上的等待方连着条件变量一起销毁"。
 *
 * 超时返回 0：报错比挂住好，也比"以为测了其实没测"好。
 */
static int wait_for_waiters(threadpool *pool, size_t n, long timeout_ms) {
	const struct timespec step = { 0, 200000 };   /* 200 µs */
	struct timespec       start;
	struct timespec       now;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (threadpool_waiters(pool) >= n) {
			return 1;
		}

		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		{
			long elapsed_ms = (long)(now.tv_sec - start.tv_sec) * 1000L
			                + (long)(now.tv_nsec - start.tv_nsec) / 1000000L;

			if (elapsed_ms >= timeout_ms) {
				return 0;
			}
		}
		(void)nanosleep(&step, NULL);
	}
}

/* 等到工作线程取走占位任务（running >= 1），之后创建的等待方才会进入等待 */
static int wait_for_running(threadpool *pool, long timeout_ms) {
	const struct timespec step = { 0, 200000 };
	struct timespec       start;
	struct timespec       now;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (threadpool_running(pool) >= 1) {
			return 1;
		}

		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		{
			long elapsed_ms = (long)(now.tv_sec - start.tv_sec) * 1000L
			                + (long)(now.tv_nsec - start.tv_nsec) / 1000000L;

			if (elapsed_ms >= timeout_ms) {
				return 0;
			}
		}
		(void)nanosleep(&step, NULL);
	}
}

/* ---------------------------------------------------------------- */
/* 看门狗：把"卡死"变成退出码 3 与一段上下文                          */
/* ---------------------------------------------------------------- */

/*
 * 阶段编号只用于诊断输出。看门狗读的是 g_phase，主线程在每一步之前写它，
 * 于是"上次推进停在哪一阶段"就是卡住的位置。
 */
enum {
	PH_PREPARE = 0,   /* create / submit */
	PH_RUNNING,       /* 等工作线程取走占位任务 */
	PH_WAITERS,       /* 等 N 个等待方进入临界区 */
	PH_HANDOFF,       /* 放行后等等待方与 destroy 返回 */
	PH_SELFTEST,      /* 自检：故意永卡 */
	PH_COUNT
};

static const char *const phase_name[PH_COUNT] = {
	"准备（create / submit）",
	"等工作线程取走占位任务",
	"等 N 个等待方进入临界区",
	"放行后等等待方与 destroy 返回",
	"看门狗自检（故意永卡）",
};

/*
 * 这三个量同时被主线程与看门狗线程访问，所以必须是原子类型：
 * 两个线程对一个非原子对象的一次读与一次写并发，按 C11 5.1.2.4p25 就是
 * 数据竞争，行为未定义——用 _Atomic 加 relaxed 就够了，这里要的只是
 * "读到某个旧值也不影响判断"，不需要用它给别的数据建立 happens-before。
 */
static _Atomic long long g_progress_ms;
static _Atomic int       g_phase;
static _Atomic int       g_round;
static int               g_watchdog_s;

static long long now_ms(void) {
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static void progress(int phase, int round) {
	atomic_store_explicit(&g_phase, phase, memory_order_relaxed);
	atomic_store_explicit(&g_round, round, memory_order_relaxed);
	atomic_store_explicit(&g_progress_ms, now_ms(), memory_order_relaxed);
}

static void *watchdog_main(void *arg) {
	const long long limit = (long long)g_watchdog_s * 1000LL;

	(void)arg;

	for (;;) {
		struct timespec nap = { 0, 250000000L };   /* 250 ms */
		long long       last;
		long long       now;

		(void)nanosleep(&nap, NULL);

		last = atomic_load_explicit(&g_progress_ms, memory_order_relaxed);
		now  = now_ms();
		if (now - last < limit) {
			continue;
		}

		{
			int ph = atomic_load_explicit(&g_phase, memory_order_relaxed);
			int rd = atomic_load_explicit(&g_round, memory_order_relaxed);

			(void)fprintf(stderr,
				"\n!! 看门狗：%lld ms 没有推进（第 %d 轮，阶段：%s）\n"
				"   这通常就是 destroy 与屏障等待之间那条竞态：某个线程永久挂在\n"
				"   一个已被销毁的条件变量上，既不返回也不报错。按 C11 6.2.4p2，\n"
				"   对象生存期结束之后的访问是未定义行为——\"卡死\"与\n"
				"   heap-use-after-free 是同一条竞态在不同内存状态下的两种表现。\n"
				"   退出码 3 = 卡死（1 = 等待方返回了非 TPOOL_OK，2 = 参数错误）。\n",
				now - last, rd,
				(ph >= 0 && ph < PH_COUNT) ? phase_name[ph] : "未知");
			/*
			 * _Exit 而不是 exit：此刻其它线程可能正持着锁、或在 malloc 内部，
			 * exit 要跑 atexit 处理器并刷所有 stdio 流，那些清理会在一个
			 * 不一致的状态上执行。所以先自己把两条流刷掉，再直接结束进程。
			 */
			(void)fflush(stdout);
			(void)fflush(stderr);
			_Exit(3);
		}
	}

	return NULL;
}

/*
 * 起看门狗。失败就直接报错退出：没有上界的压力检查正是这次要消灭的东西，
 * 悄悄降级成"没有看门狗"等于把旧问题原样留下，还多了一层"以为有兜底"。
 */
static int watchdog_start(int seconds) {
	pthread_t t;

	if (seconds <= 0) {
		return 1;              /* 显式要求不启用 */
	}

	g_watchdog_s = seconds;
	atomic_store(&g_progress_ms, now_ms());
	atomic_store(&g_phase, PH_PREPARE);
	atomic_store(&g_round, 0);

	if (pthread_create(&t, NULL, watchdog_main, NULL) != 0) {
		(void)fprintf(stderr, "看门狗线程创建失败：拒绝在没有上界的情况下继续\n");
		return 0;
	}
	(void)pthread_detach(t);
	return 1;
}

/* ---------------------------------------------------------------- */

/*
 * 自检：故意永卡，让看门狗自己去结束进程。
 *
 * 这是唯一能在正常路径上验证看门狗的办法。少了它，看门狗写错的后果是
 * "卡死那天才知道"——而那一天正是最需要它的时刻。
 */
static int run_selftest(int seconds) {
	printf("看门狗自检：故意永卡，%d s 后应以退出码 3 结束\n", seconds);
	(void)fflush(stdout);

	if (!watchdog_start(seconds)) {
		return 1;
	}
	progress(PH_SELFTEST, 0);

	for (;;) {
		struct timespec nap = { 1, 0 };
		(void)nanosleep(&nap, NULL);
	}
}

int main(int argc, char **argv) {
	int       rounds = 300;
	int       nw     = MAX_WAITERS / 2;
	int       wd     = 30;
	waiter    w[MAX_WAITERS];
	pthread_t wt[MAX_WAITERS];
	int       bad = 0;

	if (argc > 1 && strcmp(argv[1], "selftest") == 0) {
		int seconds = (argc > 2) ? atoi(argv[2]) : 2;

		if (seconds < 1) {
			(void)fprintf(stderr, "用法：%s selftest [看门狗秒数>=1]\n", argv[0]);
			return 2;
		}
		return run_selftest(seconds);
	}

	if (argc > 1) {
		rounds = atoi(argv[1]);
	}
	if (argc > 2) {
		nw = atoi(argv[2]);
	}
	if (argc > 3) {
		wd = atoi(argv[3]);
	}

	if (rounds < 1 || nw < 1 || nw > MAX_WAITERS || wd < 0) {
		(void)fprintf(stderr,
		              "用法：%s [轮数>=1] [等待方个数 1~%d] [看门狗秒数>=0]\n"
		              "      %s selftest [看门狗秒数>=1]\n",
		              argv[0], MAX_WAITERS, argv[0]);
		return 2;
	}

	if (!watchdog_start(wd)) {
		return 1;
	}

	printf("destroy × 屏障等待：%d 轮，每轮 %d 个等待方，看门狗 %d s\n",
	       rounds, nw, wd);

	for (int i = 0; i < rounds; i++) {
		threadpool     *pool = NULL;
		pthread_t       dt;
		struct timespec now;
		gate            g;

		progress(PH_PREPARE, i);

		if (threadpool_create(1, &pool) != TPOOL_OK) {
			(void)fprintf(stderr, "第 %d 轮 create 失败\n", i);
			return 1;
		}

		/*
		 * 占位任务一直占着唯一的工作线程，直到第 (5) 步放行。
		 * 用闸门而不是固定时长的忙等，是因为后者要么太短（等待方还没
		 * 进来，任务就结束了，见文件头第 2 条），要么太长（拖慢 300 轮）。
		 */
		gate_init(&g);
		if (threadpool_submit(pool, task_hold, &g) != TPOOL_OK) {
			(void)fprintf(stderr, "第 %d 轮提交失败\n", i);
			gate_destroy(&g);
			threadpool_destroy(pool);
			return 1;
		}

		/* (1) 确认任务已经被取走，否则等待方会看到空队列走快速路径 */
		progress(PH_RUNNING, i);
		if (!wait_for_running(pool, 2000)) {
			(void)fprintf(stderr, "第 %d 轮：工作线程 2 s 内没取走占位任务\n", i);
			gate_destroy(&g);
			threadpool_destroy(pool);
			return 1;
		}

		/* (2) 创建 N 个等待方，全部带 5 s 的截止时间 */
		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		for (int j = 0; j < nw; j++) {
			w[j].pool     = pool;
			w[j].deadline = now;
			w[j].deadline.tv_sec += DEADLINE_S;
			w[j].status   = TPOOL_ERR_PARAM;

			if (pthread_create(&wt[j], NULL, waiter_main, &w[j]) != 0) {
				(void)fprintf(stderr, "第 %d 轮第 %d 个等待线程创建失败\n", i, j);
				gate_release(&g);
				(void)threadpool_wait(pool);
				threadpool_destroy(pool);
				gate_destroy(&g);
				return 1;
			}
		}

		/* (3) 确认 N 个等待方都已进入临界区，不是"大概进去了" */
		progress(PH_WAITERS, i);
		if (!wait_for_waiters(pool, (size_t)nw, 2000)) {
			(void)fprintf(stderr,
			              "第 %d 轮：2 s 内只有 %zu/%d 个等待方进入等待\n",
			              i, threadpool_waiters(pool), nw);
			gate_release(&g);
			for (int j = 0; j < nw; j++) {
				(void)pthread_join(wt[j], NULL);
			}
			threadpool_destroy(pool);
			gate_destroy(&g);
			return 1;
		}

		/* (4) 起 destroy 线程。它此刻卡在"等 running / waiters 归零"上 */
		progress(PH_HANDOFF, i);
		if (pthread_create(&dt, NULL, destroy_main, pool) != 0) {
			(void)fprintf(stderr, "第 %d 轮 destroy 线程创建失败\n", i);
			gate_release(&g);
			for (int j = 0; j < nw; j++) {
				(void)pthread_join(wt[j], NULL);
			}
			threadpool_destroy(pool);
			gate_destroy(&g);
			return 1;
		}

		/*
		 * (5) 放行。这一步之前 destroy 是否已经进了排空循环，只影响
		 * "两个等待者是否被同一次 broadcast 唤醒"，不影响正确性：
		 * 若 destroy 起晚了，waiters 会先归零，destroy 只是少一次争用。
		 * destroy 从起线程到进 cond_wait 只有取锁、置 closed、broadcast 三步，
		 * 2 ms 足够它到位；这里不能用"观测"来判定，因为"destroy 已进入"
		 * 没有对外可读的状态（closed 没有查询接口）。
		 */
		sleep_ms(2);
		gate_release(&g);

		for (int j = 0; j < nw; j++) {
			(void)pthread_join(wt[j], NULL);
			if (w[j].status != TPOOL_OK) {
				bad++;
			}
		}
		(void)pthread_join(dt, NULL);
		gate_destroy(&g);

		/* destroy 返回之后池已经不存在，这里不能再碰 pool */
	}

	printf("结果：%d 个等待方里返回非 TPOOL_OK 的有 %d 个\n", rounds * nw, bad);
	return (bad == 0) ? 0 : 1;
}
