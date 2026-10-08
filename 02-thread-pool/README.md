# 02 线程池

用 C 语言实现一个固定大小的线程池：程序启动时预先创建 N 个工作线程，
它们阻塞等待任务；主线程把任务插入队列并唤醒其中一个去执行。

## 为什么需要线程池

每来一个任务就 `pthread_create` 一个新线程的做法有两个明显代价：

1. **创建开销**：线程创建涉及内核态对象分配和栈空间申请，是相对昂贵的系统调用。
2. **数量失控**：任务量突增时会创建大量线程，CPU 在线程间频繁切换，
   大量时间耗在上下文切换而不是实际计算上。

线程池的思路是**复用**：一次性创建固定数量的线程，让它们长期存活，
反复从任务队列里取任务执行。线程数固定，创建开销只付一次。

## 核心数据结构

程序里有三个结构体，构成两条链表。

| 结构体 | 作用 | 所在链表 |
|---|---|---|
| `struct nTask` | 一个待执行的任务：函数指针 + 用户数据 | 挂在 `nManager.tasks` 上 |
| `struct nWorker` | 一个工作线程：线程 ID + 终止标志 | 挂在 `nManager.workers` 上 |
| `nManager`（别名 `ThreadPool`） | 管理器，持有两条链表头和同步原语 | 管理者本身 |

`nManager` 里的两个同步原语：

- `pthread_mutex_t mutex` —— **互斥锁**，保证同一时刻只有一个线程在操作任务链表
- `pthread_cond_t cond` —— **条件变量**，让空闲线程在"队列为空"时挂起，
  不占用 CPU，直到有新任务时被唤醒

两条链表由两个宏操作，`LIST_INSERT` 采用**头插法**（新元素插在表头），
`LIST_REMOVE` 处理被删元素位于表头、表中、表尾以及链表只剩一个元素的各
种情形。用宏而不是函数，是因为宏不区分类型，同一套逻辑可以同时服务于
`nTask` 和 `nWorker` 两种节点。

## 工作流程

**初始化**（`nThreadPoolCreate`）：
清空管理器 → 循环创建 `numWorkers` 个工作线程 → 每个线程挂到 `workers` 链表。

**提交任务**（`nThreadPoolPushTask`，生产者）：
加锁 → 把任务头插进 `tasks` 链表 → `pthread_cond_signal` 唤醒一个正在等待的线程 → 解锁。

**执行任务**（`nThreadPoolCallback`，消费者）：
循环做四件事 —— 加锁 → 若队列为空则 `pthread_cond_wait` 挂起 → 取出表头任务并摘链 →
解锁后执行。注意**执行阶段不持锁**，否则整个线程池会退化成串行。

**销毁**（`nThreadPoolDestory`）：
置终止标志 → `pthread_cond_broadcast` 唤醒所有线程 → 清理链表指针。

## 文件说明

| 文件 | 内容 |
|---|---|
| `threadpool.c` | 原始实现，可编译运行的完整版本 |
| `threadpool_annotated.c` | 逐行注释版 |
| `threadpool_bug_demo.c` | 故障演示版，用于观察并发缺陷 |
| `threadpool_win32.c` | Windows 平台移植版 |
| `线程池.png` | 结构笔记图 |

## 编译与运行

```bash
gcc threadpool.c -o bin/threadpool -lpthread
./bin/threadpool
```

程序会创建 20 个工作线程，提交 1000 个任务，然后等待回车退出。

## 已知问题

原始实现里有几处缺陷，留作后续修正：

1. **`nThreadPoolDestory` 里 `worker->terminate;` 是无效语句** ——
   它只读取了成员变量，没有赋值，本意应该是 `worker->terminate = 1;`。
   因此工作线程的终止标志从未被设置，销毁流程实际不生效。

2. **`nThreadPoolPushTask` 声明返回 `int`，但函数体没有 `return` 语句** ——
   调用方读到的返回值是未定义的。

3. **用 `memcpy` 复制 `PTHREAD_COND_INITIALIZER` / `PTHREAD_MUTEX_INITIALIZER`** ——
   POSIX 只保证这两个宏可用于静态初始化。运行期初始化应当调用
   `pthread_cond_init()` 和 `pthread_mutex_init()`（源码注释里原本就是这两个调用）。

4. **销毁流程没有 `pthread_join`，也没有释放 `nWorker` 结构体的完整逻辑** ——
   存在资源泄漏。`nThreadPoolCallback` 里的 `free(worker)` 只在线程自身退出时执行，
   而销毁路径并未等待线程结束。

## 待办

- [ ] 修正上述 4 个缺陷
- [ ] 补充任务队列为空/非空两种状态下的时序验证
- [ ] 对比固定线程数与动态扩缩的吞吐差异
