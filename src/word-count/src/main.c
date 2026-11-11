/*
 * main.c —— 命令行驱动
 *
 * 这一层只负责参数解析、任务分发、输出排版与退出码，
 * 所有统计逻辑都在 wordcount 模块里。把这层单独分出来，
 * 是为了让模块可以被别的程序链接使用，而不必连带一个 main。
 *
 * 两档执行方式：
 *
 *   顺序（默认）  主线程按 argv 顺序逐个处理文件。
 *   并行（-j N）  一个共享队列 + N 个工作线程。队列里的任务单元是
 *                 "某个文件的一个字节区间"，而不是"一个文件"：
 *                 大于等于 2 MiB 的普通文件按分隔符对齐切成若干段，
 *                 每段一个任务；其余文件整份作为一个任务。
 *
 * 为什么任务单元是字节区间而不是文件：按文件分发时，总时长由最大的那个文件
 * 决定——其余工作线程把小块文件做完就空转。任务单元细化到区间之后，
 * 大文件的时间被摊到所有线程上，而队列是 FIFO 的，剩下的问题只是
 * "哪个任务排在前面"，见下面关于提交顺序的说明。
 *
 * 两条硬性约束，它们是"并行输出可以直接和串行输出做 cmp"的前提：
 *
 *   1. 输出顺序 = argv 顺序，与任务完成顺序无关。每个任务只写自己那个槽位，
 *      主线程等全部任务结束后再按 argv 顺序统一打印。若让任务自己 printf，
 *      输出顺序就取决于哪个区间先读完，结果是同一批文件每次运行的行序都可能不同。
 *   2. 错误也在 argv 顺序上报告。errno 是线程局部的（POSIX.1-2008 规定 errno
 *      为 thread-local），任务返回之后主线程读到的是自己的 errno，与工作线程无关，
 *      因此错误号必须在任务函数内部取值并存进槽位。
 *
 * 提交顺序按任务字节数从大到小（LPT，longest processing time first）。
 * 理由不是"大的先做看起来公平"，而是列表调度的完工时间上界：m 个同构工作线程、
 * 队列 FIFO 时，把任务按耗时降序提交，makespan 不超过最优值的 4/3 - 1/(3m)。
 * 反过来按 argv 顺序提交时，末尾若剩下一个大任务，其余线程只能空转等它，
 * 最坏情况退化成单线程（那正是"按文件分发"的老问题，只是从文件级挪到了区间级）。
 * 实现上不需要调度器：区间长度已经等价于耗时，一次 qsort 就够。
 *
 * 第三条约束来自资源：切开的文件要让它的区间任务共用一个 fd（一次
 * fstat 定尺寸、多次 pread 读），这个 fd 于是从规划阶段活到任务结束，
 * 同时打开的文件数就等于可切文件数。文件多到撞上 RLIMIT_NOFILE 时，
 * 超出的文件退化成整文件任务而不是报错退出——配额（fd_budget）在开工前
 * 按软上限算出，见 fd_budget_init。
 *
 * 为什么并发放在驱动层而不是模块里：libwordcount.a 的使用者不一定需要并发，
 * 更不一定愿意链接 pthread。并发属于"怎么调度 I/O 与 CPU"，不属于"怎么数词"，
 * 所以 wordcount.h 里没有任何线程相关的东西。
 */

#include "wordcount.h"
#include "threadpool.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/* 输出列宽：与 wc 保持一致的观感 */
#define COL_WIDTH 8

/* -j 的上限。只为挡住把机器拖死的输入，不是性能建议。 */
#define MAX_JOBS 256

