/* ============================================================================
 *  threadpool_bug_demo.c
 *  —— 【故意写错版】用来亲眼看原代码那个 bug 的后果
 *
 *  与 threadpool_win32.c 的唯一区别：
 *      正确的： worker->terminate = 1;   （设置"该下班了"）
 *      本文件的：worker->terminate;       （原代码的写法，什么也没做）
 *
 *  观察重点：1000 个任务依然会全部做完（问题不在干活，而在收工），
 *            但程序会永远卡住、再也不结束。
 *            原因：工人永远收不到下岗通知，销毁函数里的
 *                  WaitForSingleObject 就会一直等下去。
 *
 *  这一版能编译通过，编译器一个警告都不给。这就是它危险的地方。
 * ============================================================================ */

/* ============================================================================
 *  threadpool_win32.c
 *  —— threadpool.c 的「Windows 可运行孪生版」
 *
 *  为什么要做这一份？
 *    原代码用的是 POSIX 的 pthread，Windows 的 MSVC 里没有这个库，
 *    所以你在本机根本编不过、看不到效果。
 *    本文件把 pthread 的每个函数一一换成 Windows 的等价物，
 *    结构、变量名、执行流程【完全一致】，另外修掉了原代码里的几个 bug。
 *
 *  【pthread → Windows 对照表】（这是本文件最值得记住的一张表）
 *    ┌─────────────────────────────┬──────────────────────────────────────────┐
 *    │ POSIX (Linux/macOS)         │ Windows (Win32 API)                      │
 *    ├─────────────────────────────┼──────────────────────────────────────────┤
 *    │ pthread_t                   │ HANDLE                                   │
 *    │ pthread_mutex_t             │ CRITICAL_SECTION                         │
 *    │ pthread_cond_t              │ CONDITION_VARIABLE                       │
 *    │ pthread_create              │ CreateThread                             │
 *    │ pthread_join                │ WaitForSingleObject(handle, INFINITE)     │
 *    │ pthread_mutex_init          │ InitializeCriticalSection                │
 *    │ pthread_mutex_lock          │ EnterCriticalSection                     │
 *    │ pthread_mutex_unlock        │ LeaveCriticalSection                     │
 *    │ pthread_cond_init           │ InitializeConditionVariable              │
 *    │ pthread_cond_wait           │ SleepConditionVariableCS                 │
 *    │ pthread_cond_signal         │ WakeConditionVariable                    │
 *    │ pthread_cond_broadcast      │ WakeAllConditionVariable                 │
 *    │ PTHREAD_XXX_INITIALIZER     │ （没有对应物，必须显式调用 init 函数）      │
 *    └─────────────────────────────┴──────────────────────────────────────────┘
 *
 *  编译运行：
 *    win_cpp.py build threadpool_win32.c --out build
 *    win_cpp.py run build/threadpool_win32.exe
 * ============================================================================ */


/* ---------------------------------------------------------------------------
 * 第 1 部分：头文件
 * ------------------------------------------------------------------------- */

#define WIN32_LEAN_AND_MEAN
/*  告诉 windows.h："只给我常用那部分，别把那些又老又少用的头文件都拉进来。"
 *  作用是加快编译速度、减少名字冲突。定义必须写在 #include <windows.h> 之前。 */

#include <windows.h>
/*  Windows 的总头文件。上面那张对照表里的所有类型和函数都在这里。
 *  一份代码从 Linux 搬到 Windows，要替换的东西基本都在这一个头文件里。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/*  这三个和原文件完全一样：printf / malloc / free / memset。
 *  注意这一部分【跨平台不需要改】——C 标准库在哪儿都长一样，
 *  只有"线程"这种和操作系统强相关的东西才需要换。 */



/* ---------------------------------------------------------------------------
 * 第 2 部分：链表宏（与原文逐字相同，没有任何修改）
 *
 * 为什么能原样照搬？因为它们操作的是纯粹的指针，
 * 和操作系统没有半点关系。这就是"抽象"的好处：
 * 变化的部分被隔离在少数几个函数里。
 * ------------------------------------------------------------------------- */

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



/* ---------------------------------------------------------------------------
 * 第 3 部分：三个数据结构
 * ------------------------------------------------------------------------- */

struct nTask {
	void (*task_func)(struct nTask *task);
	/*  [★重点] 函数指针 = 回调。
	 *  线程池不写死"要干什么活"，只留一个格子，用户把函数地址塞进来，
	 *  到时候线程池照着地址调用过去。这叫【回调】（callback）。
	 *
	 *  读法：( 返回类型 ) ( *变量名 ) ( 参数列表 )
	 *        void       (*task_func)  (struct nTask *)
	 *  括号不能省：写成 void *task_func(struct nTask*) 就变成
	 *  "一个叫 task_func 的函数，返回 void*"，那是完全不同的东西。
	 */

	void *user_data;
	/*  万能指针，装用户的业务数据。
	 *  线程池只负责把这一坨指针搬来搬去，完全不管里面是什么。
	 *  代价：用的时候必须自己强转回原来的类型。 */

	struct nTask *prev;
	struct nTask *next;
	/*  侵入式链表：任务结构体自己就带链表节点。
	 *  省一次 malloc/free，代价是挂进链表后用户不能再随便动这两个字段。 */
};


struct nWorker {
	HANDLE threadid;
	/*  替代 pthread_t。Windows 里的线程用"句柄"（handle）来代表，
	 *  句柄是一个系统内部对象的引用，用完要 CloseHandle 归还，
	 *  和文件句柄是同一套机制。
	 *  如果想拿线程的"身份证号"（数字 ID），用 GetCurrentThreadId()，
	 *  那是另一回事——下面的演示程序正好要用到它。 */

