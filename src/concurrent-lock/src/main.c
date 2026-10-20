/*
 * main.c —— 并发计数器的命令行驱动
 *
 * 这一层只做三件事：解析参数、开线程、排版输出。
 * 计数逻辑与同步策略都在 counter 模块里。
 *
 * 两种用法：
 *   基准模式（默认）  不加 -s，纯粹量吞吐
 *   演示模式          -s，每次自增后睡 1 ms，把竞态窗口放到肉眼可见
 *
 * 为什么要分两种：加 sleep 之后总耗时由 sleep 决定，完全掩盖了
 * 五种实现之间的差异，没法当性能数据用；但只有加了 sleep，raw 的
 * 丢更新才会稳定复现（不加的话，10 个线程抢一个缓存行，
 * 大部分自增恰好错开，丢的比例很小且不稳定）。
 */

#include "counter.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* 线程数上限。只是为了挡住 -t 1000000 这种把机器拖死的输入。 */
#define MAX_THREADS 4096

typedef struct {
	counter  *counter;
	long long iterations;
	int       sleep_each;
} worker_arg;

static double now_ms(void) {
	struct timespec ts;

	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void sleep_1ms(void) {
	struct timespec req;
	struct timespec rem;

	req.tv_sec  = 0;
	req.tv_nsec = 1000000L;      /* 1 ms */

	while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
		req = rem;
	}
}

static void *worker(void *arg) {
	worker_arg *a = (worker_arg *)arg;

	if (a->sleep_each) {
		for (long long i = 0; i < a->iterations; i++) {
			(void)counter_add(a->counter, 1);
			sleep_1ms();
		}
	} else {
		for (long long i = 0; i < a->iterations; i++) {
			(void)counter_add(a->counter, 1);
		}
	}

	return NULL;
}

/*
 * 跑一轮：建计数器、起线程、等全部结束、读最终值。
 * 成功返回 0，失败返回非 0 并把原因写到 stderr。
 */
static int run_round(counter_kind kind, int threads, long long per_thread,
                     int sleep_each, long long *final_value, double *elapsed_ms) {
	pthread_t  *tids;
	worker_arg *args;
	counter    *c = NULL;
	counter_status status;
	double      start;
	int         started = 0;
	int         rc = 0;

	status = counter_create(kind, 0, &c);
	if (status != COUNTER_OK) {
		(void)fprintf(stderr, "counter: 无法创建 %s 计数器：%s\n",
		              counter_kind_name(kind), counter_status_string(status));
		return 1;
	}

	tids = (pthread_t *)calloc((size_t)threads, sizeof *tids);
	args = (worker_arg *)calloc((size_t)threads, sizeof *args);
	if (tids == NULL || args == NULL) {
		(void)fprintf(stderr, "counter: 分配线程数组失败\n");
		free(tids);
		free(args);
		counter_destroy(c);
		return 1;
	}

	/*
	 * 计时必须从"创建第一个线程之前"开始。
	 * pthread_create 一返回，新线程就可能已经在自增了；如果把 start
	 * 放在创建循环之后，创建期间的劳动量就被排除在测量之外，
	 * 线程越多低估得越厉害。
	 */
	start = now_ms();

	for (int i = 0; i < threads; i++) {
		args[i].counter    = c;
		args[i].iterations = per_thread;
		args[i].sleep_each = sleep_each;

		if (pthread_create(&tids[i], NULL, worker, &args[i]) != 0) {
			(void)fprintf(stderr, "counter: 创建第 %d 个线程失败\n", i);
			rc = 1;
			break;
		}
		started++;
	}

	/* 只等真正创建成功的那些线程 */
	for (int i = 0; i < started; i++) {
		(void)pthread_join(tids[i], NULL);
	}

	*elapsed_ms  = now_ms() - start;
	*final_value = counter_get(c);

	free(tids);
	free(args);
	counter_destroy(c);
	return rc;
}

static void usage(FILE *out, const char *prog) {
	(void)fprintf(out,
	    "Usage: %s [-k KIND] [-t THREADS] [-n ITERATIONS] [-r ROUNDS] [-s]\n"
	    "\n"
	    "Options:\n"
	    "  -k KIND        raw|mutex|spin|atomic|asm|all   (default: all)\n"
	    "  -t THREADS     线程数                          (default: 10)\n"
	    "  -n ITERATIONS  每个线程自增次数                (default: 1000000)\n"
	    "  -r ROUNDS      每种实现重复几轮取平均          (default: 3)\n"
	    "  -s             每次自增后 sleep 1ms。注意这是把竞态窗口拉开，\n"
	    "                 让丢更新变得罕见，而不是放大它（见 demo 目标）\n"
	    "  -h             显示本说明\n"
	    "\n"
	    "Example:\n"
	    "  %s -k all -t 10 -n 1000000      # 五种实现的吞吐对比\n"
	    "  %s -k raw -t 10 -n 200 -s       # 演示丢更新\n",
	    prog, prog, prog);
}

