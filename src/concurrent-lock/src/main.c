/*
 * main.c —— 并发计数器的命令行驱动
 *
 * 这一层只做三件事：解析参数、产生并发、排版输出。
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
 *
 * 独立的第二个维度是"谁来产生并发"，用 -p 选择：
 *   线程驱动（默认）  驱动自己 pthread_create 出 threads 个线程，
 *                     每个线程跑一个定长的自增循环。
 *   池驱动（-p）      驱动把工作切成 tasks 个任务提交给 thread-pool
 *                     （../thread-pool 模块），由池里 workers 个工作线程执行，
 *                     每个任务做 batch 次自增。
 *
 * 为什么要这个维度：两种驱动在同一份计数器实现上叠加的额外同步完全不同。
 * 线程驱动除了计数器本身没有任何共享状态，测得的就是"counter_add 的代价"；
 * 池驱动在提交路径上还有一条全局队列与其互斥锁，当任务粒度小到与队列操作
 * 同一量级时，测得的其实是队列而不是计数器。把两种驱动放在一起才能确定
 * 第 6 节那张表的数字在什么粒度下仍然成立——这是基准的适用边界，不是花活。
 */

#include "counter.h"
#include "threadpool.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* 线程数上限。只是为了挡住 -t 1000000 这种把机器拖死的输入。 */
#define MAX_THREADS 4096

/*
 * 池驱动时的队列容量上限。
 *
 * 用有界池（threadpool_create_bounded）+ 阻塞提交（threadpool_submit_wait），
 * 而不是无界池：batch=1 时任务数可以到百万级，每填一个任务节点占
 * sizeof(struct task) = 24 字节，无界队列会把提交速度与消费速度的差
 * 全部变成内存（百万任务 = 24 MB，而且峰值不可预测）。有界队列把内存
 * 固定在 容量 × 24 字节，代价是提交端在队列满时阻塞——这个阻塞本来
 * 就是流水线的一部分，计进耗时是诚实的。
 *
 * 容量默认 4096，用 -q 覆盖。之所以要能改：背压是否真的发生取决于
 * "容量"与"消费速度"的相对关系，容量写死时"背压有没有生效"这件事
 * 只能靠加大任务数去撞，测不出确定性；给小容量（例如 1）才能构造出
 * "提交端必然阻塞"的场景。
 */
#define POOL_QUEUE_CAP 4096

typedef struct {
	counter  *counter;
	long long iterations;
	int       sleep_each;
} worker_arg;

/*
 * 池驱动的任务参数。所有任务共用同一个实例：它只读，不含任何
 * 会被任务函数改写的东西。任务函数不得改动它，池也不会释放它
 * （见 threadpool.h 的所有权约定：arg 归调用方）。
 */
typedef struct {
	counter  *counter;
	long long batch;
	int       sleep_each;
} pool_task_arg;

/* 一轮池驱动结束后从池上读回的三个计数，用于核对"没有任务被丢掉" */
typedef struct {
	unsigned long long submitted;
	unsigned long long completed;
	unsigned long long backpressure;
} pool_stats;

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
 * 池驱动的任务函数：做 batch 次自增。
 *
 * 任务函数签名由 threadpool 规定（void (*)(void *)），没有返回值，
 * 因此自增失败无法上报。这里不需要上报：counter_add 的唯一失败方式是
 * 传入 NULL 句柄，而句柄来自本轮已经创建成功的计数器。
 */