	int terminate;
	/*  下班标志：0 = 继续干，非 0 = 该收工了。
	 *  本文件里读写它都在锁的保护下，所以不会出现数据竞争。 */

	struct nManager *manager;
	/*  反指回经理。工人要靠它找到 tasks 链表。 */

	struct nWorker *prev;
	struct nWorker *next;
};


typedef struct nManager {
	struct nTask *tasks;
	struct nWorker *workers;

	CRITICAL_SECTION mutex;
	/*  替代 pthread_mutex_t。
	 *  "Critical Section"直译是"临界区"，在 Windows 里它就是互斥锁。
	 *  【使用规矩和 pthread 一样】成对：Enter... / Leave...，
	 *  而且每一条出口路径都不能漏。
	 *
	 *  ⚠️ 一个和 pthread 不同的地方：CRITICAL_SECTION 创建时需要
	 *     InitializeCriticalSection 分配系统资源，用完要
	 *     DeleteCriticalSection 归还。pthread 的锁则两种都行。
	 *
	 *  【它的特点】同一线程重复 Enter 是允许的（可重入），
	 *  但必须 Leave 同样次数。这也是它名字的由来——
	 *  它本来是给"进临界区就锁、出临界区就放"这种简单用法设计的。
	 */

	CONDITION_VARIABLE cond;
	/*  替代 pthread_cond_t。
	 *  这个是 Windows Vista 之后才有的，好在 Win10 完全支持。
	 *  它必须和一把 CRITICAL_SECTION 配对使用，
	 *  等待的函数叫 SleepConditionVariableCS
	 *  （CS = Critical Section，明明白白写在名字里）。
	 */
} ThreadPool;



/* ---------------------------------------------------------------------------
 * 第 4 部分：工人的工作循环
 *
 * 【和原代码的差异】
 *   1. 签名换成 Windows 的格式：DWORD WINAPI f(LPVOID)
 *   2. 结尾不自己 free(worker)，改由销毁函数统一回收
 *      （原因见文末说明）
 *   3. 补上了 return 0，消除"未定义返回值"的警告
 * ------------------------------------------------------------------------- */