/*
 * 切分的三个常量。
 *
 * MIN_CHUNK_BYTES 既是"要不要切"的门槛，也是每段的大小下限：
 * 建池、逐段 pread、切点对齐扫描都是固定开销，段太小就摊不平。
 * 低于 2 MiB 的文件不切——一次顺序 fread 本来就只有几毫秒。
 *
 * CHUNKS_PER_WORKER 让段数多于线程数：各段耗时不可能完全均匀
 * （词长分布不同），段数只有线程数时，最慢的那段决定总时长。
 * 它是"每个可切文件"的段数上限，不是整个队列的任务数上限：
 * 后者天然被"每段不小于 1 MiB"这条下限约束住。
 *
 * 对齐读取的预算分两层：每个建议切点的预算是"段长的 1/4"，再受
 * ALIGN_SCAN_BYTES 封顶。
 *
 * wc_align_chunk 从建议切点往后逐字节找第一个分隔符，找不到就一直在读，
 * 直到它收到的 limit。若把 limit 传成文件末尾，那么当整段内容落在同一个
 * 超长单词里时（基因序列、单行 minified JSON），每个建议切点都会读到文件
 * 末尾才放弃：nchunks-1 个切点合计读 (nchunks-1) 遍文件，之后还要顺序再读
 * 一遍。这不是"读两遍"——-j 16（64 段）时是 31.5 遍（见 README 6.5 的实测）。
 *
 * 所以每次对齐只给一个有界预算：预算内没有分隔符就放弃这个切点。
 * 放弃切点不影响正确性：被接受的边界仍然是"≥ 建议切点的第一个分隔符"，
 * 因此两两单调，只是段数变少（3.7 节）。
 *
 * 为什么预算不能是一个固定的字节数：对齐读取发生在 (nchunks-1) 个切点上，
 * 总量随段数线性增长，而段数随 -j 线性增长。固定 1 MiB 时，64 MiB 文件、
 * -j 16（64 段）的 63 个切点各读 1 MiB，合计 63 MiB，占文件大小的 98%，
 * 全部花在"找分隔符"上，而这些字节紧接着还要被计数再读一遍（6.5 节实测）。
 * 把预算改成与段长成比例，总量就有了一条与 -j、与段数都无关的上界：
 *
 *     Σ budget_i ≤ Σ chunk_i / 4 = size / 4
 *
 * 这个不等式只用到 min(x, ALIGN_SCAN_BYTES) ≤ x，不依赖段长是否均匀，
 * 也不依赖有多少个切点被放弃（放弃只减少读取）。正常文本碰不到这条上界
 * ——分隔符平均每 5~6 字节出现一次，每个切点读几个字节；它挡住的是
 * "整段落在一个超长单词里"这类输入。
 *
 * 取 1/4 的理由：对齐读取是纯附加 I/O，被它读过的字节随后还要被计数再读
 * 一遍，所以它直接抬高总 I/O 量。1/4 意味着最坏情况下 I/O 量是文件大小的
 * 1.25 倍；6.5 节实测这个最坏情况（64 MiB 全字母、-j 16）是 169 ms 对顺序
 * 154 ms，而固定 1 MiB 的预算是 213 ms。这个除数是取舍而不是推导：再收紧
 * 就会在分隔符稀疏的正常输入（行很长的日志、CSV）上白白丢切点、损失并行度。
 * 96 KiB 一行的输入在 256 KiB 预算下仍不丢切点，1 MiB 一行的全放弃（6.6 节）。
 *
 * ALIGN_SCAN_BYTES 仍然保留，作用是让"段本来就很大"（低 -j、大文件）时
 * 预算不至于跟着段长线性膨胀：400 MiB / -j 8 时段长 12.5 MiB，比例预算
 * 会是 3.1 MiB，而这个文件按 1 MiB 就足够找到分隔符了。
 */
#define MIN_CHUNK_BYTES   (1024 * 1024)
#define CHUNKS_PER_WORKER 4
#define ALIGN_SCAN_BYTES  (1024 * 1024)
#define ALIGN_BUDGET_DIV  4

/*
 * 预算为 0 时每个切点都会被立刻放弃、切分静默退化成顺序路径。段长下限
 * 保证预算至少有 MIN_CHUNK_BYTES / ALIGN_BUDGET_DIV 字节，但这个关系
 * 编译器不会替人检查，改上面两个常量时容易踩空。
 */
_Static_assert(MIN_CHUNK_BYTES / ALIGN_BUDGET_DIV > 0,
               "段长下限除以预算除数必须是正数");

/*
 * fd 配额 = RLIMIT_NOFILE 的软上限 - 预留量，其中预留量包含了
 * "同时进行的整文件任务"这一项（见 fd_budget_init）。这里记的是
 * 除它以外的固定部分。
 */
#define FD_RESERVE_EXTRA 4

/* 规划阶段的 fd 配额：不是"文件数上限"，而是"同时能保留几个切分 fd" */
typedef struct {
	long long limit;    /* RLIMIT_NOFILE 的软上限；-1 表示无上限 */
	int       reserve;  /* 从上限里扣掉的预留量 */
	int       budget;   /* 最多能同时持有几个切分 fd */
	int       held;     /* 当前已持有几个 */
	int       degraded; /* 因预算用尽而放弃切分的文件数 */
} fd_budget;

/*
 * 算出这次的配额。
 *
 * 预留量的构成（这就是"为什么不是随便填个常数"）：
 *
 *   jobs 个  整文件任务在各自线程里 fopen 一个文件。整文件任务不占切分配额
 *            （它们是任务里打开、读完就关），但它们与切分任务同时进行，
 *            最坏情况是 jobs 个工作线程各持一个。漏掉这一项时，
 *            -j 16 配 ulimit -n 24 会让整文件任务的 open 拿到 EMFILE，
 *            于是"配额算得没错、程序照样出错"。
 *   3 个     stdin / stdout / stderr。
 *   1 个     规划阶段的临时打开：预算耗尽之后仍要 open + fstat + close 才知道
 *            文件多大（LPT 的权重需要这个值），这一步不能在配额用光时失败。
 *
 * 也就是说 held + jobs + 3 + 1 <= limit 恒成立，这是本函数唯一要保证的性质。
 */
