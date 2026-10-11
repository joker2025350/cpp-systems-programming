

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>



#define LIST_INSERT(item, list) do {	\
	item->prev = NULL;					\
	item->next = list;					\
	if ((list) != NULL) (list)->prev = item; \
	(list) = item;						\
} while(0)


#define LIST_REMOVE(item, list) do {	\
	if (item->prev != NULL) item->prev->next = item->next; \
	if (item->next != NULL) item->next->prev = item->prev; \
	if (list == item) list = item->next; 					\
	item->prev = item->next = NULL;							\
} while(0)



struct nTask {
	void (*task_func)(struct nTask *task);
	void *user_data;

	struct nTask *prev;
	struct nTask *next;
};


struct nWorker {
	pthread_t threadid;
	int terminate;
	struct nManager *manager;

	struct nWorker *prev;
	struct nWorker *next;
};


typedef struct nManager {
	struct nTask *tasks;
	struct nWorker *workers;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
} ThreadPool;


// callback != task
//
// 注意：本函数不再释放 worker 结构体。worker 由 nThreadPoolDestory 统一释放。
// 若这里也 free，销毁流程再 free 一次就是双重释放（double free），属未定义行为。
static void *nThreadPoolCallback(void *arg) {

	struct nWorker *worker = (struct nWorker*)arg;

	while (1) {

		pthread_mutex_lock(&worker->manager->mutex);
		while (worker->manager->tasks == NULL) {
			if (worker->terminate) break;
			pthread_cond_wait(&worker->manager->cond, &worker->manager->mutex);
		}
		if (worker->terminate) {
			pthread_mutex_unlock(&worker->manager->mutex);
			break;
		}

		struct nTask *task = worker->manager->tasks;
		LIST_REMOVE(task, worker->manager->tasks);

		pthread_mutex_unlock(&worker->manager->mutex);

		task->task_func(task);
	}

	return NULL;

}

// API
int nThreadPoolCreate(ThreadPool *pool, int numWorkers) {

	int ret = 0;

	if (pool == NULL) return -1;
	if (numWorkers < 1) numWorkers = 1;
	memset(pool, 0, sizeof(ThreadPool));

	// 运行期初始化必须调用 init 函数。
	// PTHREAD_MUTEX_INITIALIZER / PTHREAD_COND_INITIALIZER 这两个宏
	// 只在定义静态变量时可用，用 memcpy 复制它们的内部表示不可移植。
	ret = pthread_mutex_init(&pool->mutex, NULL);
	if (ret) {
		printf("pthread_mutex_init : %d\n", ret);
		return -1;
	}

	ret = pthread_cond_init(&pool->cond, NULL);
	if (ret) {
		printf("pthread_cond_init : %d\n", ret);
		pthread_mutex_destroy(&pool->mutex);
		return -1;
	}

	int i = 0;
	for (i = 0;i < numWorkers;i ++) {
		struct nWorker *worker = (struct nWorker*)malloc(sizeof(struct nWorker));
		if (worker == NULL) {
			perror("malloc");
			return -2;
		}
		memset(worker, 0, sizeof(struct nWorker));
		worker->manager = pool; //

		ret = pthread_create(&worker->threadid, NULL, nThreadPoolCallback, worker);
		if (ret) {
			perror("pthread_create");
			free(worker);
			return -3;
		}

		LIST_INSERT(worker, pool->workers);
	}

	// success
	return 0;

}

// API
//
// 销毁顺序：置标志 → 唤醒 → join → 释放。
// 每一步的前后关系都不能调换，理由见行内注释。
int nThreadPoolDestory(ThreadPool *pool, int nWorker) {

	if (pool == NULL) return -1;

	(void)nWorker; // 线程数量无需在此使用，保留参数以兼容原接口

	// 1) 置终止标志
	//    必须在持锁状态下写，否则与工作线程读取该标志构成数据竞争
	pthread_mutex_lock(&pool->mutex);

	struct nWorker *worker = NULL;
	for (worker = pool->workers;worker != NULL;worker = worker->next) {
		worker->terminate = 1;
	}

	// 2) 唤醒全部阻塞在条件变量上的线程，让它们重新检查终止标志
	pthread_cond_broadcast(&pool->cond);

	pthread_mutex_unlock(&pool->mutex);

	// 3) 等待每个工作线程真正结束，再释放它占用的结构体
	//    join 必须在 free 之前：线程仍在运行时它的结构体不能被回收
	worker = pool->workers;
	while (worker != NULL) {
		struct nWorker *next = worker->next;
		pthread_join(worker->threadid, NULL);
		free(worker);
		worker = next;
	}
	pool->workers = NULL;

	// 4) 释放队列中尚未被取走的任务
	//    注意这里只释放 task 结构体本身。task->user_data 的释放方式
	//    只有对应的 task_func 知道，因此未被执行的任务，其 user_data
	//    需要由提交方自行回收。
	struct nTask *task = pool->tasks;
	while (task != NULL) {
		struct nTask *next = task->next;
		free(task);
		task = next;
	}
	pool->tasks = NULL;

	// 5) 销毁同步原语
	pthread_mutex_destroy(&pool->mutex);
	pthread_cond_destroy(&pool->cond);

	return 0;

}


// API
int nThreadPoolPushTask(ThreadPool *pool, struct nTask *task) {

	if (pool == NULL || task == NULL) return -1;

	pthread_mutex_lock(&pool->mutex);

	LIST_INSERT(task, pool->tasks);

	pthread_cond_signal(&pool->cond);

	pthread_mutex_unlock(&pool->mutex);

	return 0;

}




//
// sdk --> debug thread pool

#if 1

#define THREADPOOL_INIT_COUNT	20
#define TASK_INIT_SIZE			1000


void task_entry(struct nTask *task) { //type

	//struct nTask *task = (struct nTask*)task;
	int idx = *(int *)task->user_data;

	printf("idx: %d\n", idx);

	free(task->user_data);
	free(task);
}


int main(void) {

	ThreadPool pool = {0};

	nThreadPoolCreate(&pool, THREADPOOL_INIT_COUNT);
	// pool --> memset();

	int i = 0;
	for (i = 0;i < TASK_INIT_SIZE;i ++) {
		struct nTask *task = (struct nTask *)malloc(sizeof(struct nTask));
		if (task == NULL) {
			perror("malloc");
			exit(1);
		}
		memset(task, 0, sizeof(struct nTask));

		task->task_func = task_entry;
		task->user_data = malloc(sizeof(int));
		*(int*)task->user_data  = i;


		nThreadPoolPushTask(&pool, task);
	}

	getchar();

	// 销毁线程池。原代码缺这一步，导致工作线程从未被回收
	nThreadPoolDestory(&pool, THREADPOOL_INIT_COUNT);

	return 0;

}


#endif

