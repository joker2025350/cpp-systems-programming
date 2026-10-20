/*
 * main.c —— threadpool 模块的命令行驱动
 *
 * 只链接 libthreadpool.a，不重复编译模块源码：这样"这个模块能被独立链接"
 * 在每次构建时都被验证一次。
 *
 * 子命令：
 *   run   [-t 线程数] [-n 任务数] [-w 每个任务的忙等毫秒]
 *   spawn [-n 任务数]        对照组：每个任务新建一个线程
 *   help
 *
 * run 与 spawn 用同一份任务函数与同一份工作量，两者的耗时可以直接比较。
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
	printf("  run   [-t 线程数] [-n 任务数] [-w 每任务忙等毫秒]   用线程池执行\n");
	printf("  spawn [-n 任务数] [-w 每任务忙等毫秒]              每任务新建线程（对照）\n");
	printf("  help                                              显示本页\n\n");
	printf("默认：-t 4 -n 16 -w 0\n");
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
	const char *cmd = (argc > 1) ? argv[1] : "help";
	int         nthreads = 4;
	int         ntasks   = 16;
	double      work_ms  = 0.0;

	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		usage(argv[0]);
		return 0;
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
	if (strcmp(cmd, "spawn") == 0) {
		return cmd_spawn(ntasks, work_ms);
	}

	(void)fprintf(stderr, "未知命令：%s\n\n", cmd);
	usage(argv[0]);
	return 2;
}
