/*
 * wordcount.c —— 文本统计模块的实现
 *
 * 扫描策略：单遍、流式、O(1) 额外空间。
 * 不使用 fgetc 逐字符读取，而是由调用方（或 wc_count_file 的默认封装）
 * 按块送入数据，逐块扫描。这样既不依赖 stdio 内部缓冲区的大小，
 * 也让"单词跨越块边界"这件事变成一个显式、可测试的问题。
 */

#include "wordcount.h"

#include <ctype.h>
#include <errno.h>
#include <unistd.h>   /* pread：POSIX，按绝对偏移读，不动机位 */

/* 扫描状态：不在单词内 / 在单词内 */
enum {
	WC_STATE_OUT = 0,
	WC_STATE_IN  = 1
};

/*
 * 判断一个字节是否为单词分隔符。
 *
 * 判据是"不是字母数字"，也就是把单词定义为最长的连续字母数字段。
 * 常说的"分隔符 = 空白或标点"是同一件事的另一种说法，但对 ASCII 而言，
 * 字母数字的补集除了空白和标点，还包含控制字符与 0x80 以上的高位字节。
 * 本模块是字节导向的，且运行在 C locale 下，因此高位字节一律算分隔符。
 * 这对 UTF-8 文本的影响见 README 的"已知局限"。
 *
 * 参数类型必须是 unsigned char，这一点不能省：
 * C11 7.4p1 规定，传给 ctype 系列函数的值要么等于 EOF，
 * 要么可表示为 unsigned char。若把 char 直接传进去，
 * 在 char 有符号的平台上遇到 0x80 以上的字节会得到负值，
 * 而负值既不是 EOF 也不是合法的 unsigned char，属于未定义行为。
 * 调用方传进来的原始字节先转成 unsigned char，再转 int，
 * 取值范围落在 [0, 255]，始终合法。
 */
static int wc_is_separator(unsigned char c) {
	return isalnum((int)c) == 0;
}

void wc_counter_reset(wc_counter *c) {
	if (c == NULL) {
		return;
	}

	c->state        = WC_STATE_OUT;
	c->total.lines  = 0;
	c->total.words  = 0;
	c->total.bytes  = 0;
}

wc_status wc_counter_update(wc_counter *c, const void *data, size_t size) {
	if (c == NULL) {
		return WC_ERR_PARAM;
	}
	if (data == NULL && size != 0) {
		return WC_ERR_PARAM;
	}

	const unsigned char *p = (const unsigned char *)data;

	/*
	 * 先把累计量和状态取到局部变量，循环结束后再一次性写回。
	 * 这样循环体内不必反复访问结构体成员，而且即使调用方
	 * 传入的 data 恰好指向 c 自身的内存，也不会读到写了一半的状态。
	 */
	unsigned long words = c->total.words;
	unsigned long lines = c->total.lines;
	int           state = (int)c->state;

	for (size_t i = 0; i < size; i++) {
		unsigned char ch = p[i];

		if (ch == '\n') {
			lines++;
		}

		/*
		 * 只在 OUT -> IN 的那一次跳变上计数。
		 * 这样每个单词恰好被计一次，不需要在循环结束后补一次收尾判断，
		 * 因而天然处理了"文件末尾没有换行符"的情形。
		 */
		if (wc_is_separator(ch)) {
			state = WC_STATE_OUT;
		} else if (state == WC_STATE_OUT) {
			state = WC_STATE_IN;
			words++;
		}
	}

	c->state       = (unsigned int)state;
	c->total.words = words;
	c->total.lines = lines;
	c->total.bytes += (unsigned long)size;

	return WC_OK;
}

void wc_counter_get(const wc_counter *c, wc_stat *out) {
	if (c == NULL || out == NULL) {
		return;
	}

	*out = c->total;
}

wc_status wc_count_buffer(const void *data, size_t size, wc_stat *out) {
	if (out == NULL) {
		return WC_ERR_PARAM;
	}

	wc_counter c;
	wc_counter_reset(&c);

	wc_status status = wc_counter_update(&c, data, size);
	if (status != WC_OK) {
		return status;
	}

	*out = c.total;
	return WC_OK;
}

wc_status wc_count_stream(FILE *stream, wc_stat *out) {
	if (stream == NULL || out == NULL) {
		return WC_ERR_PARAM;
	}

	wc_counter c;
	wc_counter_reset(&c);

	/*
	 * WC_BUFFER_SIZE 默认 16 KiB。注意这块缓冲区位于栈上：
	 * 若调用方把该宏调得很大，就要相应确认调用线程的栈够用。
	 * 嵌入式环境下线程栈常常只有几 KiB，那种场合应当直接使用
	 * wc_counter_update 自行驱动 I/O，把缓冲区放在自己指定的位置。
	 */
	unsigned char buf[WC_BUFFER_SIZE];
	size_t n;

	while ((n = fread(buf, 1, sizeof buf, stream)) > 0) {
		wc_status status = wc_counter_update(&c, buf, n);
		if (status != WC_OK) {
			return status;
		}
	}

	/*
	 * fread 返回 0 有两种可能：正常读到文件末尾，或者发生了读错误。
	 * ferror 用来区分这两种情况——只看返回值会把读错误当成正常结束。
	 * 此处直接返回，errno 由失败的 fread 设置，保持原样交给调用方。
	 */
	if (ferror(stream)) {
		return WC_ERR_READ;
	}

	*out = c.total;
	return WC_OK;
}