/* 解析整数参数，失败时返回 0 并给出一条可读的提示 */
static int parse_long(const char *text, long *out) {
	char *end = NULL;
	long  value;

	errno = 0;
	value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0') {
		return 0;
	}
	*out = value;
	return 1;
}

static int resolve_kind(const char *name, counter_kind *out) {
	for (int i = 0; i < COUNTER_KIND_COUNT; i++) {
		if (strcmp(name, counter_kind_name((counter_kind)i)) == 0) {
			*out = (counter_kind)i;
			return 1;
		}
	}
	return 0;
}

int main(int argc, char *argv[]) {
	counter_kind kind      = COUNTER_KIND_RAW;
	int          run_all   = 1;
	int          threads   = 10;
	long long    per_thread = 1000000;
	int          rounds    = 3;
	int          sleep_each = 0;
	int          opt;

	while ((opt = getopt(argc, argv, "k:t:n:r:sh")) != -1) {
		switch (opt) {
		case 'k':
			if (strcmp(optarg, "all") == 0) {
				run_all = 1;
			} else if (resolve_kind(optarg, &kind)) {
				run_all = 0;
			} else {
				(void)fprintf(stderr, "counter: 未知实现 %s\n", optarg);
				usage(stderr, argv[0]);
				return 2;
			}
			break;

		case 't': {
			long v;

			if (!parse_long(optarg, &v) || v < 1 || v > MAX_THREADS) {
				(void)fprintf(stderr, "counter: 线程数必须在 1..%d 之间\n", MAX_THREADS);
				return 2;
			}
			threads = (int)v;
			break;
		}

		case 'n': {
			long v;

			if (!parse_long(optarg, &v) || v < 1) {
				(void)fprintf(stderr, "counter: 自增次数必须为正整数\n");
				return 2;
			}
			per_thread = (long long)v;
			break;
		}

		case 'r': {
			long v;

			if (!parse_long(optarg, &v) || v < 1 || v > 1000) {
				(void)fprintf(stderr, "counter: 轮数必须在 1..1000 之间\n");
				return 2;
			}
			rounds = (int)v;
			break;
		}

		case 's':
			sleep_each = 1;
			break;

		case 'h':
			usage(stdout, argv[0]);
			return 0;

		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}

	(void)printf("threads=%d  per-thread=%lld  rounds=%d  sleep=%s\n\n",
	             threads, per_thread, rounds, sleep_each ? "on (1ms)" : "off");

	(void)printf("%-8s %14s %14s %6s %12s %10s\n",
	             "kind", "expected", "final", "match", "avg_ms", "Mops/s");
	(void)printf("--------------------------------------------------------------------------\n");

	for (int k = 0; k < COUNTER_KIND_COUNT; k++) {
		counter_kind current = run_all ? (counter_kind)k : kind;
		long long    expected;
		long long    final_sum = 0;
		double       ms_sum    = 0.0;
		int          ok_rounds = 0;
		int          failed    = 0;

		if (!run_all && k > 0) {
			break;
		}
		if (!counter_kind_supported(current)) {
			(void)printf("%-8s %14s %14s %6s %12s %10s\n",
			             counter_kind_name(current), "-", "-", "-",
			             "unsupported", "-");
			continue;
		}

		expected = (long long)threads * per_thread;

		for (int r = 0; r < rounds; r++) {
			long long final_value = 0;
			double    elapsed     = 0.0;

			if (run_round(current, threads, per_thread, sleep_each,
			              &final_value, &elapsed) != 0) {
				failed = 1;
				break;
			}
			final_sum += final_value;
			ms_sum    += elapsed;
			if (final_value == expected) {
				ok_rounds++;
			}
		}

		if (failed != 0) {
			continue;
		}

		{
			long long avg_final = final_sum / rounds;
			double    avg_ms    = ms_sum / (double)rounds;
			double    ops       = (double)expected * (double)rounds;
			double    mops      = (ms_sum > 0.0) ? ops / ms_sum / 1000.0 : 0.0;
			const char *match;

			if (ok_rounds == rounds) {
				match = "yes";
			} else if (ok_rounds == 0) {
				match = "NO";
			} else {
				match = "part";
			}

			(void)printf("%-8s %14lld %14lld %6s %12.2f %10.2f\n",
			             counter_kind_name(current), expected, avg_final,
			             match, avg_ms, mops);
		}
	}

	(void)printf("\n");
	if (sleep_each) {
		(void)printf("注意：sleep 模式下的耗时由 sleep 决定，不能用来比较实现快慢。\n");
	}
	(void)printf("raw 一行的 final 小于 expected 是正常的：它按定义会丢更新。\n");
	(void)printf("match 为 part 说明多轮里有时对有时错，也是 raw 的典型表现。\n");

	return 0;
}
