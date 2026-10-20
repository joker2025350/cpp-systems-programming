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
#define THREADPOOL_VERSION_MINOR 0
#define THREADPOOL_VERSION_PATCH 0

#define THREADPOOL_VERSION_STRING "1.0.0"

/* ------------------------------------------------------------------
 * 状态码
 * ------------------------------------------------------------------ */

typedef enum {
	TPOOL_OK           = 0,
	TPOOL_ERR_PARAM    = 1,  /* 参数非法，例如 nthreads < 1、fn == NULL */
	TPOOL_ERR_NOMEM    = 2,  /* 内存或同步原语初始化失败 */
	TPOOL_ERR_SYSTEM   = 3,  /* pthread_create / pthread_* 返回错误码 */
	TPOOL_ERR_CLOSED   = 4,  /* 池已进入停机流程，不再接收新任务 */
	TPOOL_ERR_BLOCKING = 5   /* 该调用会永久阻塞当前线程，被拒绝执行 */
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
 */
tpool_status threadpool_submit(threadpool *p, tpool_task_fn fn, void *arg);

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

/* 状态码的英文短名，用于日志 */
const char *threadpool_status_string(tpool_status status);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* THREADPOOL_H */
