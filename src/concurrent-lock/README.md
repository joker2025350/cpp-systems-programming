# 并发计数（counter 模块）

一个可复用的并发计数器模块，加一个建立在它之上的基准与演示驱动。

模块把"对同一个整数做自增"这件事封装成同一个接口，底下挂五种实现：裸自增、
互斥锁、自旋锁、编译器原子内建、x86 内联汇编。调用方换一个枚举值就能换实现，
因此可以在完全相同的负载下比较它们的正确性与吞吐——这正是"同一份代码
在 `#if 0` 分支之间手动切换"做不到的事。

```bash
$ ./bin/counter -k all -t 10 -n 1000000 -r 3
threads=10  per-thread=1000000  rounds=3  sleep=off

kind           expected          final  match       avg_ms     Mops/s
--------------------------------------------------------------------------
raw            10000000        1359560     NO        54.06     184.98
mutex          10000000       10000000    yes       405.79      24.64
spin           10000000       10000000    yes      1011.34       9.89
atomic         10000000       10000000    yes       143.32      69.77
asm            10000000       10000000    yes       161.67      61.85
```

`raw` 那一行丢了 86% 的更新，是本文档里最直接的证据：
`count++` 在多线程下不是原子操作，而且按 C11 的定义，这种写法根本就是错的。

---

## 1 目录结构

```
src/concurrent-lock/
├── include/
│   └── counter.h            对外接口：种类枚举、状态码、不透明句柄
├── src/
│   ├── counter.c            五种实现
│   └── main.c               基准与演示驱动
├── tests/
│   └── test_counter.c       68 项检查（TSan 构建下跳过 raw 用例）
├── Makefile
└── README.md
```

配图在仓库根的 `docs/images/concurrent-lock.png`。

编译产物落在 `bin/`，已被根 `.gitignore` 忽略。

---

## 2 对外接口

### 2.1 种类与状态码

```c
typedef enum {
    COUNTER_KIND_RAW    = 0,   /* 无同步，仅用于演示数据竞争 */
    COUNTER_KIND_MUTEX  = 1,
    COUNTER_KIND_SPIN   = 2,
    COUNTER_KIND_ATOMIC = 3,   /* __atomic_* builtin */
    COUNTER_KIND_ASM    = 4    /* x86 内联汇编 */
} counter_kind;

typedef enum {
    COUNTER_OK              = 0,
    COUNTER_ERR_PARAM       = 1,
    COUNTER_ERR_NOMEM       = 2,
    COUNTER_ERR_UNSUPPORTED = 3    /* 当前平台不支持这种实现 */
} counter_status;
```

`COUNTER_ERR_UNSUPPORTED` 单独列出来，是因为 `asm` 实现依赖 x86 指令，
在 ARM 上不成立。把"不支持"和"内存不足"混成一个错误码，调用方就没法
在 ARM 上写好降级逻辑——它会以为内存出了问题而去重试。

### 2.2 不透明句柄

```c
typedef struct counter counter;        /* 定义在 counter.c 里 */
```

这是本仓库里唯一一个用不透明句柄的模块，04 和 05 都是公开结构体、可以在栈上直接分配。
差异来自内部成员：这里要放 `pthread_mutex_t` 和 `pthread_spinlock_t`。

公开它们意味着 `<counter.h>` 必须 `#include <pthread.h>`，把 pthread 的类型、
宏、特性开关泄漏给每一个调用方；更要紧的是结构体布局被固定下来，
换个平台只要 pthread 内部布局不同就 ABI 不兼容。用不透明句柄，头文件只依赖
`<stddef.h>`，内部结构随便改，调用方不必重新编译。

代价只有一个：对象只能在堆上分配。

### 2.3 函数

| 函数 | 说明 |
|---|---|
| `counter_create(kind, initial, &out)` | 创建；失败时 `*out` 置 NULL |
| `counter_destroy(c)` | 销毁，释放内部同步原语 |
| `counter_add(c, delta)` | 加 delta，返回加之后的值 |
| `counter_get(c)` | 读当前值 |
| `counter_kind_name(kind)` | `"raw"` / `"mutex"` / `"spin"` / `"atomic"` / `"asm"` |
| `counter_kind_supported(kind)` | 当前平台是否可用 |
| `counter_kind_is_atomic(kind)` | 是否保证结果正确（只有 raw 为 0） |
| `counter_status_string(status)` | 状态码转可读描述，未知值返回 `"unknown status"` |