static DWORD WINAPI nThreadPoolCallback(LPVOID arg) {
/*  这段签名要逐个词看，因为它是 Windows 强行规定的格式：
 *    static   —— 只在本文件内可见（原代码也有）
 *    DWORD    —— 返回类型，就是 unsigned long，Windows 里到处用它
 *    WINAPI   —— 调用约定（calling convention）。
 *                规矩是：参数的入栈顺序、由谁负责清理栈。
 *                Windows 的 API 默认用 __stdcall，而普通 C 函数用 __cdecl。
 *                不写 WINAPI，线程函数地址传进 CreateThread 时
 *                栈会被清两次 → 程序立刻崩，而且报错信息完全看不出
 *                是这个原因。这是从 Linux 搬代码到 Windows 最容易踩的坑。
 *    LPVOID arg —— 就是 void*（LP = Long Pointer，Windows 的老式命名习惯）
 *  连起来读：一个返回 DWORD、用 WINAPI 约定、参数是 void* 的函数。
 */

	struct nWorker *worker = (struct nWorker*)arg;
	/*  把万能指针还原成工人指针。
	 *  类型必须和 CreateThread 时传进去的那个一致。 */

	for (;;) {
	/*  死循环。工人一生就在这四步里转：拿锁 → 有活干/没活睡 → 放锁 → 干活。
	 *  （for(;;) 和 while(1) 完全等价，选哪个看个人习惯。）
	 */

		EnterCriticalSection(&worker->manager->mutex);
		/*  进临界区 = 上锁。
		 *  & 是取地址，因为函数要拿到这个对象本身，不是它的副本。
		 *  worker->manager->mutex 连着走两步指针：
		 *  先找到"我的经理"，再找到"经理的那把锁"。
		 *
		 *  拿不到锁时这条线程会【阻塞】——不占 CPU，被系统挂起，
		 *  等别人 LeaveCriticalSection 时才被放进来。
		 */

		while (worker->manager->tasks == NULL) {
		/*  [★重点★] 必须用 while，绝不能改成 if。
		 *
		 *  这是并发编程的头号送分题，面试也爱问。三个理由：
		 *
		 *  ① 醒来 ≠ 有活干。
		 *     20 个工人被同时叫醒，只来了 1 个任务，
		 *     抢到的那个把任务摘走了，另外 19 个醒来一看链表还是空的。
		 *     所以醒来后必须重新检查，不成立就接着睡。
		 *
		 *  ② 虚假唤醒（spurious wakeup）。
		 *     操作系统完全有权"无缘无故"把等待中的线程唤醒，
		 *     这是标准明确允许的行为，不是 bug。
		 *     写 if 的代码遇到虚假唤醒就会往下走到"取任务"，
		 *     结果取到 NULL，一解引用就崩。
		 *
		 *  ③ 此刻手里拿着锁，读到的 tasks 值才可信。
		 */

			if (worker->terminate) break;
			/*  睡前先看一眼要不要下班。
			 *  这个 break 只跳出【内层】while，
			 *  所以外面还要再判一次 terminate 才能真正离开。
			 */

			SleepConditionVariableCS(&worker->manager->cond,
			                         &worker->manager->mutex,
			                         INFINITE);
			/*  [★重点★] 睡觉，等价于 pthread_cond_wait。三个参数：
			 *    &cond  —— 睡在哪个条件变量上
			 *    &mutex —— 睡着时要放开哪把锁（也是醒来要重新抢的锁）
			 *    INFINITE —— 超时时间：一直等，没有时限
			 *                （它的兄弟是 SleepConditionVariableCS 的
			 *                  定时版本，可以设毫秒数，超时就返回，
			 *                  用于"等不到也要干点别的"的场景）
			 *
			 *  这个函数同样是【原子地】做三件事：
			 *    ① 放开 mutex  ② 挂起自己  ③ 被唤醒后重新抢回 mutex
			 *
			 *  ①的原子性至关重要，否则会出现这种致命时序：
			 *    工人A：检查链表 → 空
			 *    （此时主线程插入任务并发出唤醒通知——没人听，白白丢掉）
			 *    工人A：才刚睡下
			 *    → 任务永远躺着没人做，程序看起来像死机。
			 *  操作系统保证①和②之间不插入任何代码，彻底堵死这个洞。
			 */

			/*  被唤醒后从这里继续，此时手里【握着锁】，
			 *  于是回到 while 顶部重新检查 tasks 是否为空。 */
		}

		if (worker->terminate) {
		/*  跳出内层 while 有两种可能，必须区分：
		 *    a) 上面那个 break —— 要下班
		 *    b) 条件不成立了（有任务）—— 要干活
		 *  因此这里必须再判一次。
		 *  这也是"break 只跳最内层"带来的必然结果。
		 */

			LeaveCriticalSection(&worker->manager->mutex);
			/*  下班前把锁还回去。
			 *  【铁律】上锁/解锁必须成对，每个出口路径都不能漏。
			 *  漏一处，别的线程永远卡在 Enter 上不报错、不崩溃，
			 *  只是"程序莫名其妙不动了"，这是最难查的一类 bug。
			 */

			break;
			/*  跳出外层 for(;;)，工人正式收工，线程函数返回，线程结束。 */
		}

		struct nTask *task = worker->manager->tasks;
		/*  有活干了。记下链表头那个任务（此时持锁，读到的值准确）。 */

		LIST_REMOVE(task, worker->manager->tasks);
		/*  从链表上摘掉它。
		 *  【必须在锁里摘】否则两个工人可能同时看到同一个任务、
		 *  都以为自己拿到了、于是同一件活被干两遍。
		 *  "先摘下来再执行"，让任务在摘下的瞬间就归属于某一个工人，
		 *  这从设计上避免了重复执行。 */

		LeaveCriticalSection(&worker->manager->mutex);
		/*  [★重点] 注意解锁的位置——它在【执行任务之前】。
		 *
		 *  为什么不干完活再放？
		 *  那样 20 个工人就退化成一字长蛇阵，一次只有一个能干活，
		 *  多线程彻底失去意义，还容易死锁。
		 *
		 *  【设计原则】锁只保护"读改共享数据"那一小段，越短越好。
		 *  用户任务是耗时操作，绝对不能握着锁去跑。
		 *  现在锁已释放，别的工人可以立刻去抢下一个任务，实现真正并行。
		 */

		task->task_func(task);
		/*  通过函数指针调用用户的任务函数。
		 *  读法：task->task_func 取出那个函数地址，
		 *        后面跟 (task) 就是给它传个参数。
		 *
		 *  【为什么要把 task 自己传进去】
		 *  因为用户函数只有这一个参数，它需要从里面拿 user_data
		 *  （自己的业务数据）。所以任务函数是"自带上下文"的。
		 *
		 *  这一行在【无锁】状态下执行，所以：
		 *    好处：多个工人真正并行；
		 *    代价：用户函数里要是碰了线程池的共享数据，必须自己再加锁。
		 *          这是线程池用错的最主要原因，务必记牢。
		 */
	}

	/*  【和原代码不同的一处】原代码在这里写 free(worker)，也就是
	 *  让线程自己把自己释放掉。这份版本故意【去掉了】它，原因有两个：
	 *    1) 主线程销毁时要读 worker->threadid 才能 join，
	 *       如果线程已经把自己 free 了，主线程读到的是野指针。
	 *    2) pool->workers 链表里还挂着这个节点，别人走过路过会踩到空。
	 *  正确做法就是：谁创建谁释放，统一由销毁函数回收。 */

	return 0;
	/*  返回 0 表示线程正常结束。
	 *  原代码在这里没写 return，是个未被定义的行为；
	 *  补上这一句，编译器的那条 C4716 警告就消失了。 */
}



/* ---------------------------------------------------------------------------
 * 第 5 部分：三个 API
 * ------------------------------------------------------------------------- */