static void fd_budget_init(fd_budget *b, int jobs) {
	struct rlimit rl;

	b->limit    = -1;
	b->reserve  = 0;
	b->budget   = 0;
	b->held     = 0;
	b->degraded = 0;

	if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
		/*
		 * 取不到上限时按"一个都不保留"处理。这个方向是保守的：切分只是
		 * 加速手段，退化之后结果仍然正确；反过来假定上限很大，就会在真正
		 * 受限的环境里把配额用光，把"慢一点"变成"跑不完"。
		 */
		return;
	}

	b->reserve = jobs + FD_RESERVE_EXTRA;

	/*
	 * rlim_t 是无符号类型，RLIM_INFINITY 就是它的全 1 值，不能直接参与
	 * 减法后再转 int（会变成 -1 或者巨大正数）。两者都归到 INT_MAX：
	 * 上限大到这个程度时，配额已经不是约束，"上限不可达"就够了。
	 */
	if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > (rlim_t)INT_MAX) {
		b->budget = INT_MAX;
		return;
	}

	b->limit = (long long)rl.rlim_cur;

	if (rl.rlim_cur > (rlim_t)b->reserve) {
		b->budget = (int)rl.rlim_cur - b->reserve;
	} else {
		/*
		 * 上限连预留量都不够：一个切分 fd 都不保留，全部文件走整文件任务。
		 * 这里不能取负数——负的 budget 会让 held >= budget 恒真（行为正确）
		 * 但表达含糊，而且打印出来是错的。
		 */
		b->budget = 0;
	}
}

static void print_stat(const wc_stat *st, const char *label);

/* ------------------------------------------------------------------
 * 顺序路径
 * ------------------------------------------------------------------ */

/*
 * 主线程自己按 argv 顺序处理全部文件。
 * 没有给 -j、只给 -j 1、或者读标准输入时走这里。
 */
static int run_sequential(const char *const *paths, int nfiles, wc_stat *sum) {
	wc_stat st;
	int     failed = 0;

	for (int i = 0; i < nfiles; i++) {
		wc_status status = wc_count_file(paths[i], &st);
		int       saved  = errno;

		if (status != WC_OK) {
			(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
			              paths[i], wc_status_string(status), strerror(saved));
			failed = 1;
			continue;
		}

		print_stat(&st, paths[i]);
		sum->lines += st.lines;
		sum->words += st.words;
		sum->bytes += st.bytes;
	}

	return failed;
}

/* ------------------------------------------------------------------
 * 计划：把一个文件规划成若干个字节区间
 * ------------------------------------------------------------------ */

/* 一个字节区间 [from, to)，长度就是它的调度权重 */
typedef struct {
	off_t from;
	off_t to;
} seg_range;

/*
 * 一个文件的计划与状态。
 *
 * fd 只在"这个文件真的要切"时保持打开：切分期间所有段共用一个 fd，
 * 靠 pread 的"不动偏移"保证互不干扰，同时避免"规划时 fstat 的 inode 与
 * 执行时打开的 inode 不是同一个"这个 TOCTOU 窗口。代价是同时打开的文件数
 * 等于可切文件数，因此需要一份配额（fd_budget）；配额用尽的文件退化成
 * 整文件任务（见 fd_budget_init）。
 */
typedef struct {
	const char *path;
	int         fd;         /* 可切文件持有的 fd；否则 -1 */
	seg_range  *ranges;     /* 段区间，nranges 个；0 表示整文件一个任务 */
	int         nranges;
	int         first;      /* 该文件第一个任务在 tasks[] 里的下标 */
	int         ntask;      /* 该文件的任务数；0 表示计划失败 */
	off_t       size;       /* 普通文件的 st_size，其余为 0 */
	int         known_size; /* size 是否有效（决定自检与调度权重） */
	int         aborted;    /* 计划阶段已打印错误，报告阶段不再输出 */
	wc_status   status;     /* WC_OK 或计划阶段的失败状态 */
	int         saved_errno;
} file_entry;

/*
 * 规划一个文件：
 *   nranges >= 2  → 切成这么多段，fd 保持打开；
 *   nranges == 0  → 整文件一个任务，按路径顺序读，fd 已关闭。
 *
 * 判定条件是"普通文件、大小不小于 2 MiB、且 -j >= 2"，任何一条不满足
 * 都退回整文件任务。管道与终端不可定位；目录与设备文件的 st_size
 * 没有"统计开始时"这个确定值（fstat 的 st_size 对目录的含义是实现相关的），
 * 对它们做定长切分没有意义。
 *
 * 还有一条外部条件：fb 里剩下的配额。配额用尽时同样退回整文件任务，
 * 理由见 FD_RESERVE 的说明。
 */
