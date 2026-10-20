/*
 * threadpool.c —— 线程池模块的实现
 *
 * 结构：一条 FIFO 单链表任务队列 + 固定数组的工作线程 ID + 一个互斥锁 + 两个条件变量。
 *
 * 为什么是两个条件变量而不是一个：
 *   工作线程在"队列空"时等待，threadpool_wait 在"队列非空或仍有任务在执行"时等待，
 *   两者等待的条件正好相反。共用一个条件变量时，每次状态变化只能 broadcast，
 *   否则可能唤醒错的那一类等待者、而该唤醒的却没醒（丢失唤醒）。
 *   broadcast 会把所有等待者全部唤醒再让它们各自重新检查条件，正确但代价是
 *   "惊群"（thundering herd）：任务提交一次，N 个空闲线程全被唤醒抢一个任务。
 *   分开之后，submit 只 signal 工作线程，任务完成只 broadcast 等待空闲者，
 *   两类等待者互不打扰，submit 侧也只需要一次 signal。
 *
 * 为什么队列用 FIFO（尾插头取）而不是旧实现的头插头取：
 *   头插头取就是 LIFO，最后提交的任务最先执行。在"提交速度持续高于消费速度"
 *   的负载下，先提交的任务会被后提交的不断压到队尾，可能长时间得不到执行。
 *   这里的代价只是一次 tail 指针维护。
 */

#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>

/* ------------------------------------------------------------------
 * 内部结构
 * ------------------------------------------------------------------ */

/* 任务节点。归池所有，submit 时分配，任务函数返回后释放。 */
typedef struct task {
	tpool_task_fn fn;
	void         *arg;
	struct task  *next;
} task;

struct threadpool {
	pthread_mutex_t lock;

	/*
	 * work：工作线程等"有新任务或需要退出"。
	 * idle：threadpool_wait / threadpool_destroy 等"队列空且无人执行"。
	 */
	pthread_cond_t  work;
	pthread_cond_t  idle;

	/* 任务队列，head 出队、tail 入队，构成 FIFO */
	task  *head;
	task  *tail;

	/* 以下计数全部在 lock 保护下读写 */
	size_t             queued;      /* 在队列里、尚未被取走 */
	size_t             running;     /* 已出队、尚未返回 */
	unsigned long long submitted;   /* 累计提交成功 */
	unsigned long long completed;   /* 累计执行完成 */

	pthread_t *tids;                /* 工作线程 ID 数组，长度 nthreads */
	int        nthreads;            /* 请求的线程数 */
	int        started;             /* 实际创建成功的线程数 */

	int        closed;              /* 1 = 拒绝新任务 */
	int        stop;                /* 1 = 队列已空，工作线程应退出 */
};

/* ------------------------------------------------------------------
 * 工作线程主循环
 * ------------------------------------------------------------------ */

static void *worker_main(void *arg) {
	threadpool *p = (threadpool *)arg;

	pthread_mutex_lock(&p->lock);

	for (;;) {
		/*
		 * 等待条件必须写成循环：pthread_cond_wait 返回并不代表条件成立。
		 * POSIX 允许虚假唤醒（spurious wakeup），也可能被 broadcast 唤醒
		 * 但任务已被别的工作线程抢走。单次 if 判断在这两种情形下都会
		 * 让线程去取一个不存在的任务。
		 */
		while (p->head == NULL && !p->stop) {
			pthread_cond_wait(&p->work, &p->lock);
		}

		/* 退出条件：停机标志已置位，且队列已排空 */
		if (p->head == NULL && p->stop) {
			break;
		}

		/* 出队：从表头取，保持 FIFO */
		task *t = p->head;
		p->head = t->next;
		if (p->head == NULL) {
			p->tail = NULL;
		}
		p->queued--;
		p->running++;

		/*
		 * 执行阶段不持锁。若持锁执行，所有工作线程会串行化，
		 * 线程池退化成"一个线程干活 + N-1 个线程排队等锁"。
		 */
		pthread_mutex_unlock(&p->lock);

		t->fn(t->arg);
		free(t);

		pthread_mutex_lock(&p->lock);
		p->running--;
		p->completed++;

		/* 队列空且无人执行 —— 等待空闲的那一方可以醒了 */
		if (p->head == NULL && p->running == 0) {
			pthread_cond_broadcast(&p->idle);
		}
	}

	pthread_mutex_unlock(&p->lock);

	return NULL;
}

/* ------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------ */