int nThreadPoolCreate(ThreadPool *pool, int numWorkers) {

	if (pool == NULL) return -1;
	/*  防御性检查：调用者传了空指针，后面 memset 会直接崩。
	 *  库代码不能假设调用者是对的，要让函数"优雅返回错误码"而不是崩溃。 */

	if (numWorkers < 1) numWorkers = 1;
	/*  容错：0 或负数当成 1。
	 *  试想真的是 0：一个工人都没有，任务丢进去永远没人处理，
	 *  唤醒通知也没人听，整个程序表现出来的就是"卡死了"。
	 *  所以这里必须兜底。 */

	memset(pool, 0, sizeof(ThreadPool));
	/*  [★重点] 全部填 0。
	 *  pool 是 main 里在栈上定义的局部变量，那块内存里是上一个函数留下的垃圾。
	 *  不清零的话，tasks / workers 就是随机地址，
	 *  后面 while (tasks == NULL) 可能永远不成立，或者 LIST_INSERT 直接写飞。
	 *  清零后两条链表的头指针自动是 NULL，正好表示"空链表"。 */

	InitializeCriticalSection(&pool->mutex);
	/*  初始化锁。这一步【不能省】——CRITICAL_SECTION 内部有个结构，
	 *  必须先让系统把它建好（Windows 会分配一个内核事件对象），
	 *  否则 EnterCriticalSection 会直接崩。
	 *  这和原代码里用 memcpy 拷一个 PTHREAD_MUTEX_INITIALIZER 的做法不同，
	 *  Windows 没有"静态初始值常量"这种东西，只能显式调用初始化函数。 */

	InitializeConditionVariable(&pool->cond);
	/*  初始化条件变量。同样不能省。 */

	int i = 0;
	for (i = 0; i < numWorkers; i ++) {
	/*  循环 numWorkers 次，每次造一个工人、启动一条线程。 */

		struct nWorker *worker = (struct nWorker*)malloc(sizeof(struct nWorker));
		/*  给工人申请内存。
		 *  【为什么必须 malloc 不能放栈上】栈上变量在函数返回后就失效了，
		 *  而线程要在函数返回后继续跑很久，需要一份"活得更久"的内存。
		 *  堆内存只有显式 free 才回收，正合适。 */

		if (worker == NULL) {
			perror("malloc");
			/*  perror 会先打印 "malloc"，再自动补上系统给的失败原因。 */
			return -2;
			/*  ⚠️ 这里没做回滚：前面已经成功的工人和线程还挂着没清理。
		 *     严谨的库会跳到一段统一清理的代码，或者干脆要求
		 *     调用者先销毁再重建。教学代码先简化。 */
		}

		memset(worker, 0, sizeof(struct nWorker));
		/*  清零：让 terminate 是 0、prev/next 是 NULL，
		 *  避免带着垃圾值进入链表。 */

		worker->manager = pool;
		/*  [★重点] 建立"我 → 我的经理"这条反指连线。
		 *  因为传给线程的参数只有 worker 自己，
		 *  而工人干活要找到 tasks 链表，tasks 在经理身上。
		 *  没有这条线，工人在回调里就"失忆"了。 */

		LIST_INSERT(worker, pool->workers);
		/*  【和原代码不同的一处】原代码是先 CreateThread 再入册，
		 *  中间有个极小的时间窗：线程已经在跑了，但名册上还没记它。
		 *  本版本把顺序调过来了——先入册，再启动。
		 *  这样一旦 CreateThread 失败，我们也能在名册里找到它并摘掉。
		 *
		 *  【为什么调序是安全的】
		 *  新线程要干的活全靠 worker 这块内存里的内容，
		 *  而这块内存此刻已经填好了（manager 已赋值、prev/next 已被宏设好），
		 *  提前入册不会让它读到任何未初始化的东西。
		 */

		worker->threadid = CreateThread(
			NULL,                       /* 安全属性，NULL = 默认 */
			0,                          /* 初始栈大小，0 = 用默认的 1MB */
			nThreadPoolCallback,        /* 新线程要执行的函数（写函数名，不加括号！）*/
			worker,                      /* 传给新线程的参数，用 void* 接 */
			0,                          /* 创建标志，0 = 立即开始运行 */
			NULL                        /* 输出线程ID，不需要就传 NULL */
		);
		/*  [★重点] 创建线程。一句话让操作系统多跑一条执行流。
		 *
		 *  和 pthread_create 的三个关键区别：
		 *    1) 返回值是 HANDLE（一个句柄），失败返回 NULL；
		 *       pthread 是返回错误码，成功返回 0。
		 *    2) 线程函数必须在签名里写 WINAPI；
		 *    3) 用完要 CloseHandle，否则句柄会一直占着系统资源。
		 *
		 *  【关键性质】它是"异步"的——立刻返回，新线程什么时候开始跑
		 *  完全不受你控制。所以 worker 的字段必须在调用之前全部填好。
		 *  本代码顺序是对的（先 memset、先赋 manager、先入册，再 CreateThread）。
		 */

		if (worker->threadid == NULL) {
		/*  失败时 CreateThread 返回 NULL。
		 *  注意判断方式：pthread 那边是 if (ret)（非 0 即失败），
		 *  Windows 这边是 if (handle == NULL)。两套约定不一样，别混。 */

			LIST_REMOVE(worker, pool->workers);
			/*  上面入册了，现在启动失败，把它从名册里摘掉。 */

			free(worker);
			return -3;
		}
	}

	return 0;
	/*  全部创建成功。 */

}



