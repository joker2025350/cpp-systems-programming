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
 *   单文件切分    只有一个文件且 -j N（N >= 2）时，把这个文件本身切成若干段，
 *                 每段一个任务。切点必须落在分隔符上，否则段首的词会被
 *                 切成两半、计两次；切点怎么找由 wc_align_chunk 负责。
 *
 * 三种方式共用同一套输出逻辑，因此三种方式的输出必须逐字节相同。
 * 这一点是 tests/check_split.sh 的判据。
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 输出列宽：与 wc 保持一致的观感 */
#define COL_WIDTH 8

/* -j 的上限。只为挡住把机器拖死的输入，不是性能建议。 */
#define MAX_JOBS 256

/*
 * 单文件切分的两个常量。
 *
 * MIN_CHUNK_BYTES 既是"要不要切"的门槛，也是每段的大小下限：
 * 建池、逐段 pread、切点对齐扫描都是固定开销，段太小就摊不平。
 * 低于 2 MiB 的文件不切——一次顺序 fread 本来就只有几毫秒。
 *
 * CHUNKS_PER_WORKER 让段数多于线程数：各段耗时不可能完全均匀
 * （词长分布不同），段数只有线程数时，最慢的那段决定总时长。
 */
#define MIN_CHUNK_BYTES   (1024 * 1024)
#define CHUNKS_PER_WORKER 4

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

/* ------------------------------------------------------------------
 * 单文件切分
 * ------------------------------------------------------------------ */

/*
 * 一个段的处理槽位。与 file_slot 同一个套路：每个任务只写自己那一格，
 * 主线程在 threadpool_wait 之后才读，因此不需要额外同步。
 */
typedef struct {
	int         fd;          /* 所有段共用同一个 fd，靠 pread 的"不动偏移"保证互不干扰 */
	off_t       from;        /* 段起点，0 或某个分隔符的偏移 */
	off_t       to;          /* 段终点（不含） */
	int         submitted;   /* 这段是否真的提交成了任务 */
	wc_status   status;
	int         saved_errno; /* 在任务线程内部取 errno，理由同 count_one */
	wc_stat     stat;
} chunk_slot;

static void chunk_task(void *arg) {
	chunk_slot *slot = (chunk_slot *)arg;

	slot->status      = wc_count_fd_range(slot->fd, slot->from, slot->to, &slot->stat);
	slot->saved_errno = errno;
}

/* run_split_file 的返回码 */
enum split_result {
	SPLIT_FAILED = -1,   /* 失败，错误信息已经打印 */
	SPLIT_SKIP   = 0,    /* 不适用，调用方退回顺序路径 */
	SPLIT_DONE   = 1     /* 成功，结果已经累加进 *sum */
};

/*
 * 把单个文件切成若干段并行统计。
 *
 * 只在"普通文件、大小不小于 2 MiB、且切出的段数不少于 2"时才真的并行；
 * 其它情况返回 SPLIT_SKIP，由调用方走顺序路径。管道与终端不可定位，
 * 目录与设备文件的大小没有"统计开始时"这个确定值（fstat 的 st_size
 * 对目录的含义是实现相关的），对它们做定长切分没有意义。
 */