tpool_status threadpool_create(int nthreads, threadpool **out) {
	threadpool *p;
	int         rc;

	if (out == NULL) {
		return TPOOL_ERR_PARAM;
	}
	*out = NULL;

	if (nthreads < 1) {
		return TPOOL_ERR_PARAM;
	}

	/*
	 * calloc 而不是 malloc + memset：两者都会把对象清零，
	 * calloc 的语义就是"清零的数组"，意图更直接。
	 */
	p = (threadpool *)calloc(1, sizeof *p);
	if (p == NULL) {
		return TPOOL_ERR_NOMEM;
	}

	p->tids = (pthread_t *)calloc((size_t)nthreads, sizeof *p->tids);
	if (p->tids == NULL) {
		free(p);
		return TPOOL_ERR_NOMEM;
	}
	p->nthreads = nthreads;

	/*
	 * 运行期初始化必须调用 init 函数，不能用 memcpy 复制
	 * PTHREAD_MUTEX_INITIALIZER / PTHREAD_COND_INITIALIZER 的内部表示：
	 * 这两个宏按 POSIX 只保证可用于静态初始化，其展开内容由实现决定。
	 */
	rc = pthread_mutex_init(&p->lock, NULL);
	if (rc != 0) {
		free(p->tids);
		free(p);
		return TPOOL_ERR_SYSTEM;
	}

	rc = pthread_cond_init(&p->work, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&p->lock);
		free(p->tids);
		free(p);
		return TPOOL_ERR_SYSTEM;
	}

	rc = pthread_cond_init(&p->idle, NULL);
	if (rc != 0) {
		pthread_cond_destroy(&p->work);
		pthread_mutex_destroy(&p->lock);
		free(p->tids);
		free(p);
		return TPOOL_ERR_SYSTEM;
	}

	for (int i = 0; i < nthreads; i++) {
		rc = pthread_create(&p->tids[i], NULL, worker_main, p);
		if (rc != 0) {
			/*
			 * 回滚。旧实现在这里直接 return，已创建的工作线程既没被置停机
			 * 标志也没被 join，它们继续在一个已经被调用方遗忘的池上等待任务：
			 * 线程泄漏，且池对象释放后这些线程持有的是悬空指针。
			 *
			 * 顺序：先置 stop 并 broadcast（让线程看到退出条件），
			 * 再 join，最后才析构同步原语 —— 反过来的话，线程可能正在
			 * 一个已销毁的条件变量上等待，属未定义行为。
			 */
			pthread_mutex_lock(&p->lock);
			p->stop = 1;
			pthread_cond_broadcast(&p->work);
			pthread_mutex_unlock(&p->lock);

			for (int j = 0; j < p->started; j++) {
				pthread_join(p->tids[j], NULL);
			}

			pthread_cond_destroy(&p->idle);
			pthread_cond_destroy(&p->work);
			pthread_mutex_destroy(&p->lock);
			free(p->tids);
			free(p);
			return TPOOL_ERR_SYSTEM;
		}
		p->started++;
	}

	*out = p;
	return TPOOL_OK;
}

void threadpool_destroy(threadpool *p) {
	if (p == NULL) {
		return;
	}

	pthread_mutex_lock(&p->lock);

	/* 1) 关门：此后 submit 一律返回 TPOOL_ERR_CLOSED */
	p->closed = 1;

	/*
	 * 2) 等已提交任务全部执行完。
	 *    这一步让 destroy 成为"优雅停机"而不是"丢弃队列"。
	 *    等待期间工作线程照常取任务，所以不会因为这里持锁而饿死 ——
	 *    pthread_cond_wait 会释放锁。
	 */
	while (p->queued > 0 || p->running > 0) {
		pthread_cond_wait(&p->idle, &p->lock);
	}

	/* 3) 队列已空，通知工作线程退出 */
	p->stop = 1;
	pthread_cond_broadcast(&p->work);
	pthread_mutex_unlock(&p->lock);

	/* 4) join 之后线程确实不再运行，才能析构它可能正在使用的同步原语 */
	for (int i = 0; i < p->started; i++) {
		pthread_join(p->tids[i], NULL);
	}

	/*
	 * 5) 防御性清理。正常路径上队列已经排空，这里为空操作；
	 *    保留是为了让"有不变量被破坏"时表现为可控的内存回收，
	 *    而不是把节点留给 LeakSanitizer 去猜。
	 */
	task *t = p->head;
	while (t != NULL) {
		task *next = t->next;
		free(t);
		t = next;
	}

	pthread_cond_destroy(&p->idle);
	pthread_cond_destroy(&p->work);
	pthread_mutex_destroy(&p->lock);
	free(p->tids);
	free(p);
}

/* ------------------------------------------------------------------
 * 提交与同步
 * ------------------------------------------------------------------ */

