/*
 * test_split.c —— 单文件切分的正确性测试
 *
 * 要回答的问题只有一个：把 [0, size) 按分隔符对齐切开、各段分别统计、
 * 再把三个计数相加，结果是否与整文件扫描逐字节相同。
 *
 * 为什么这件事必须单独测：段首如果落在词中间，那个词的左半截在上一段
 * （已经被计入）、右半截在本段又被计入，单词数就多算了。这类错误不影响
 * 行数与字节数，只有在"词恰好横跨切点"的输入上才出现，而切点是按文件大小
 * 平均分的——它落在哪里完全取决于文件内容长度，人工很难恰好构造。
 * 因此这里不挑输入，而是把每个输入在所有切段数下都跑一遍。
 *
 * 用例分三类：
 *   1. wc_align_chunk 本身：给定偏移，它该返回哪个偏移；
 *   2. 整段统计与分段统计的一致性（每个输入 × 每种段数）；
 *   3. 错误路径：非法参数、文件在统计期间被截短、空区域。
 *
 * 编译：make test（与 test_wordcount 一起运行）
 */

#include "wordcount.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_checks   = 0;
static int g_failures = 0;

static void expect(const char *name, const wc_stat *got,
                   unsigned long lines, unsigned long words, unsigned long bytes) {
	g_checks++;

	if (got->lines != lines || got->words != words || got->bytes != bytes) {
		g_failures++;
		(void)printf("FAIL  %-46s 实际 (行 %lu, 词 %lu, 字节 %lu)，"
		             "期望 (行 %lu, 词 %lu, 字节 %lu)\n",
		             name,
		             got->lines, got->words, got->bytes,
		             lines, words, bytes);
		return;
	}

	(void)printf("ok    %s\n", name);
}

static void expect_status(const char *name, wc_status got, wc_status want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-46s 状态码 %d，期望 %d\n", name, (int)got, (int)want);
		return;
	}

	(void)printf("ok    %s\n", name);
}

/* 断言两个整数相等，用于检查 wc_align_chunk 的返回值 */
static void expect_off(const char *name, off_t got, off_t want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-46s 实际 %lld，期望 %lld\n",
		             name, (long long)got, (long long)want);
		return;
	}

	(void)printf("ok    %s\n", name);
}

/* ---------------------------------------------------------------- */

/*
 * 把一段字节写成临时文件，返回可读的 fd，路径写进 path。
 * 失败直接退出：测试环境没有可写临时目录时，后面的断言都没有意义。
 */
static int make_temp(const void *data, size_t size, char *path, size_t pathlen) {
	const char *dir = getenv("TMPDIR");

	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}

	(void)snprintf(path, pathlen, "%s/wc_split_XXXXXX", dir);

	int fd = mkstemp(path);
	if (fd < 0) {
		(void)fprintf(stderr, "FATAL 无法创建临时文件：%s\n", path);
		exit(2);
	}

	/* write 可能少写（大文件、信号中断），必须循环到写完 */
	const char *p   = (const char *)data;
	size_t      off = 0;

	while (off < size) {
		ssize_t n = write(fd, p + off, size - off);
		if (n <= 0) {
			(void)fprintf(stderr, "FATAL 临时文件写入失败\n");
			exit(2);
		}
		off += (size_t)n;
	}

	return fd;
}

/*
 * 与驱动层同构的切分：先算建议切点，再对齐，最后逐段相加。
 * 刻意不复用驱动层的代码——驱动层是 main.c 里的 static 函数，
 * 测试与它共用一份实现的话，"算法错了"会同时出现在两边而互相印证不了。
 */
