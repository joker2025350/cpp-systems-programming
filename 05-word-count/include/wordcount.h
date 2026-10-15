/*
 * wordcount.h —— 文本统计模块的公开接口
 *
 * 模块职责：对字节流做单遍扫描，统计行数、单词数与字节数。
 * 不依赖任何全局状态，不使用任何内部静态缓冲区，
 * 因此同一个进程里可以同时持有任意多个互相独立的计数器。
 *
 * 线程模型：全部函数都是可重入的。只要不同的线程使用各自的
 * wc_counter 对象，并发调用是安全的（FILE * 自身的并发访问
 * 由调用方按 stdio 的约定负责）。
 */

#ifndef WORDCOUNT_H
#define WORDCOUNT_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 版本信息
 * ------------------------------------------------------------------ */

#define WC_VERSION_MAJOR 1
#define WC_VERSION_MINOR 0
#define WC_VERSION_PATCH 0
#define WC_VERSION_STRING "1.0.0"

/* ------------------------------------------------------------------
 * 统计结果
 * ------------------------------------------------------------------
 * 三个计数都是单调递增的累计量。
 * unsigned long 在 LP64 平台（Linux x86-64 / AArch64）上是 64 位，
 * 足够覆盖任何实际文件；在 32 位平台上上限约为 42.9 亿，
 * 若需处理更大的输入请改用 uintmax_t。
 */
typedef struct {
	unsigned long lines;   /* '\n' 出现的次数 */
	unsigned long words;   /* 最长连续字母数字段的个数 */
	unsigned long bytes;   /* 已扫描的字节总数 */
} wc_stat;

/* ------------------------------------------------------------------
 * 状态码
 * ------------------------------------------------------------------
 * 沿用 C 语言的惯例：0 表示成功，非 0 表示失败。
 * WC_OK 的值固定为 0，因此可以直接用 if (status) 判断失败。
 *
 * 返回 WC_ERR_OPEN / WC_ERR_READ / WC_ERR_CLOSE 时，
 * errno 保留导致失败的系统错误号，调用方可据此给出更具体的信息。
 * 返回 WC_ERR_PARAM 时 errno 无意义。
 */
typedef enum {
	WC_OK         = 0,   /* 成功 */
	WC_ERR_PARAM  = 1,   /* 参数非法，例如传入了 NULL */
	WC_ERR_OPEN   = 2,   /* 无法打开文件 */
	WC_ERR_READ   = 3,   /* 读取过程中出错 */
	WC_ERR_CLOSE  = 4    /* 关闭文件时出错 */
} wc_status;

/* ------------------------------------------------------------------
 * 流式计数器
 * ------------------------------------------------------------------
 * 模块的核心。它把"扫描状态"与"累计结果"封装在一起，
 * 使得同一份数据无论被切成多少段送入，结果都完全一致。
 *
 * 这一点是流式处理的必要条件：单词可能跨越两次送入的边界
 * （例如第一次以 "hel" 结束、第二次以 "lo" 开始），
 * 若不保存最后一次扫描的状态，就会把一个词误判成两个。
 *
 * 结构体成员为公开的，便于在栈上直接分配；
 * 但调用方不应直接读写其中的字段，请使用下方函数。
 */
typedef struct {
	unsigned int state;    /* 内部：上一次扫描结束时的入/出词状态 */
	wc_stat      total;    /* 累计结果 */
} wc_counter;

/*
 * 便捷读取函数的默认块大小。
 *
 * 这个值是"每次读多少字节"与"占多少栈空间"之间的折中，
 * 实测取舍见 README。调用方若有更合适的值，
 * 请改用 wc_counter_update 自行驱动 I/O，不要修改此宏。
 *
 * 允许在编译命令上用 -DWC_BUFFER_SIZE=n 覆盖，
 * 因此这里用 #ifndef 包一层，避免重复定义。
 */
#ifndef WC_BUFFER_SIZE
#define WC_BUFFER_SIZE 16384u
#endif

/* ------------------------------------------------------------------
 * 接口
 * ------------------------------------------------------------------ */

/*
 * 重置计数器，把状态与三个累计量全部归零。
 * c 为 NULL 时直接返回，不做任何操作。
 * 首次使用 wc_counter 之前必须先调用它。
 */
void wc_counter_reset(wc_counter *c);

/*
 * 把 [data, data + size) 这段字节追加到计数器 c。
 *
 * data 允许为 NULL，此时 size 必须为 0（表示一段空输入）。
 * 返回 WC_ERR_PARAM 时计数器内容不被修改。
 *
 * 该函数可以按任意粒度重复调用，结果与一次性传入全部数据完全相同。
 */
wc_status wc_counter_update(wc_counter *c, const void *data, size_t size);

/*
 * 取出当前累计结果。out 为 NULL 或 c 为 NULL 时直接返回。
 * 这是一次值拷贝，不会影响计数器的后续使用。
 */
void wc_counter_get(const wc_counter *c, wc_stat *out);

/*
 * 统计一段内存。等价于 reset + update + get。
 * 适合已经持有完整数据（例如 mmap 映射、网络收包缓冲）的场景。
 */
wc_status wc_count_buffer(const void *data, size_t size, wc_stat *out);

/*
 * 统计一个已打开的流，读到文件末尾为止。
 * 内部使用 WC_BUFFER_SIZE 字节的栈缓冲区，调用返回时不会关闭 stream。
 * 到达文件末尾但 ferror 为真时返回 WC_ERR_READ。
 */
wc_status wc_count_stream(FILE *stream, wc_stat *out);

/*
 * 统计一个文件。以二进制方式打开，内部负责关闭句柄，
 * 因此任何一条返回路径都不会泄漏文件描述符。
 * 这是本模块面向"直接拿来用"的一层封装。
 */
wc_status wc_count_file(const char *path, wc_stat *out);

/*
 * 把状态码转成一句可读的英文描述。
 * 传入未知值时返回 "unknown status"，不会返回 NULL。
 */
const char *wc_status_string(wc_status status);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* WORDCOUNT_H */