int nThreadPoolDestory(ThreadPool *pool) {

	struct nWorker *worker = NULL;

	EnterCriticalSection(&pool->mutex);
	/*  要改 worker->terminate 了，先拿锁。
	 *  为什么这里要用锁？因为工人也在读这个字段（在它自己的 while 里）。
	 *  C 语言里普通 int 的跨线程读写不是原子的，
	 *  如果一边在读一边在写，严格来说是未定义行为。
	 *  用锁把读写都框起来，就是最简单可靠的解决办法。 */

	for (worker = pool->workers; worker != NULL; worker = worker->next) {
		worker->terminate;   /* [故意复现原代码的 bug] 少了 = 1，等于什么都没做 */
		/		 *  [✅ 修正点 1] 原代码这里写的是 worker->terminate; ——
		 *  少了个 = 1，等于什么都没干，工人永远不会下班。
		 *  这类错误叫"无效语句"：语法合法，但没有任何效果。
		 *
		 *  【实测】本机 MSVC 2019 用 /W3 和 /W4 编译那一行，
		 *  连一个警告都不给。也就是说：不报错、不崩溃、不提示，
		 *  功能静静失效。这是最难查的一类 bug。
		 *  唯一可靠的发现方式就是——真的跑一遍，并验证结果。
		 *  配套的 threadpool_bug_demo.c 就是为这件事准备的：
		 *  你把那一行还原成原样再跑，会发现 1000 个任务照样能做完
		 *  （因为问题不在干活，而在收工），
		 *  但程序会永远卡在下面的 WaitForSingleObject 上，再也不结束。
		 *  亲眼看一次，比读十遍注释都有用。
		 *
		 *  ⚠️ 遍历时改动链表是危险的（提前取好的 next 可能失效），
		 *     但这里只改节点内的 terminate，没动 prev/next，所以安全。
		 *     养成好习惯的话，还是该先存 next 再前进。 */
	}

	WakeAllConditionVariable(&pool->cond);
	/*  [★重点] 广播：叫醒【所有】睡着的工人。
	 *
	 *  对比两个函数，别用错：
	 *    WakeConditionVariable     —— 只叫醒一个（用于"来活了，一个人去接"）
	 *    WakeAllConditionVariable  —— 叫醒全部（用于"条件整体变了，大家都醒醒"）
	 *
	 *  这里必须用"全部"。因为 terminate 是"全局下岗通知"，
	 *  如果只叫醒一个，剩下 19 个会一直睡着，线程池永远关不掉。
	 *
	 *  ⚠️ 和 pthread 的一个差异要记住：
	 *     Windows 的条件变量唤醒是【不排队】的。
	 *     pthread_cond_broadcast 会保证唤醒当时所有等待者，
	 *     WakeAllConditionVariable 也类似，但如果有线程
	 *     "刚检查完条件、还没进入等待"就会被漏掉——
	 *     所以这里持锁发通知是必须的（我们确实持着锁）。
	 *
	 *  💡 注：本处的 pool->mutex 是在调用 WakeAll 时仍然持有的，
	 *     这符合"持锁改状态 + 持锁发通知"的安全写法。
	 */

	LeaveCriticalSection(&pool->mutex);
	/*  状态改完、通知发完，放锁。
	 *  工人被唤醒后会自己再去抢锁，重新检查条件，然后退出循环。 */

	for (worker = pool->workers; worker != NULL; worker = worker->next) {
		WaitForSingleObject(worker->threadid, INFINITE);
		/*  [✅ 修正点 2] 等一下，等这条线程真正结束。等价于 pthread_join。
		 *
		 *  【为什么必须等】因为"通知下班"只是发了个请求，
		 *  工人可能正干到任务的一半。如果这时主线程就去 free 掉数据、
		 *  甚至程序直接退出，工人手里的指针全成了野指针 →
		 *  随机崩溃，而且崩的位置离真正的原因十万八千里，极难排查。
		 *  等待，是唯一能保证"它真的不会再碰这些内存了"的办法。
		 *
		 *  两个参数：
		 *    worker->threadid  —— 等谁
		 *    INFINITE          —— 一直等，不设超时
		 *
		 *  【原代码的问题】它压根没有这一步，
		 *  只是把两个链表头指针置 NULL 就返回了，
		 *  线程还在后台跑着，内存却已经被"宣布作废"。
		 */

		CloseHandle(worker->threadid);
		/*  线程已经结束，句柄没用了，还给系统。
		 *  每个 CreateThread 都要配一个 CloseHandle，
		 *  否则句柄泄漏——虽然不会立刻出问题，
		 *  但长跑的服务里句柄数量有上限，迟早会耗尽。
		 */
	}

	worker = pool->workers;
	while (worker != NULL) {
		struct nWorker *next = worker->next;
		/*  先把下一个存下来，再释放当前这个。
		 *  否则 free(worker) 之后再读 worker->next 就是访问已释放内存。
		 *  这个小技巧在链表释放里是必备的。 */

		free(worker);
		worker = next;
	}
	/*  [✅ 修正点 3] 把工人内存真正释放掉。原代码没做，属于内存泄漏。 */

	{
		struct nTask *task = pool->tasks;
		while (task != NULL) {
			struct nTask *next = task->next;
			/*  跑完之前就被丢弃的任务，在这里统一清理。
			 *  释放顺序：先把 next 存好，再释放任务里挂的用户数据，
			 *  最后释放任务本身。 */
			free(task->user_data);
			free(task);
			task = next;
		}
	}
	/*  说明：本实现是"直接丢弃剩余任务"。
	 *  如果想做成"优雅关闭"（先把队列里的活干完再退出），
	 *  就不能在循环开头判 terminate 就 break，
	 *  而要改成"只有链表空了、且 terminate 为 1"才退出。
	 *  两种语义各有用途，真实框架里通常会提供开关。 */

	pool->workers = NULL;
	pool->tasks = NULL;
	/*  两个链表头置空，让这个 pool 回到"干净的空状态"。 */

	DeleteCriticalSection(&pool->mutex);
	/*  [✅ 修正点 4] 归还锁占用的系统资源。
	 *  Windows 的 CRITICAL_SECTION 会关联一个内核对象，
	 *  不 Delete 就泄漏了。这一步必须在"确定再也没人会用它"之后做。 */

	return 0;
}