tpool_status threadpool_submit(threadpool *p, tpool_task_fn fn, void *arg) {
	if (p == NULL || fn == NULL) {
		return TPOOL_ERR_PARAM;
	}

	/*
	 * 先在锁外分配：malloc 可能进入内核（arena 扩展、mmap），
	 * 放在临界区里会让所有工作线程陪着等一次内存分配。
	 */
	task *t = (task *)malloc(sizeof *t);
	if (t == NULL) {
		return TPOOL_ERR_NOMEM;
	}
	t->fn   = fn;
	t->arg  = arg;
	t->next = NULL;

	pthread_mutex_lock(&p->lock);

	if (p->closed) {
		pthread_mutex_unlock(&p->lock);
		free(t);
		return TPOOL_ERR_CLOSED;
	}

	/* 尾插，配合表头出队构成 FIFO */
	if (p->tail != NULL) {
		p->tail->next = t;
	} else {
		p->head = t;
	}
	p->tail = t;

	p->queued++;
	p->submitted++;

	/* 只唤醒一个空闲线程：一个任务只需要一个消费者 */
	pthread_cond_signal(&p->work);

	pthread_mutex_unlock(&p->lock);

	return TPOOL_OK;
}

tpool_status threadpool_wait(threadpool *p) {
	if (p == NULL) {
		return TPOOL_ERR_PARAM;
	}

	pthread_mutex_lock(&p->lock);

	/*
	 * 拒绝工作线程调用。检测方式是拿 pthread_self 与 tids 数组逐个
	 * pthread_equal 比较：pthread_t 不保证是可比较大小的算术类型，
	 * 标准只提供 pthread_equal 来判等，用 == 在部分平台编译不过或结果错误。
	 *
	 * 不检测的后果是永久阻塞：本函数等的是 running == 0，而调用者自己
	 * 正是一个 running 的任务，等式永远不成立，且没有任何超时机制能救。
	 */
	{
		pthread_t self = pthread_self();
		for (int i = 0; i < p->started; i++) {
			if (pthread_equal(p->tids[i], self) != 0) {
				pthread_mutex_unlock(&p->lock);
				return TPOOL_ERR_BLOCKING;
			}
		}
	}

	while (p->queued > 0 || p->running > 0) {
		pthread_cond_wait(&p->idle, &p->lock);
	}

	pthread_mutex_unlock(&p->lock);
	return TPOOL_OK;
}

/* ------------------------------------------------------------------
 * 状态查询
 * ------------------------------------------------------------------ */

/*
 * 查询函数接收 const 指针，但加锁需要可变指针（pthread_mutex_lock 的
 * 语义会修改锁对象）。const 在这里表达的是"不改变池的逻辑状态"，
 * 锁的内部状态变化不属于调用方可见的状态，因此转型是安全的。
 */
size_t threadpool_queued(const threadpool *p) {
	if (p == NULL) {
		return 0;
	}
	threadpool *mp = (threadpool *)p;
	pthread_mutex_lock(&mp->lock);
	size_t n = mp->queued;
	pthread_mutex_unlock(&mp->lock);
	return n;
}

size_t threadpool_running(const threadpool *p) {
	if (p == NULL) {
		return 0;
	}
	threadpool *mp = (threadpool *)p;
	pthread_mutex_lock(&mp->lock);
	size_t n = mp->running;
	pthread_mutex_unlock(&mp->lock);
	return n;
}

unsigned long long threadpool_completed(const threadpool *p) {
	if (p == NULL) {
		return 0;
	}
	threadpool *mp = (threadpool *)p;
	pthread_mutex_lock(&mp->lock);
	unsigned long long n = mp->completed;
	pthread_mutex_unlock(&mp->lock);
	return n;
}

unsigned long long threadpool_submitted(const threadpool *p) {
	if (p == NULL) {
		return 0;
	}
	threadpool *mp = (threadpool *)p;
	pthread_mutex_lock(&mp->lock);
	unsigned long long n = mp->submitted;
	pthread_mutex_unlock(&mp->lock);
	return n;
}

int threadpool_thread_count(const threadpool *p) {
	if (p == NULL) {
		return 0;
	}
	return p->nthreads;   /* 创建后不变，无需加锁 */
}

const char *threadpool_status_string(tpool_status status) {
	switch (status) {
	case TPOOL_OK:           return "ok";
	case TPOOL_ERR_PARAM:    return "invalid argument";
	case TPOOL_ERR_NOMEM:    return "out of memory";
	case TPOOL_ERR_SYSTEM:   return "pthread call failed";
	case TPOOL_ERR_CLOSED:   return "pool closed";
	case TPOOL_ERR_BLOCKING: return "call would block forever";
	}
	return "unknown";
}