static int run_split_file(const char *path, int jobs, wc_stat *sum) {
	struct stat sb;
	int         fd;
	off_t       size;
	int         nchunks;
	int         workers;
	int         ntasks;
	int         failed = 0;
	off_t      *bounds = NULL;
	chunk_slot *slots  = NULL;
	threadpool *pool   = NULL;
	wc_stat     local  = {0, 0, 0};

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		int saved = errno;
		(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
		              path, wc_status_string(WC_ERR_OPEN), strerror(saved));
		return SPLIT_FAILED;
	}

	/*
	 * 用 fstat 而不是 stat：要切的是已经打开的这个 fd，判定和读取必须
	 * 针对同一个对象，否则中间被换成别的文件时判断与读取对不上。
	 * fstat 失败时不去猜原因，直接退回顺序路径——那里会重新打开、
	 * 报出真正的原因。
	 */
	if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
		(void)close(fd);
		return SPLIT_SKIP;
	}

	size = sb.st_size;
	if (size < 2 * MIN_CHUNK_BYTES) {
		(void)close(fd);
		return SPLIT_SKIP;
	}

	/*
	 * 段数先按"每 MiB 一段"估，再压到 jobs * CHUNKS_PER_WORKER 以内：
	 * 段数超过线程数太多时，每段的对齐扫描与任务分派会开始占可观比例。
	 */
	nchunks = (int)(size / MIN_CHUNK_BYTES);
	if (nchunks > jobs * CHUNKS_PER_WORKER) {
		nchunks = jobs * CHUNKS_PER_WORKER;
	}
	if (nchunks < 2) {
		(void)close(fd);
		return SPLIT_SKIP;
	}

	bounds = (off_t *)calloc((size_t)nchunks + 1, sizeof *bounds);
	slots  = (chunk_slot *)calloc((size_t)nchunks, sizeof *slots);
	if (bounds == NULL || slots == NULL) {
		(void)fprintf(stderr, "wordcount: 分配 %d 个切点失败\n", nchunks);
		failed = 1;
		goto done;
	}

	bounds[0]       = 0;
	bounds[nchunks] = size;

	/*
	 * 先除后乘：size * k 在超大文件上可能溢出 off_t（有符号 64 位的上限
	 * 是 8 EiB，虽然现在没有这种盘，但溢出是未定义行为，不该留在这里）。
	 */
	for (int k = 1; k < nchunks; k++) {
		off_t     suggested = (size / nchunks) * k;
		off_t     aligned   = size;
		wc_status st;

		st = wc_align_chunk(fd, suggested, size, &aligned);
		if (st != WC_OK) {
			int saved = errno;
			(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
			              path, wc_status_string(st), strerror(saved));
			failed = 1;
			goto done;
		}

		/*
		 * 这里不需要"对齐结果必须大于上一段起点"的夹紧。wc_align_chunk 返回的是
		 * "≥ 给定偏移的第一个分隔符"，这个函数对参数单调不减，因此
		 * aligned_k >= aligned_{k-1} 恒成立，边界不会交叉、内容不会被数两次。
		 * 两个建议切点落进同一个超长单词时，两者都会对齐到那个词的结尾，
		 * 于是中间那些段长度为 0——空段在下面被跳过。
		 */
		bounds[k] = aligned;
	}

	ntasks = 0;
	for (int k = 0; k < nchunks; k++) {
		slots[k].fd   = fd;
		slots[k].from = bounds[k];
		slots[k].to   = bounds[k + 1];
		if (bounds[k] < bounds[k + 1]) {
			ntasks++;
		}
	}

	/*
	 * 可观测性开关：设了 WORDCOUNT_TRACE_SPLIT 就把切分结果打到 stderr。
	 *
	 * 需要它的原因很具体——tests/check_split.sh 比对的是"切分输出 = 顺序输出"，
	 * 而这条断言在切分根本没有发生时也成立（文件太小、不是普通文件都会退回
	 * 顺序路径）。不把"切了几段"变成可观测量，测试就会在什么都没测到的
	 * 情况下报绿。不加命令行选项是避免污染正常输出契约：默认情况下
	 * 三种方式的 stdout 与 stderr 仍然完全一致。
	 */
	if (getenv("WORDCOUNT_TRACE_SPLIT") != NULL) {
		(void)fprintf(stderr, "wordcount: 切分 %s：%d 段，%d 个工作线程\n",
		              path, ntasks, (jobs < ntasks) ? jobs : ntasks);
	}

	/*
	 * 所有切点都被同一个单词吃掉时只有一段，切了也没有并行度。
	 * 这时退回顺序路径：结果一样，但少建一次池。
	 */
	if (ntasks < 2) {
		(void)close(fd);
		free(bounds);
		free(slots);
		return SPLIT_SKIP;
	}

	workers = (jobs < ntasks) ? jobs : ntasks;

	tpool_status st = threadpool_create(workers, &pool);
	if (st != TPOOL_OK) {
		(void)fprintf(stderr, "wordcount: 无法创建 %d 个工作线程的线程池：%s\n",
		              workers, threadpool_status_string(st));
		failed = 1;
		goto done;
	}

	for (int k = 0; k < nchunks; k++) {
		/* 空段不提交：任务函数什么都不做，但分派本身要约 600 ns */
		if (slots[k].from >= slots[k].to) {
			continue;
		}

		st = threadpool_submit(pool, chunk_task, &slots[k]);
		if (st != TPOOL_OK) {
			(void)fprintf(stderr, "wordcount: %s: 提交第 %d/%d 段的任务失败：%s\n",
			              path, k + 1, ntasks, threadpool_status_string(st));
			failed = 1;
			break;
		}
		slots[k].submitted = 1;
	}

	(void)threadpool_wait(pool);

	if (!failed) {
		for (int k = 0; k < nchunks; k++) {
			if (!slots[k].submitted) {
				continue;
			}
			if (slots[k].status != WC_OK) {
				/*
				 * saved_errno 为 0 表示不是系统调用失败，而是这段在读到
				 * 终点之前就碰到了文件末尾——文件在统计期间被截短。
				 * 这种情况不能拿 strerror(0) 说成 "Success"。
				 */
				if (slots[k].saved_errno == 0) {
					(void)fprintf(stderr,
					              "wordcount: %s: %s: 第 %d/%d 段在读到第 %lld 字节之前就遇到文件末尾\n",
					              path, wc_status_string(slots[k].status),
					              k + 1, ntasks, (long long)slots[k].to);
				} else {
					(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
					              path, wc_status_string(slots[k].status),
					              strerror(slots[k].saved_errno));
				}
				failed = 1;
				break;
			}

			local.lines += slots[k].stat.lines;
			local.words += slots[k].stat.words;
			local.bytes += slots[k].stat.bytes;
		}
	}

	/*
	 * 最后一道自检：逐段相加的字节数必须等于 fstat 报告的 st_size。
	 * 各段都返回 OK 时理论上必然相等，写在这里是为了让"文件在统计期间
	 * 被改短"这件事宁可报错，也不静默输出一个偏小的结果。
	 */
	if (!failed && local.bytes != (unsigned long)size) {
		(void)fprintf(stderr,
		              "wordcount: %s: 统计期间文件大小发生变化（预期 %lld 字节，读到 %lu 字节）\n",
		              path, (long long)size, local.bytes);
		failed = 1;
	}