`counter_kind_supported` 存在的意义是让调用方**在创建之前**就知道能不能用，
而不是把 `create` 的失败当成内存不足。

### 2.4 `counter_get` 的 const 问题

接口声明为 `long long counter_get(const counter *c)`，但实现内部要对
mutex / spin 加锁——而 `pthread_mutex_lock` 的参数是 `pthread_mutex_t *`，
不是 const。

这里去掉了 const 重定向：

```c
pthread_mutex_t *lock = (pthread_mutex_t *)&c->u.mtx.lock;
```

这是 C 语言里"逻辑上只读、物理上需要同步"的常见处理。const 的承诺比字面看起来弱一点，
但比把接口改成非 const 要好：调用方拿到的语义仍然是"这个调用不会改变计数器的值"。

---

## 3 设计要点

### 3.1 用枚举分派，不用函数指针表

五种实现共用一个 `struct counter`，`kind` 决定走哪个分支：

```c
long long counter_add(counter *c, long long delta) {
    switch (c->kind) {
    case COUNTER_KIND_MUTEX:  /* lock / += / unlock */
    case COUNTER_KIND_SPIN:   /* spin_lock / += / spin_unlock */
    case COUNTER_KIND_ATOMIC: return __atomic_add_fetch(&c->u.raw, delta, __ATOMIC_SEQ_CST);
    case COUNTER_KIND_ASM:    return asm_add(&c->u.raw, delta);
    case COUNTER_KIND_RAW:    /* 故意的数据竞争 */
    }
}
```

分派只有这一处，函数指针表带来的间接跳转反而让五个分支更难读。
等实现多到十几个、或者需要运行时注册自定义策略时再换表。

### 3.2 `union` 而不是三个并列成员

```c
struct counter {
    counter_kind kind;
    union {
        long long raw;
        struct { pthread_mutex_t lock;      long long value; } mtx;
        struct { pthread_spinlock_t lock;   long long value; } spin;
    } u;
};
```

用 union 有两个好处：一个 `counter` 只占"最大那个实现"的空间；
并且"raw 与 mutex 同时被初始化"这种逻辑上不可能的状态无法被表示出来。

代价是三种实现的值不在同一个偏移上（`raw` 偏移 8，`mtx.value` 偏移 48），
所以访问必须分情况写，没法用一个 `value_ptr()` 统一。这是有意的取舍。

### 3.3 数据竞争是未定义行为，不是"结果不确定"

这是本文档里最需要说清楚的一条。

C11 5.1.2.4 规定：两个线程访问同一个对象、至少一个是写、且两者之间没有
同步关系，就构成 **data race**，而 data race 的行为是**未定义**（undefined behavior）。

"未定义"不是"结果可能偏小"，而是"程序本身是错的"。编译器有权假设
data race 不存在，从而做出任意优化：把 `count` 缓存在寄存器里不写回、
把循环折叠成一次加法、甚至直接删掉这段代码。实际观察到"计数偏小"
只是众多可能结果里最温和的一种。

`COUNTER_KIND_RAW` 保留下来只为一件事：给基准程序一个反面样本。
它不应该出现在任何真实代码里，包括"临时调试"。

### 3.4 原子操作的三种写法，以及为什么选 `__atomic` 做默认

| 写法 | 可移植性 | 说明 |
|---|---|---|
| `__atomic_add_fetch` | GCC / Clang，跨架构 | 编译器按目标架构生成合适的指令序列 |
| `lock; xaddq` 内联汇编 | 仅 x86 | 能精确控制生成的指令，但每换一个架构都要重写 |
| C11 `<stdatomic.h>` | 标准 C11 | 最"正规"，但需要 `atomic_long` 类型配合，且部分嵌入式工具链支持不全 |

模块同时提供 `atomic`（`__atomic` 内建）与 `asm`（x86 汇编）两种，
是为了让基准能直接对比"编译器生成的"和"手写的"到底差多少——实测差别在 5% 以内，
见第 6 节。结论是：**除非确有需要，不要手写内联汇编**。

