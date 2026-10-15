/*
 * test_wordcount.c —— wordcount 模块的单元测试
 *
 * 全部用例都只用内存中的数据，不依赖任何外部文件，
 * 因此可以在任何目录下直接运行。
 *
 * 编译：make test
 */

#include "wordcount.h"

#include <stdio.h>
#include <string.h>

static int g_checks   = 0;
static int g_failures = 0;

/* 断言三个计数同时相等 */
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

/* 断言状态码相等 */
static void expect_status(const char *name, wc_status got, wc_status want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-46s 状态码 %d，期望 %d\n", name, (int)got, (int)want);
		return;
	}

	(void)printf("ok    %s\n", name);
}

/* 对一段 C 字符串整体统计，返回结果 */
static wc_stat count_cstr(const char *s) {
	wc_stat st = {0, 0, 0};
	(void)wc_count_buffer(s, strlen(s), &st);
	return st;
}

/* ---------------------------------------------------------------- */

static void test_empty_and_separators(void) {
	wc_stat st;

	/* 空输入：data 可以为 NULL，只要 size 为 0 */
	expect_status("空输入（NULL + size 0）",
	              wc_count_buffer(NULL, 0, &st), WC_OK);
	expect("空输入的结果", &st, 0, 0, 0);

	/* 只有空白与标点，一个字母数字都没有 */
	st = count_cstr("   \t\n\r\n  ,.;!  \n");
	expect("只有分隔符", &st, 3, 0, 16);

	/* 单个单词，且不带结尾换行 —— 最容易漏掉最后一个词的场景 */
	st = count_cstr("hello");
	expect("单个单词，无结尾换行", &st, 0, 1, 5);

	/* 连续多个单词 */
	st = count_cstr("hello world");
	expect("两个单词", &st, 0, 2, 11);

	/* 标点紧贴单词 */
	st = count_cstr("one,two;three.");
	expect("标点作为分隔符", &st, 0, 3, 14);

	/* 数字也算单词 */
	st = count_cstr("version 2 build 15");
	expect("数字计入单词", &st, 0, 4, 18);
}

static void test_lines_and_bytes(void) {
	wc_stat st;

	/* 行数 = '\n' 的个数。最后一行若没有换行符，不计入行数 */
	st = count_cstr("a\nb\nc\n");
	expect("三行，行尾都有换行", &st, 3, 3, 6);

	st = count_cstr("a\nb\nc");
	expect("三行，最后一行无换行", &st, 2, 3, 5);

	/* 空行也计入行数 */
	st = count_cstr("\n\n\n");
	expect("三个空行", &st, 3, 0, 3);
}

/*
 * 跨块边界：这是流式模块必须处理、而单文件 demo 通常忽略的场景。
 * 把一个词切成任意多段送入，结果都必须与整体送入一致。
 */
static void test_chunk_boundary(void) {
	const char *text = "hello world";
	const size_t len = strlen(text);

	/* 整体送入作为基准 */
	wc_stat whole = count_cstr(text);
	expect("基准：整体送入", &whole, 0, 2, 11);

	/* 逐字节送入，每次一个字符 */
	{
		wc_counter c;
		wc_counter_reset(&c);
		for (size_t i = 0; i < len; i++) {
			(void)wc_counter_update(&c, text + i, 1);
		}
		wc_stat st;
		wc_counter_get(&c, &st);
		expect("逐字节送入（每段 1 字节）", &st, whole.lines, whole.words, whole.bytes);
	}

	/* 按各种块大小切分，覆盖"单词正好被切断"的情形 */
	for (size_t chunk = 1; chunk <= len; chunk++) {
		wc_counter c;
		wc_counter_reset(&c);

		size_t off = 0;
		while (off < len) {
			size_t n = len - off;
			if (n > chunk) {
				n = chunk;
			}
			(void)wc_counter_update(&c, text + off, n);
			off += n;
		}

		wc_stat st;
		wc_counter_get(&c, &st);

		char name[64];
		(void)snprintf(name, sizeof name, "跨块边界：每段 %zu 字节", chunk);
		expect(name, &st, whole.lines, whole.words, whole.bytes);
	}
}

