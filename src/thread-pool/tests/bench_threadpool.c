/*
 * bench_threadpool.c —— 线程池的三组测量
 *
 *   1. 空任务吞吐：只测"提交 → 调度 → 执行 → 回收"这一圈的开销，
 *      任务函数本身什么都不做。这是线程池的固定成本下限。
 *   2. 与"每任务新建线程"对比：同样的任务量，一次 pthread_create +
 *      pthread_join 能换多少个池内任务。
 *   3. 固定工作量任务的并行加速比：每个任务忙等 100 µs，
 *      看墙钟时间随线程数怎么变。
 *
 * 计时只用 CLOCK_MONOTONIC。单次测量噪声较大，因此每组跑 REPEAT 轮取最小值：
 * 最小值最接近"没有别的进程干扰时"的真实成本，平均值会被偶发的调度抖动抬高。
 *
 * 编译运行：make bench
 */

#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define REPEAT 5

static double now_ms(void) {
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* 忙等 ms 毫秒。不用 nanosleep：睡眠会让出 CPU，测出来的就不是并行加速比 */
static void busy_wait_ms(double ms) {
	double end = now_ms() + ms;

	while (now_ms() < end) {
		/* 空转 */
	}
}

static void task_nop(void *arg) {
	(void)arg;
}

static void task_busy_100us(void *arg) {
	(void)arg;
	busy_wait_ms(0.1);
}

/* ---------------------------------------------------------------- */

/* 向 nthreads 个线程的池提交 ntasks 个空任务，返回每任务纳秒数 */
static double bench_empty(int nthreads, int ntasks) {
	double      best = 1e18;

	for (int r = 0; r < REPEAT; r++) {
		threadpool *p   = NULL;
		double      t0;
		double      t1;

		if (threadpool_create(nthreads, &p) != TPOOL_OK) {
			return -1.0;
		}
		t0 = now_ms();
		for (int i = 0; i < ntasks; i++) {
			(void)threadpool_submit(p, task_nop, NULL);
		}
		(void)threadpool_wait(p);
		t1 = now_ms();

		threadpool_destroy(p);

		if (t1 - t0 < best) {
			best = t1 - t0;
		}
	}
	return best * 1e6 / (double)ntasks;   /* ms -> ns，再摊到每个任务 */
}

/* 每任务新建一个线程并 join，返回每任务纳秒数 */
static void *trivial_thread(void *arg) {
	(void)arg;
	return NULL;
}

static double bench_thread_per_task(int ntasks) {
	double best = 1e18;

	for (int r = 0; r < REPEAT; r++) {
		double t0 = now_ms();

		for (int i = 0; i < ntasks; i++) {
			pthread_t tid;

			if (pthread_create(&tid, NULL, trivial_thread, NULL) != 0) {
				return -1.0;
			}
			(void)pthread_join(tid, NULL);
		}
		double t1 = now_ms();

		if (t1 - t0 < best) {
			best = t1 - t0;
		}
	}
	return best * 1e6 / (double)ntasks;
}

/* 固定工作量任务的总耗时（毫秒） */
static double bench_busy(int nthreads, int ntasks) {
	double best = 1e18;

	for (int r = 0; r < REPEAT; r++) {
		threadpool *p = NULL;
		double      t0;

		if (threadpool_create(nthreads, &p) != TPOOL_OK) {
			return -1.0;
		}
		t0 = now_ms();
		for (int i = 0; i < ntasks; i++) {
			(void)threadpool_submit(p, task_busy_100us, NULL);
		}
		(void)threadpool_wait(p);
		double t1 = now_ms();

		threadpool_destroy(p);

		if (t1 - t0 < best) {
			best = t1 - t0;
		}
	}
	return best;
}

/* ---------------------------------------------------------------- */

int main(void) {
	const int sizes[] = { 1, 2, 4, 8 };
	const int nsizes  = (int)(sizeof sizes / sizeof sizes[0]);

	printf("线程池基准（单位：每任务纳秒，%d 轮取最小值）\n", REPEAT);
	printf("========================================================================\n");

	printf("\n[1] 空任务：提交 → 调度 → 执行 → 回收\n");
	printf("  %-8s %12s %12s %14s\n", "线程数", "任务数", "ns/任务", "任务/秒");
	for (int i = 0; i < nsizes; i++) {
		int    ntasks = 200000;
		double ns;

		if (sizes[i] > 8) {
			continue;
		}
		ns = bench_empty(sizes[i], ntasks);
		if (ns < 0) {
			printf("  %-8d 创建失败\n", sizes[i]);
			continue;
		}
		printf("  %-8d %12d %12.1f %14.0f\n",
		       sizes[i], ntasks, ns, 1e9 / ns);
	}

	printf("\n[2] 对照：每个任务新建一个线程再 join\n");
	{
		int    ntasks = 20000;
		double ns     = bench_thread_per_task(ntasks);

		if (ns > 0) {
			printf("  %-8s %12d %12.1f %14.0f\n", "—", ntasks, ns, 1e9 / ns);
		}
	}

	printf("\n[3] 固定工作量：每任务忙等 100 µs，共 1000 个任务\n");
	{
		int    ntasks = 1000;
		double base   = bench_busy(1, ntasks);

		printf("  %-8s %12s %12s %14s\n", "线程数", "总耗时(ms)", "加速比", "效率");
		for (int i = 0; i < nsizes; i++) {
			double t = bench_busy(sizes[i], ntasks);

			if (t <= 0) {
				continue;
			}
			printf("  %-8d %12.1f %12.2f %13.0f%%\n",
			       sizes[i], t, base / t, 100.0 * (base / t) / (double)sizes[i]);
		}
	}

	printf("\n说明：加速比以 1 线程为基准；效率 = 加速比 / 线程数。\n");
	return 0;
}
