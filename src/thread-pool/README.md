# thread-pool —— 固定大小线程池

预先创建 N 个工作线程，它们从一条共享的任务队列里取任务执行。
对外是一个静态库：调用方提交任务函数和参数，其余的生命周期由线程池管理。

线程池解决的是线程创建的固定成本问题。`pthread_create` 要做线程栈、内核调度实体、
TLS 块等一整套资源分配，实测在这台机器上"创建 + join"一个什么都不做的线程约 17 µs；
而复用已有线程完成同样一次调度大约 0.12 µs。两者差两个数量级，
所以任务量大而单个任务小时，复用是唯一可行的做法。

## 目录结构

```
thread-pool/
├── include/
│   └── threadpool.h          公开接口：不透明句柄 + 10 个函数 + 6 个状态码
├── src/
│   ├── threadpool.c          实现：FIFO 队列、工作线程主循环、停机流程
│   └── main.c                命令行驱动，只链接 libthreadpool.a
├── tests/
│   ├── test_threadpool.c     54 项单元测试
│   ├── check_cli.sh          19 项命令行端到端检查
│   └── bench_threadpool.c    三组基准
├── notes/
│   ├── threadpool_annotated.c   模块化之前的逐行注释版（历史材料，不参与构建）
│   ├── threadpool_bug_demo.c    故意保留缺陷的演示版（历史材料，不参与构建）
│   └── threadpool_win32.c       同一结构的 Win32 移植版（历史材料，不参与构建）
├── Makefile                  all / lib / cli / test / check / bench / sanitize / tsan / clean
└── README.md
```

`notes/` 里的三个文件是模块化之前的学习材料，与现在的实现结构不同（例如它们让任务
函数自己 `free` 掉任务节点，现在的实现把节点所有权收回了池内部）。留着是为了对照，
**不参与任何构建目标**，读代码时不要拿它们当当前接口的说明。

## 对外接口

`include/threadpool.h`。所有对象通过不透明句柄 `threadpool *` 访问。
内部要放 `pthread_mutex_t` 与两个 `pthread_cond_t`，公开它们会强迫每个调用方
`include <pthread.h>`，并把 pthread 同步原语的内部布局固定成 ABI 的一部分。

| 函数 | 作用 |
|---|---|
| `threadpool_create(nthreads, &pool)` | 创建池，返回前 N 个工作线程已就绪 |
| `threadpool_destroy(pool)` | 优雅停机：排空队列 → join 线程 → 释放 |
| `threadpool_submit(pool, fn, arg)` | 提交任务，成功即保证被执行一次 |
| `threadpool_wait(pool)` | 屏障：等到队列空且无人执行 |
| `threadpool_queued(pool)` | 队列中待执行的任务数 |
| `threadpool_running(pool)` | 正在执行的任务数 |
| `threadpool_completed(pool)` | 累计执行完成数 |
| `threadpool_submitted(pool)` | 累计提交成功数 |
| `threadpool_thread_count(pool)` | 工作线程数 |
| `threadpool_status_string(s)` | 状态码的英文短名 |

状态码：`TPOOL_OK`、`TPOOL_ERR_PARAM`、`TPOOL_ERR_NOMEM`、`TPOOL_ERR_SYSTEM`、
`TPOOL_ERR_CLOSED`、`TPOOL_ERR_BLOCKING`。

### 所有权

这条约定决定了调用方要不要写 `free`，因此单独列出：

- **任务节点归池所有。** `submit` 内部用 `malloc` 分配节点，任务函数返回后由池释放。
  调用方拿不到这个节点，也不需要关心它。
- **任务参数 `arg` 归调用方所有。** 池把它原样转交给任务函数，不解引用、不释放，
  `destroy` 时也不会去回收未执行任务的 `arg`。需要回收就由调用方在任务函数里做，
  或者等 `wait`/`destroy` 返回后统一做。
- **池对象归调用方所有**，由 `threadpool_create` 分配、`threadpool_destroy` 释放。

### 线程模型

