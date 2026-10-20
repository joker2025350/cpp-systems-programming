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
| `docs/images/thread-pool.png` | 结构笔记图（在仓库根的 `docs/` 下） |

## 编译与运行

```bash
mkdir -p bin
gcc -pthread threadpool.c -o bin/threadpool
./bin/threadpool
```

程序会创建 20 个工作线程，提交 1000 个任务，然后等待回车退出。

## 缺陷修复记录

首次实现里有 4 处缺陷，现已全部修正。

### 1. 销毁流程中的无效语句

```c
for (worker = pool->workers;worker != NULL;worker = worker->next) {
    worker->terminate;      // 只读取成员变量，什么也没改变
}
```

`worker->terminate;` 是一条**没有副作用的表达式语句**：它把成员变量的值读出来就丢弃了。
因此终止标志从未被置为 1，被唤醒的线程重新检查标志、发现它不是 1，又回到等待状态，
销毁流程实际上完全不生效。

修正：`worker->terminate = 1;`

### 2. 有返回类型的函数缺少 return

`nThreadPoolPushTask` 声明返回 `int`，函数体却没有 `return` 语句。C 语言规定：
在要求返回值的函数中执行到末尾而未 return，若调用方使用了这个返回值，行为是未定义的。

修正：补 `return 0;`

### 3. 用 memcpy 复制初始化宏

```c
pthread_cond_t blank_cond = PTHREAD_COND_INITIALIZER;
memcpy(&pool->cond, &blank_cond, sizeof(pthread_cond_t));
```

POSIX 只保证 `PTHREAD_MUTEX_INITIALIZER` 与 `PTHREAD_COND_INITIALIZER`
可用于**静态初始化**。这两个宏展开成什么完全由实现决定，标准既不保证其内部表示
可以被逐字节复制，也不保证复制出来的对象有效。运行期初始化必须调用对应的 init 函数。

修正：改用 `pthread_mutex_init()` 与 `pthread_cond_init()`，并检查返回值。

### 4. 销毁流程未回收线程（牵连出双重释放风险）

原代码只把 `pool->workers = NULL;` 一赋了事，既没等待工作线程结束，
也没释放 `nWorker` 结构体，属于资源泄漏。

但这里有个陷阱：原 `nThreadPoolCallback` 结尾有 `free(worker);`，即线程退出时释放自己。
若销毁流程也去释放，同一个指针会被释放两次，构成**双重释放（double free）**——
这是未定义行为，通常导致堆结构损坏或程序崩溃。

因此修复前必须先确定 **worker 结构体的所有权归谁**。本项目选择归线程池管理器：

- 回调函数不再自释放，改为 `return NULL;`
- 销毁流程按固定顺序执行：
  置终止标志 → `pthread_cond_broadcast` 唤醒 → 逐个 `pthread_join` →
  `free(worker)` → 释放残留任务 → 销毁同步原语

顺序不可调换：`join` 必须在 `free` 之前，否则线程可能仍在使用自己的结构体。
同理，`terminate` 的写入必须在持锁状态下进行，否则与工作线程的读取构成数据竞争。

### 附带修正

- `nThreadPoolCallback` 声明返回 `void *` 但缺 return，补 `return NULL;`
- `nThreadPoolPushTask` / `nThreadPoolDestory` 增加空指针检查
- `nThreadPoolDestory` 的 `nWorker` 参数未使用，加 `(void)nWorker;` 抑制警告，
  保留参数以兼容原接口
- `main` 原本没有调用 `nThreadPoolDestory`，补上，让销毁路径真正被执行

## 验证结果

在 Ubuntu 22.04 / gcc 11.4.0 上完成以下验证：

| 检查项 | 方法 | 结果 |
|---|---|---|
| 编译 | `gcc -Wall -Wextra -pthread` | 零警告 |
| 运行 | 20 线程、1000 任务 | 1000 条任务输出，退出码 0 |
| 内存安全 | `gcc -fsanitize=address` | 无任何报告（无越界、无双重释放、无泄漏） |
| 数据竞争 | `gcc -fsanitize=thread` | 无任何报告 |

ThreadSanitizer 在默认配置下可能因系统 ASLR 报 `unexpected memory mapping`，
这是工具与内核内存布局的兼容问题，用 `setarch $(uname -m) -R` 关闭 ASLR 后即可正常运行。

## 待办

- [x] 修正上述 4 个缺陷
- [x] 通过消毒剂检查内存安全与数据竞争
- [ ] 补充任务队列为空/非空两种状态下的时序验证
- [ ] 对比固定线程数与动态扩缩的吞吐差异
