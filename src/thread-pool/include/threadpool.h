/*
 * threadpool.h —— 线程池模块的公开接口
 *
 * 模块职责：维护固定数量的工作线程（worker）与一条待执行任务队列，
 * 调用方提交任务函数，由工作线程取出并执行，执行完自动回收任务节点。
 *
 * 术语（先定义，后文直接用这些词）：
 *
 *   工作线程（worker）      由线程池自己 pthread_create 出来、长期存活的线程。
 *   任务（task）            一次函数调用请求：函数指针 + 一个 void * 参数。
 *   任务队列（task queue）  待执行任务构成的 FIFO 链表，由互斥锁保护的临界区维护。
 *   生产者（producer）      调用 threadpool_submit 的线程，通常是主线程。
 *   空闲线程（idle worker） 已从队列取不到任务、阻塞在条件变量上的工作线程。
 *   屏障（barrier）         threadpool_wait：阻塞到"队列空且无任务正在执行"为止。
 *   优雅停机（graceful）    置位"不再接收新任务"后，把已提交任务全部执行完再退出线程，
 *                          与"立即丢弃队列并退出"相对。
 *   有界队列（bounded queue）队列里"待执行任务数"有一个上限 max_queued。
 *                          上限只算尚未被取走的任务，不含正在执行的，因此在途任务
 *                          总数至多为 max_queued + nthreads。
 *   背压（backpressure）    生产速度高于消费速度时，让生产端承担等待，而不是让
 *                          队列与内存无限增长。本模块给出两种施加方式：立即失败
 *                          （threadpool_submit 返回 TPOOL_ERR_FULL）与阻塞等待
 *                          （threadpool_submit_wait）。
 *
 * 为什么用不透明句柄（opaque handle）而不是像 04/05 那样公开结构体：
 * 内部要放 pthread_mutex_t 与两个 pthread_cond_t。公开它们意味着头文件必须
 * include <pthread.h>，把 pthread 的类型、宏和平台相关的结构体布局泄漏给每个
 * 调用方；而 pthread 同步原语的内部表示在标准里没有定义，是 ABI 细节。
 * 用不透明句柄，头文件只依赖 <stddef.h>，内部结构可以自由改动。
 * 代价是对象只能在堆上分配，且必须由 threadpool_create 初始化。
 *
 * 所有权约定（本模块最重要的一条）：
 *
 *   - 任务节点（内部 struct task）归线程池所有。submit 时由池分配，
 *     任务函数返回后由池释放。调用方不分配、不释放、也拿不到它。
 *   - 任务参数 arg 归调用方所有。池只把它原样转交给任务函数，
 *     既不解引用，也不释放，也不在 destroy 时回收未执行任务的 arg。
 *     —— 这一点与旧实现不同：旧实现要求任务函数自己 free 掉 task 节点和
 *     user_data，等于把内部数据结构的生命周期交给外部代码管理。
 *
 * 线程模型：
 *   - threadpool_submit / threadpool_wait 可以从任意线程调用，包括工作线程
 *     内部（任务函数里再次提交任务）。
 *   - threadpool_wait 不能由工作线程调用：它在等"正在执行的任务数归零"，
 *     而调用它自己就算一个正在执行的任务，必然永久阻塞。本实现对这种调用
 *     返回 TPOOL_ERR_BLOCKING，不进入等待。
 *   - threadpool_destroy 必须由提交方线程调用，且调用时不能有工作线程仍在
 *     调用本模块的函数（否则就是在摧毁对方正在等待的同步原语，属未定义行为）。
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THREADPOOL_VERSION_MAJOR 1
#define THREADPOOL_VERSION_MINOR 1
#define THREADPOOL_VERSION_PATCH 0

#define THREADPOOL_VERSION_STRING "1.1.0"

/* ------------------------------------------------------------------
 * 状态码
 * ------------------------------------------------------------------ */

