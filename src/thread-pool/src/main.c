/*
 * main.c —— threadpool 模块的命令行驱动
 *
 * 只链接 libthreadpool.a，不重复编译模块源码：这样"这个模块能被独立链接"
 * 在每次构建时都被验证一次。
 *
 * 子命令：
 *   run     [-t 线程数] [-n 任务数] [-w 每个任务的忙等毫秒]
 *   bounded [-t 线程数] [-c 队列上限] [-n 任务数] [-w 每个任务的忙等毫秒]
 *   timeout [-c 队列上限] [-w 占住线程毫秒] [-m 超时毫秒]
 *   spawn   [-n 任务数]        对照组：每个任务新建一个线程
 *   help
 *
 * run 与 spawn 用同一份任务函数与同一份工作量，两者的耗时可以直接比较。
 * bounded 与 run 的区别只在创建方式与提交方式：有界队列 + 阻塞式提交，
 * 因此可以观察背压计数。
 * timeout 在 bounded 的基础上再给等待加一个截止时间，用来观察"等到不到"这个
 * 结果本身——它只能从返回码读出来，耗时分不清"等到了"与"等到放弃"。
 */

#include "threadpool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void busy_wait_ms(double ms) {
	double end = now_ms() + ms;

	while (now_ms() < end) {
		/* 空转，模拟一段 CPU 工作 */
	}
}

static void sleep_ms(long ms) {
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

/*
 * 等"正在执行的任务数"达到 want，最多 timeout_ms。返回 0 表示等到了。
 *
 * 这里用来构造前置条件（"占位任务已被工作线程取走"），不是性能断言：
 * 轮询而不是固定 sleep，是因为固定 sleep 只能保证"至少过去这么久"，
 * 不能保证状态已经变化，负载重时会偶发地读到旧状态。
 */
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

/* 任务参数。每个任务独占一个，主线程在 wait 之后读取结果。 */
typedef struct {
	int    index;
	double work_ms;
	long long checksum;   /* 任务写入：index 的累加，用于证明任务确实跑过 */
} job;

static void job_entry(void *arg) {
	job *j = (job *)arg;

	if (j->work_ms > 0.0) {
		busy_wait_ms(j->work_ms);
	}
	j->checksum = (long long)j->index * (long long)j->index + 1;
}

/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
	printf("用法：%s <命令> [选项]\n\n", prog);
	printf("命令：\n");
	printf("  run     [-t 线程数] [-n 任务数] [-w 每任务忙等毫秒]       用线程池执行\n");
	printf("  bounded [-t 线程数] [-c 队列上限] [-n 任务数] [-w 毫秒]   有界队列 + 阻塞提交\n");
	printf("  timeout [-c 队列上限] [-w 占住线程毫秒] [-m 超时毫秒]     有界队列 + 带截止时间的提交\n");
	printf("  spawn   [-n 任务数] [-w 每任务忙等毫秒]                   每任务新建线程（对照）\n");
	printf("  help                                                      显示本页\n\n");
	printf("默认：-t 4 -c 8 -n 16 -w 0\n");
	printf("timeout 的默认：-c 2 -w 100 -m 30（固定 1 个工作线程，要求 -m < -w）\n");
}

static int parse_int(const char *s, int *out) {
	char *end = NULL;
	long  v   = strtol(s, &end, 10);

	if (end == s || *end != '\0' || v < 0 || v > 100000000L) {
		return -1;
	}
	*out = (int)v;
	return 0;
}

static int parse_double(const char *s, double *out) {
	char  *end = NULL;
	double v   = strtod(s, &end);

	if (end == s || *end != '\0' || v < 0.0) {
		return -1;
	}
	*out = v;
	return 0;
}

/* ------------------------------------------------------------------ */