static wc_status chunked_stat(int fd, off_t size, int nchunks, wc_stat *out) {
	off_t bounds[40];

	if (nchunks < 1 || nchunks >= (int)(sizeof bounds / sizeof bounds[0])) {
		return WC_ERR_PARAM;
	}

	bounds[0]       = 0;
	bounds[nchunks] = size;

	for (int k = 1; k < nchunks; k++) {
		off_t     aligned = size;
		wc_status st      = wc_align_chunk(fd, (size / nchunks) * k, size, &aligned);

		if (st != WC_OK) {
			return st;
		}

		/*
		 * 不做任何夹紧。驱动层也不夹紧，理由是 wc_align_chunk 对参数单调不减：
		 * 若这里出现 bounds[k] < bounds[k-1]，那就是库违反了这条性质，
		 * 必须让它在这里暴露成"两段重叠、内容被数两次"，而不是被测试悄悄修正。
		 */
		bounds[k] = aligned;
	}

	wc_stat sum = {0, 0, 0};

	for (int k = 0; k < nchunks; k++) {
		wc_stat part;
		wc_status st = wc_count_fd_range(fd, bounds[k], bounds[k + 1], &part);

		if (st != WC_OK) {
			return st;
		}
		sum.lines += part.lines;
		sum.words += part.words;
		sum.bytes += part.bytes;
	}

	*out = sum;
	return WC_OK;
}

/* ---------------------------------------------------------------- */

/*
 * 第一类：wc_align_chunk 本身。
 *
 * 判据是"返回的偏移处一定不是字母数字"，或者返回的是 limit。
 * 这里用确定的输入把几个具体偏移钉死，防止实现改成"往后找一个词的开头"
 * 之类的等价但不相同的语义——那样也能算出正确结果，却会在段尾多读一段，
 * 属于性能退化而不是错误，只有写死期望值才能发现。
 */
static void test_align(void) {
	const char *text = "abc def ghi";
	const off_t len  = (off_t)strlen(text);   /* 11 */
	char        path[256];
	int         fd = make_temp(text, (size_t)len, path, sizeof path);
	off_t       got;

	/* 0 -> 3（第一个空格），4 -> 7，8 -> 11（没有更多分隔符，返回 limit） */
	expect_status("对齐：0 起点", wc_align_chunk(fd, 0, len, &got), WC_OK);
	expect_off("对齐：0 -> 3", got, 3);

	expect_status("对齐：从 4 开始", wc_align_chunk(fd, 4, len, &got), WC_OK);
	expect_off("对齐：4 -> 7", got, 7);

	expect_status("对齐：从 8 开始", wc_align_chunk(fd, 8, len, &got), WC_OK);
	expect_off("对齐：8 -> 11（无分隔符）", got, 11);

	/* 起点已经是分隔符时原样返回，不能越过它 */
	expect_status("对齐：起点是分隔符", wc_align_chunk(fd, 3, len, &got), WC_OK);
	expect_off("对齐：3 -> 3", got, 3);

	/* 空区间：from == limit，直接返回 limit */
	expect_status("对齐：空区间", wc_align_chunk(fd, len, len, &got), WC_OK);
	expect_off("对齐：空区间 -> limit", got, len);

	/* limit 之内找不到分隔符时绝不能越过 limit 去读 */
	expect_status("对齐：limit 截断", wc_align_chunk(fd, 0, 3, &got), WC_OK);
	expect_off("对齐：limit=3 时 0 -> 3", got, 3);

	/* 一段全是字母数字：整段没有切点 */
	(void)close(fd);
	(void)unlink(path);
}

static void test_align_no_separator(void) {
	const char *text = "abcdefghij";
	const off_t len  = (off_t)strlen(text);
	char        path[256];
	int         fd = make_temp(text, (size_t)len, path, sizeof path);
	off_t       got;

	expect_status("对齐：全字母输入", wc_align_chunk(fd, 0, len, &got), WC_OK);
	expect_off("对齐：全字母 -> limit", got, len);

	expect_status("对齐：文件中段全字母", wc_align_chunk(fd, 2, 8, &got), WC_OK);
	expect_off("对齐：2..8 无分隔符 -> 8", got, 8);

	(void)close(fd);
	(void)unlink(path);
}

