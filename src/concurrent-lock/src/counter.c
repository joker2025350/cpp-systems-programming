/*
 * counter.c —— 并发计数器模块的实现
 *
 * 五种实现共用同一个结构体，靠 kind 分派。这里用 switch 而不是函数指针表，
 * 是因为分派只有这一处，表带来的间接跳转反而让 5 个分支更难读；
 * 等实现多到十几个、或者需要运行时注册时再换表。
 */

#include "counter.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 平台能力
 * ------------------------------------------------------------------ */

/*
 * asm 实现用的是 x86 的 lock; xaddq。ARM 上要用 ldaxr/stlxr 或
 * LSE 的 ldadd，指令不同，不能共用一段汇编码。
 *
 * 用条件编译把支持范围写清楚，而不是在运行时靠 SIGILL 才发现。
 */
#if defined(__x86_64__) || defined(__i386__)
#define COUNTER_HAVE_ASM 1
#else
#define COUNTER_HAVE_ASM 0
#endif

/* ------------------------------------------------------------------
 * 内部结构
 * ------------------------------------------------------------------ */

/*
 * union 而不是三个并列成员，是为了让一个 counter 只占"最大那个实现"
 * 的空间。代价是三种实现的值不在同一个偏移上，访问必须分情况写
 * （见 counter_add / counter_get），没法用一个 value_ptr 统一。
 *
 * 这是有意的取舍：省内存、并且避免"raw 与 mutex 同时被初始化"这种
 * 逻辑上不可能的状态被表示出来。
 *
 * 为什么 mutex / spinlock 是指针而不是直接展开成成员：counter_get 接收
 * const counter *，它在语义上只读，但要读到一致的值必须加锁，而加锁会写
 * 锁对象。C 没有 C++ 的 mutable，锁若直接是 union 的成员，从 const 指针上
 * 取 &c->u.mtx.lock 得到的类型是 const pthread_mutex_t *，传给
 * pthread_mutex_lock（形参 pthread_mutex_t *）只能显式转型丢掉 const。
 * 那按 C11 6.7.3p6 只在对象本身不是 const 定义时才良定义——结论没错，
 * 但正确性要靠类型系统之外的知识来保证，编译器只能给一条 -Wcast-qual。
 * 改成指针成员后，const 只限定指针本身，它指向的锁仍然可写，
 * 于是只读接口不需要任何转型。
 *
 * 代价是 mutex / spin 两种实现各多一次分配（每个 counter 一次，不在热路径上）。
 */
struct counter {
	counter_kind kind;
	union {
		long long raw;
		struct {
			pthread_mutex_t *lock;   /* 单独分配，理由见上 */
			long long        value;
		} mtx;
		struct {
			/*
			 * 用 void * 持有而不是 pthread_spinlock_t *：glibc 把
			 * pthread_spinlock_t 定义成 `volatile int` 的 typedef
			 * （bits/pthreadtypes.h:103），于是 pthread_spinlock_t *
			 * 就是 volatile int *，直接传给 free 会丢 volatile 限定符，
			 * 触发 -Wdiscarded-qualifiers（在 -Wall 里）。
			 * 用 void * 保存 malloc 返回的块，加锁时再转成
			 * pthread_spinlock_t *（void * 与对象指针之间的隐式转换
			 * 在 C 里不需要转型），这条路径上就没有任何限定符丢失。
			 */
			void      *lock_mem;
			long long  value;
		} spin;
	} u;
};

/* ------------------------------------------------------------------
 * x86 内联汇编
 * ------------------------------------------------------------------ */

#if COUNTER_HAVE_ASM

/*
 * lock; xaddq 的语义是"交换后相加"：
 *   临时 = 内存值
 *   内存值 = 内存值 + 寄存器值
 *   寄存器 = 临时
 * lock 前缀在执行期间锁定对应的缓存行，使这条读-改-写整体原子。
 *
 * 约束的写法值得单独说，因为这里有两个容易写错的地方：
 *
 *   1. 内存操作数必须是 "+m"（读且写），不能是 "m"（只读）。
 *      xadd 会改写内存，声明成只读等于告诉编译器"执行完这块内存
 *      还是原值"。原始版本写的是 "m"(*value)，之所以没出错，
 *      全靠同时写了 "memory" clobber —— 编译器因此不敢在 asm
 *      前后把内存值缓存到寄存器。可一旦少了那个 clobber，
 *      读到的就是陈旧值。
 *
 *   2. xadd 的源操作数是那个"先装加数、后装旧值"的寄存器，
 *      所以 delta 与 old 必须占同一个寄存器。这里用 "0"(delta)
 *      把输入绑到第 0 个操作数上，直接把这件事说出来；写成
 *      "=a"(old) ... "a"(delta) 则是让编译器自己去发现两个
 *      操作数需要同一个寄存器，能否成立取决于分配结果。
 *
 * 内联汇编的约束写错不会报错，只会算错，所以宁可显式绑定。
 */