- `submit` / `wait` 可以从任意线程调用，包括工作线程内部（任务里再提交任务是允许的）。
- `wait` **不能**由工作线程调用：它等的是"正在执行的任务数归零"，而调用者本身
  就是一个正在执行的任务。实现里用 `pthread_equal(pthread_self(), tids[i])` 检测
  并返回 `TPOOL_ERR_BLOCKING`，不进入等待。
- `destroy` 必须由提交方线程调用。它返回之后池对象失效，任何后续调用都是未定义行为。

## 设计要点

### 1. 两个条件变量，而不是一个

工作线程等待的条件是"队列非空或该退出"，`wait` / `destroy` 等待的条件是
"队列空且无人执行"——两者正好相反。共用一个条件变量时，每次状态变化都只能
`pthread_cond_broadcast`，否则可能唤醒错的那一类等待者，而该唤醒的继续睡着
（丢失唤醒）。broadcast 是正确的，代价是惊群：提交一个任务，N 个空闲线程全部被
唤醒抢同一个任务，其中 N-1 个重新睡回去，白付一次上下文切换。

分成 `work` 与 `idle` 之后，`submit` 只 `signal` 一个工作线程，
任务完成只在"队列空且 running 归零"时 broadcast 给 `idle`。
基准里 1 线程到 8 线程空任务吞吐从 121 ns 掉到 636 ns，说明这条路径上仍然
有共享状态竞争（单个互斥锁保护的单条队列），但至少不是每次提交都惊群。

### 2. 队列是 FIFO，不是 LIFO

单链表可以头插也可以尾插，取的时候从表头取。头插头取就是栈：最后提交的任务最先执行。
在"提交速度持续高于消费速度"的负载下，先提交的任务会被后来的不断压到队尾，
可能长时间得不到执行——这是**饥饿**，不是"顺序不同"那么轻的问题。

本实现维护 `head` 与 `tail` 两个指针，尾插入队、表头出队，得到严格的 FIFO。
代价只是多维护一个指针，收益是"提交顺序 = 执行顺序"这个可以直接写成测试的保证：

```
--- 单工作线程下的 FIFO 顺序 ---
ok    执行了 N 个
ok    执行顺序 == 提交顺序（FIFO）
```

多工作线程时只能保证**取出顺序**是 FIFO，完成顺序由调度决定——同一条队列上，
先取到任务的线程不一定先跑完。所以测试只在 1 个线程的池上断言执行顺序。

### 3. 等待条件必须写成循环

```c
while (p->head == NULL && !p->stop) {
    pthread_cond_wait(&p->work, &p->lock);
}
```

`pthread_cond_wait` 返回并不等于条件成立，有两种情况：

- **虚假唤醒**（spurious wakeup），POSIX 明确允许 `pthread_cond_wait` 在没有
  `pthread_cond_signal`/`broadcast` 的情况下返回。标准这样规定是为了允许实现
  用"唤醒后重新检查"的简单结构，因此调用方必须自己重新检查条件。
- 被 broadcast 唤醒，但队列里那个任务已经被别的线程取走了。

写成 `if` 的话，这两条路径都会让线程去取一个不存在的任务（`head == NULL` 时解引用）。

### 4. 执行阶段不持锁

工作任务从队列取走之后立刻 `pthread_mutex_unlock`，再去调用任务函数。
如果持锁执行，所有工作线程会退化成串行：一个线程干活，其余线程在锁上排队，
线程池变成"一个线程 + N-1 个等待者"，吞吐比单线程还低（多出锁竞争成本）。

代价是任务函数不能假设池的互斥锁保护着任何外部状态——它什么锁都没持有。

### 5. 优雅停机：排空队列，而不是丢弃

`threadpool_destroy` 的顺序是：

```
置 closed（拒绝新任务） → 等 queued == 0 && running == 0 → 置 stop → join → 释放
```

等待期间工作线程照常取任务（`pthread_cond_wait` 会释放锁），因此不会因为
destroy 持锁而饿死它们。