static int cmd_run(int nthreads, int ntasks, double work_ms) {
	threadpool *p    = NULL;
	job        *jobs = NULL;
	double      t0;
	double      t1;
	int         rc   = 1;

	p = NULL;
	if (threadpool_create(nthreads, &p) != TPOOL_OK) {
		(void)fprintf(stderr, "threadpool_create 失败\n");
		return 1;
	}

	jobs = (job *)calloc((size_t)ntasks, sizeof *jobs);
	if (jobs == NULL) {
		(void)fprintf(stderr, "calloc 失败\n");
		threadpool_destroy(p);
		return 1;
	}

	printf("线程池：%d 个工作线程，提交 %d 个任务，每任务忙等 %.2f ms\n",
	       nthreads, ntasks, work_ms);

	t0 = now_ms();
	for (int i = 0; i < ntasks; i++) {
		jobs[i].index   = i;
		jobs[i].work_ms = work_ms;
		if (threadpool_submit(p, job_entry, &jobs[i]) != TPOOL_OK) {
			(void)fprintf(stderr, "第 %d 个任务提交失败\n", i);
			goto done;
		}
	}
	(void)threadpool_wait(p);
	t1 = now_ms();

	/* 抽查结果：任务确实执行过，且拿到的是自己的参数 */
	{
		int bad = 0;

		for (int i = 0; i < ntasks; i++) {
			long long want = (long long)i * (long long)i + 1;

			if (jobs[i].checksum != want) {
				bad++;
			}
		}
		printf("校验：%d 个任务结果正确，%d 个不正确\n", ntasks - bad, bad);
		if (bad != 0) {
			goto done;
		}
	}

	printf("submitted=%llu completed=%llu queued=%zu running=%zu\n",
	       threadpool_submitted(p),
	       threadpool_completed(p),
	       threadpool_queued(p),
	       threadpool_running(p));
	printf("墙钟耗时 %.1f ms（串行工作量为 %.1f ms）\n",
	       t1 - t0, (double)ntasks * work_ms);
	rc = 0;

done:
	threadpool_destroy(p);
	free(jobs);
	return rc;
}

/*
 * 有界队列版本：与 cmd_run 用同一份任务函数与工作量，区别只有两点 ——
 * 创建时给队列上限，提交用阻塞式接口。
 *
 * 输出的 capacity / backpressure 是这条路径唯一的可观测证据：
 * 只报耗时的话，"提交端被背压挡住"与"任务本身慢"看起来完全一样。
 */
static int cmd_bounded(int nthreads, int capacity, int ntasks, double work_ms) {
	threadpool *p    = NULL;
	job        *jobs = NULL;
	double      t0;
	double      t1;
	int         rc   = 1;

	if (threadpool_create_bounded(nthreads, (size_t)capacity, &p) != TPOOL_OK) {
		(void)fprintf(stderr, "threadpool_create_bounded 失败\n");
		return 1;
	}

	jobs = (job *)calloc((size_t)ntasks, sizeof *jobs);
	if (jobs == NULL) {
		(void)fprintf(stderr, "calloc 失败\n");
		threadpool_destroy(p);
		return 1;
	}

	printf("有界线程池：%d 个工作线程，队列上限 %d，提交 %d 个任务，每任务忙等 %.2f ms\n",
	       nthreads, capacity, ntasks, work_ms);

	t0 = now_ms();
	for (int i = 0; i < ntasks; i++) {
		tpool_status st;

		jobs[i].index   = i;
		jobs[i].work_ms = work_ms;
		st = threadpool_submit_wait(p, job_entry, &jobs[i]);
		if (st != TPOOL_OK) {
			(void)fprintf(stderr, "第 %d 个任务提交失败：%s\n",
			              i, threadpool_status_string(st));
			goto done;
		}
	}
	(void)threadpool_wait(p);
	t1 = now_ms();

	{
		int bad = 0;

		for (int i = 0; i < ntasks; i++) {
			long long want = (long long)i * (long long)i + 1;

			if (jobs[i].checksum != want) {
				bad++;
			}
		}
		printf("校验：%d 个任务结果正确，%d 个不正确\n", ntasks - bad, bad);
		if (bad != 0) {
			goto done;
		}
	}

	printf("capacity=%zu backpressure=%llu\n",
	       threadpool_capacity(p), threadpool_backpressure(p));
	printf("submitted=%llu completed=%llu queued=%zu running=%zu\n",
	       threadpool_submitted(p),
	       threadpool_completed(p),
	       threadpool_queued(p),
	       threadpool_running(p));
	printf("墙钟耗时 %.1f ms（串行工作量为 %.1f ms）\n",
	       t1 - t0, (double)ntasks * work_ms);
	rc = 0;

done:
	threadpool_destroy(p);
	free(jobs);
	return rc;
}