/* 在单词中间切断，专门验证状态被保留了下来 */
static void test_split_inside_word(void) {
	wc_counter c;
	wc_counter_reset(&c);

	(void)wc_counter_update(&c, "hel", 3);   /* 一个词的左半 */
	(void)wc_counter_update(&c, "lo", 2);    /* 右半，应仍是同一个词 */

	wc_stat st;
	wc_counter_get(&c, &st);
	expect("在词中间切断仍算一个词", &st, 0, 1, 5);

	/* 反过来确认：如果在分隔符处切，两个词分开计 */
	wc_counter_reset(&c);
	(void)wc_counter_update(&c, "hello ", 6);
	(void)wc_counter_update(&c, "world", 5);
	wc_counter_get(&c, &st);
	expect("在分隔符处切断算两个词", &st, 0, 2, 11);
}

static void test_reset_and_reuse(void) {
	wc_counter c;
	wc_counter_reset(&c);

	(void)wc_counter_update(&c, "first pass", 10);

	wc_stat st;
	wc_counter_get(&c, &st);
	expect("第一次使用", &st, 0, 2, 10);

	/* 重置之后必须完全归零，不能残留上一次的状态或计数 */
	wc_counter_reset(&c);
	(void)wc_counter_update(&c, "second", 6);
	wc_counter_get(&c, &st);
	expect("重置后复用", &st, 0, 1, 6);

	/*
	 * 重置后第一次送入以字母数字开头，
	 * 若 state 没被清零就会漏计这一个词。
	 */
	wc_counter_reset(&c);
	(void)wc_counter_update(&c, "abc", 3);
	wc_counter_get(&c, &st);
	expect("重置后状态确实归零", &st, 0, 1, 3);
}

static void test_invalid_arguments(void) {
	wc_stat st;

	expect_status("out 为 NULL",
	              wc_count_buffer("abc", 3, NULL), WC_ERR_PARAM);

	expect_status("计数器为 NULL",
	              wc_counter_update(NULL, "abc", 3), WC_ERR_PARAM);

	expect_status("data 为 NULL 但 size 非 0",
	              wc_counter_update(&(wc_counter){0}, NULL, 3), WC_ERR_PARAM);

	expect_status("流为 NULL",
	              wc_count_stream(NULL, &st), WC_ERR_PARAM);

	expect_status("路径为 NULL",
	              wc_count_file(NULL, &st), WC_ERR_PARAM);

	/* 这两个只要求不崩溃 */
	wc_counter_reset(NULL);
	wc_counter_get(NULL, &st);
	expect_status("wc_counter_reset(NULL) 不崩溃", WC_OK, WC_OK);
}

static void test_status_string(void) {
	const char *s;

	/* 已知状态码必须给出一句非空描述 */
	for (int i = 0; i <= 4; i++) {
		g_checks++;
		s = wc_status_string((wc_status)i);
		if (s == NULL || s[0] == '\0') {
			g_failures++;
			(void)printf("FAIL  %-46s 返回了空描述\n", "wc_status_string 已知值");
		}
	}
	(void)printf("ok    %s\n", "wc_status_string 已知值非空");

	/* 未知状态码不能返回 NULL，否则调用方 printf("%s") 会崩溃 */
	g_checks++;
	s = wc_status_string((wc_status)999);
	if (s == NULL || strcmp(s, "unknown status") != 0) {
		g_failures++;
		(void)printf("FAIL  %-46s 期望 \"unknown status\"\n",
		             "wc_status_string 未知值");
		return;
	}
	(void)printf("ok    %s\n", "wc_status_string 未知值");
}

/* ---------------------------------------------------------------- */

int main(void) {
	(void)printf("wordcount %s 单元测试\n\n", WC_VERSION_STRING);

	test_empty_and_separators();
	test_lines_and_bytes();
	test_chunk_boundary();
	test_split_inside_word();
	test_reset_and_reuse();
	test_invalid_arguments();
	test_status_string();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