### 3.5 内联汇编的两个坑

```c
__asm__ volatile("lock; xaddq %0, %1"
                 : "=r"(old), "+m"(*value)
                 : "0"(delta)
                 : "cc", "memory");
```

**坑一：内存操作数必须是 `"+m"`，不能是 `"m"`。**
`xadd` 会改写内存，声明成只读等于告诉编译器"执行完这块内存还是原值"。
原始版本写的是 `"m"(*value)`，之所以没出错，全靠同时写了 `"memory"` clobber——
编译器因此不敢在 asm 前后把内存值缓存进寄存器。一旦少了那个 clobber，
读到的就是陈旧值。**两个约束在互相打补丁，而其中一个是错的**，
这种代码能跑但不可维护。

**坑二：`delta` 与 `old` 必须占同一个寄存器。**
`xadd` 的源操作数就是那个"先装加数、后装旧值"的寄存器。这里用 `"0"(delta)`
把输入绑到第 0 个操作数上，直接把这件事说出来；原始版本写
`"=a"(old) ... "a"(delta)`，是让编译器自己去发现两个操作数需要同一个寄存器，
能否成立取决于分配结果。

内联汇编的约束写错**不会报错，只会算错**。这是它比 C 代码危险的根本原因，
也是"能不用就不用"的最强理由。

### 3.6 `lock` 前缀到底做了什么

`xadd` 本身不是原子的：它内部是"读内存、加、写内存"三步。多核上两个核心
可以同时读到同一个旧值，各自加完写回，其中一个的修改被覆盖。

`lock` 前缀让处理器在指令执行期间锁住对应的**缓存行**（不是锁总线，
早期 x86 才是总线锁），其他核心对同一缓存行的访问必须等这条指令完成。
于是"读-改-写"整体不可分割。

这也是自旋锁在高竞争下表现很差的原因：所有自旋的核心都在反复用
`lock cmpxchg` 或 `lock xadd` 去碰同一根缓存行，缓存行在核心之间来回弹
（cache line ping-pong），总线流量成为瓶颈。第 6 节的数据里 spin 比
mutex 慢 2.4 倍，就是这个机制的直接结果。

---

## 4 构建

```bash
make            # 构建静态库 bin/libcounter.a 与命令行工具 bin/counter
make lib        # 只构建静态库
make cli        # 只构建命令行工具
make test       # 单元测试
make bench      # 五种实现的吞吐对比
make demo       # 高竞争 vs 加 sleep 的对照
make sanitize   # ASan + UBSan，产物在 bin/sanitize/
make tsan       # ThreadSanitizer（跳过 raw），产物在 bin/tsan/
make clean      # 删除 bin/（含全部变体）
```

默认编译参数：

```make
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g -pthread
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   += -pthread
```

四点说明：

- **`-pthread` 放在 `CFLAGS` 而不是 `LDLIBS`**。它既是链接选项，也会定义
  `_REENTRANT` 之类的宏影响编译；只放链接期会漏掉编译期的部分。
- **`-D_POSIX_C_SOURCE=200809L` 是必须的**。`-std=c11` 会定义 `__STRICT_ANSI__`，
  关掉 glibc 的默认扩展，`pthread_spin_lock` 与 `nanosleep` 的声明就看不到了。
  不加这一句的症状是"函数隐式声明"——在 64 位平台上 `pthread_spin_lock`
  返回值被截断成 `int`，很可能编译通过、运行出错。
- **`tsan` 与 `sanitize` 必须分开**。ASan 与 TSan 的运行时插桩互斥，
  不能同时链接；TSan 还依赖固定的地址布局，需要 `setarch $(uname -m) -R`
  关闭 ASLR，否则可能一开始就报 `unexpected memory mapping`。
- **变体构建用独立的输出目录**。所有产物落在 `$(OUT)` 下，默认 `bin/`；
  `sanitize` 与 `tsan` 通过递归调用自身把 `OUT` 覆盖成 `bin/sanitize`、
  `bin/tsan`。原因是 **make 判断"要不要重建"只比较时间戳，不比较编译选项**：
  共用 `bin/` 时，先 `make tsan` 再 `make`，普通构建会认为 `bin/counter`
  已是最新而不重新链接，于是留下一个带 TSan 插桩的可执行文件，
  一运行就死于 `unexpected memory mapping`，而报错与"刚敲了 make"
  之间看不出任何因果关系。这类问题不会在单次构建里暴露，
  只会在"先跑检查、再跑基准"的顺序里冒出来。