/*
 * 有上界的等待：队列满时最多等到一个截止时间。
 *
 * 这个子命令要回答的问题是"背压有没有上界"，而耗时答不了：提交端等了 30 ms
 * 有两种可能——等到 30 ms 才有空位（成功），或者一等等不到、到点放弃（超时）。
 * 两者在墙钟上看只差一点点，所以必须看返回码，以及"任务到底有没有被执行"。
 *
 * 构造：1 个工作线程 + 容量 capacity 的有界队列。先提交一个忙等 hold_ms 的任务，
 * 它抢走唯一的工作线程；再把队列填满。此后队列在 hold_ms 之内不可能出现空位，
 * 因此只要 timeout_ms < hold_ms，wait_until 必然超时——这个结论与机器快慢无关，
 * 只依赖 FIFO：排在队头的 filler 要等工作线程忙完才能执行。
 */
static int cmd_timeout(int nthreads, int capacity, double hold_ms, int timeout_ms) {
	threadpool     *p       = NULL;
	job            *fillers = NULL;
	job             hold;
	job             timed;
	struct timespec deadline;
	tpool_status    st;
	double          waited;
	int             rc = 1;

	/*
	 * 多线程时"队列满"不是稳定状态：空闲的工作线程会立刻取走 filler，
	 * 空位马上出现，超时不成立。这不是接口的限制，是这个演示的构造前提，
	 * 所以在这里显式拒绝，而不是让使用者看到一个不解释的 OK。
	 */
	if (nthreads != 1) {
		(void)fprintf(stderr,
		              "timeout 子命令固定用 1 个工作线程：多于 1 个时空闲线程会立刻"
		              "取走队列里的任务，队列满不成立\n");
		return 2;
	}
	if (hold_ms < 1.0 || hold_ms > 60000.0) {
		(void)fprintf(stderr, "-w 必须在 1 ~ 60000 ms 之间\n");
		return 2;
	}
	if ((double)timeout_ms >= hold_ms) {
		(void)fprintf(stderr,
		              "-m 必须小于 -w（当前 -m %d、-w %.0f）：否则工作线程可能在截止时间"
		              "之前腾出空位，超时不触发\n",
		              timeout_ms, hold_ms);
		return 2;
	}

	if (threadpool_create_bounded(nthreads, (size_t)capacity, &p) != TPOOL_OK) {
		(void)fprintf(stderr, "threadpool_create_bounded 失败\n");
		return 1;
	}

	fillers = (job *)calloc((size_t)capacity, sizeof *fillers);
	if (fillers == NULL) {
		(void)fprintf(stderr, "calloc 失败\n");
		threadpool_destroy(p);
		return 1;
	}

	printf("有界线程池：%d 个工作线程，队列上限 %d\n", nthreads, capacity);
	printf("占住线程的任务忙等 %.2f ms，提交的截止时间 %d ms\n", hold_ms, timeout_ms);

	/* 1) 占住唯一的工作线程，并等它真的被取走 —— 不然下面"队列空着"就不成立 */
	hold.index    = 0;
	hold.work_ms  = hold_ms;
	hold.checksum = -1;
	if (threadpool_submit(p, job_entry, &hold) != TPOOL_OK) {
		(void)fprintf(stderr, "占住线程的任务提交失败\n");
		goto done;
	}
	if (wait_running(p, 1, 2000) != 0) {
		(void)fprintf(stderr, "工作线程没有在 2 s 内取走占位任务，本演示的前提不成立\n");
		goto done;
	}

	/*
	 * 2) 填满队列。此刻占位任务已被取走、队列为空，所以正好放得下 capacity 个
	 *    filler；一个也不多提交，输出的计数才是确定的。
	 *
	 *    为什么能确定"在截止时间之前不会腾出空位"：FIFO 决定队头的 filler 要等
	 *    占位任务忙完才轮到，而占位任务要忙 hold_ms > timeout_ms。
	 */
	for (int i = 0; i < capacity; i++) {
		fillers[i].index    = i + 1;
		fillers[i].work_ms  = 0.0;
		fillers[i].checksum = -1;
		if (threadpool_submit(p, job_entry, &fillers[i]) != TPOOL_OK) {
			(void)fprintf(stderr, "第 %d 个 filler 提交失败（队列提前满了）\n", i);
			goto done;
		}
	}
	printf("队列：queued=%zu capacity=%zu\n",
	       threadpool_queued(p), threadpool_capacity(p));

	/* 3) 带截止时间的提交。时刻由模块算，避免与条件变量的时钟错配 */
	if (threadpool_deadline_after(&deadline, (long)timeout_ms) != TPOOL_OK) {
		(void)fprintf(stderr, "threadpool_deadline_after 失败\n");
		goto done;
	}

	timed.index    = -1;
	timed.work_ms  = 0.0;
	timed.checksum = -1;   /* 保持 -1 = 这个任务从未执行 */

	{
		double t0 = now_ms();

		st = threadpool_submit_wait_until(p, job_entry, &timed, &deadline);
		waited = now_ms() - t0;
	}

	if (st == TPOOL_ERR_TIMEOUT) {
		printf("结果：队列已满，等待 %.1f ms 后超时，任务未被接受\n", waited);
	} else if (st == TPOOL_OK) {
		printf("结果：等到空位，任务已被接受（等待 %.1f ms）\n", waited);
	} else {
		printf("结果：%s（等待 %.1f ms）\n", threadpool_status_string(st), waited);
	}

	printf("capacity=%zu backpressure=%llu timeouts=%llu\n",
	       threadpool_capacity(p),
	       threadpool_backpressure(p),
	       threadpool_timeouts(p));
	printf("submitted=%llu completed=%llu queued=%zu running=%zu\n",
	       threadpool_submitted(p),
	       threadpool_completed(p),
	       threadpool_queued(p),
	       threadpool_running(p));

	/* 4) 排空：hold 与 filler 全部执行，超时的那个任务一个字节都不该动过 */
	(void)threadpool_wait(p);

	printf("排空后 completed=%llu，超时的任务 %s\n",
	       threadpool_completed(p),
	       (timed.checksum == -1) ? "从未执行" : "被执行了（不应发生）");

	if (st == TPOOL_ERR_TIMEOUT && timed.checksum == -1) {
		rc = 0;
	} else {
		(void)fprintf(stderr,
		              "本应超时却%s；把 -m 调小或 -w 调大再试\n",
		              (st == TPOOL_OK) ? "成功了" : "返回了别的状态");
	}

done:
	threadpool_destroy(p);
	free(fillers);
	return rc;
}