static void pool_task(void *arg) {
	pool_task_arg *a = (pool_task_arg *)arg;

	if (a->sleep_each) {
		for (long long i = 0; i < a->batch; i++) {
			(void)counter_add(a->counter, 1);
			sleep_1ms();
		}
	} else {
		for (long long i = 0; i < a->batch; i++) {
			(void)counter_add(a->counter, 1);
		}
	}
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

/*
 * 池驱动跑一轮：建计数器与线程池、提交 tasks 个任务、等池排空、读最终值。
 *
 * 计时窗口与 run_round 对齐：从"第一个任务提交之前"到"最后一个任务执行完毕"，
 * 包含提交、排队、执行三段的全部时间。提交是单线程串行做的，如果等到提交完
 * 再开始计时，就把生产端的时间从分母里拿掉了，吞吐会被系统性高估。
 *
 * 成功返回 0，失败返回非 0。stats 里的三个计数在成功时才有意义。
 */
static int run_round_pool(counter_kind kind, int workers, size_t queue_cap,
                          long long tasks, long long batch, int sleep_each,
                          long long *final_value, double *elapsed_ms,
                          pool_stats *stats) {
	counter       *c = NULL;
	threadpool    *pool = NULL;
	pool_task_arg  arg;
	counter_status cs;
	tpool_status   ts;
	double         start;
	int            rc = 0;

	stats->submitted    = 0;
	stats->completed    = 0;
	stats->backpressure = 0;

	cs = counter_create(kind, 0, &c);
	if (cs != COUNTER_OK) {
		(void)fprintf(stderr, "counter: 无法创建 %s 计数器：%s\n",
		              counter_kind_name(kind), counter_status_string(cs));
		return 1;
	}

	ts = threadpool_create_bounded(workers, queue_cap, &pool);
	if (ts != TPOOL_OK) {
		(void)fprintf(stderr, "counter: 无法创建 %d 线程的池：%s\n",
		              workers, threadpool_status_string(ts));
		counter_destroy(c);
		return 1;
	}

	arg.counter    = c;
	arg.batch      = batch;
	arg.sleep_each = sleep_each;

	start = now_ms();

	for (long long i = 0; i < tasks; i++) {
		/*
		 * submit_wait 而不是 submit：有界队列，队列满时必须等空位。
		 * 它返回 FULL 只有一种情况——调用者是工作线程，而这里是主线程，
		 * 因此任何非 OK 的返回都是真的出了问题，直接中止本轮。
		 */
		ts = threadpool_submit_wait(pool, pool_task, &arg);
		if (ts != TPOOL_OK) {
			(void)fprintf(stderr, "counter: 第 %lld 个任务提交失败：%s\n",
			              i, threadpool_status_string(ts));
			rc = 1;
			break;
		}
	}

	if (rc == 0) {
		ts = threadpool_wait(pool);
		if (ts != TPOOL_OK) {
			(void)fprintf(stderr, "counter: 等待任务排空失败：%s\n",
			              threadpool_status_string(ts));
			rc = 1;
		}
	}

	*elapsed_ms = now_ms() - start;

	/*
	 * 读最终值必须在 destroy 之前：destroy 会销毁同步原语，
	 * 之后再 counter_get 就是访问已释放对象（C11 6.2.4p2：对象生存期已结束）。
	 * 顺序是先取计数、再取池的统计、最后 destroy 池。
	 */
	*final_value        = counter_get(c);
	stats->submitted    = threadpool_submitted(pool);
	stats->completed    = threadpool_completed(pool);
	stats->backpressure = threadpool_backpressure(pool);

	threadpool_destroy(pool);
	counter_destroy(c);
	return rc;
}

static void usage(FILE *out, const char *prog) {
	(void)fprintf(out,
	    "Usage: %s [-k KIND] [-t N] [-n COUNT] [-b BATCH] [-q CAP] [-r ROUNDS] [-p] [-s]\n"
	    "\n"
	    "驱动方式（-p 决定 -t / -n 的含义）：\n"
	    "  线程驱动（默认）  自己开 -t 个线程，每个线程自增 -n 次\n"
	    "  池驱动（-p）      向 thread-pool 提交 -n 个任务，池里 -t 个工作线程，\n"
	    "                    每个任务自增 -b 次，队列上限 -q 个待执行任务\n"
	    "\n"
	    "Options:\n"
	    "  -k KIND        raw|mutex|spin|atomic|asm|all   (default: all)\n"
	    "  -t N           线程数 / 工作线程数             (default: 10)\n"
	    "  -n COUNT       线程驱动：每线程自增次数        (default: 1000000)\n"
	    "                 池驱动：任务数\n"
	    "  -b BATCH       池驱动的每任务自增次数（默认 1）。\n"
	    "                 0 表示任务体不做事，那一行量的是纯调度开销\n"
	    "  -q CAP         池驱动的队列上限（默认 %d）。提交端在队列满时阻塞\n"
	    "  -r ROUNDS      每种实现重复几轮取平均          (default: 3)\n"
	    "  -p             池驱动，任务提交给 thread-pool\n"
	    "  -s             每次自增后 sleep 1ms。注意这是把竞态窗口拉开，\n"
	    "                 让丢更新变得罕见，而不是放大它（见 demo 目标）\n"
	    "  -h             显示本说明\n"
	    "\n"
	    "Example:\n"
	    "  %s -k all -t 10 -n 1000000      # 五种实现的吞吐对比\n"
	    "  %s -k raw -t 10 -n 200 -s       # 演示丢更新\n"
	    "  %s -k atomic -p -t 8 -n 1000 -b 64   # 池驱动，1000 个任务各 64 次\n",
	    prog, POOL_QUEUE_CAP, prog, prog, prog);
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
	int          use_pool  = 0;
	int          batch     = 1;
	int          batch_given = 0;
	size_t       queue_cap = POOL_QUEUE_CAP;
	int          cap_given = 0;
	int          opt;

	while ((opt = getopt(argc, argv, "k:t:n:b:q:pr:sh")) != -1) {
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
				(void)fprintf(stderr, "counter: 次数必须为正整数\n");
				return 2;
			}
			per_thread = (long long)v;
			break;
		}

		case 'b': {
			long v;

			if (!parse_long(optarg, &v) || v < 0) {
				(void)fprintf(stderr, "counter: 每任务次数必须为非负整数\n");
				return 2;
			}
			if (v > INT_MAX) {
				(void)fprintf(stderr, "counter: 每任务次数过大\n");
				return 2;
			}
			batch       = (int)v;
			batch_given = 1;
			break;
		}

		case 'p':
			use_pool = 1;
			break;

		case 'q': {
			long v;

			/*
			 * 上限取 1..65536。下界 1 是 threadpool_create_bounded 的要求
			 * （max_queued < 1 直接返回 PARAM）；上界只是为了让"队列容量"
			 * 这个参数不至于变成另一个能拖死机器的旋钮。
			 */
			if (!parse_long(optarg, &v) || v < 1 || v > 65536) {
				(void)fprintf(stderr, "counter: 队列上限必须在 1..65536 之间\n");
				return 2;
			}
			queue_cap = (size_t)v;
			cap_given = 1;
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

	/*
	 * -b 只在池驱动下有意义。给了却没用上就报错，而不是静默忽略：
	 * 静默忽略会让"我明明设了每任务 64 次"变成"跑了 100 万个单次任务"，
	 * 输出里的 expected 却仍然自洽，看不出哪里错了。
	 */
	if (batch_given && !use_pool) {
		(void)fprintf(stderr, "counter: -b 只在池驱动（-p）下有效\n");
		return 2;
	}
	if (cap_given && !use_pool) {
		(void)fprintf(stderr, "counter: -q 只在池驱动（-p）下有效\n");
		return 2;
	}

	/*
	 * 期望值 = 线程数 × 每线程次数（线程驱动）或 任务数 × 每任务次数（池驱动）。
	 * 两者都可能超出 long long 的范围，而有符号整数溢出按 C11 6.5p5 是未定义行为，
	 * 不能指望"溢出成一个负数然后比对不相等"。这里提前挡掉。
	 */
	if (!use_pool) {
		if (per_thread > LLONG_MAX / (long long)threads) {
			(void)fprintf(stderr, "counter: 总次数超出 long long 范围\n");
			return 2;
		}
	} else {
		if (batch > 0 && per_thread > LLONG_MAX / (long long)batch) {
			(void)fprintf(stderr, "counter: 总次数超出 long long 范围\n");
			return 2;
		}
	}

	if (use_pool) {
		(void)printf("driver=pool  workers=%d  tasks=%lld  batch=%d  rounds=%d  "
		             "sleep=%s  queue=%zu\n\n",
		             threads, per_thread, batch, rounds,
		             sleep_each ? "on (1ms)" : "off", queue_cap);
	} else {
		(void)printf("threads=%d  per-thread=%lld  rounds=%d  sleep=%s\n\n",
		             threads, per_thread, rounds, sleep_each ? "on (1ms)" : "off");
	}

	(void)printf("%-8s %14s %14s %6s %12s %10s\n",
	             "kind", "expected", "final", "match", "avg_ms", "Mops/s");
	(void)printf("--------------------------------------------------------------------------\n");

	/*
	 * 池的三个计数跨实现、跨轮累加，最后汇总一次。
	 * 声明放在使用处之前（C11 允许块内混排声明与语句），
	 * 而不是提到函数开头：它们只在这个循环里有意义。
	 */
	unsigned long long pool_submitted    = 0;
	unsigned long long pool_completed    = 0;
	unsigned long long pool_backpressure = 0;

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

		if (use_pool) {
			expected = per_thread * (long long)batch;
		} else {
			expected = (long long)threads * per_thread;
		}

		for (int r = 0; r < rounds; r++) {
			long long  final_value = 0;
			double     elapsed     = 0.0;
			pool_stats stats       = { 0, 0, 0 };
			int        round_failed;

			if (use_pool) {
				round_failed = run_round_pool(current, threads, queue_cap,
				                              per_thread, batch, sleep_each,
				                              &final_value, &elapsed, &stats);
			} else {
				round_failed = run_round(current, threads, per_thread, sleep_each,
				                         &final_value, &elapsed);
			}

			if (round_failed != 0) {
				failed = 1;
				break;
			}
			final_sum += final_value;
			ms_sum    += elapsed;
			pool_submitted    += stats.submitted;
			pool_completed    += stats.completed;
			pool_backpressure += stats.backpressure;
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

	/*
	 * 池的三个计数放在表外，一次汇总打印。
	 * submitted 与 completed 相等是"没有任务被丢掉"的直接证据：
	 * 它们由池自己计数，与驱动层算出的 expected 是两条独立路径。
	 */
	if (use_pool) {
		(void)printf("池统计：submitted=%llu completed=%llu backpressure=%llu"
		             "（提交端因队列满阻塞）\n",
		             pool_submitted, pool_completed, pool_backpressure);
		if (batch == 0) {
			(void)printf("batch=0：任务体不做自增，这一行量的是纯调度开销，"
			             "Mops/s 无意义。\n");
		}
	}

	if (sleep_each) {
		(void)printf("注意：sleep 模式下的耗时由 sleep 决定，不能用来比较实现快慢。\n");
	}
	(void)printf("raw 一行的 final 小于 expected 是正常的：它按定义会丢更新。\n");
	(void)printf("match 为 part 说明多轮里有时对有时错，也是 raw 的典型表现。\n");

	return 0;
}