static void plan_file(file_entry *e, int jobs, fd_budget *fb) {
	struct stat sb;
	off_t       size;
	int         nchunks;
	off_t       chunk_bytes;
	off_t       align_budget;
	off_t       prev = 0;
	int         nranges = 0;
	int         skipped = 0;
	long long   align_read = 0;
	long long   max_scan   = 0;
	seg_range  *ranges = NULL;
	int         fd;

	e->status      = WC_OK;
	e->saved_errno = 0;
	e->fd          = -1;
	e->size        = 0;
	e->known_size  = 0;

	fd = open(e->path, O_RDONLY);
	if (fd < 0) {
		e->status      = WC_ERR_OPEN;
		e->saved_errno = errno;
		return;
	}

	/*
	 * 用 fstat 而不是 stat：要切的是已经打开的这个 fd，判定和读取必须
	 * 针对同一个对象，否则中间被换成别的文件时判断与读取对不上。
	 */
	if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
		(void)close(fd);
		return;
	}

	size           = sb.st_size;
	e->size        = size;
	e->known_size  = 1;

	if (size < (off_t)(2 * MIN_CHUNK_BYTES) || jobs < 2) {
		(void)close(fd);
		return;
	}

	/*
	 * fd 配额用尽：不再尝试切分，直接退化成整文件任务。
	 *
	 * 这个判断必须在对齐扫描之前。扫描的产出是一组区间，而区间任务只能
	 * 靠"规划阶段留下的那个 fd"来读（TASK_RANGE 走 pread），没有 fd 就
	 * 用不上；先把切点算出来再发现留不住 fd，扫描读的那些字节全是白读。
	 *
	 * 计数只统计"因为配额而没切"的文件，所以判定放在这里而不是放在
	 * 后面 ranges 算完之后——那样会把"对齐失败退回一段"的文件也算进来，
	 * 混淆两个不同的原因。
	 */
	if (fb->held >= fb->budget) {
		fb->degraded++;
		(void)close(fd);
		return;
	}

	/*
	 * 段数先按"每 MiB 一段"估，再压到 jobs * CHUNKS_PER_WORKER 以内：
	 * 段数超过线程数太多时，每段的对齐扫描与任务分派会开始占可观比例。
	 * 这条上限逐文件计算，因此整个队列的任务数上限是"可切文件数 × jobs × 4"，
	 * 实际还会被"每段不小于 1 MiB"压得更低。
	 */
	nchunks = (int)(size / MIN_CHUNK_BYTES);
	if (nchunks > jobs * CHUNKS_PER_WORKER) {
		nchunks = jobs * CHUNKS_PER_WORKER;
	}
	if (nchunks < 2) {
		(void)close(fd);
		return;
	}

	ranges = (seg_range *)calloc((size_t)nchunks, sizeof *ranges);
	if (ranges == NULL) {
		(void)fprintf(stderr, "wordcount: %s: 分配 %d 个切点失败\n", e->path, nchunks);
		(void)close(fd);
		e->aborted = 1;
		return;
	}

	/*
	 * 每个建议切点的对齐读取预算 = min(段长 / 4, ALIGN_SCAN_BYTES)。
	 *
	 * suggested 是 (size / nchunks) * k，所以各段长度相等，
	 * 预算在循环外算一次就够。最后一段可能短一点，但那一段没有切点，
	 * 不参与对齐扫描。
	 */
	chunk_bytes  = size / nchunks;
	align_budget = chunk_bytes / ALIGN_BUDGET_DIV;

	if (align_budget > ALIGN_SCAN_BYTES) {
		align_budget = ALIGN_SCAN_BYTES;
	}

	/*
	 * 先除后乘：size * k 在超大文件上可能溢出 off_t（有符号 64 位的上限
	 * 是 8 EiB，虽然现在没有这种盘，但溢出是未定义行为，不该留在这里）。
	 */
	for (int k = 1; k < nchunks; k++) {
		off_t     suggested = (size / nchunks) * k;
		off_t     scan_to;
		off_t     aligned = size;
		long long scan;
		wc_status st;

		/*
		 * 预算终点用减法算，不写 suggested + align_budget：
		 * 后者在 suggested 接近 off_t 上限时会进位成负数（C11 6.5p5，
		 * 有符号整数溢出是未定义行为），而 size - suggested 恒不为负。
		 */
		scan_to = (size - suggested > align_budget)
		              ? suggested + align_budget
		              : size;

		st = wc_align_chunk(fd, suggested, scan_to, &aligned);
		if (st != WC_OK) {
			int saved = errno;
			(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
			              e->path, wc_status_string(st), strerror(saved));
			free(ranges);
			(void)close(fd);
			e->aborted = 1;
			return;
		}

		/*
		 * 对齐扫描读掉的就是 aligned - suggested：wc_align_chunk 从 suggested
		 * 开始逐字节往后找，找到分隔符就停，没找到就停在 limit。
		 * 这个量在测试里是可断言的（预算的界），比拿耗时当判据稳。
		 */
		scan = (long long)(aligned - suggested);
		align_read += scan;

		/*
		 * aligned == scan_to 表示 [suggested, scan_to) 里一个分隔符都没有。
		 * scan_to == size 时是"一直到文件末尾都没有切点"，scan_to < size 时
		 * 是"预算用完"，两种情况的处理相同：放弃这个切点，它所在的那一段
		 * 并进前一段，右边界由下一个被接受的切点或文件末尾给出。
		 *
		 * 这里不能改成"把 aligned 夹紧到上一个边界"：那会造出一个零长度段，
		 * 看上去无害，却把"库返回了 limit"这个事实抹掉了，与 3.7 节拒绝
		 * 夹紧是同一个理由——错误必须暴露，不能被就地修正。
		 */
		if (aligned == scan_to) {
			skipped++;
			continue;
		}

		/*
		 * 只统计"被接受的切点"的扫描距离。被放弃的那一次必然读满预算，
		 * 拿它做样本只会让最大值恒等于预算，把"这个文件的切点离分隔符有多远"
		 * 这个真正要量的东西盖住。
		 */
		if (scan > max_scan) {
			max_scan = scan;
		}

		/*
		 * 边界是"筛"出来的：被放弃的建议切点不占位置，因此每接受一个
		 * 就当场闭合一段。accepted 的边界两两单调（suggested 每步前进
		 * 一个段长，远大于预算），所以这里不会出现零长度段；仍然判一次，
		 * 是因为"长度为正"是下面 ntasks 计数的前提。
		 */
		if (aligned > prev) {
			ranges[nranges].from = prev;
			ranges[nranges].to   = aligned;
			nranges++;
			prev = aligned;
		}
	}

	if (size > prev) {
		ranges[nranges].from = prev;
		ranges[nranges].to   = size;
		nranges++;
	}

	/*
	 * 可观测性开关：设了 WORDCOUNT_TRACE_SPLIT 就把切分结果打到 stderr。
	 *
	 * 需要它的原因很具体——tests/check_split.sh 比对的是"切分输出 = 顺序输出"，
	 * 而这条断言在切分根本没有发生时也成立（文件太小、不是普通文件都会退回
	 * 整文件任务）。不把"切了几段"变成可观测量，测试就会在什么都没测到的
	 * 情况下报绿。不加命令行选项是避免污染正常输出契约：默认情况下
	 * 两种方式的 stdout 与 stderr 仍然完全一致。
	 *
	 * 第五轮起多打三项：建议切点个数、被放弃的个数、对齐扫描读了多少字节。
	 * 第六轮再打每个切点的预算，第七轮再打"接受切点最长扫描"——它们是三条
	 * 互相独立的策略量，注入验证里会分别失败（5.10 节）。
	 *
	 * 第八轮去掉了行尾的"N 个工作线程"：工作线程数现在是整个队列的属性
	 * （min(-j, 总任务数)），不再是单个文件的属性，它移到调度行里。
	 */
	if (getenv("WORDCOUNT_TRACE_SPLIT") != NULL) {
		(void)fprintf(stderr,
		              "wordcount: 切分 %s：%d 段（建议切点 %d 个，接受 %d 个，"
		              "放弃 %d 个），对齐扫描读 %lld 字节，预算 %lld 字节/切点，"
		              "接受切点最长扫描 %lld 字节\n",
		              e->path, nranges, nchunks - 1, nranges - 1, skipped,
		              align_read, (long long)align_budget, max_scan);
	}

	/*
	 * 只剩一段时切了也没有并行度。这时退回整文件任务：结果一样，
	 * 但少一份区间任务与一个长期打开的 fd。
	 */
	if (nranges < 2) {
		free(ranges);
		(void)close(fd);
		return;
	}

	e->fd      = fd;
	e->ranges  = ranges;
	e->nranges = nranges;
	fb->held++;
}