/* 对照组：每个任务 pthread_create 一个线程，join 之后才算完成 */
/*
 * pthread_create 要求入口是 void *(*)(void *)，而 job_entry 是 void (*)(void *)。
 * 用函数指针强转把两者混用是未定义行为：参数与返回值的传递约定可能不同，
 * 标准也没有规定强转之后调用必须成功。这里加一层真正的适配函数。
 */
static void *spawn_entry(void *arg) {
	job_entry(arg);
	return NULL;
}

static int cmd_spawn(int ntasks, double work_ms) {
	pthread_t *tids = NULL;
	job       *jobs = NULL;
	double     t0;
	double     t1;
	int        rc = 0;

	if (ntasks < 1) {
		(void)fprintf(stderr, "任务数必须 >= 1\n");
		return 1;
	}

	tids = (pthread_t *)calloc((size_t)ntasks, sizeof *tids);
	jobs = (job *)calloc((size_t)ntasks, sizeof *jobs);
	if (tids == NULL || jobs == NULL) {
		(void)fprintf(stderr, "calloc 失败\n");
		free(tids);
		free(jobs);
		return 1;
	}

	printf("对照：%d 个任务，每个新建一个线程，每任务忙等 %.2f ms\n", ntasks, work_ms);

	t0 = now_ms();
	for (int i = 0; i < ntasks; i++) {
		jobs[i].index   = i;
		jobs[i].work_ms = work_ms;
		if (pthread_create(&tids[i], NULL, spawn_entry, &jobs[i]) != 0) {
			(void)fprintf(stderr, "pthread_create 失败（第 %d 个）\n", i);
			rc = 1;
			break;
		}
	}
	for (int i = 0; i < ntasks; i++) {
		(void)pthread_join(tids[i], NULL);
	}
	t1 = now_ms();

	printf("墙钟耗时 %.1f ms\n", t1 - t0);
	free(tids);
	free(jobs);
	return rc;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
	const char *cmd        = (argc > 1) ? argv[1] : "help";
	int         nthreads   = 4;
	int         ntasks     = 16;
	int         capacity   = 8;
	int         timeout_ms = 30;
	double      work_ms    = 0.0;

	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		usage(argv[0]);
		return 0;
	}

	/*
	 * timeout 的默认值单独给：它的 -w 不是"每个任务的忙等"而是"占住线程的时长"，
	 * 默认 0 会让演示失去意义（工作线程立刻腾出空位，永远不会超时）。
	 */
	if (strcmp(cmd, "timeout") == 0) {
		nthreads = 1;
		capacity = 2;
		work_ms  = 100.0;
	}

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			if (parse_int(argv[++i], &nthreads) != 0) {
				(void)fprintf(stderr, "-t 需要非负整数\n");
				return 2;
			}
		} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			if (parse_int(argv[++i], &ntasks) != 0) {
				(void)fprintf(stderr, "-n 需要非负整数\n");
				return 2;
			}
		} else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
			if (parse_double(argv[++i], &work_ms) != 0) {
				(void)fprintf(stderr, "-w 需要非负数\n");
				return 2;
			}
		} else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			if (parse_int(argv[++i], &capacity) != 0) {
				(void)fprintf(stderr, "-c 需要非负整数\n");
				return 2;
			}
		} else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
			if (parse_int(argv[++i], &timeout_ms) != 0) {
				(void)fprintf(stderr, "-m 需要非负整数\n");
				return 2;
			}
		} else {
			(void)fprintf(stderr, "无法识别的选项：%s\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	if (strcmp(cmd, "run") == 0) {
		if (nthreads < 1) {
			(void)fprintf(stderr, "-t 必须 >= 1\n");
			return 2;
		}
		if (ntasks < 1) {
			(void)fprintf(stderr, "-n 必须 >= 1\n");
			return 2;
		}
		return cmd_run(nthreads, ntasks, work_ms);
	}
	if (strcmp(cmd, "bounded") == 0) {
		if (nthreads < 1) {
			(void)fprintf(stderr, "-t 必须 >= 1\n");
			return 2;
		}
		if (capacity < 1) {
			(void)fprintf(stderr, "-c 必须 >= 1\n");
			return 2;
		}
		if (ntasks < 1) {
			(void)fprintf(stderr, "-n 必须 >= 1\n");
			return 2;
		}
		return cmd_bounded(nthreads, capacity, ntasks, work_ms);
	}
	if (strcmp(cmd, "timeout") == 0) {
		if (capacity < 1) {
			(void)fprintf(stderr, "-c 必须 >= 1\n");
			return 2;
		}
		if (capacity > 65536) {
			(void)fprintf(stderr, "-c 最大 65536（演示用不到更多）\n");
			return 2;
		}
		return cmd_timeout(nthreads, capacity, work_ms, timeout_ms);
	}
	if (strcmp(cmd, "spawn") == 0) {
		return cmd_spawn(ntasks, work_ms);
	}

	(void)fprintf(stderr, "未知命令：%s\n\n", cmd);
	usage(argv[0]);
	return 2;
}