typedef enum {
	TPOOL_OK           = 0,
	TPOOL_ERR_PARAM    = 1,  /* 参数非法，例如 nthreads < 1、fn == NULL */
	TPOOL_ERR_NOMEM    = 2,  /* 内存或同步原语初始化失败 */
	TPOOL_ERR_SYSTEM   = 3,  /* pthread_create / pthread_* 返回错误码 */
	TPOOL_ERR_CLOSED   = 4,  /* 池已进入停机流程，不再接收新任务 */
	TPOOL_ERR_BLOCKING = 5,  /* 该调用会永久阻塞当前线程，被拒绝执行 */
	TPOOL_ERR_FULL     = 6   /* 有界池的队列已满，任务未被接受（仅 submit 会返回） */
} tpool_status;

/* ------------------------------------------------------------------
 * 对象与任务函数
 * ------------------------------------------------------------------ */

/*
 * 不透明句柄，定义在 src/threadpool.c 里。
 * 类型名与标签名相同（typedef struct threadpool threadpool），
 * 因此 threadpool * 与 struct threadpool * 是同一个类型。
 */
typedef struct threadpool threadpool;

/*
 * 任务函数签名。arg 是 submit 时传入的那个指针，池原样转交、不做任何解释。
 * 任务函数返回即视为该任务完成，它不需要、也不应该释放任何池内对象。
 */
typedef void (*tpool_task_fn)(void *arg);

/* ------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------ */

/*
 * 创建线程池，nthreads 个工作线程在函数返回前已全部创建并进入等待。
 *
 * nthreads < 1 返回 TPOOL_ERR_PARAM（不自动纠正成 1 —— 静默改写调用方
 * 的参数会让"我明明要 0 个线程"这类误用变成"程序跑得比预期慢"）。
 * 任一环节失败时 *out 置为 NULL，池不留下半初始化状态：
 * 已经创建的工作线程会被置位停机标志、broadcast、join 回收之后再返回。
 */
tpool_status threadpool_create(int nthreads, threadpool **out);

/*
 * 创建带背压的线程池：队列最多容纳 max_queued 个待执行任务。
 *
 * 为什么需要它：threadpool_create 的队列无界。生产速度持续高于消费速度时，
 * 队列与任务节点占用的内存随提交量线性增长，最终由 malloc 失败来终止程序——
 * 而这个失败发生在任意一次 submit 里，调用方通常没有任何回退路径。有界队列
 * 把"内存耗尽"提前变成"提交被拒绝或阻塞"，这是一个可以被处理的错误。
 *
 * max_queued 只限制待执行任务数，不限制正在执行的任务：任意时刻在途任务
 * 至多 max_queued + nthreads 个，因此给定 max_queued 与 nthreads 就能算出
 * 内存上限（每个任务节点 sizeof(struct task)，当前 24 字节）。
 *
 * max_queued < 1 返回 TPOOL_ERR_PARAM。不把 0 解释成"无界"——已经有
 * threadpool_create 表达无界，同一个参数位承载两种极端语义只会让调用点难读。
 */
tpool_status threadpool_create_bounded(int nthreads, size_t max_queued, threadpool **out);

/*
 * 优雅停机：拒绝新任务 → 等已提交任务全部执行完 → 唤醒并 join 工作线程 →
 * 销毁同步原语 → 释放池对象。传入 NULL 时直接返回。
 *
 * 队列里剩下的任务会被执行，而不是丢弃。旧实现在这里直接 free 掉未执行的
 * 任务节点，调用方既拿不到返回值也不知道丢了哪些任务 —— 这种"静默丢工作"
 * 在库的接口里是不可接受的默认行为。需要丢弃语义时，调用方自己控制提交时机。
 *
 * 注意：未执行任务的 arg 不会被释放（见文件头的所有权约定）。
 */
void threadpool_destroy(threadpool *p);

/* ------------------------------------------------------------------
 * 提交与同步
 * ------------------------------------------------------------------ */