为什么不能直接丢弃队列：那样调用方既拿不到"哪些任务没执行"的信息，也没有返回值，
任务参数还留着自己手上——一个静默的、不可观测的工作丢失。库的默认行为不该是这个。
验证方式是把 200 个 1 ms 的任务提交进 1 个线程的池，然后**不等**就 destroy：
200 个任务必须全部执行完，destroy 耗时约 211 ms。

### 6. create 失败要回滚，不能留半截状态

`pthread_create` 可能因资源限制失败。此时已经创建的工作线程正在
`pthread_cond_wait` 上等任务。直接 `return` 的后果是：线程泄漏，且池对象被释放后
这些线程持有悬空指针。正确顺序与 destroy 相同——置 `stop`、broadcast、join，
最后才析构同步原语；反过来的话线程可能正在一个已销毁的条件变量上等待，
按 POSIX 是未定义行为。

### 7. `pthread_t` 只能用 `pthread_equal` 比较

`wait` 里判断"调用者是不是本池的工作线程"时，不能用 `==`：
POSIX 只规定 `pthread_t` 是"用于标识线程的不透明类型"，
没有规定它是算术类型或指针，可比较性由实现决定。Linux 的 glibc 里它是
`unsigned long`，`==` 恰好能编过；换一个实现就可能编译不过或者比错。

## 构建

```bash
cd src/thread-pool
make            # libthreadpool.a + threadpool 命令行工具
make test       # 54 项单元测试
make check      # 19 项命令行端到端检查
make bench      # 三组基准
make sanitize   # ASan + UBSan，产物在 bin/sanitize/
make tsan       # TSan，产物在 bin/tsan/
make clean
```

命令行工具：

```bash
./bin/threadpool run   -t 4 -n 1000 -w 0.05   # 4 个线程跑 1000 个任务，每个忙等 0.05 ms
./bin/threadpool spawn -n 1000 -w 0.05        # 对照组：每任务新建一个线程
./bin/threadpool help
```

`run` 与 `spawn` 用同一个任务函数、同一份工作量，可以直接对比墙钟时间。

变体构建（`sanitize` / `tsan`）落在独立目录。原因与其它模块相同：make 判断
"要不要重建"只比较时间戳、不比较编译选项，共用 `bin/` 会留下插桩过的二进制。

## 测试与验证

环境：Ubuntu 22.04 / gcc 11.4.0 / AMD Ryzen 7 5700X（8 核 16 线程）。

| 项目 | 命令 | 结果 |
|---|---|---|
| 默认构建 | `make` | 0 条警告（`-std=c11 -Wall -Wextra -Wconversion -O2 -g`） |
| 单元测试 | `make test` | 54 项，0 失败 |
| 命令行检查 | `make check` | 19 项，0 失败 |
| ASan + UBSan + LSan | `make sanitize` | 0 条警告，54 项全过，无报告 |
| ThreadSanitizer | `make tsan` | 0 条警告，54 项全过，无 data race 报告 |

单元测试覆盖的六类行为：

| 类别 | 断言内容 |
|---|---|
| 参数与生命周期 | `create(0)` / `create(-1)` / `out == NULL` 被拒绝且不留野指针；连续 10 轮 create/destroy 无线程与内存泄漏 |
| 任务语义 | 1000 个任务各执行一次；参数按 `index*2` 正确送达；计数一致（submitted = completed = 1000） |
| 队列语义 | 单线程池 32 个任务的执行顺序等于提交顺序 |
| 并发语义 | 4 个生产者并发提交 1000 个任务，无一丢失；任务内部再提交成功且父子都执行 |
| 同步语义 | `wait` 幂等（第二次 < 50 ms 返回）；`wait` 之后池可继续使用；工作线程调用 `wait` 返回 `TPOOL_ERR_BLOCKING` 且主线程不死锁 |
| 停机语义 | 200 个 1 ms 任务提交后立即 destroy，200 个全部执行完；10 万个空任务的压力测试计数无丢失 |