int nThreadPoolPushTask(ThreadPool *pool, struct nTask *task) {

	if (pool == NULL || task == NULL) return -1;
	/*  [✅ 修正点 5] 原代码没做参数校验。
	 *  如果 task 是 NULL，LIST_INSERT 展开第一步就是 item->prev = NULL，
	 *  对空指针写入 → 进程当场崩溃，而且崩在宏里面，
	 *  调用栈看起来莫名其妙。提前拦住，问题定位就轻松得多。 */

	EnterCriticalSection(&pool->mutex);
	/*  拿锁。要改共享数据 tasks 链表，改之前必须锁门。 */

	LIST_INSERT(task, pool->tasks);
	/*  把任务插到链表【头部】。
	 *  因为插在头、工人也从头部取，所以这其实是个【栈】：后进先出。
	 *  结果就是提交顺序 0,1,2,...999，执行顺序大致是 999,998,...0。
	 *  想改成先进先出（队列），需要额外维护一个尾指针，插入时插到尾部。
	 *
	 *  另外提醒：同一个 task 不要重复提交两次，
	 *  否则它的 prev/next 会被串成环，整个链表就毁了。
	 *  严谨的实现会加一个"已在队列中"的标志位。 */

	WakeConditionVariable(&pool->cond);
	/*  [★重点] 叫醒【一个】睡着的工人：快来，有活了！
	 *
	 *  为什么这里用"一个"而不是"全部"？
	 *  因为只多了一个任务，理论上一个人就够。
	 *  把所有人叫醒是浪费——19 个人醒来抢不到活还得睡回去，
	 *  这叫【惊群效应】（thundering herd），白白增加系统开销。
	 *
	 *  ⚠️ 这一行是在【持锁状态】下执行的，这不是错误，反而是最安全的写法：
	 *     "改链表"和"发通知"合成了一个不可分割的动作，
	 *     不会有工人卡在"检查到链表为空"和"真正睡下去"之间漏掉通知。
	 *     代价是被唤醒的工人醒来后要先等一下锁，但这个开销很小。
	 *     另一种写法是"持锁改数据 → 放锁 → 再发通知"，性能略好，
	 *     但时序推理复杂得多，新手不建议。 */

	LeaveCriticalSection(&pool->mutex);

	return 0;
	/*  [✅ 修正点 6] 原代码声明返回 int 却没有 return，
	 *  调用方拿到的是垃圾值。这里补上。 */
}



/* ============================================================================
 *  第 6 部分：演示程序
 *
 *  目标：让线程池"看得见"。
 *    - 开局打印每个工人的编号和系统线程 ID
 *    - 每个任务打印自己被哪个工人处理了
 *    - 跑完统计：1000 个任务是不是全干完了？20 个工人各干了多少？
 *  这样才能真正验证"线程池确实在工作，而且确实在并行"。
 * ============================================================================ */

#define THREADPOOL_INIT_COUNT	20
/*  工人数量：20 条线程。
 *  经验值：CPU 密集型任务，线程数≈CPU 核心数最划算；
 *          I/O 密集型（等网络、等磁盘）可以设到几十甚至几百。 */

#define TASK_INIT_SIZE			1000
/*  提交 1000 个任务。20 个工人对付 1000 件活，
 *  平均每人 50 件——这就是线程池的意义：建 20 次线程干完 1000 件事。
 *  如果不用池，就要创建/销毁 1000 次线程。 */

#define MAX_TRACKED_THREADS		64
/*  统计表最多能记多少条线程。20 个工人够用了。 */


/*  ---- 下面是演示用的全局变量（不属于线程池本身，只是为了统计和打印） ---- */

struct ThreadStat {
	DWORD tid;      /* 系统给的线程 ID */
	LONG  count;    /* 这条线程干了多少件活 */
	int   label;    /* 给它起的短名字，1 号、2 号……方便打印 */
};

static struct ThreadStat g_stats[MAX_TRACKED_THREADS];
static int g_stats_count = 0;
static CRITICAL_SECTION g_stats_cs;
/*  保护上面这张统计表的锁。
 *  [★重点] 这个东西原代码里没有，因为原代码根本没做统计。
 *  但这也正好演示了一个重要事实：
 *  线程池只保护它自己的 tasks/workers 链表；
 *  用户自己加的共享数据，得用户自己加锁。 */

static volatile LONG g_done = 0;
/*  已完成任务数。volatile 告诉编译器"这个变量会被别人从外面改，
 *  每次用都去内存里重新读，别给我优化到寄存器里"。
 *  配合 InterlockedIncrement 使用（下面是 Windows 提供的原子加法）。 */

static CRITICAL_SECTION g_print_cs;
/*  保护 printf 的锁。
 *  printf 内部虽然有自己的保护机制，但"打印一整行"并不原子，
 *  多个线程同时打印会看到两行粘在一起。
 *  加一把锁，保证一行完整地打印完再轮到别人。 */


static int record_and_label(DWORD tid) {
/*  记一次"这个线程干了活"，并返回它的短编号（1 起）。
 *  第一次见到某个线程 ID 时分配一个新编号。
 *  返回 -1 表示表满了（正常情况不会发生）。
 */
	int i;
	int result = -1;

	EnterCriticalSection(&g_stats_cs);

	for (i = 0; i < g_stats_count; i++) {
		if (g_stats[i].tid == tid) {
			g_stats[i].count++;
			result = g_stats[i].label;
			break;
		}
	}

	if (result == -1 && g_stats_count < MAX_TRACKED_THREADS) {
		g_stats[g_stats_count].tid   = tid;
		g_stats[g_stats_count].count = 1;
		g_stats[g_stats_count].label = g_stats_count + 1;
		result = g_stats[g_stats_count].label;
		g_stats_count++;
	}

	LeaveCriticalSection(&g_stats_cs);
	return result;
}


