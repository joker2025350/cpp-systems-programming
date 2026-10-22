/*
 * bench_file.c —— 文件读写的吞吐测量
 *
 * 这两个函数是 BLOB 往返的两端，它们的耗时决定了"传一张 5 MB 的图片"
 * 里有多少时间花在本地磁盘上。测的是模块自己的实现，不访问数据库，
 * 也不依赖 libmysqlclient。
 *
 * 计时用 CLOCK_MONOTONIC：它不受系统时间调整（adjtime、NTP 步进）影响。
 * 用 CLOCK_REALTIME 的话，测量期间一次对时就能让耗时变成负数。
 */

#include "mysqlclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ROUNDS 5

static double now_ms(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;

	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(void) {

	static const size_t sizes[] = {
		1024u * 1024u,
		8u * 1024u * 1024u,
		64u * 1024u * 1024u
	};
	const char *path = "bin/bench_payload.bin";
	unsigned char *buf;
	size_t i;
	size_t r;

	(void)printf("%10s %10s %10s %10s %14s\n",
	             "size", "write(ms)", "W(MB/s)", "read(ms)", "R(MB/s)");
	(void)printf("--------------------------------------------------------------\n");

	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		size_t size = sizes[i];
		double best_w = 0.0;
		double best_r = 0.0;

		buf = (unsigned char *)malloc(size);
		if (buf == NULL) {
			(void)fprintf(stderr, "无法分配 %lu 字节\n", (unsigned long)size);
			return 1;
		}

		/* 写入非零图案：全零页在文件系统层面可能被压缩或省略，
		 * 测出来的吞吐会虚高，与实际写入一张图片不符。 */
		for (r = 0; r < size; r++) {
			buf[r] = (unsigned char)(r * 31u + 7u);
		}

		for (r = 0; r < ROUNDS; r++) {
			double t0 = now_ms();
			double t1;

			if (mc_file_write(path, buf, size) != MC_OK) {
				(void)fprintf(stderr, "写入失败\n");
				free(buf);
				return 1;
			}
			t1 = now_ms();

			if (best_w == 0.0 || t1 - t0 < best_w) best_w = t1 - t0;
		}

		for (r = 0; r < ROUNDS; r++) {
			double t0 = now_ms();
			double t1;
			size_t got = 0;

			if (mc_file_read(path, buf, size, &got) != MC_OK || got != size) {
				(void)fprintf(stderr, "读回长度不符\n");
				free(buf);
				return 1;
			}
			t1 = now_ms();

			if (best_r == 0.0 || t1 - t0 < best_r) best_r = t1 - t0;
		}

		(void)printf("%9luK %10.2f %10.1f %10.2f %14.1f\n",
		             (unsigned long)(size / 1024u),
		             best_w, ((double)size / 1048576.0) / (best_w / 1000.0),
		             best_r, ((double)size / 1048576.0) / (best_r / 1000.0));

		free(buf);
	}

	(void)remove(path);
	(void)printf("\n每格取 %d 轮中的最小值（排除调度抖动）\n", ROUNDS);

	return 0;
}
