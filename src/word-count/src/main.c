/*
 * main.c —— 命令行驱动
 *
 * 这一层只负责参数解析、任务分发、输出排版与退出码，
 * 所有统计逻辑都在 wordcount 模块里。把这层单独分出来，
 * 是为了让模块可以被别的程序链接使用，而不必连带一个 main。
 *
 * 两种执行方式：
 *
 *   顺序（默认）  主线程按 argv 顺序逐个处理文件。
 *   并行（-j N）  每个文件作为一个任务提交给 thread-pool 的线程池，
 *                 由至多 N 个工作线程同时处理。
 *
 * 并行模式有两条硬性约束，它们是"并行输出可以直接和串行输出做 cmp"的前提：
 *
 *   1. 输出顺序 = argv 顺序，与任务完成顺序无关。每个任务只写自己那个槽位，
 *      主线程等全部任务结束后再按 argv 顺序统一打印。若让任务自己 printf，
 *      输出顺序就取决于哪个文件先读完，结果是同一批文件每次运行的行序都可能不同。
 *   2. 错误也在 argv 顺序上报告。errno 是线程局部的（POSIX.1-2008 规定 errno
 *      为 thread-local），任务返回之后主线程读到的是自己的 errno，与工作线程无关，
 *      因此错误号必须在任务函数内部取值并存进槽位。
 *
 * 为什么并发放在驱动层而不是模块里：libwordcount.a 的使用者不一定需要并发，
 * 更不一定愿意链接 pthread。并发属于"怎么调度 I/O 与 CPU"，不属于"怎么数词"，
 * 所以 wordcount.h 里没有任何线程相关的东西。
 */

#include "wordcount.h"
#include "threadpool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 输出列宽：与 wc 保持一致的观感 */
#define COL_WIDTH 8

/* -j 的上限。只为挡住把机器拖死的输入，不是性能建议。 */
#define MAX_JOBS 256

/*
 * 一个文件的处理槽位。
 *
 * 槽位数等于文件数，第 i 个槽位只由处理第 i 个文件的那个任务写，
 * 主线程在 threadpool_wait 返回之后才读。槽位之间没有共享，
 * 因此不需要任何额外的同步原语——这是让 TSan 保持干净的关键：
 * 并发代码不出问题的最省事写法，是根本不让它共享可写状态。
 */
typedef struct {
	const char *path;        /* NULL 表示标准输入 */
	wc_status   status;      /* 统计结果的状态码 */
	int         saved_errno; /* 失败时的 errno，在任务线程内部取值 */
	wc_stat     stat;        /* 三个计数 */
} file_slot;

static void print_stat(const wc_stat *st, const char *label);

/*
 * 处理一个输入源，把结果写进槽位。
 * slot->path 为 NULL 时从 stream 读（标准输入），否则按路径读文件。
 */
static void count_one(file_slot *slot, FILE *stream) {
	if (slot->path != NULL) {
		slot->status = wc_count_file(slot->path, &slot->stat);
	} else {
		slot->status = wc_count_stream(stream, &slot->stat);
	}

	/*
	 * errno 必须在调用线程里取值。wc_count_file 的约定是失败时保留 errno，
	 * 但"保留"只对同一条线程有意义：主线程的 errno 是它自己的副本，
	 * 工作线程里发生的 ENOENT 不会传过来。
	 */
	slot->saved_errno = errno;
}

/* 线程池的任务签名是 void (*)(void *)，参数就是该任务独占的槽位 */
static void count_task(void *arg) {
	count_one((file_slot *)arg, NULL);
}

/*
 * 按 argv 顺序输出一个槽位的结果，并累加到 sum。
 * 成功返回 0，失败返回 1。
 */
static int report_slot(const file_slot *slot, wc_stat *sum) {
	if (slot->status != WC_OK) {
		/*
		 * WC_ERR_OPEN / WC_ERR_READ / WC_ERR_CLOSE 三种情况都会保留 errno，
		 * 用 strerror 给出具体原因。WC_ERR_PARAM 不会出现在这里：
		 * 它只由编程错误（传 NULL）触发，而 path 与 stream 都由本层保证有效。
		 */
		(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
		              (slot->path != NULL) ? slot->path : "(stdin)",
		              wc_status_string(slot->status),
		              strerror(slot->saved_errno));
		return 1;
	}

	print_stat(&slot->stat, slot->path);

	/* 累加总计数。大文件时 unsigned long 不会溢出，直接相加即可。 */
	sum->lines += slot->stat.lines;
	sum->words += slot->stat.words;
	sum->bytes += slot->stat.bytes;

	return 0;
}

/*
 * 顺序路径：主线程自己处理全部文件。
 * 只有一个文件、或者没有给 -j 时走这里。
 */
static int run_sequential(const char *const *paths, int nfiles, wc_stat *sum) {
	file_slot slot;
	int       failed = 0;

	for (int i = 0; i < nfiles; i++) {
		memset(&slot, 0, sizeof slot);
		slot.path = paths[i];

		count_one(&slot, NULL);

		if (report_slot(&slot, sum) != 0) {
			failed = 1;
		}
	}

	return failed;
}

/*
 * 并行路径：每个文件一个任务，全部完成后按 argv 顺序汇总输出。
 * 提交失败时只汇总已经提交出去的任务，未提交的槽位不会被当成"统计成功"打印。
 */
