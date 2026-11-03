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
 *   那一行；加上之后 300 轮 × 32 个等待方干净通过。
 *
 * 为什么每轮开 W 个等待方而不是 1 个：单个等待方醒来后只做"取锁、读两个
 * 计数、减一、放锁"，比 destroy 的收尾（join + 四次销毁 + 两次 free）快得多，
 * 几乎总是先跑完。多个等待方会让后几个还排在同一个互斥锁上，正好落在
 * "对象已释放、仍在访问"的区间里。
 *
 * 用法：stress_waiter_destroy [轮数] [每轮等待方个数]
 * 退出码：0 全部返回 TPOOL_OK；1 有等待方返回了别的状态
 */

#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_WAITERS 64
#define HOLD_US     2000   /* 占住唯一工作线程的时长 */
#define DEADLINE_S  5      /* 屏障的截止时间：远大于 HOLD_US，正常不该超时 */

/* 忙等 us 微秒。用 nanosleep 而不是空转：这里要的是"任务确实占住线程"，
 * 不是"消耗 CPU"，空转会让 32 个等待方的调度也变得不稳定。 */
static void task_usleep(void *arg) {
	const long     *us = (const long *)arg;
	struct timespec ts;

	ts.tv_sec  = 0;
	ts.tv_nsec = *us * 1000L;
	(void)nanosleep(&ts, NULL);
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

int main(int argc, char **argv) {
	int       rounds = (argc > 1) ? atoi(argv[1]) : 300;
	int       nw     = (argc > 2) ? atoi(argv[2]) : MAX_WAITERS / 2;
	long      hold_us = HOLD_US;
	waiter    w[MAX_WAITERS];
	pthread_t wt[MAX_WAITERS];
	int       bad = 0;

	if (rounds < 1 || nw < 1 || nw > MAX_WAITERS) {
		(void)fprintf(stderr, "用法：%s [轮数>=1] [等待方个数 1~%d]\n",
		              argv[0], MAX_WAITERS);
		return 2;
	}

	printf("destroy × 屏障等待：%d 轮，每轮 %d 个等待方\n", rounds, nw);

	for (int i = 0; i < rounds; i++) {
		threadpool     *pool = NULL;
		pthread_t       dt;
		struct timespec now;

		if (threadpool_create(1, &pool) != TPOOL_OK) {
			(void)fprintf(stderr, "第 %d 轮 create 失败\n", i);
			return 1;
		}

		/*
		 * 先让唯一的工作线程忙起来，等待方与 destroy 都在这 2 ms 里就位：
		 * 等待方一定看到"还有任务在跑"才进入等待，destroy 一定看到 running > 0
		 * 才进入排空循环。两者都在等待时任务结束，才构成要检验的时序。
		 */
		if (threadpool_submit(pool, task_usleep, &hold_us) != TPOOL_OK) {
			(void)fprintf(stderr, "第 %d 轮提交失败\n", i);
			threadpool_destroy(pool);
			return 1;
		}

		(void)clock_gettime(CLOCK_MONOTONIC, &now);
		for (int j = 0; j < nw; j++) {
			w[j].pool     = pool;
			w[j].deadline = now;
			w[j].deadline.tv_sec += DEADLINE_S;
			w[j].status   = TPOOL_ERR_PARAM;

			if (pthread_create(&wt[j], NULL, waiter_main, &w[j]) != 0) {
				(void)fprintf(stderr, "第 %d 轮第 %d 个等待线程创建失败\n", i, j);
				return 1;
			}
		}
		if (pthread_create(&dt, NULL, destroy_main, pool) != 0) {
			(void)fprintf(stderr, "第 %d 轮 destroy 线程创建失败\n", i);
			return 1;
		}

		for (int j = 0; j < nw; j++) {
			(void)pthread_join(wt[j], NULL);
			if (w[j].status != TPOOL_OK) {
				bad++;
			}
		}
		(void)pthread_join(dt, NULL);

		/* 等 destroy 返回之后池已经不存在，这里不能再碰 pool */
	}

	printf("结果：%d 个等待方里返回非 TPOOL_OK 的有 %d 个\n", rounds * nw, bad);
	return (bad == 0) ? 0 : 1;
}