/* ------------------------------------------------------------------
 * 统一调度：区间任务 + LPT 提交顺序
 * ------------------------------------------------------------------ */

enum {
	TASK_WHOLE = 0,  /* 整文件，任务里按路径重新打开并用 stdio 顺序读 */
	TASK_RANGE = 1   /* 一个字节区间，用规划阶段留下的 fd 做 pread */
};

/*
 * 一个任务槽位。每个任务只写自己那一格，主线程在 threadpool_wait
 * 返回之后才读，因此不需要额外的同步原语——这是让 TSan 保持干净的关键：
 * 并发代码不出问题的最省事写法，是根本不让它共享可写状态。
 */
typedef struct {
	const char *path;
	int         kind;
	int         fd;
	int         file;        /* 所属文件在 entries[] 里的下标 */
	int         seg;         /* 文件内的段号，0 起 */
	off_t       from;
	off_t       to;
	int         submitted;   /* 是否真的进了队列（或内联执行） */
	wc_status   status;
	int         saved_errno; /* 在任务线程内部取 errno，理由见文件头 */
	wc_stat     stat;
} seg_task;

static void task_exec(seg_task *t) {
	if (t->kind == TASK_RANGE) {
		t->status = wc_count_fd_range(t->fd, t->from, t->to, &t->stat);
	} else {
		t->status = wc_count_file(t->path, &t->stat);
	}

	/*
	 * errno 必须在调用线程里取值。wc_count_* 的约定是失败时保留 errno，
	 * 但"保留"只对同一条线程有意义：主线程的 errno 是它自己的副本，
	 * 工作线程里发生的 ENOENT 不会传过来。
	 */
	t->saved_errno = errno;
}