第一次跑 TSan 时报告了一处 data race，位置在**测试代码**：

```
WARNING: ThreadSanitizer: data race
  Write of size 4 by thread T26: #0 task_mark tests/test_threadpool.c:100
  Previous write by thread T9:   #0 task_mark tests/test_threadpool.c:100
```

原因是压力测试的第二轮让 100 个任务共用同一个 `slot`，多个工作线程并发写
同一块内存。池本身没有问题，是测试把"每个任务独占参数"这条约定用错了。
改成每个任务一个 `slot` 之后 TSan 干净。这件事记在这里，是因为它说明了
一个容易忽略的点：**并发测试自己必须先没有数据竞争，否则 sanitizer 的报告
会把注意力引向无辜的被测代码。**

## 性能

数值为 5 轮取最小值，两轮独立测量给出区间。任务函数本身为空时测的是调度成本。

**空任务（提交 → 调度 → 执行 → 回收），20 万个任务：**

| 线程数 | ns/任务 | 任务/秒 |
|---|---|---|
| 1 | 121 ~ 131 | 7.6 ~ 8.2 M |
| 2 | 146 ~ 154 | 6.5 ~ 6.8 M |
| 4 | 290 ~ 305 | 3.3 ~ 3.5 M |
| 8 | 637 ~ 654 | 1.5 ~ 1.6 M |

**对照：每任务新建线程再 join，2 万个任务，约 16700 ~ 17130 ns/任务。**

也就是说单线程池的调度成本约为"新建+销毁一个线程"的 **1/140**。
这个比值是线程池存在的全部理由；任务越短，比值的影响越大。

**固定工作量：每任务忙等 100 µs，共 1000 个任务：**

| 线程数 | 总耗时 | 加速比 | 效率 |
|---|---|---|---|
| 1 | 100.1 ms | 1.00 | 100% |
| 2 | 50.1 ms | 2.00 | 100% |
| 4 | 25.0 ms | 4.00 | 100% |
| 8 | 12.5 ms | 7.98 | 100% |

两个结论：

1. **空任务吞吐随线程数单调下降。** 1 线程 121 ns，8 线程 654 ns，慢了 5 倍多。
   原因不是池"变重了"，而是所有工作线程争同一把互斥锁和同一条队列的
   `head`/`tail` 指针，这些字段在同一条缓存行上，每次出队都要把该缓存行
   搬到当前核心（cache line ping-pong）。空任务时这点成本就是全部成本，
   于是负加速。**单队列 + 单锁的线程池只适合任务粒度明显大于调度开销的场景。**
2. **任务粒度足够大时接近线性。** 100 µs 的任务在 8 核上得到 7.98 倍，
   说明队列竞争在 100 µs 的尺度上可以忽略。选择线程池时真正要判断的是
   任务的平均耗时与调度成本（约 0.1 ~ 0.7 µs）的比值。

## 实现记录

### 模块化之前修掉的 4 处缺陷

原实现（见 `notes/threadpool_annotated.c`）有四处错误，都在这次模块化之前修正：

1. 销毁流程里写的是 `worker->terminate;` —— 读一下就丢掉的**无副作用表达式语句**，
   终止标志从未被置 1，被唤醒的线程重新检查标志后又回去等待，销毁完全不生效，
   进程永久挂住。编译器对这条语句不给任何警告。
2. `nThreadPoolPushTask` 声明返回 `int` 却没有 `return`。C11 6.9.1p12：
   在要求返回值的函数中执行到 `}` 而不返回值，若调用方使用了这个值，行为未定义。
3. 用 `memcpy` 复制 `PTHREAD_COND_INITIALIZER`。POSIX 只保证这两个宏可用于
   **静态初始化**，其内部表示由实现决定，逐字节复制不保证得到一个有效对象。
4. 销毁流程未 join 未 free，而回调函数结尾又自己 `free(worker)` —— 修一处必然触发
   另一处的双重释放。最终确定 worker 结构体归池所有，回调不再自释放。

### 这次模块化改了什么

