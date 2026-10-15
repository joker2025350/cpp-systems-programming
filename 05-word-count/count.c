#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

/* 扫描时只有两种状态：当前不在单词内 OUT / 当前在单词内 IN。
 * 用 enum 而不是 #define，好处是这些名字有类型、调试器可见，
 * 而且不会被预处理阶段无差别替换。 */
enum {
	OUT = 0,
	IN  = 1
};

/* 判断一个字符是否构成单词分隔符。
 *
 * 判据是"不是字母数字"：单词被定义为最长的连续字母数字段。
 * 等价说法是"分隔符 = 空白或标点"，但对 ASCII 而言，
 * 字母数字的补集还包含控制字符与 0x80 以上的高位字节。
 * 这一点在 UTF-8 文本上很关键，见 README 的说明。
 *
 * 参数类型必须是 int，不能是 char，原因有两条：
 *   1. 调用方传入的是 fgetc 的返回值，它需要能表示 EOF（负值）。
 *      char 在 x86 上有符号，恰好能存下 -1，看起来能用；
 *      但在 ARM 等平台上 char 默认无符号，EOF 会被截断成 255，
 *      与 EOF 永远不相等，循环就此失控。用 int 才符合 fgetc 的
 *      返回类型（C11 7.21.7.1）。
 *   2. C11 7.4p1 规定，传给 ctype 系列函数的值要么等于 EOF，
 *      要么可表示为 unsigned char。传负的 char 属于未定义行为。
 */
static int is_separator(int c) {
	return isalnum(c) == 0;
}

/* 统计 filename 中的单词数。
 *
 * 单词的定义：一段最长的、连续的、不含分隔符的字符序列。
 * 采用状态机实现——只在 OUT -> IN 的那一次跳变上计数，
 * 因此每个单词恰好被计一次，额外空间为 O(1)，
 * 与文件大小无关，也不需要把文件整个读进内存。
 *
 * 成功时返回 0，并把词数写入 *count；
 * 失败时返回 errno 的值（非 0），*count 不被修改。
 *
 * 注意这里必须由本函数负责 fclose：fp 是在本函数内 fopen 的，
 * 所有权在本函数内，任何一条返回路径都不能把它漏掉。
 */
static int count_word(const char *filename, long *count) {
	FILE *fp = fopen(filename, "r");
	if (fp == NULL) {
		return errno;
	}

	long word = 0;
	int status = OUT;
	int c;

	while ((c = fgetc(fp)) != EOF) {
		if (is_separator(c)) {
			status = OUT;
		} else if (status == OUT) {
			status = IN;
			word++;
		}
	}

	/* fgetc 返回 EOF 有两种可能：正常读到文件末尾，或者发生了读错误。
	 * ferror 用来区分这两种情况——只看 EOF 会把读错误当成正常结束。 */
	if (ferror(fp)) {
		int e = errno;      /* 先取出 errno，因为 fclose 可能改写它 */
		fclose(fp);
		return e;
	}

	/* fclose 自身也可能失败（例如延迟写入时磁盘满），不能忽略 */
	if (fclose(fp) != 0) {
		return errno;
	}

	*count = word;
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file> [file ...]\n", argv[0]);
		return 1;
	}

	/* 只要有一个文件处理失败，最终退出码就是非 0，
	 * 这样才能被 shell 的 && / || 和脚本正确判断。 */
	int failed = 0;

	for (int i = 1; i < argc; i++) {
		long count = 0;
		int err = count_word(argv[i], &count);

		if (err != 0) {
			fprintf(stderr, "count: %s: %s\n", argv[i], strerror(err));
			failed = 1;
		} else {
			printf("%8ld %s\n", count, argv[i]);
		}
	}

	return failed;
}