**把模块当库用：**

```bash
cc -std=c11 -Wall -Wextra -O2 -pthread -Iinclude your_app.c bin/libcounter.a -o your_app
```

---

## 5 测试与验证

验证环境：

| 项目 | 值 |
|---|---|
| CPU | AMD Ryzen 7 5700X（8 核 16 线程） |
| 内核 | Linux 6.8.0-138 |
| 编译器 | gcc 11.4.0 |
| 编译参数 | `-std=c11 -Wall -Wextra -Wconversion -O2 -g -pthread` |

### 5.1 编译警告

| 编译参数 | 结果 |
|---|---|
| 默认 | **0 条** |
| `make sanitize` | 0 条 |
| `make tsan` | 0 条 |

`make tsan` 这一行到 2026-10-29 才真正成立。TSan 构建用 `-DCOUNTER_TEST_SKIP_RAW`
关掉了 raw 用例——它故意制造数据竞争，开着必然报错——但只关了 `main` 里的调用点，
没关函数的定义。于是 `test_raw_demonstrates_loss` 与它专用的 `report_value`
成了没有引用的 `static` 函数，GCC 报 `-Wunused-function`。

这个缺陷此前没有暴露的原因不在模块里：根 `Makefile` 一直没有 `tsan` 转发目标，
只有单个模块自己定义了它，所以全仓构建从不走这条路径。2026-10-29 补上根目标、
CI 第一次跑全仓 `make tsan` 时才发现。修法是把两个定义也纳入同一个条件编译块。

### 5.2 单元测试

`make test`，68 项检查：

```
counter 1.0.0 单元测试

ok    创建 raw / mutex / spin / atomic / asm
ok    创建后指针非空
ok    初值为 0
ok    counter_destroy(NULL) 不崩溃
ok    out 为 NULL
ok    非法 kind
ok    失败后 out 仍为 NULL
ok    raw 支持非零初值
...
ok    mutex 单线程累加 1000
ok    mutex add 返回新值
ok    mutex 支持负增量
...
ok    mutex 8 线程 × 5 万次不丢
ok    mutex 结束后的读数一致
ok    spin 8 线程 × 5 万次不丢
ok    atomic 8 线程 × 5 万次不丢
ok    asm 8 线程 × 5 万次不丢
info  raw 8 线程 × 20 万次（会丢更新）    实际 230893（期望 1600000，差 1369107）
ok    raw 结果在合理区间内
ok    raw 不是原子的
ok    mutex 是原子的
...
ok    未知状态码

68 项检查，0 项失败
```

三组用例值得单独说明：

- **单线程基线**：五种实现都必须给出正确结果。这一步排除"实现本身算错"
  的可能，之后多线程下的差异才能归因到同步策略上。
- **8 线程 × 5 万次精确相等**：四种同步实现必须一条不丢。这是模块的核心承诺，
  也是唯一能证明 `asm_add` 的约束写对了的测试。
- **raw 只报告、不断言**：不能要求 raw"一定丢更新"——它是未定义行为，
  理论上结果可以是任何东西。用例只把它打印出来，并检查结果落在 `[0, expected]`
  这个合理区间内（严格说这也不由标准保证）。

### 5.3 消毒剂

**ASan + UBSan（`make sanitize`）**：

```
68 项检查，0 项失败

--- 命令行（ASan + UBSan）---
atomic           400000         400000    yes         4.44      90.01
```

无报告。

**ThreadSanitizer（`make tsan`）**：67 项检查（跳过 raw），**无 data race 报告**。
这证明 mutex / spin / atomic / asm 四种实现确实没有数据竞争——
ASan 查不出这类问题，只有 TSan 能。

把 raw 的用例放开，TSan 立刻抓到：

```
WARNING: ThreadSanitizer: data race (pid=231094)
SUMMARY: ThreadSanitizer: data race src/counter.c:227 in counter_add
```