wc_status wc_count_file(const char *path, wc_stat *out) {
	if (path == NULL || out == NULL) {
		return WC_ERR_PARAM;
	}

	errno = 0;
	FILE *fp = fopen(path, "rb");
	if (fp == NULL) {
		return WC_ERR_OPEN;              /* errno 由 fopen 设置 */
	}

	wc_status status = wc_count_stream(fp, out);

	/*
	 * fclose 也可能失败（例如延迟写入时磁盘满），不能忽略；
	 * 但它会改写 errno，所以先把它保存下来，
	 * 以免覆盖掉 wc_count_stream 留下的更具体的原因。
	 */
	int saved = errno;
	if (fclose(fp) != 0 && status == WC_OK) {
		return WC_ERR_CLOSE;             /* errno 由 fclose 设置 */
	}
	errno = saved;

	return status;
}

/*
 * pread 的包装：只处理 EINTR。
 *
 * pread 与 read 一样，在信号处理器返回后可能返回 -1 并把 errno 置为 EINTR，
 * 这不是错误而是"这次没读到"。POSIX 允许实现这么做，调用方必须重试，
 * 否则一次普通的信号就会让统计失败——而失败点与信号到达的时刻有关，
 * 属于最难复现的那一类缺陷。
 */
static ssize_t wc_pread(int fd, void *buf, size_t size, off_t offset) {
	for (;;) {
		ssize_t n = pread(fd, buf, size, offset);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		return n;
	}
}

wc_status wc_align_chunk(int fd, off_t from, off_t limit, off_t *out) {
	if (fd < 0 || out == NULL || from < 0 || limit < from) {
		return WC_ERR_PARAM;
	}

	unsigned char buf[WC_BUFFER_SIZE];
	off_t         pos = from;

	while (pos < limit) {
		off_t  remain = limit - pos;
		size_t want   = sizeof buf;

		/* 只读到 limit 为止：limit 之外的内容属于下一段，不能拿来做判断 */
		if ((off_t)want > remain) {
			want = (size_t)remain;
		}

		ssize_t n = wc_pread(fd, buf, want, pos);
		if (n < 0) {
			return WC_ERR_READ;
		}
		if (n == 0) {
			/*
			 * 还没到 limit 就读到文件末尾：文件比调用方以为的短。
			 * 这不算系统调用失败，把 errno 显式清零，
			 * 免得调用方拿上一个失败调用的残留值去 strerror。
			 */
			errno = 0;
			return WC_ERR_READ;
		}

		for (ssize_t i = 0; i < n; i++) {
			if (wc_is_separator(buf[i])) {
				*out = pos + (off_t)i;
				return WC_OK;
			}
		}

		pos += (off_t)n;
	}

	/* [from, limit) 内没有分隔符：整段都在某个单词内部，没有可用切点 */
	*out = limit;
	return WC_OK;
}

wc_status wc_count_fd_range(int fd, off_t from, off_t to, wc_stat *out) {
	if (fd < 0 || out == NULL || from < 0 || to < from) {
		return WC_ERR_PARAM;
	}

	wc_counter c;
	wc_counter_reset(&c);

	unsigned char buf[WC_BUFFER_SIZE];
	off_t         pos = from;

	while (pos < to) {
		off_t  remain = to - pos;
		size_t want   = sizeof buf;

		if ((off_t)want > remain) {
			want = (size_t)remain;
		}

		ssize_t n = wc_pread(fd, buf, want, pos);
		if (n < 0) {
			return WC_ERR_READ;
		}
		if (n == 0) {
			/* 文件在统计期间被截短，见头文件对 errno 为 0 的说明 */
			errno = 0;
			return WC_ERR_READ;
		}

		wc_status status = wc_counter_update(&c, buf, (size_t)n);
		if (status != WC_OK) {
			return status;
		}

		pos += (off_t)n;
	}

	*out = c.total;
	return WC_OK;
}

const char *wc_status_string(wc_status status) {
	switch (status) {
	case WC_OK:        return "success";
	case WC_ERR_PARAM: return "invalid argument";
	case WC_ERR_OPEN:  return "cannot open file";
	case WC_ERR_READ:  return "read error";
	case WC_ERR_CLOSE: return "close error";
	default:           return "unknown status";
	}
}