接口层面（这是主要收益，不只是搬文件）：

| 旧接口 | 新接口 | 改的原因 |
|---|---|---|
| `int nThreadPoolCreate(ThreadPool *pool, int n)` | `tpool_status threadpool_create(int n, threadpool **out)` | 旧接口要求调用方在栈上放一个 `ThreadPool`（含 `pthread_mutex_t`），池内部再 `memset` 清零它。若该对象已经被初始化过，`memset` 会直接抹掉同步原语的内部状态。改成池自己分配，调用方不可能传进一个半初始化的对象 |
| 调用方 `malloc` 一个 `struct nTask` 并填函数指针 | `threadpool_submit(pool, fn, arg)` | 旧设计把内部数据结构暴露给调用方，还必须让任务函数自己 `free(task)` —— 任务的返回值语义变成"你必须记得释放我"，漏一个就是泄漏 |
| 无 | `threadpool_wait(pool)` | 旧实现只能靠 `getchar()` 等，程序里没有任何"任务做完了"的可编程信号 |
| `nThreadPoolDestory` 丢弃队列中未执行的任务 | `destroy` 排空队列 | 见设计要点 5 |
| 无 | `queued` / `running` / `completed` / `submitted` | 没有计数器时，测试无法断言"每个任务恰好执行一次"，只能看打印输出 |
| 每条链表都用两个宏 | 队列直接写在结构体里 | 宏在只有一个使用者时只是增加阅读成本；`LIST_REMOVE` 需要 `item` 与 `list` 同时是左值，用起来容易写错 |

`main.c` 里有一个顺带记下的标准细节：`pthread_create` 的入口类型是
`void *(*)(void *)`，而任务函数是 `void (*)(void *)`。把前者强转成后者
（或反向）是未定义行为——标准没有规定两种函数指针类型可以互相转换后调用，
返回值的传递约定可能不同。对照组因此加了一层真正的适配函数 `spawn_entry`，
而不是强转。

### 与仓库其它模块的一致性

- 目录、Makefile 目标集、README 节次与 `concurrent-lock` / `contact-list` / `word-count` 相同。
- 驱动只链接 `libthreadpool.a`，因此"这个模块能被独立链接"每次构建都被验证一次。
- 已被根 `Makefile` 自动发现（`make list` 里能看到 `thread-pool`），无需改动根 Makefile。

## 已知局限

- **单一全局队列 + 单一互斥锁。** 空任务吞吐随线程数下降（见性能第 1 条）。
  真正的解决办法是分片队列（每个工作线程一条）配合 work stealing，
  本轮没有实现。
- **无界队列。** 提交速度持续高于消费速度时，队列与内存线性增长，没有背压。
  `threadpool_submit` 永不阻塞，这对调用方简单，但没有保护内存的机制。
- **`destroy` 没有强制版本。** 它一定等队列排空；若某个任务永久阻塞，
  `destroy` 就永久阻塞。没有超时参数，也没有"取消队列中剩余任务"的接口。
- **`destroy` 不能从任务函数里调用**，否则它在等 `running` 归零，而自己就是其中之一。
  这一点没有像 `wait` 那样做运行时检测（`destroy` 返回 `void`，无法上报错误）。
- **`wait` 不是栅栏。** 它只保证"调用之前已提交的任务"都完成；返回之后
  别的线程再提交的任务不受约束。
- **没有优先级、延时任务、任务取消。** 队列只有一种语义。
- **每个池独占自己的线程。** 一个进程里开两个 8 线程的池就占 16 个线程，
  没有全局线程数上限或线程共享。

## 待办

- [ ] 分片队列 + work stealing，与当前单队列做吞吐对比
- [ ] 有界队列与提交阻塞（背压），以及队列满时的返回码
- [ ] `destroy` 的超时与强制中止版本
- [ ] 让 `concurrent-lock` 的基准用本模块提交任务，而不是自己 `pthread_create`：
      两个模块协作后，"锁的实现差异"与"任务调度开销"可以在同一套代码路径上比较