void task_entry(struct nTask *task) {
/*  用户自己的任务函数。就是被塞进 task->task_func 的那个。
 *  [★重点] 签名必须和 struct nTask 里声明的函数指针类型【完全一致】：
 *     返回 void，参数是 struct nTask *
 *  差一点点（比如参数少个 *）编译器都会在赋值那行报类型不兼容。 */

	int idx = *(int *)task->user_data;
	/*  [★重点] 全文件最绕的一行，从右往左分三步读：
	 *
	 *  ① task->user_data
	 *     取出那个万能指针（静态类型是 void*），它实际指向一个 int。
	 *
	 *  ② (int *)
	 *     强制类型转换：把 void* 变成 int*。
	 *     为什么必须转？因为对 void* 不能直接"取值"——
	 *     编译器不知道要取几个字节。转成 int* 它才知道"取 4 字节当整数看"。
	 *
	 *  ③ 最左边的 *
	 *     解引用（取值）：去这个地址把里面的东西读出来，得到一个 int。
	 *
	 *  这里必须保证当初存进去的确实是 int，
	 *  否则编译器不报错，程序读出垃圾数字甚至崩溃。
	 *  这是 void* 的代价：灵活性换走了类型安全。
	 */

	DWORD tid = GetCurrentThreadId();
	/*  注意！这里拿到的是"当前正在执行这行代码的线程"的 ID。
	 *  它必然就是某一条工人线程——这就是我们怎么知道
	 *  "这件活是被哪个工人干的"。 */

	int label = record_and_label(tid);

	EnterCriticalSection(&g_print_cs);
	printf("task %4d  ->  worker W%02d  (tid %5lu)\n", idx, label, (unsigned long)tid);
	LeaveCriticalSection(&g_print_cs);
	/*  打印一行。用锁把整行包起来，避免多线程输出粘在一起。
	 *
	 *  printf 里可以顺便学三个格式符：
	 *    %4d   整数，宽度 4，不足左边补空格（让数字对齐好看）
	 *    %02d  整数，宽度 2，不足左边补 0
	 *    %5lu  unsigned long，宽度 5
	 *
	 *  【观察点】你会看到输出里的 idx 是【从大到小】趋势的
	 *  （999, 998, ...），因为任务插在链表头部、工人也从头部取，
	 *  相当于一个栈。而 W 的编号是乱跳的，说明确实是 20 条线程在抢着干。 */

	InterlockedIncrement(&g_done);
	/*  已完成任务数 +1。
	 *  [★重点] 为什么必须用 InterlockedIncrement，不能直接写 g_done++ ？
	 *
	 *  因为 g_done++ 在 CPU 上其实是三条指令：
	 *      读 g_done 到寄存器 → 寄存器加 1 → 写回 g_done
	 *  如果两个线程同时执行，可能出现：
	 *      A 读到 100，B 也读到 100
	 *      A 算出 101 写回，B 也算出 101 写回
	 *      → 明明加了两次，结果只多了 1
	 *  这叫【竞态条件】，最后统计出来的数字会莫名偏小，而且时对时错。
	 *
	 *  InterlockedIncrement 是 CPU 提供的一条特殊指令，保证
	 *  "读-改-写"整个过程不会被任何其他线程插队，也就是【原子操作】。
	 *  它比加锁快得多，专门用于这种单纯的"计数器加一"。
	 *
	 *  （Linux/GCC 上对应的写法是 __sync_fetch_and_add(&g_done, 1)
	 *    或者 C11 标准的 atomic_fetch_add。） */

	free(task->user_data);
	free(task);
	/*  [★重点] 谁申请谁释放。
	 *  这两块内存是 main 里 malloc 的，所以也要成对 free 掉。
	 *
	 *  【为什么在这里 free 而不是在 main 里】
	 *  任务一旦提交进线程池，就跑去别的线程执行了，
	 *  主线程根本不知道它什么时候跑完，也就没法在正确的时间 free。
	 *  唯一知道"这件活用完了"的时刻，就是任务函数执行到结尾。
	 *  所以释放的责任天然落在任务函数自己头上。
	 *
	 *  【这是最易错的地方】用户经常忘了在这里 free，
	 *  结果每处理一个任务就泄漏一份内存，跑久了内存被吃光。
	 *  线程池帮不了你——它不知道 user_data 里到底是什么，
	 *  也不知道是不是还有别人在共用。只能靠约定 + 写清楚文档。
	 */
}