这正是 `COUNTER_TEST_SKIP_RAW` 存在的原因：`make tsan` 的目的是验证
"同步实现没有竞争"，让 raw 的必然报错把整个测试判失败没有意义；
但同时也保留"不定义这个宏就能亲眼看竞争报告"的路径。

顺便说明一个常被误解的点：**ASan 通过不代表没有并发问题**。
它检查的是内存越界与释放后使用，对数据竞争完全无感。

### 5.4 端到端

| 场景 | 命令 | 结果 |
|---|---|---|
| 五种实现对比 | `./bin/counter -k all` | 表格输出，raw 的 match 为 NO |
| 单一实现 | `./bin/counter -k atomic` | 只跑一行 |
| 参数校验 | `-t 0` / `-n -1` / `-k foo` | 打印提示，退出码 2 |
| 帮助 | `-h` | 打印用法，退出码 0 |

---

## 6 性能

测试条件：10 线程，每个线程自增 1,000,000 次，重复 3 轮取平均，`-O2`。

多次独立测量（同一台机器、不同时间点）的结果：

| 实现 | 3 轮平均耗时 | 吞吐 | 最终计数 | 是否精确 |
|---|---|---|---|---|
| `raw` | 53.5 ~ 55.2 ms | 181 ~ 187 Mops/s | 1,359,560 ~ 1,393,944 | **否（丢 86%）** |
| `mutex` | 386.7 ~ 409.3 ms | 24.4 ~ 25.9 Mops/s | 10,000,000 | 是 |
| `spin` | 935.2 ~ 1011.3 ms | 9.9 ~ 10.7 Mops/s | 10,000,000 | 是 |
| `atomic` | 143.3 ~ 159.2 ms | 62.8 ~ 69.8 Mops/s | 10,000,000 | 是 |
| `asm` | 151.9 ~ 162.1 ms | 61.7 ~ 65.9 Mops/s | 10,000,000 | 是 |

波动来自睿频、邻居负载和调度迁移，跨次运行可以到 10% 左右。**这张表该用来看
量级与倍数关系，不适合逐位对比**——同一个实现两次测量差 10% 是正常现象。

四个结论：

**1. 原子操作比互斥锁快约 2.6 ~ 2.8 倍。** 互斥锁每次加解锁都是一次函数调用，
无竞争时走用户态的快速路径，有竞争时进内核（futex 睡眠与唤醒）。
原子指令只是一条 `lock xadd`，不涉及内核。

**2. 自旋锁比互斥锁慢约 2.3 ~ 2.4 倍——这个结果反直觉，但完全符合机制。**
10 个线程争同一根缓存行时，自旋的线程不停地用原子指令反复碰锁状态，
缓存行在 16 个逻辑核之间来回弹，总线流量把收益全吃掉；而互斥锁让等待者
真正睡眠，把 CPU 让给持锁者。**自旋锁的适用条件是临界区极短且竞争不激烈**，
高竞争下它比互斥锁更差。这一条与"自旋锁比互斥锁快"的常见说法相反，
差别就在于负载：单核或低竞争的短临界区里，自旋确实省掉了上下文切换。

**3. 手写内联汇编没有跑赢编译器内建。** `asm` 与 `atomic` 的差距在 5% 以内，
有时 `asm` 略快、有时 `atomic` 略快，都落在测量波动范围内。
`__atomic_add_fetch` 在 x86 上生成的就是
`lock xadd`，两者本质是同一条指令。**除非要控制特定寄存器、使用编译器不支持的
指令，否则手写汇编只增加出错面，不带来性能。**

**4. `raw` 的"快"没有意义。** 它快是因为大部分工作根本不存在——
180 Mops/s 对应的是"十次自增里只有 1.4 次真正生效"。把无效劳动算进吞吐
是基准测试最常见的自欺方式，所以表格里专门留了 `final` 与 `match` 两列。

### 6.1 原始程序里的 `usleep(1)` 反而掩盖了竞态

原始 `lock.c` 在自增循环里加了一句 `usleep(1)`，看起来是为了"放大竞态窗口"。
实测结果正好相反：

| 条件 | 期望 | 实际 | 丢失 |
|---|---|---|---|
| 10 线程 × 100 万次，不 sleep | 10,000,000 | 1,377,593 | 86.2% |
| 10 线程 × 200 次，每次后 sleep 1ms | 2,000 | 2,000 | 0% |

