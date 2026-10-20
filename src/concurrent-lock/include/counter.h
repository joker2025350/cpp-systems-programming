/*
 * counter.h —— 并发计数器模块的公开接口
 *
 * 模块职责：提供同一个"原子/非原子自增"语义的五种实现，
 * 让调用方在同一套接口下比较它们的正确性与吞吐。
 *
 * 五种实现：
 *   raw     裸自增，没有任何同步。数据竞争，结果必然小于期望值。
 *           存在的意义只有一个：证明竞态确实会丢更新。
 *   mutex   pthread_mutex_lock / unlock
 *   spin    pthread_spin_lock / unlock
 *   atomic  编译器 builtin __atomic_add_fetch，可移植
 *   asm     x86 内联汇编 lock; xaddq，仅在 x86 上可用
 *
 * 为什么用不透明句柄（opaque handle）而不像 04/05 那样公开结构体：
 * 内部要放 pthread_mutex_t 与 pthread_spinlock_t。公开它们意味着头文件
 * 必须 include <pthread.h>，把 pthread 的类型和宏泄漏给每一个调用方；
 * 还要固定住两种锁在结构体里的偏移，任何平台只要 pthread 内部布局不同
 * 就 ABI 不兼容。用不透明句柄，头文件只依赖 <stddef.h>，
 * 内部结构随便改，调用方不必重新编译。
 *
 * 代价是对象只能在堆上分配。这一点与本仓库 04/05 有意不同：
 * 那两个模块的成员都是定长数组与指针，栈分配没有副作用；
 * 这里不能，因为同步原语必须由 init 函数初始化。
 *
 * 线程模型：所有函数对同一个 counter 的并发调用都是安全的
 * （raw 除外——它按定义不做任何同步）。
 */

#ifndef COUNTER_H
#define COUNTER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COUNTER_VERSION_MAJOR 1
#define COUNTER_VERSION_MINOR 0
#define COUNTER_VERSION_PATCH 0

#define COUNTER_VERSION_STRING "1.0.0"

/* ------------------------------------------------------------------
 * 实现种类
 * ------------------------------------------------------------------ */

typedef enum {
	COUNTER_KIND_RAW    = 0,   /* 无同步，仅用于演示数据竞争 */
	COUNTER_KIND_MUTEX  = 1,
	COUNTER_KIND_SPIN   = 2,
	COUNTER_KIND_ATOMIC = 3,   /* __atomic_* builtin */
	COUNTER_KIND_ASM    = 4    /* x86 内联汇编 */
} counter_kind;

#define COUNTER_KIND_COUNT 5

/* ------------------------------------------------------------------
 * 状态码
 * ------------------------------------------------------------------ */

typedef enum {
	COUNTER_OK              = 0,
	COUNTER_ERR_PARAM       = 1,   /* 参数非法 */
	COUNTER_ERR_NOMEM       = 2,   /* 内存或同步原语初始化失败 */
	COUNTER_ERR_UNSUPPORTED = 3    /* 当前平台不支持这种实现 */
} counter_status;

/* ------------------------------------------------------------------
 * 对象
 * ------------------------------------------------------------------ */

/*
 * 不透明句柄。定义在 counter.c 里，调用方只持有指针。
 * 类型名与标签名相同（typedef struct counter counter），
 * 因此既可以直接叫 counter *，也可以叫 struct counter *。
 */
typedef struct counter counter;

/*
 * 创建一个计数器，初值为 initial。
 *
 * 成功时把新对象的指针写入 *out 并返回 COUNTER_OK；
 * 失败时 *out 被置为 NULL，不会留下野指针。
 *
 * kind 在当前平台不可用时返回 COUNTER_ERR_UNSUPPORTED——
 * 例如 asm 模式在非 x86 平台上。调用方应当先用
 * counter_kind_supported 判断，而不是把失败当成内存不足。
 */
counter_status counter_create(counter_kind kind, long long initial, counter **out);

/*
 * 销毁计数器，释放内部同步原语。
 * 传入 NULL 时直接返回。销毁之后指针不再有效。
 */
void counter_destroy(counter *c);

/*
 * 加上 delta，返回加上之后的值。
 *
 * 注意 raw 实现的返回值不可信：它在写入之后才读回，
 * 而另一个线程可能刚好在两者之间改掉了值。
 * 需要"读回自己写入的结果"就必须用带同步的实现。
 */
long long counter_add(counter *c, long long delta);

/*
 * 读取当前值。
 *
 * 对 mutex / spin 实现会加锁读取；对 atomic / asm 实现用一次原子载入；
 * 对 raw 实现直接读——如果此时有别的线程在写，这就是数据竞争。
 */
long long counter_get(const counter *c);

/* ------------------------------------------------------------------
 * 辅助
 * ------------------------------------------------------------------ */

/* 种类的短名："raw" / "mutex" / "spin" / "atomic" / "asm" */
const char *counter_kind_name(counter_kind kind);

/* 当前平台上这种实现是否可编译、可运行 */
int counter_kind_supported(counter_kind kind);

/*
 * 这种实现是否保证结果正确。
 * 只有 raw 返回 0。用于基准程序决定要不要把期望值当成断言。
 */
int counter_kind_is_atomic(counter_kind kind);

const char *counter_status_string(counter_status status);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* COUNTER_H */