/*
 * 第二类：整段与分段的一致性。
 *
 * 每个输入先做整文件统计作为基准，再在 nchunks = 1..上界 下重算一遍，
 * 三者都必须相等。段数上界取到"段比字节还多"，这样可以覆盖
 * 大量空段（切点全被同一批分隔符吃掉）的情形。
 */
static void test_chunked_equals_whole(void) {
	static const struct {
		const char *name;
		const char *text;
	} cases[] = {
		{ "单行两词",        "hello world" },
		{ "五词无换行",      "alpha beta gamma delta epsilon" },
		{ "标点与换行",      "one,two;three.\nfour\nfive" },
		{ "结尾无换行",      "a\nb\nc" },
		{ "连续空行",        "x\n\n\ny" },
		{ "只有分隔符",      "   \n\t ,.;!  " },
		{ "长词交替",        "abcdefghij klmnopqrst uvwxyz0123 9876543210" },
		{ "UTF-8 高位字节",  "\xe4\xb8\xad\xe6\x96\x87 abc \xe7\xbb\x93" },
		{ "词紧贴分隔符",    "a b c d e f g h i j k l m n o p" },
	};

	for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
		const char *text = cases[ci].text;
		size_t      len  = strlen(text);
		char        path[256];
		int         fd = make_temp(text, len, path, sizeof path);

		wc_stat whole;
		expect_status("整文件统计", wc_count_file(path, &whole), WC_OK);

		int maxchunks = (int)len + 2;
		if (maxchunks > 20) {
			maxchunks = 20;
		}

		for (int k = 1; k <= maxchunks; k++) {
			wc_stat   got;
			wc_status st = chunked_stat(fd, (off_t)len, k, &got);
			char      name[96];

			(void)snprintf(name, sizeof name, "%s · %d 段", cases[ci].name, k);

			if (st != WC_OK) {
				expect_status(name, st, WC_OK);
				continue;
			}
			expect(name, &got, whole.lines, whole.words, whole.bytes);
		}

		(void)close(fd);
		(void)unlink(path);
	}
}

/*
 * 一个 2 MiB 的单词：所有切点都会被对齐推到文件末尾，于是只剩一段。
 * 这条用例盯的是 wc_align_chunk 在"整段无分隔符"时的返回，
 * 以及 wc_count_fd_range 处理"起点等于终点"的空段。
 */
static void test_one_huge_word(void) {
	const size_t size = 2u * 1024u * 1024u;
	char        *buf  = (char *)malloc(size);
	char         path[256];

	if (buf == NULL) {
		g_checks++;
		g_failures++;
		(void)printf("FAIL  无法分配 2 MiB 缓冲区\n");
		return;
	}
	memset(buf, 'a', size);

	int fd = make_temp(buf, size, path, sizeof path);
	free(buf);

	wc_stat whole;
	expect_status("2 MiB 单词：整文件", wc_count_file(path, &whole), WC_OK);
	expect("2 MiB 单词：整文件结果", &whole, 0, 1, (unsigned long)size);

	wc_stat got;
	expect_status("2 MiB 单词：4 段", chunked_stat(fd, (off_t)size, 4, &got), WC_OK);
	expect("2 MiB 单词：4 段结果", &got, 0, 1, (unsigned long)size);

	/* 空段：起点等于终点，必须是全零而不是错误 */
	wc_stat empty = {1, 1, 1};
	expect_status("空段", wc_count_fd_range(fd, 1024, 1024, &empty), WC_OK);
	expect("空段结果为全零", &empty, 0, 0, 0);

	/*
	 * 单调性：三个建议切点都落在这一个词里，对齐结果必须都等于 size，
	 * 且后一个不小于前一个。驱动层靠这条性质保证边界不交叉，
	 * 一旦被违反，相邻两段会重叠、重叠内容被数两次。
	 */
	{
		off_t a1 = -1, a2 = -1, a3 = -1;

		expect_status("单调性：1/4 处", wc_align_chunk(fd, (off_t)size / 4, (off_t)size, &a1), WC_OK);
		expect_status("单调性：1/2 处", wc_align_chunk(fd, (off_t)size / 2, (off_t)size, &a2), WC_OK);
		expect_status("单调性：3/4 处", wc_align_chunk(fd, (off_t)size * 3 / 4, (off_t)size, &a3), WC_OK);
		expect_off("单调性：1/4 -> size", a1, (off_t)size);
		expect_off("单调性：1/2 -> size", a2, (off_t)size);
		expect_off("单调性：3/4 -> size", a3, (off_t)size);
		expect_off("单调性：a1 <= a2", (a2 >= a1) ? 1 : 0, 1);
		expect_off("单调性：a2 <= a3", (a3 >= a2) ? 1 : 0, 1);
	}

	(void)close(fd);
	(void)unlink(path);
}