/* 线程池的任务签名是 void (*)(void *)，参数就是该任务独占的槽位 */
static void task_thunk(void *arg) {
	task_exec((seg_task *)arg);
}

/*
 * LPT 比较函数：字节数降序；相等时按 (文件下标, 段号) 升序。
 *
 * 第二级比较不是装饰：qsort 不保证稳定，若只比长度，等长的任务之间的
 * 相对次序由实现决定（glibc 的归并排序与 musl 的平滑排序结果就不同），
 * 于是同一次输入在不同 libc 上的提交顺序不同，排障时无法复现。
 */
static int cmp_task_weight(const void *a, const void *b) {
	const seg_task *ta = *(const seg_task *const *)a;
	const seg_task *tb = *(const seg_task *const *)b;
	off_t           wa = ta->to - ta->from;
	off_t           wb = tb->to - tb->from;

	if (wa != wb) {
		return (wa > wb) ? -1 : 1;
	}
	if (ta->file != tb->file) {
		return (ta->file < tb->file) ? -1 : 1;
	}
	if (ta->seg != tb->seg) {
		return (ta->seg < tb->seg) ? -1 : 1;
	}
	return 0;
}

/*
 * 并行路径：先规划（决定每个文件怎么切），再建一条共享队列，
 * 按任务字节数降序提交，最后按 argv 顺序输出。
 *
 * 规划与执行分开是为了让任务总数在执行前已知：线程数取
 * min(jobs, 任务数)，任务数少于线程数时多出来的线程一个任务都取不到，
 * 只是白占一块线程栈和一份调度实体。
 */