static long long asm_add(long long *value, long long delta) {
	long long old;

	__asm__ volatile("lock; xaddq %0, %1"
	                 : "=r"(old), "+m"(*value)
	                 : "0"(delta)
	                 : "cc", "memory");

	/* old 是加之前的旧值，加上 delta 就是新值 */
	return old + delta;
}

#endif /* COUNTER_HAVE_ASM */

/* ------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------ */

counter_status counter_create(counter_kind kind, long long initial, counter **out) {
	counter *c;

	if (out == NULL) {
		return COUNTER_ERR_PARAM;
	}
	*out = NULL;

	if (!counter_kind_supported(kind)) {
		return COUNTER_ERR_UNSUPPORTED;
	}

	c = (counter *)malloc(sizeof *c);
	if (c == NULL) {
		return COUNTER_ERR_NOMEM;
	}
	memset(c, 0, sizeof *c);
	c->kind = kind;

	switch (kind) {
	case COUNTER_KIND_MUTEX: {
		/*
		 * 锁先分配、再初始化。失败路径要按"分配成功但 init 失败"回滚：
		 * 只 free(c) 会漏掉这块锁对象，只 free(lock) 会把 c 留在半初始化
		 * 状态 —— 两者都不会立刻出错，前者是退出时的泄漏，后者是
		 * destroy 时对一个未初始化的锁调 pthread_mutex_destroy。
		 */
		pthread_mutex_t *lock = (pthread_mutex_t *)malloc(sizeof *lock);

		if (lock == NULL) {
			free(c);
			return COUNTER_ERR_NOMEM;
		}
		if (pthread_mutex_init(lock, NULL) != 0) {
			free(lock);
			free(c);
			return COUNTER_ERR_NOMEM;
		}
		c->u.mtx.lock  = lock;
		c->u.mtx.value = initial;
		break;
	}

	case COUNTER_KIND_SPIN: {
		/*
		 * 第二个参数是 pshared。这里用 PTHREAD_PROCESS_PRIVATE，
		 * 因为计数器只在本进程的线程之间共享。
		 * 若要用 PTHREAD_PROCESS_SHARED 放进共享内存给多进程用，
		 * 锁本身还必须位于共享映射里——只把 flag 改成 SHARED 是不够的。
		 *
		 * 分配块用 void * 接住，理由见 struct counter 里 spin.lock_mem
		 * 的注释（pthread_spinlock_t 是 volatile int 的 typedef）。
		 */
		void               *mem = malloc(sizeof(pthread_spinlock_t));
		pthread_spinlock_t *lock;

		if (mem == NULL) {
			free(c);
			return COUNTER_ERR_NOMEM;
		}
		lock = mem;
		if (pthread_spin_init(lock, PTHREAD_PROCESS_PRIVATE) != 0) {
			free(mem);
			free(c);
			return COUNTER_ERR_NOMEM;
		}
		c->u.spin.lock_mem = mem;
		c->u.spin.value    = initial;
		break;
	}

	default:
		/* raw / atomic / asm 共用一个 long long */
		c->u.raw = initial;
		break;
	}

	*out = c;
	return COUNTER_OK;
}

void counter_destroy(counter *c) {
	if (c == NULL) {
		return;
	}

	switch (c->kind) {
	case COUNTER_KIND_MUTEX:
		(void)pthread_mutex_destroy(c->u.mtx.lock);
		free(c->u.mtx.lock);
		break;
	case COUNTER_KIND_SPIN:
		/*
		 * 先销毁锁，再释放承载它的内存块：pthread_spin_destroy 之后
		 * 这块内存就不能再被任何线程使用，但 free 之前它仍然存在。
		 * 顺序反过来（先 free 再 destroy）是对已释放内存操作，
		 * 按 C11 6.2.4p2 属于未定义行为。
		 */
		(void)pthread_spin_destroy((pthread_spinlock_t *)c->u.spin.lock_mem);
		free(c->u.spin.lock_mem);
		break;
	default:
		break;
	}

	free(c);
}

/* ------------------------------------------------------------------
 * 读写
 * ------------------------------------------------------------------ */