/*
 * 第三类：错误路径。
 *
 * 文件在统计期间被截短是最容易被写成"静默成功"的一条：
 * pread 返回 0 表示"没有更多数据"，若把它当成正常结束，
 * 调用方会拿到一个偏小的字节数，且没有任何错误信号。
 */
static void test_errors(void) {
	const char *text = "0123456789";
	char        path[256];
	int         fd = make_temp(text, strlen(text), path, sizeof path);
	wc_stat     st = {7, 7, 7};
	off_t       off = -1;

	expect_status("区间：fd 非法", wc_count_fd_range(-1, 0, 10, &st), WC_ERR_PARAM);
	expect("区间：fd 非法时 out 未被修改", &st, 7, 7, 7);

	expect_status("区间：out 为 NULL", wc_count_fd_range(fd, 0, 10, NULL), WC_ERR_PARAM);
	expect_status("区间：起点为负", wc_count_fd_range(fd, -1, 10, &st), WC_ERR_PARAM);
	expect_status("区间：终点小于起点", wc_count_fd_range(fd, 8, 4, &st), WC_ERR_PARAM);
	expect("区间：参数错误时 out 未被修改", &st, 7, 7, 7);

	expect_status("对齐：fd 非法", wc_align_chunk(-1, 0, 10, &off), WC_ERR_PARAM);
	expect_status("对齐：out 为 NULL", wc_align_chunk(fd, 0, 10, NULL), WC_ERR_PARAM);
	expect_status("对齐：起点为负", wc_align_chunk(fd, -1, 10, &off), WC_ERR_PARAM);
	expect_status("对齐：终点小于起点", wc_align_chunk(fd, 6, 2, &off), WC_ERR_PARAM);
	expect_off("对齐：参数错误时 out 未被修改", off, -1);

	/* 现在把文件截到 4 字节，再按原来的 10 字节统计 */
	if (ftruncate(fd, 4) != 0) {
		g_checks++;
		g_failures++;
		(void)printf("FAIL  ftruncate 失败\n");
	} else {
		wc_stat   small = {9, 9, 9};
		wc_status got   = wc_count_fd_range(fd, 0, 10, &small);

		/* errno 必须在返回后立刻取：后面任何一次 printf 都可能改写它 */
		int saved_errno = errno;

		expect_status("区间：文件被截短", got, WC_ERR_READ);
		expect("区间：截短时 out 未被修改", &small, 9, 9, 9);
		expect_off("区间：截短时 errno 被清零", (off_t)saved_errno, 0);
	}

	(void)close(fd);
	(void)unlink(path);
}

/* ---------------------------------------------------------------- */

int main(void) {
	(void)printf("wordcount %s 单文件切分测试\n\n", WC_VERSION_STRING);

	test_align();
	test_align_no_separator();
	test_chunked_equals_whole();
	test_one_huge_word();
	test_errors();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