static int run_parallel(const char *const *paths, int nfiles, int jobs, wc_stat *sum) {
	file_entry *entries;
	seg_task   *tasks  = NULL;
	seg_task  **order  = NULL;
	threadpool *pool   = NULL;
	fd_budget   fdb;
	int         ntasks = 0;
	int         nwhole = 0;
	int         pos    = 0;
	int         failed = 0;

	entries = (file_entry *)calloc((size_t)nfiles, sizeof *entries);
	if (entries == NULL) {
		(void)fprintf(stderr, "wordcount: 分配 %d 个文件的计划槽位失败\n", nfiles);
		return 1;
	}

	fd_budget_init(&fdb, jobs);

	for (int i = 0; i < nfiles; i++) {
		entries[i].path = paths[i];
		plan_file(&entries[i], jobs, &fdb);

		if (entries[i].aborted) {
			failed = 1;
			continue;
		}
		if (entries[i].status != WC_OK) {
			continue;      /* 打不开：错误在报告阶段按 argv 顺序打印 */
		}

		entries[i].ntask = (entries[i].nranges > 0) ? entries[i].nranges : 1;
		ntasks += entries[i].ntask;
		if (entries[i].nranges == 0) {
			nwhole++;
		}
	}

	/*
	 * fd 配额是第九轮新增的可观测量，理由与调度行相同（5.9 / 5.10 / 5.11
	 * 三个假通过都出在"某条路径根本没被走到，断言却照样成立"）：一条
	 * "输出与顺序一致"的断言在配额根本没生效时也成立，只有把"可用几个、
	 * 实际持有几个、几个被降级"打出来，测试才能区分
	 * "配额生效并优雅退化"与"配额压根没被算过"。
	 *
	 * 没有文件需要 fd 时（held 与 degraded 都为 0）这一行不打印：
	 * tests/check_split.sh 有一条"小于 2 MiB 的文件不该产生任何记录"，
	 * 靠的就是"根本没走切分"时 stderr 为空。无条件打印会把这层断言降级成
	 * "有一行与切分无关的记录"，而它检验的正是"切分路径没被走到"。
	 */
	if (getenv("WORDCOUNT_TRACE_SPLIT") != NULL &&
	    (fdb.held > 0 || fdb.degraded > 0)) {
		if (fdb.limit < 0) {
			(void)fprintf(stderr,
			              "wordcount: 文件描述符配额：软上限 无上限，已持有 %d 个，"
			              "因配额用尽放弃切分的文件 %d 个\n",
			              fdb.held, fdb.degraded);
		} else {
			(void)fprintf(stderr,
			              "wordcount: 文件描述符配额：软上限 %lld、预留 %d、可用 %d 个，"
			              "已持有 %d 个，因配额用尽放弃切分的文件 %d 个\n",
			              fdb.limit, fdb.reserve, fdb.budget, fdb.held, fdb.degraded);
		}
	}

	if (ntasks > 0) {
		tasks = (seg_task *)calloc((size_t)ntasks, sizeof *tasks);
		order = (seg_task **)calloc((size_t)ntasks, sizeof *order);
		if (tasks == NULL || order == NULL) {
			(void)fprintf(stderr, "wordcount: 分配 %d 个任务槽位失败\n", ntasks);
			ntasks = 0;
			failed = 1;
			goto report;
		}
	}

	for (int i = 0; i < nfiles; i++) {
		file_entry *e = &entries[i];

		if (e->ntask == 0) {
			continue;
		}

		e->first = pos;

		for (int k = 0; k < e->ntask; k++) {
			seg_task *t = &tasks[pos];

			t->path = e->path;
			t->file = i;
			t->seg  = k;

			if (e->nranges > 0) {
				t->kind = TASK_RANGE;
				t->fd   = e->fd;
				t->from = e->ranges[k].from;
				t->to   = e->ranges[k].to;
			} else {
				t->kind = TASK_WHOLE;
				t->fd   = -1;
				t->from = 0;
				t->to   = e->size;
			}

			order[pos] = t;
			pos++;
		}
	}

	if (ntasks >= 2) {
		int          workers = (jobs < ntasks) ? jobs : ntasks;
		tpool_status st      = threadpool_create(workers, &pool);

		if (st != TPOOL_OK) {
			/*
			 * 不静默退回串行。使用者写了 -j 8 就是期望并行，悄悄改成单线程之后
			 * 输出完全正常、只是耗时对不上，这种"能跑但不符合预期"的状态
			 * 比直接报错难查得多。
			 */
			(void)fprintf(stderr, "wordcount: 无法创建 %d 个工作线程的线程池：%s\n",
			              workers, threadpool_status_string(st));
			failed = 1;
			goto report;
		}

		qsort(order, (size_t)ntasks, sizeof *order, cmp_task_weight);

		/*
		 * 调度行是第八轮新增的可观测量。原因与 5.9 / 5.10 记的两个假通过
		 * 同源：多文件 + 切分这条路径若根本没被走到（每个文件都退化成一个
		 * 整文件任务），"输出与顺序一致"照样成立。只有把"任务数 > 文件数"
		 * 和"首个任务有多大"打出来，测试才能断言切分真的发生了、
		 * 并且确实按 LPT 排过序。
		 */
		if (getenv("WORDCOUNT_TRACE_SPLIT") != NULL) {
			(void)fprintf(stderr,
			              "wordcount: 调度：共 %d 个任务（其中整文件任务 %d 个），"
			              "工作线程 %d 个，首个任务 %lld 字节，按任务字节数降序提交\n",
			              ntasks, nwhole, workers,
			              (long long)(order[0]->to - order[0]->from));
		}

		for (int k = 0; k < ntasks; k++) {
			st = threadpool_submit(pool, task_thunk, order[k]);
			if (st != TPOOL_OK) {
				(void)fprintf(stderr, "wordcount: 提交第 %d/%d 个任务失败：%s\n",
				              k + 1, ntasks, threadpool_status_string(st));
				failed = 1;
				break;
			}
			order[k]->submitted = 1;
		}

		/*
		 * threadpool_wait 返回时，所有已提交的任务都已执行完毕。
		 * 这个等待在池内部的互斥量上完成（工作线程在持锁状态下把 running 减一），
		 * 因此它与任务写槽位的动作之间构成 happens-before 关系：
		 * 主线程从这里往后读槽位，不需要再加任何同步。
		 */
		(void)threadpool_wait(pool);
		threadpool_destroy(pool);
		pool = NULL;
	} else if (ntasks == 1) {
		/*
		 * 只有一个任务时不建池：建池 + 一次 wait + destroy 要几十微秒，
		 * 而"在别的线程上做"没有任何收益。内联执行与提交执行走的是同一个
		 * task_exec，所以两条路的结果不会分叉。
		 */
		tasks[0].submitted = 1;
		task_exec(&tasks[0]);
	}

report:
	for (int i = 0; i < nfiles; i++) {
		file_entry *e = &entries[i];
		wc_stat     st = {0, 0, 0};
		int         bad = 0;

		if (e->aborted) {
			failed = 1;
			continue;
		}

		if (e->status != WC_OK) {
			/*
			 * WC_ERR_OPEN 保留 errno（open 设置的），用 strerror 给出具体原因。
			 * WC_ERR_PARAM 不会出现在这里：它只由编程错误（传 NULL）触发，
			 * 而 path 由本层保证有效。
			 */
			(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
			              e->path, wc_status_string(e->status),
			              strerror(e->saved_errno));
			failed = 1;
			continue;
		}

		if (ntasks == 0) {
			/* 任务槽位没分配出来，上面已经报过错 */
			continue;
		}

		for (int k = 0; k < e->ntask; k++) {
			seg_task *t = &tasks[e->first + k];

			if (!t->submitted) {
				bad = 1;      /* 提交在它之前就失败了 */
				break;
			}
			if (t->status == WC_OK) {
				continue;
			}

			/*
			 * saved_errno 为 0 表示不是系统调用失败，而是这段在读到
			 * 终点之前就碰到了文件末尾——文件在统计期间被截短。
			 * 这种情况不能拿 strerror(0) 说成 "Success"。
			 * 这条说明只对切开的文件成立：整文件任务走 stdio 读到 EOF，
			 * 没有"这段该有多少字节"这个先验，仍按老样子报 errno。
			 */
			if (t->saved_errno == 0 && e->nranges > 0) {
				(void)fprintf(stderr,
				              "wordcount: %s: %s: 第 %d/%d 段在读到第 %lld 字节之前就遇到文件末尾\n",
				              e->path, wc_status_string(t->status),
				              k + 1, e->ntask, (long long)t->to);
			} else {
				(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
				              e->path, wc_status_string(t->status),
				              strerror(t->saved_errno));
			}
			bad = 1;
			break;
		}

		if (bad) {
			failed = 1;
			continue;
		}

		for (int k = 0; k < e->ntask; k++) {
			seg_task *t = &tasks[e->first + k];

			st.lines += t->stat.lines;
			st.words += t->stat.words;
			st.bytes += t->stat.bytes;
		}

		/*
		 * 最后一道自检：逐段相加的字节数必须等于 fstat 报告的 st_size。
		 * 各段都返回 OK 时理论上必然相等，写在这里是为了让"文件在统计期间
		 * 被改短"这件事宁可报错，也不静默输出一个偏小的结果。
		 * 整文件任务走的是 stdio 读到 EOF，没有这个已知长度，跳过。
		 */
		if (e->nranges > 0 && e->known_size &&
		    st.bytes != (unsigned long)e->size) {
			(void)fprintf(stderr,
			              "wordcount: %s: 统计期间文件大小发生变化（预期 %lld 字节，读到 %lu 字节）\n",
			              e->path, (long long)e->size, st.bytes);
			failed = 1;
			continue;
		}

		print_stat(&st, e->path);
		sum->lines += st.lines;
		sum->words += st.words;
		sum->bytes += st.bytes;
	}

	for (int i = 0; i < nfiles; i++) {
		if (entries[i].fd >= 0) {
			(void)close(entries[i].fd);
		}
		free(entries[i].ranges);
	}

	free(order);
	free(tasks);
	free(entries);

	return failed;
}