long long counter_add(counter *c, long long delta) {
	switch (c->kind) {
	case COUNTER_KIND_MUTEX:
		(void)pthread_mutex_lock(c->u.mtx.lock);
		c->u.mtx.value += delta;
		{
			long long value = c->u.mtx.value;
			(void)pthread_mutex_unlock(c->u.mtx.lock);
			return value;
		}

	case COUNTER_KIND_SPIN:
		{
			pthread_spinlock_t *lock = c->u.spin.lock_mem;

			(void)pthread_spin_lock(lock);
			c->u.spin.value += delta;
			{
				long long value = c->u.spin.value;
				(void)pthread_spin_unlock(lock);
				return value;
			}
		}

	case COUNTER_KIND_ATOMIC:
		/*
		 * __atomic_add_fetch 返回加之后的值，与接口语义一致。
		 * 用 SEQ_CST 是最强的顺序，为了让"最终值等于期望值"这件事
		 * 不依赖调用方对内存序的理解；需要更快时再降为 RELAXED。
		 */
		return __atomic_add_fetch(&c->u.raw, delta, __ATOMIC_SEQ_CST);

	case COUNTER_KIND_ASM:
#if COUNTER_HAVE_ASM
		return asm_add(&c->u.raw, delta);
#else
		/* counter_create 已经挡掉这种情况，这里只为让编译器满意 */
		return c->u.raw += delta;
#endif

	case COUNTER_KIND_RAW:
	default:
		/*
		 * 故意的数据竞争。
		 * 按 C11 5.1.2.4，两个线程无同步地访问同一个对象、
		 * 且至少一个是写，就是 data race，行为未定义。
		 * 这里的未定义不是"结果可能偏小"，而是"程序本身是错的"——
		 * 编译器可以假设不存在这种访问，从而做出任意优化。
		 * 保留它只是为了给基准程序一个反面样本。
		 */
		c->u.raw += delta;
		return c->u.raw;
	}
}

long long counter_get(const counter *c) {
	switch (c->kind) {
	case COUNTER_KIND_MUTEX:
		/*
		 * 从 const counter * 上读 c->u.mtx.lock，得到的类型是
		 * pthread_mutex_t *const（const 限定的是这个指针值，不是它指向的锁），
		 * 拷进一个普通指针即可，不需要转型。上一版这里写的是
		 * (pthread_mutex_t *)&c->u.mtx.lock —— 锁直接嵌在结构里时，只能靠
		 * 丢 const 才能加锁，见 struct counter 上面的说明。
		 */
		{
			pthread_mutex_t *lock = c->u.mtx.lock;
			long long        value;

			(void)pthread_mutex_lock(lock);
			value = c->u.mtx.value;
			(void)pthread_mutex_unlock(lock);
			return value;
		}

	case COUNTER_KIND_SPIN:
		{
			pthread_spinlock_t *lock = c->u.spin.lock_mem;
			long long           value;

			(void)pthread_spin_lock(lock);
			value = c->u.spin.value;
			(void)pthread_spin_unlock(lock);
			return value;
		}

	case COUNTER_KIND_ATOMIC:
	case COUNTER_KIND_ASM:
		/*
		 * 原子载入。RELAXED 就够了：这里只要求"不读到撕裂的值"，
		 * 不需要与别的内存操作建立先后关系。
		 */
		return __atomic_load_n(&c->u.raw, __ATOMIC_RELAXED);

	case COUNTER_KIND_RAW:
	default:
		/*
		 * 用原子载入读，至少保证这一次读不会撕裂。
		 * 但写端仍是无同步的普通写，两边的访问仍构成数据竞争，
		 * 所以这个"保证"只在读端单独看时成立，整体依然是 UB。
		 */
		return __atomic_load_n(&c->u.raw, __ATOMIC_RELAXED);
	}
}

/* ------------------------------------------------------------------
 * 辅助
 * ------------------------------------------------------------------ */

const char *counter_kind_name(counter_kind kind) {
	switch (kind) {
	case COUNTER_KIND_RAW:    return "raw";
	case COUNTER_KIND_MUTEX:  return "mutex";
	case COUNTER_KIND_SPIN:   return "spin";
	case COUNTER_KIND_ATOMIC: return "atomic";
	case COUNTER_KIND_ASM:    return "asm";
	default:                  return "unknown";
	}
}

int counter_kind_supported(counter_kind kind) {
	switch (kind) {
	case COUNTER_KIND_RAW:
	case COUNTER_KIND_MUTEX:
	case COUNTER_KIND_SPIN:
	case COUNTER_KIND_ATOMIC:
		return 1;
	case COUNTER_KIND_ASM:
		return COUNTER_HAVE_ASM;
	default:
		return 0;
	}
}

int counter_kind_is_atomic(counter_kind kind) {
	return (kind == COUNTER_KIND_RAW) ? 0 : 1;
}

const char *counter_status_string(counter_status status) {
	switch (status) {
	case COUNTER_OK:              return "success";
	case COUNTER_ERR_PARAM:       return "invalid argument";
	case COUNTER_ERR_NOMEM:       return "out of memory";
	case COUNTER_ERR_UNSUPPORTED: return "unsupported on this platform";
	default:                      return "unknown status";
	}
}
