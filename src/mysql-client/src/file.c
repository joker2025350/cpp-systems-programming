/*
 * file.c —— 文件读写辅助（不访问数据库）
 *
 * 这两个函数是 BLOB 往返的两端：从磁盘取出一段字节喂给 mc_blob_write，
 * 把 mc_blob_read 取回的一段字节写回磁盘。它们与数据库无关，
 * 因此单独成型，可以被不链接 libmysqlclient 的单元测试完全覆盖。
 *
 * 与最初那份单文件实现的差别集中在读取方式上，见 mc_file_read 的注释。
 */

#include "mysqlclient.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mc_status mc_file_read(const char *path, void *buf, size_t cap, size_t *out_len) {

	FILE     *fp;
	char     *dst = (char *)buf;
	size_t    used = 0;
	mc_status status = MC_OK;
	int       saved_errno = 0;

	if (path == NULL || buf == NULL || out_len == NULL) return MC_ERR_PARAM;

	/*
	 * 容量为 0 直接拒绝，而不是返回"读了 0 字节"。
	 * 后者与"空文件"无法区分，调用方会把一次必然失败的读取当成成功。
	 */
	if (cap == 0) return MC_ERR_PARAM;

	fp = fopen(path, "rb");
	if (fp == NULL) return MC_ERR_FILE_OPEN;   /* errno 由 fopen 设置，原样留给调用方 */

	/*
	 * 边读边判断是否越界，不用 fseek + ftell 预先取长度。
	 *
	 * 原来的写法先 fseek(fp, 0, SEEK_END) 再 ftell 拿长度，隐含了
	 * "文件可 seek 且长度已知"这个前提。这个前提对普通文件成立，
	 * 对下面三类输入不成立：
	 *   - FIFO / 管道：POSIX 规定不可 seek，fseek 失败并设置 ESPIPE，
	 *     ftell 返回 -1；
	 *   - 终端与 /dev 下的字符设备：同上；
	 *   - /proc、/sys 里的伪文件：可 seek，但报告的长度是 0，
	 *     按"长度 0"去 fread 一个字节都读不到。
	 * 用 fread 一直读到 EOF，这三种来源都能正常读出内容；
	 * 越界仍然能被发现——缓冲区填满后再多读一个字节即可。
	 */
	while (used < cap) {
		size_t n = fread(dst + used, 1, cap - used, fp);

		used += n;

		/*
		 * n == 0 表示"读完了"或"出错了"，两者由 ferror 区分。
		 * 不能用 feof 判断：C11 7.21.10.2 规定文件结束指示器由
		 * **失败的**读取设置，是事后标志，用它当循环条件会多跑一轮。
		 */
		if (n == 0) break;
	}

	if (ferror(fp)) {
		status      = MC_ERR_FILE_READ;
		saved_errno = errno;
	} else if (used == cap) {
		/*
		 * 缓冲区正好填满：可能文件恰好 cap 字节，也可能更长。
		 * 多读一个字节就能区分，代价是一次 fgetc。
		 */
		int ch = fgetc(fp);

		if (ch != EOF) {
			status = MC_ERR_TOOBIG;
		} else if (ferror(fp)) {
			status      = MC_ERR_FILE_READ;
			saved_errno = errno;
		}
	}

	/*
	 * out_len 只在成功时写。失败时 used 只表示"已经搬进缓冲区的残片长度"，
	 * 把它交给调用方等于默许使用一份不完整的数据。
	 */
	if (status == MC_OK) *out_len = used;

	/*
	 * fclose 会刷新缓冲区、可能触发真正的 write(2)，因此它可能改写 errno。
	 * 先取出失败点的 errno，关闭之后再放回去，调用方的 strerror(errno)
	 * 才指向真正的失败原因。
	 */
	if (fclose(fp) != 0 && status == MC_OK) {
		status      = MC_ERR_FILE_READ;
		saved_errno = errno;
	}

	if (saved_errno != 0) errno = saved_errno;

	return status;
}

mc_status mc_file_write(const char *path, const void *buf, size_t len) {

	FILE     *fp;
	char     *tmp;
	size_t    tmp_len;
	size_t    written;
	mc_status status = MC_OK;
	int       saved_errno = 0;

	if (path == NULL || buf == NULL) return MC_ERR_PARAM;

	/* len 为 0 是合法的：创建一个空文件。 */

	/*
	 * 先写 "<path>.tmp"，成功之后再 rename 覆盖目标文件。
	 *
	 * 直接 fopen(path, "wb") 会在打开的一瞬间把目标文件截断为 0 字节
	 * （"w" 模式的语义就是 truncate，见 C11 7.21.5.3）。此后的写入一旦失败
	 * ——磁盘满（ENOSPC）、配额用尽（EDQUOT）、进程被杀——目标文件就只剩
	 * 一份残缺内容，而原来的内容已经没了。
	 *
	 * rename(2) 在同一个文件系统内是原子替换：任何时刻从并发读者的角度，
	 * 看到的要么是完整的旧内容，要么是完整的新内容。因此临时文件必须与
	 * 目标放在同一个目录（用 path 加后缀），不能放 /tmp——跨文件系统时
	 * rename 返回 EXDEV，内核不会帮你搬数据。
	 *
	 * 代价一：要求目录可写，而不只是目标文件可写。
	 * 代价二：目标是管道或字符设备（/dev/stdout）时不适用——对它们 rename
	 * 只会失败。本函数只用于普通文件路径。
	 */
	tmp_len = strlen(path) + sizeof ".tmp";
	tmp = (char *)malloc(tmp_len);
	if (tmp == NULL) return MC_ERR_NOMEM;

	(void)snprintf(tmp, tmp_len, "%s.tmp", path);

	fp = fopen(tmp, "wb");
	if (fp == NULL) {
		free(tmp);
		return MC_ERR_FILE_OPEN;
	}

	written = fwrite(buf, 1, len, fp);
	if (written != len) {
		status      = MC_ERR_FILE_WRITE;
		saved_errno = errno;
	}

	/*
	 * ISO C 7.21.3 规定数据先进入流的缓冲区，真正的 write(2) 由 fflush
	 * 或 fclose 触发。所以 fwrite 返回值正确只说明"数据交给了缓冲区"；
	 * 磁盘满、配额用尽这些错误要到刷新时才报出来。不检查这两个返回值，
	 * 就等于把一次失败的写入报告成成功。
	 */
	if (fflush(fp) != 0 && status == MC_OK) {
		status      = MC_ERR_FILE_WRITE;
		saved_errno = errno;
	}

	if (fclose(fp) != 0 && status == MC_OK) {
		status      = MC_ERR_FILE_WRITE;
		saved_errno = errno;
	}

	if (status != MC_OK) {
		/* 失败时删掉半成品，别把残片留在使用者眼前 */
		(void)remove(tmp);
		free(tmp);
		if (saved_errno != 0) errno = saved_errno;
		return status;
	}

	if (rename(tmp, path) != 0) {
		saved_errno = errno;
		(void)remove(tmp);
		free(tmp);
		errno = saved_errno;
		return MC_ERR_FILE_WRITE;
	}

	free(tmp);

	return MC_OK;
}