static int run_parallel(const char *const *paths, int nfiles, int jobs, wc_stat *sum) {
	threadpool  *pool = NULL;
	file_slot   *slots;
	tpool_status st;
	int          workers;
	int          submitted = 0;
	int          failed    = 0;

	/*
	 * 工作线程数取 min(jobs, 文件数)。任务数少于线程数时，
	 * 多出来的工作线程一个任务都取不到，只是白占一块线程栈和一份调度实体。
	 */
	workers = (jobs < nfiles) ? jobs : nfiles;

	slots = (file_slot *)calloc((size_t)nfiles, sizeof *slots);
	if (slots == NULL) {
		(void)fprintf(stderr, "wordcount: 分配 %d 个结果槽位失败\n", nfiles);
		return 1;
	}

	st = threadpool_create(workers, &pool);
	if (st != TPOOL_OK) {
		/*
		 * 不静默退回串行。使用者写了 -j 8 就是期望并行，悄悄改成单线程之后
		 * 输出完全正常、只是耗时对不上，这种"能跑但不符合预期"的状态
		 * 比直接报错难查得多。
		 */
		(void)fprintf(stderr, "wordcount: 无法创建 %d 个工作线程的线程池：%s\n",
		              workers, threadpool_status_string(st));
		free(slots);
		return 1;
	}

	for (int i = 0; i < nfiles; i++) {
		slots[i].path = paths[i];

		st = threadpool_submit(pool, count_task, &slots[i]);
		if (st != TPOOL_OK) {
			(void)fprintf(stderr, "wordcount: 提交第 %d 个文件的任务失败：%s\n",
			              i, threadpool_status_string(st));
			failed = 1;
			break;
		}
		submitted++;
	}

	/*
	 * threadpool_wait 返回时，所有已提交的任务都已执行完毕。
	 * 这个等待在池内部的互斥量上完成（工作线程在持锁状态下把 running 减一），
	 * 因此它与任务写槽位的动作之间构成 happens-before 关系：
	 * 主线程从这里往后读槽位，不需要再加任何同步。
	 */
	(void)threadpool_wait(pool);
	threadpool_destroy(pool);

	for (int i = 0; i < submitted; i++) {
		if (report_slot(&slots[i], sum) != 0) {
			failed = 1;
		}
	}

	free(slots);
	return failed;
}

static void print_stat(const wc_stat *st, const char *label) {
	if (label == NULL) {
		(void)printf("%*lu %*lu %*lu\n",
		             COL_WIDTH, st->lines,
		             COL_WIDTH, st->words,
		             COL_WIDTH, st->bytes);
	} else {
		(void)printf("%*lu %*lu %*lu %s\n",
		             COL_WIDTH, st->lines,
		             COL_WIDTH, st->words,
		             COL_WIDTH, st->bytes,
		             label);
	}
}

static void usage(FILE *out, const char *prog) {
	(void)fprintf(out,
	    "Usage: %s [-j JOBS] [FILE...]\n"
	    "\n"
	    "Options:\n"
	    "  -j JOBS   用至多 JOBS 个线程并行处理多个文件（1..%d）。\n"
	    "            工作线程数取 min(JOBS, 文件数)；只给一个文件时仍走顺序路径。\n"
	    "  -h        显示本说明\n"
	    "\n"
	    "没有 FILE 参数时读标准输入。多个文件时末尾补一行 total。\n"
	    "并行与顺序两种方式的输出逐字节相同（含错误信息与退出码）。\n"
	    "\n"
	    "Example:\n"
	    "  %s -j 4 logs/*.log      # 4 个线程并行统计\n"
	    "  cat a.txt | %s          # 从管道读\n",
	    prog, MAX_JOBS, prog, prog);
}

int main(int argc, char *argv[]) {
	int jobs = 0;   /* 0 表示没给 -j，走顺序路径 */
	int opt;

	while ((opt = getopt(argc, argv, "j:h")) != -1) {
		switch (opt) {
		case 'j': {
			char *end = NULL;
			long  value;

			errno = 0;
			value = strtol(optarg, &end, 10);
			if (errno != 0 || end == optarg || *end != '\0' ||
			    value < 1 || value > MAX_JOBS) {
				(void)fprintf(stderr, "wordcount: -j 必须是 1..%d 之间的整数\n",
				              MAX_JOBS);
				usage(stderr, argv[0]);
				return 2;
			}
			jobs = (int)value;
			break;
		}

		case 'h':
			usage(stdout, argv[0]);
			return 0;

		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}

	{
		const char *const *paths  = (const char *const *)(argv + optind);
		int                nfiles = argc - optind;
		wc_stat            sum    = {0, 0, 0};
		int                failed;

		if (nfiles == 0) {
			/* 没有文件名参数时读标准输入，这样可以直接接管道 */
			file_slot slot;

			memset(&slot, 0, sizeof slot);
			count_one(&slot, stdin);

			/* 标准输入只有一份，-j 对它没有意义，直接走同一套输出逻辑 */
			return report_slot(&slot, &sum);
		}

		/*
		 * 只有两个及以上文件才值得建池。-j 1 仍然走并行路径，
		 * 这样"单 worker 的池"这条代码路径也在测试覆盖范围内。
		 */
		if (jobs >= 1 && nfiles >= 2) {
			failed = run_parallel(paths, nfiles, jobs, &sum);
		} else {
			failed = run_sequential(paths, nfiles, &sum);
		}

		/* 多个文件时补一行总计，与 wc 的行为一致 */
		if (nfiles > 1) {
			print_stat(&sum, "total");
		}

		return failed;
	}
}