int main(void) {

	ThreadPool pool = {0};
	/*  在栈上定义线程池对象，用 0 初始化所有成员。
	 *  （typedef 起过别名之后，写 ThreadPool 就不用再加 struct 前缀了。） */

	InitializeCriticalSection(&g_stats_cs);
	InitializeCriticalSection(&g_print_cs);
	/*  初始化演示用的两把锁。 */

	DWORD start_tick = GetTickCount64() & 0xFFFFFFFF;
	/*  记一下开始时间，用来算总耗时。
	 *  GetTickCount64 返回从系统启动至今的毫秒数（64 位）。
	 *  （其实该用 ULONGLONG 接 64 位返回值，这里为了简单做了截断，
	 *    反正只测很短的一瞬间，不会溢出。） */

	printf("=== [BUG 复现版] 故意还原成原代码写法：worker->terminate; ===\n");
	printf("工人线程数: %d\n", THREADPOOL_INIT_COUNT);
	printf("任务总数  : %d\n\n", TASK_INIT_SIZE);

	int ret = nThreadPoolCreate(&pool, THREADPOOL_INIT_COUNT);
	/*  [✅ 修正点 7] 原代码没接返回值。
	 *  如果创建失败（比如内存不够），后面照样往下跑，
	 *  然后在 push 任务时踩空指针崩溃，你会完全不知道问题出在哪。
	 *  接住返回值并检查，是库代码最基本的使用纪律。 */

	if (ret != 0) {
		printf("创建线程池失败，错误码 %d\n", ret);
		return 1;
	}

	int i = 0;
	for (i = 0; i < TASK_INIT_SIZE; i ++) {

		struct nTask *task = (struct nTask *)malloc(sizeof(struct nTask));
		/*  给第 i 个任务申请内存。 */

		if (task == NULL) {
			perror("malloc");
			exit(1);
			/*  exit 会立刻结束整个进程。
			 *  在多线程程序里这是个"暴力"操作：直接杀光所有线程，
			 *  不给它们清理现场的机会。生产环境更温和的做法是
			 *  设个标志位让大家自己收工。这里只是演示。 */
		}
		memset(task, 0, sizeof(struct nTask));
		/*  清零。让 prev/next 处于确定状态。
		 *  虽然接下来 LIST_INSERT 会把它们覆盖掉，
		 *  但让新对象总是从"干净状态"出发是好习惯。 */

		task->task_func = task_entry;
		/*  把 task_entry 这个函数的地址存进去。
		 *  注意后面【没有括号】——写括号就变成调用它了。
		 *  不写括号时，C 会自动把"函数名"转换成"函数指针"，正好赋给 task_func。
		 *
		 *  这一行的作用：告诉线程池"这件活该怎么干"。
		 *  线程池完全不知道 task_entry 里写了什么，
		 *  它只会在合适的时候照着这个地址调用过去。 */

		task->user_data = malloc(sizeof(int));
		/*  单独申请 4 字节（一个 int 的大小）来装任务编号。
		 *
		 *  【为什么不能写成 task->user_data = &i;】
		 *  因为 i 是 main 的局部变量，1000 个任务会共用同一个 i！
		 *  等工人真正执行时，i 早就变成 1000 了，
		 *  每个任务读到的都是同一个值——你会看到输出里
		 *  所有任务的 idx 全部相同。这就是要用指针共享数据最典型的坑。
		 *  所以必须为每个任务【单独申请】一块属于自己的内存。 */

		*(int*)task->user_data = i;
		/*  往那块内存里写值。
		 *  三步：转成 int* → 解引用 → 赋值。
		 *  这一行和 task_entry 里的读取是配对的：
		 *  存的时候按 int 存，取的时候也按 int 取，类型对上才安全。 */

		nThreadPoolPushTask(&pool, task);
		/*  把任务丢进线程池。
		 *  这个函数会拿锁、把任务挂到链表、叫醒一个工人，然后立刻返回。
		 *  main 不等这件活干完，继续提交下一个——这就是【异步】。
		 *
		 *  所以这 1000 次循环跑得飞快（只是在挂链表），
		 *  真正的 1000 次执行是 20 个工人在背后并行完成的。
		 *  你马上就会看到：主线程早就跑完循环了，屏幕上任务还在刷刷地往外打。
		 */
	}

	printf("\n--- 1000 个任务已全部提交完毕，主线程现在开始等待 ---\n\n");

	while (g_done < TASK_INIT_SIZE) {
		Sleep(1);
	}
	/*  [★重点] 等所有任务真的做完。
	 *  原代码这里写的是 getchar()，靠"用户敲回车"来拖时间，
	 *  属于教学代码的权宜之计：如果用户敲得太快，任务还没跑完
	 *  main 就返回了，进程直接退出，你会误以为线程池没工作。
	 *  这里换成"轮询计数器"，是真正准确的等待方式。
	 *
	 *  Sleep(1) 是让主线程歇 1 毫秒再查一次。
	 *  如果不 Sleep 就是"忙等待"，会白白占满一个 CPU 核心。
	 *
	 *  ⚠️ 生产环境更优雅的做法是"用事件对象或条件变量来等"，
	 *     而不是轮询。轮询简单直观，适合教学。 */

	DWORD end_tick = GetTickCount64() & 0xFFFFFFFF;

	printf("\n=== 全部完成 ===\n");
	printf("已完成任务数: %ld / %d\n", (long)g_done, TASK_INIT_SIZE);
	printf("参与干活的线程数: %d\n", g_stats_count);
	printf("总耗时: %lu ms\n\n", (unsigned long)(end_tick - start_tick));

	printf("各工人处理的任务数（验证负载是否分散到多条线程）:\n");
	{
		int k;
		long sum = 0;
		for (k = 0; k < g_stats_count; k++) {
			printf("  W%02d (tid %5lu) : %4ld 件\n",
			       g_stats[k].label,
			       (unsigned long)g_stats[k].tid,
			       (long)g_stats[k].count);
			sum += g_stats[k].count;
		}
		printf("  ------------------------------\n");
		printf("  合计: %ld 件\n\n", sum);
	}
	/*  [这里是整个演示的高潮]
	 *  这张表同时验证了三件事：
	 *    1) "已完成任务数"正好是 1000 → 没有任务丢失，也没有任务被重复执行
	 *    2) "参与干活的线程数"正好是 20 → 确实用了 20 条线程，
	 *       而不是每来一个任务就新建一条（那样这里会显示 1000 左右）
	 *    3) 每行都有数字、没有哪一行是 0 → 20 条线程都真的在工作，
	 *       负载比较均匀地分散开了 */

	nThreadPoolDestory(&pool);
	/*  [✅ 修正点 8] 原代码从没调用过销毁函数！
	 *  也就是说那些销毁逻辑里的 bug 从来没被执行到。
	 *  这里补上，并且加了上面的统计来验证"优雅退出"确实生效了——
	 *  如果 terminate 那句还是少写 = 1，
	 *  程序会永远卡在销毁函数里的 WaitForSingleObject 那一步，
	 *  你会亲眼看到"程序不结束了"，也就亲身体会了那个 bug 的严重性。 */

	DeleteCriticalSection(&g_stats_cs);
	DeleteCriticalSection(&g_print_cs);

	printf("线程池已安全销毁，程序正常退出。\n");

	return 0;
}