/*
 * 提交一个任务。成功返回 TPOOL_OK 后，任务必然会被执行一次（除非进程提前退出）。
 * 池已停机（destroy 执行中或已完成）返回 TPOOL_ERR_CLOSED，任务不会被接受，
 * 调用方仍然拥有 arg。
 *
 * 有界池队列已满时立即返回 TPOOL_ERR_FULL，不阻塞、不排队、不丢弃已接受的任务。
 * 任务没有被接受，arg 仍归调用方。是否重试、重试多久由调用方决定——
 * 库在这里替调用方"等一会儿"会让超时与放弃的语义无处安放。
 */
tpool_status threadpool_submit(threadpool *p, tpool_task_fn fn, void *arg);

/*
 * 阻塞式提交：有界池队列已满时，等到出现空位再入队。
 *
 * 返回值：TPOOL_OK 表示已被接受并保证执行一次；TPOOL_ERR_CLOSED 表示等待期间
 * 池进入停机流程（destroy 会唤醒所有正在等待的提交方）；TPOOL_ERR_PARAM /
 * TPOOL_ERR_NOMEM 与 threadpool_submit 相同。
 *
 * 无界池上它等价于 threadpool_submit，永不为队列容量等待。
 *
 * 工作线程调用时不会进入等待：队列满就返回 TPOOL_ERR_FULL。理由是进度
 * （progress）依赖关系——把任务从队列里取走的只有工作线程。若工作线程在满队列上
 * 等待，而所有工作线程都在等空位，就没有任何线程会去消费队列，形成永久阻塞；
 * 更一般地，让消费者变成队列空间的生产者，是引入死锁的标准写法。
 * 所以"工作线程永远不等待空位"这条规则不是保守，而是消除一整类死锁。
 */
tpool_status threadpool_submit_wait(threadpool *p, tpool_task_fn fn, void *arg);

/*
 * 屏障：阻塞到"任务队列为空且没有任务正在执行"。
 * 返回时，所有在此调用之前成功提交的任务都已经执行完毕。
 *
 * 由工作线程调用会永久阻塞（见文件头的线程模型），因此本实现检测到这种情况
 * 直接返回 TPOOL_ERR_BLOCKING。
 *
 * 可重复调用：池已空闲时立即返回 TPOOL_OK。
 * 注意它不是栅栏（fence）：返回之后其它线程再提交的任务不受本次调用约束。
 */
tpool_status threadpool_wait(threadpool *p);

/* ------------------------------------------------------------------
 * 状态查询（都会加锁读取，可在任意时刻调用）
 * ------------------------------------------------------------------ */

/* 队列中尚未被取走的任务数 */
size_t threadpool_queued(const threadpool *p);

/* 正在执行的任务数（已出队、尚未返回的任务） */
size_t threadpool_running(const threadpool *p);

/* 已成功执行完毕的任务累计数，单调递增 */
unsigned long long threadpool_completed(const threadpool *p);

/* 已成功提交的任务累计数；恒等于 queued + running + completed */
unsigned long long threadpool_submitted(const threadpool *p);

/* 工作线程数量，即 create 时请求的 nthreads */
int threadpool_thread_count(const threadpool *p);

/*
 * 队列容量上限；无界池返回 0。
 * 返回 0 与"有界但容量为 0"不会混淆：max_queued < 1 在 create 阶段就被拒绝了。
 */
size_t threadpool_capacity(const threadpool *p);

/*
 * 累计"因队列已满而真正进入等待"的 submit_wait 次数。
 *
 * 它回答的问题是"背压有没有生效"，这是耗时与内存都答不了的问题：
 * 提交端阻塞了 3 ms 与"恰好每次都有空位、只是任务本身慢"，从外部看都是 3 ms。
 * 无界池上恒为 0。
 */
unsigned long long threadpool_backpressure(const threadpool *p);

/* 状态码的英文短名，用于日志 */
const char *threadpool_status_string(tpool_status status);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* THREADPOOL_H */
