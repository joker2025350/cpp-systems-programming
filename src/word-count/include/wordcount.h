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
 *
 * 1.1.0 起多了一对面向"按区域切分"的函数（wc_count_fd_range /
 * wc_align_chunk），它们用 pread 而不是 FILE *，因此同一 fd 上的
 * 多个区域可以被不同线程同时统计：pread 不读写文件偏移量，
 * 这一点是 POSIX 对 pread 的明文保证（"shall not change the file offset"）。
 * 库本身仍然不引用 pthread，线程只出现在驱动层。
 */

#ifndef WORDCOUNT_H
#define WORDCOUNT_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>   /* off_t：wc_count_fd_range / wc_align_chunk 用它 */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 版本信息
 * ------------------------------------------------------------------ */

#define WC_VERSION_MAJOR 1
#define WC_VERSION_MINOR 1
#define WC_VERSION_PATCH 0
#define WC_VERSION_STRING "1.1.0"

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

/* ------------------------------------------------------------------
 * 按区域切分：单文件并行的基础原语
 * ------------------------------------------------------------------
 * 这两个函数只做"数"与"找边界"，不创建线程，也不使用 FILE *。
 * 要做的判断是：把 [0, size) 切成若干段、每段交给一个线程，
 * 结果能否与整文件扫描逐字节一致。答案是能，但前提是每段的起点
 * 落在分隔符上——否则段首那个词的左半截留在上一段、右半截算在本段，
 * 单词数就会多算。边界怎么找，就是下面第一个函数。
 *
 * 二者都作用在文件描述符上，实现用 pread：它按绝对偏移读，
 * 既不读写 fd 的文件偏移量，也不受其他线程的读影响，
 * 因此同一 fd 可以被多个线程并发读不同的区域。
 * 这依赖 POSIX（pread 的语义），Windows 上没有对应等价物。
 */

/*
 * 把一个"建议切点"调整成"合法切点"。
 *
 * 从 from 开始逐字节向后找第一个分隔符，把它在文件中的偏移写进 *out。
 * 若 [from, limit) 里一个分隔符都没有（例如整个区间落在某个超长单词内），
 * *out 写成 limit，表示"这一段没有可用切点，切了就会破坏单词"。
 *
 * 返回 WC_OK 时 *out 落在 [from, limit] 闭区间内，且：
 *   - *out == limit：区间内无切点，调用方应把这段并进上一段；
 *   - 否则 data[*out] 一定是分隔符，以它为起点统计不会把词切开。
 *
 * fd 必须是可读的普通文件；from < 0 或 limit < from 返回 WC_ERR_PARAM。
 * 读失败返回 WC_ERR_READ，errno 保留系统错误号。
 *
 * 开销说明：最坏情况下这个函数要读 [from, limit) 的全部内容（单词跨越
 * 很长一段距离时）。正常文本里它只读几个字节——分隔符平均每 5~6 字节
 * 出现一次。
 */
wc_status wc_align_chunk(int fd, off_t from, off_t limit, off_t *out);

/*
 * 统计 [from, to) 这段字节，等价于把这段内容整段交给 wc_counter_update。
 *
 * 调用方负责保证 from 为 0 或者是一个合法切点（wc_align_chunk 给出的偏移）。
 * 只要满足这一点，把若干相邻区域的结果相加，就等于整文件统计的结果：
 * 行数按 '\n' 计数与切点无关，字节数按长度相加，单词数因为每段都以
 * 分隔符开头，段首不可能出现"某个词的右半截"。
 *
 * from == to 时写入全零并返回 WC_OK（空区域是合法输入：
 * 切点比建议切点靠后时会出现）。
 * fd < 0、out 为 NULL、from < 0、to < from 返回 WC_ERR_PARAM，
 * 此时 *out 不被修改。
 * 读失败、或者在读到 to 之前就遇到文件末尾（文件在统计期间被截短）
 * 返回 WC_ERR_READ，*out 同样不被修改。后一种情况下 errno 为 0——
 * 没有任何系统调用失败，只是输入比预期短，调用方不能拿 errno 去
 * strerror。
 */
wc_status wc_count_fd_range(int fd, off_t from, off_t to, wc_stat *out);

/*
 * 把状态码转成一句可读的英文描述。
 * 传入未知值时返回 "unknown status"，不会返回 NULL。
 */
const char *wc_status_string(wc_status status);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* WORDCOUNT_H */