自增的"读-改-写"窗口只有几个纳秒。加了 1 ms 的 sleep 之后，
同一线程相邻两次自增的间隔被拉长到毫秒级，10 个线程的访问在时间上
被摊开，**撞在一起的概率趋近于零**。窗口本身就是竞态发生的前提，
把它拉稀疏等于让竞态消失。

这也是为什么 `make demo` 跑两组对照：一组是真实的密集竞争，
一组是原始程序的写法。结论是**演示竞态不能靠 sleep，
要靠"去掉唯一让它不发生的那个条件"**——即去掉同步。

原始程序还有一处掩盖：它用 100000 次 × 10 线程，但主线程每秒打印一次计数、
打印 100 秒，观察者看到的是"数字在涨"，而不是"数字与期望差多少"。
**没有期望值就没有正确性**，这是那段代码最根本的问题。

---

## 7 实现记录：从 `#if 0` 到模块

这个模块出自一份 80 行的教学代码 `lock.c`。它用预处理指令在四种实现之间切换：

```c
while (i ++ < 100000) {
#if 0
        (*pcount) ++;
#elif 0
        pthread_mutex_lock(&mutex);
        (*pcount) ++;
        pthread_mutex_unlock(&mutex);
#elif 0
        pthread_spin_lock(&spinlock);
        (*pcount) ++;
        pthread_spin_unlock(&spinlock);
#else
        inc(pcount, 1);
#endif
        usleep(1);
}
```

原文件保留在 git 历史里。这次改造前后的对比：

| 原版 | 本模块 |
|---|---|
| 四种实现靠 `#if 0` 手动切换，一次只能编译一种 | 五种实现同时存在，靠 `counter_kind` 运行时选择 |
| 想比较性能得改代码、重编译、记时间 | 一条命令输出全部对比 |
| 同步原语是全局变量，一个进程只能有一个计数器 | 不透明句柄，可同时持有任意多个 |
| 实现散落在 `thread_callback` 里，测不了 | `counter_add` 可以单线程测，也可以多线程压 |
| 正确性靠肉眼看计数 | 68 项断言，期望值由 `线程数 × 每线程次数` 算出 |
| `usleep(1)` 试图放大竞态，实际掩盖了它 | `make demo` 用两组对照说明这件事 |
| `pthread_spin_init` 用了 `PTHREAD_PROCESS_SHARED`，锁却在私有内存里 | 改成 `PTHREAD_PROCESS_PRIVATE`，与内存的实际归属一致 |
| 内联汇编的约束写错（`"m"` 应为 `"+m"`）靠 clobber 兜住 | 约束显式表达真实的数据流 |

关于 `PTHREAD_PROCESS_SHARED` 那一条补充一句：原代码把 `spinlock` 声明为普通全局变量，
`init` 时却传 `PTHREAD_PROCESS_SHARED`。这个 flag 的含义是"这把锁会被放在
多个进程共享的内存里"，而普通全局变量只在本进程的地址空间内共享。
flag 与实际不符不会立刻出错，但会在将来真的要放进 `mmap(MAP_SHARED)`
时给出误导——如果那段共享内存本身的属性没设置对，锁照样不生效。
**一个只在特定条件下才有意义的参数，写错时的代价往往延迟很久才显现。**

### 7.1 结构上的改变

最关键的改动是**把"策略"从"代码路径"变成"数据"**。

原版里换实现要改源码，因为实现隐藏在预处理分支里——编译期就定死了。
本模块里实现是一个枚举值，同一个二进制可以在运行时选，于是
"比较"这件事才成为可能：只有五种实现同时存在于同一个进程里，
才能保证它们面对的是同一个 CPU、同一个负载、同一份内核调度。

这是模块化的实际收益，比"代码更整齐"重要得多。

---

## 8 五种实现的取舍

| 实现 | 正确性 | 相对吞吐 | 什么时候用 |
|---|---|---|---|
| `raw` | **否** | —— | 只用于演示，任何场景都不该用 |
| `mutex` | 是 | 1×（基准） | 临界区长、竞争激烈、可能要睡在锁上 |
| `spin` | 是 | 0.4× | 临界区只有几条指令、且竞争不激烈（如每 CPU 一个锁） |
| `atomic` | 是 | 2.7× | 单个整数的简单累加，默认选它 |
| `asm` | 是 | 2.5× | 需要控制具体指令时；一般没必要 |