done:
	if (pool != NULL) {
		threadpool_destroy(pool);
	}
	(void)close(fd);
	free(bounds);
	free(slots);

	if (failed) {
		return SPLIT_FAILED;
	}

	print_stat(&local, path);
	sum->lines += local.lines;
	sum->words += local.words;
	sum->bytes += local.bytes;

	return SPLIT_DONE;
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
	    "  -j JOBS   用至多 JOBS 个线程并行统计（1..%d）。\n"
	    "            多个文件：每个文件一个任务，工作线程数取 min(JOBS, 文件数)。\n"
	    "            单个文件：把这个普通文件按分隔符对齐切成若干段并行（需 >= 2 MiB），\n"
	    "            切点若落在词中间会退到下一个分隔符，单词不会被切开。\n"
	    "  -h        显示本说明\n"
	    "\n"
	    "没有 FILE 参数时读标准输入。多个文件时末尾补一行 total。\n"
	    "三种方式（顺序 / 多文件并行 / 单文件切分）的输出逐字节相同（含错误信息与退出码）。\n"
	    "\n"
	    "Example:\n"
	    "  %s -j 4 logs/*.log      # 4 个线程并行统计多个文件\n"
	    "  %s -j 8 huge.log        # 单个大文件切成 8 段并行\n"
	    "  cat a.txt | %s          # 从管道读\n",
	    prog, MAX_JOBS, prog, prog, prog);
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
		 * 单文件 + -j：切分这个文件本身。jobs 为 1 时不切——
		 * 一个工作线程的"并行"只是多一次入队与出队，纯属开销。
		 * 多文件时仍走按文件分发，两条路径不叠加：那需要一个带权重的
		 * 共享队列，是另一个设计问题（见 README 9.6）。
		 */
		if (jobs >= 2 && nfiles == 1) {
			int r = run_split_file(paths[0], jobs, &sum);

			if (r == SPLIT_SKIP) {
				/* 不是普通文件、太小，或者整段落在同一个单词里 */
				failed = run_sequential(paths, nfiles, &sum);
			} else {
				failed = (r == SPLIT_FAILED) ? 1 : 0;
			}
		} else if (jobs >= 1 && nfiles >= 2) {
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