/* ------------------------------------------------------------------
 * 输出
 * ------------------------------------------------------------------ */

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
	    "            不小于 2 MiB 的普通文件按分隔符对齐切成若干段，每段一个任务；\n"
	    "            其余文件整份一个任务。所有任务进同一条队列，按字节数从大到小提交，\n"
	    "            因此大文件不会把完工时间拖到最后。切点落在词中间时退到下一个\n"
	    "            分隔符，单词不会被切开；一个切点在 min(段长/4, 1 MiB) 的对齐预算内\n"
	    "            都找不到分隔符时放弃该切点（那一段并进前一段）。\n"
	    "  -h        显示本说明\n"
	    "\n"
	    "没有 FILE 参数时读标准输入（只有一条流，-j 对它无效）。\n"
	    "多个文件时末尾补一行 total。\n"
	    "两种方式（顺序 / 并行）的输出逐字节相同（含错误信息与退出码）。\n"
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
			wc_stat st = {0, 0, 0};

			wc_status status = wc_count_stream(stdin, &st);
			if (status != WC_OK) {
				(void)fprintf(stderr, "wordcount: (stdin): %s: %s\n",
				              wc_status_string(status), strerror(errno));
				return 1;
			}

			/* 标准输入只有一份，-j 对它没有意义，直接走同一套输出逻辑 */
			print_stat(&st, NULL);
			return 0;
		}

		/*
		 * -j 1 等价于顺序：一个工作线程的"并行"只是多一次入队与出队，
		 * 而且顺序路径不必为每个文件多开一个区间任务。
		 */
		if (jobs >= 2) {
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
