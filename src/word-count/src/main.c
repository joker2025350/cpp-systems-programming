/*
 * main.c —— 命令行驱动
 *
 * 这一层只负责参数解析、输出排版与退出码，
 * 所有统计逻辑都在 wordcount 模块里。把这层单独分出来，
 * 是为了让模块可以被别的程序链接使用，而不必连带一个 main。
 */

#include "wordcount.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* 输出列宽：与 wc 保持一致的观感 */
#define COL_WIDTH 8

static void print_stat(const wc_stat *st, const char *label);

/*
 * 处理一个输入源。
 * path 为 NULL 表示从 stream（标准输入）读取。
 * 成功返回 0，失败返回非 0。
 */
static int process(const char *path, FILE *stream, wc_stat *sum) {
	wc_stat st;
	wc_status status;

	if (path != NULL) {
		status = wc_count_file(path, &st);
	} else {
		status = wc_count_stream(stream, &st);
	}

	if (status != WC_OK) {
		/*
		 * WC_ERR_OPEN / WC_ERR_READ / WC_ERR_CLOSE 三种情况
		 * 都会保留 errno，用 strerror 给出具体原因。
		 * WC_ERR_PARAM 不会出现在这里，因为它只由编程错误触发。
		 */
		(void)fprintf(stderr, "wordcount: %s: %s: %s\n",
		              (path != NULL) ? path : "(stdin)",
		              wc_status_string(status),
		              strerror(errno));
		return 1;
	}

	print_stat(&st, path);

	/* 累加总计数。大文件时 unsigned long 不会溢出，直接相加即可。 */
	sum->lines += st.lines;
	sum->words += st.words;
	sum->bytes += st.bytes;

	return 0;
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

int main(int argc, char *argv[]) {
	int failed = 0;
	wc_stat sum = {0, 0, 0};

	if (argc < 2) {
		/* 没有文件名参数时读标准输入，这样可以直接接管道 */
		failed = process(NULL, stdin, &sum);
		return failed;
	}

	for (int i = 1; i < argc; i++) {
		if (process(argv[i], NULL, &sum) != 0) {
			failed = 1;
		}
	}

	/* 多个文件时补一行总计，与 wc 的行为一致 */
	if (argc > 2) {
		print_stat(&sum, "total");
	}

	return failed;
}