一条经验规则：**能用原子操作表达的，不要用锁；必须保护多步状态的，用互斥锁；
自旋锁只有在能证明"竞争极少且临界区极短"时才考虑。**

`mutex` / `spin` 的临界区里只做了一次 `+=`，所以这组数据反映的是
**锁本身的开销**，不是它们的典型用法。真实场景里临界区通常长得多，
那时锁的开销占比会下降，原子操作反而可能不够用（因为它只能保护一个整数）。

---

## 9 已知局限

### 9.1 `asm` 实现只支持 x86

`lock; xaddq` 是 x86 指令。ARM 上要用 `ldaxr` / `stlxr` 循环，或者
ARMv8.1 的 `LDADD`；RISC-V 用 `amoadd`。指令形式完全不同，不能共用一段汇编码。
模块用 `#if defined(__x86_64__) || defined(__i386__)` 把范围写清楚，
其他平台上 `counter_kind_supported(COUNTER_KIND_ASM)` 返回 0。

### 9.2 只有 64 位有符号整数的加

没有实现 CAS、交换、按位操作，也没有指针类型的计数器。
这些用同样的模式可以扩展，接口上需要再加参数区分。

### 9.3 内存序固定为 SEQ_CST

`__atomic_add_fetch` 用的是顺序一致的 `SEQ_CST`，是最强的内存序，
性能上不是最优。需要更快时应当按实际需求降到 `RELEASE` / `ACQUIRE`
或 `RELAXED`。默认给最强的是因为**正确性优先于速度**：把最弱的序交给
不熟悉内存模型的调用方，等于埋一个只在特定架构上出现的 bug。

### 9.4 没有对应的"读-改-写"复合操作

只有 `add`。真实场景里常用的 `compare_and_swap`、`fetch_and_or` 都没有。
`counter_add` 返回新值，因此可以拿来做"分配唯一 ID"，但做不了
"如果等于旧值就替换"。

### 9.5 `raw` 是未定义行为，却存在于公开接口里

把 `COUNTER_KIND_RAW` 留在生产接口里是有争议的设计。保留的理由是它让
"数据竞争到底会造成什么后果"可以复现；代价是有人可能误用它。
`counter_kind_is_atomic` 提供了一道机器可查的闸门，但它需要调用方主动去查。

---

## 10 待办

- [ ] 补 ARM 平台的 `ldaxr` / `stlxr` 实现，用交叉编译加 qemu 验证
- [ ] 增加 `COUNTER_KIND_MUTEX_ADAPTIVE`（先自旋若干次再睡），对照 `spin` 与 `mutex`
- [ ] 把内存序做成创建时的参数（`SEQ_CST` / `ACQUIRE_RELEASE` / `RELAXED`）
- [ ] 增加 `compare_and_swap` 与 `fetch_and_or`
- [ ] 用 `perf stat` 采集 cache-miss 与总线锁次数，把"spin 为什么慢"从机制解释变成数据证据
- [ ] 在 1/2/4/8/16/32 线程下各跑一遍，画出吞吐随线程数变化的曲线，找拐点
- [ ] 补"内存占用"一行——现在表格里只有时间和吞吐
- [ ] 把 `mtx.lock` / `spin.lock` 移出 `counter` 对象（改为指向单独分配的
      `sync` 块），消掉 `counter_get` 里两处 `-Wcast-qual` 转型。
      C 没有 `mutable`，让 `const counter *` 上的读既能加锁、又不丢弃 `const`
      的唯一办法，是让锁成为"指针指向的、本身非 const 的对象"。
      目前那两处转型按 C11 6.7.3p6 是良定义的（对象由 `create` 分配、
      调用方持有非 const 句柄），所以 CI 只计数不判定失败

---

## 测试环境补充

`make bench` 的完整命令：

```bash
./bin/counter -k all -t 10 -n 1000000 -r 3
```

`make demo` 会依次跑"高竞争"与"加 sleep"两组，用来对照说明
为什么原始程序的 `usleep(1)` 掩盖了竞态。
