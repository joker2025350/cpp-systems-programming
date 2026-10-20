# 05 单词统计（wordcount 模块）

一个可复用的文本统计模块，加一个建立在它之上的命令行驱动。

模块对字节流做单遍扫描，统计行数、单词数与字节数。核心是 `wc_counter` 这个流式计数器：
它把扫描状态与累计结果封装在对象里，因此同一份数据无论被切成多少段送入，结果都完全一致。
`include/` 与 `src/` 分离，`main.c` 只通过 `libwordcount.a` 链接，不重复编译模块源码——
"这个模块能被独立链接使用"这件事在每次构建时都会被动验证一遍。

```bash
$ ./bin/wordcount testdata/a.txt testdata/b.txt
       2       14      118 testdata/a.txt
    1762   114588   617637 testdata/b.txt
    1764   114602   617755 total
```

---

## 1 目录结构

```
src/word-count/
├── include/
│   └── wordcount.h          对外接口：类型、状态码、函数原型
├── src/
│   ├── wordcount.c          模块实现
│   └── main.c               命令行驱动（参数解析、输出排版、退出码）
├── tests/
│   ├── test_wordcount.c     单元测试，纯内存数据，40 项检查
│   └── check_against_wc.sh  与 wc / 独立方法交叉验证
├── testdata/
│   ├── a.txt                118 字节，人工构造，含易漏标点
│   └── b.txt                617637 字节，英文文本 1762 行
├── Makefile
└── README.md
```

编译产物统一落在 `bin/`，已被根目录的 `.gitignore` 忽略，不进版本库。

`include/` 与 `src/` 分开不是形式主义。头文件里只放调用方需要知道的东西——类型定义、
状态码、函数原型、版本号；实现细节（分隔符判据、状态编码、I/O 方式）一律不出现在头文件里。
这样调用方读 `wordcount.h` 就能用，不需要读 `wordcount.c`；而实现方改内部结构时，
只要不动接口，谁也不用重新看代码。

---

## 2 对外接口

### 2.1 统计结果

```c
typedef struct {
    unsigned long lines;   /* '\n' 出现的次数 */
    unsigned long words;   /* 最长连续字母数字段的个数 */
    unsigned long bytes;   /* 已扫描的字节总数 */
} wc_stat;
```

三个计数都是单调递增的累计量，没有负数，因此用无符号类型。`unsigned long` 在 LP64 平台
（Linux x86-64 / AArch64）上是 64 位，足够覆盖任何实际文件；在 32 位平台上上限约 42.9 亿，
若要处理更大的输入需要改用 `uintmax_t`。

### 2.2 状态码

```c
typedef enum {
    WC_OK         = 0,   /* 成功 */
    WC_ERR_PARAM  = 1,   /* 参数非法，例如传入了 NULL */
    WC_ERR_OPEN   = 2,   /* 无法打开文件 */
    WC_ERR_READ   = 3,   /* 读取过程中出错 */
    WC_ERR_CLOSE  = 4    /* 关闭文件时出错 */
} wc_status;
```

沿用 C 语言的惯例：0 表示成功，非 0 表示失败。`WC_OK` 固定为 0，因此可以直接写
`if (status)` 判断失败。返回后三种错误时 `errno` 保留导致失败的系统错误号，
调用方可以据此给出更具体的信息；返回 `WC_ERR_PARAM` 时 `errno` 无意义。

一个模块如果只用 `-1` 表示"出错了"，调用方就没法区分"文件不存在"和"读到一半磁盘坏了"，
只能自己猜。把错误分类是接口设计的一部分，不是附加功能。

### 2.3 流式计数器

```c
typedef struct {
    unsigned int state;    /* 内部：上一次扫描结束时的入/出词状态 */
    wc_stat      total;    /* 累计结果 */
} wc_counter;
```

这是模块的核心。结构体成员是公开的，为的是让调用方能在栈上直接分配、不必走
`malloc`——嵌入式环境下不希望在读一个文件时还依赖堆。但约定调用方不直接读写字段，
一切通过下面的函数。

### 2.4 函数

| 函数 | 作用 |
|---|---|
| `void wc_counter_reset(wc_counter *c)` | 把状态与三个累计量全部归零 |
| `wc_status wc_counter_update(wc_counter *c, const void *data, size_t size)` | 把一段字节追加到计数器 |
| `void wc_counter_get(const wc_counter *c, wc_stat *out)` | 取出当前累计结果（值拷贝） |
| `wc_status wc_count_buffer(const void *data, size_t size, wc_stat *out)` | 统计一段内存，等于 reset + update + get |
| `wc_status wc_count_stream(FILE *stream, wc_stat *out)` | 统计一个已打开的流，读到文件末尾，不关闭 stream |
| `wc_status wc_count_file(const char *path, wc_stat *out)` | 统计一个文件，内部负责关闭句柄 |
| `const char *wc_status_string(wc_status status)` | 状态码转可读描述，未知值返回 `"unknown status"` |

按使用场景分三层：

- **已经有完整数据**（mmap 映射、网络收包缓冲、字符串常量）→ `wc_count_buffer`
- **自己在驱动 I/O**（socket、管道、分块解压）→ `wc_counter` + `wc_counter_update`，按任意粒度喂
- **就是读个文件** → `wc_count_file` 一行搞定

### 2.5 缓冲区大小

```c
#ifndef WC_BUFFER_SIZE
#define WC_BUFFER_SIZE 16384u
#endif
```

只被 `wc_count_stream` / `wc_count_file` 使用。用 `#ifndef` 包一层，是为了允许在编译命令上
用 `-DWC_BUFFER_SIZE=n` 覆盖——没有这层保护，命令行传入的定义会被头文件里的 `#define` 覆盖掉，
结果是改了没效果而且不报错。取值取舍见第 6 节。

### 2.6 可重入性与线程模型

模块不使用任何全局状态，也没有内部的静态缓冲区，因此同一个进程里可以同时持有任意多个
互相独立的计数器。只要不同线程各自使用自己的 `wc_counter` 对象，并发调用是安全的；
`FILE *` 自身的并发访问按 stdio 的约定由调用方负责。

这条约束直接来自"状态外置"这个设计决定：状态如果在函数里用 `static` 变量保存，
这个模块就只能有一个实例，也没法在多线程里用。

---

## 3 设计要点

### 3.1 状态机：只在 `OUT -> IN` 跳变上计数

判定"是否处于单词内部"只需要两个状态：

```c
enum { WC_STATE_OUT = 0, WC_STATE_IN = 1 };

for (size_t i = 0; i < size; i++) {
    unsigned char ch = p[i];
    if (ch == '\n') lines++;
    if (wc_is_separator(ch)) {
        state = WC_STATE_OUT;
    } else if (state == WC_STATE_OUT) {
        state = WC_STATE_IN;
        words++;
    }
}
```

关键在于只在 `OUT -> IN` 的那一次跳变上计数：每个单词恰好被计一次，不需要记录上一个字符，
也不需要判断"是不是最后一个字符"。额外空间是 O(1)，与输入规模无关。

`IN -> IN`（单词中途）和 `IN -> OUT`（单词结束）都不计数：前者是同一个单词的延续，
后者已经在上一次跳变时计过了。

**计数的方向是有代价的。** 如果反过来按 `IN -> OUT` 跳变计数（"遇到分隔符时结算上一个单词"），
那么当输入以字母数字结尾、后面没有分隔符时，最后一个单词永远不会被结算——必须额外写一段
"循环结束后再判一次收尾"的代码。而 `OUT -> IN` 天然处理了这种情况，测试用例
`单个单词，无结尾换行` 就是为了盯住这一点。这是状态机计数方向的选择问题，不是边界条件的补丁问题。

### 3.2 状态外置：流式处理的必要条件

`state` 存在 `wc_counter` 对象里，而不是 `wc_counter_update` 的局部变量里。这是整个模块
能做到"任意粒度切分结果一致"的原因。

反例很直观：一段文本分两次送入，第一次以 `hel` 结束，第二次以 `lo` 开始。状态若在函数退出时
丢失，第二次送入 `l` 时看到的局部状态是 `OUT`，于是把这个词的后半段当成一个新词，
结果从 1 变成 2。`wc_counter_update` 在每次调用结束时把 `state` 写回对象，下次调用接着用，
所以两条边界合起来仍然只算一个词。

`tests/test_wordcount.c` 里的 `test_chunk_boundary` 把 `"hello world"` 按 1 到 11 字节
共 11 种粒度逐一切分送入，结果都必须与整体送入一致；`test_split_inside_word` 则专门在词中间
切断一次。这两组用例覆盖的正是单文件 demo 最容易忽略、而任何真正要处理流的模块必须处理的场景。

### 3.3 参数与所有权约定

接口上写清楚了三件事，代码里逐一落实：

1. **`data` 允许为 NULL，此时 `size` 必须为 0**（表示一段空输入）。C11 6.5.6p8 规定，
   对空指针做算术运算（包括 `p + 0`）是未定义行为，所以 `data == NULL && size != 0` 要拒掉，
   `data == NULL && size == 0` 则要在进入循环之前就返回，不能先去构造 `const unsigned char *p = data`。
2. **返回 `WC_ERR_PARAM` 时计数器内容不被修改**。调用方可以放心重试，不必先存档。
3. **`wc_count_file` 内部 `fopen`，就由它负责在每一条返回路径上 `fclose`。** 所有权跟着作用域走，
   不依赖调用方记得清理。

### 3.4 `errno` 的保存与恢复

```c
wc_status status = wc_count_stream(fp, out);
int saved = errno;                                    /* 先存 */
if (fclose(fp) != 0 && status == WC_OK) {
    return WC_ERR_CLOSE;
}
errno = saved;                                        /* 后恢复 */
return status;
```

`fclose` 会刷新 stdio 缓冲区、可能触发实际的 `write` 系统调用，因此它**可能改写 `errno`**。
如果先 `fclose` 再读 `errno`，读到的可能是关闭阶段产生的错误号，把真正的读错误覆盖掉；
如果 `fclose` 成功，它还可能把 `errno` 改成一个与本次失败无关的残留值。

顺序必须是"先存、后调 `fclose`、再恢复"。这两行看着不起眼，但少了任何一行，
错误信息都会在小概率下指向错误的原因——而 `errno` 相关的问题做压力测试很难复现。

### 3.5 语义明确定义在头文件里

`lines` 定义为 `'\n'` 出现的次数，不是"文本行数"。区别在于最后一行若没有换行符，
不计入行数；空行则计入。这个选择与 `wc -l` 一致，因此可以直接交叉验证。
如果把它定义成"文本行数"，`a\nb\nc` 到底算 2 行还是 3 行就会成为接口层面的争议，
而实现方无论选哪个都要在文档里解释半天。跟随已有的、有明确定义的工具，是最省事的选择。

---

## 4 构建

```bash
make            # 构建静态库 bin/libwordcount.a 与命令行工具 bin/wordcount
make lib        # 只构建静态库
make cli        # 只构建命令行工具
make test       # 构建并运行单元测试
make check      # 用 testdata 下的文件与 wc 做交叉验证
make sanitize   # 用 ASan + UBSan 重新构建并跑测试，产物在 bin/sanitize/
make bench      # 测量不同缓冲区大小下的吞吐
make clean      # 删除 bin/（含全部变体）
```

变体构建使用独立输出目录，是因为 make 判断是否重建只比较时间戳、不比较编译选项：
共用 `bin/` 时先跑 `make sanitize` 再跑 `make`，普通构建会认为目标已是最新，
留下一个插桩过的可执行文件。

默认编译参数：

```make
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g
CPPFLAGS += -Iinclude
```

| 参数 | 作用 |
|---|---|
| `-std=c11` | 固定语言标准，避免依赖编译器的默认方言 |
| `-Wall -Wextra` | 常规警告集 |
| `-Wconversion` | 检查隐式窄化转换。本主题最关键的一处缺陷只有它能发现（见第 7 节） |
| `-O2` | 优化；模块不依赖任何未定义行为，优化不会改变结果 |
| `-g` | 保留调试信息，配合 `make sanitize` 时能定位到具体行 |

`CFLAGS ?=` 用的是条件赋值，因此可以在命令行上覆盖，例如
`make CFLAGS="-std=c11 -Wall -Wextra -Wconversion -O0 -g -funsigned-char"`
用来模拟 `char` 无符号的平台。

**把模块作为库使用：**

```bash
cc -std=c11 -Wall -Wextra -Wconversion -O2 -Iinclude your_app.c bin/libwordcount.a -o your_app
```

只需要 `-Iinclude` 指向头文件，`libwordcount.a` 指向静态库。链接顺序上库要放在
引用它的目标文件之后——`ar` 生成的静态库按需抽取成员，顺序反了会报未定义符号。

---

## 5 测试与验证

验证环境：**Linux 6.8.0-138 / gcc 11.4.0 / x86_64**。

### 5.1 编译警告

| 编译参数 | 结果 |
|---|---|
| `-std=c11 -Wall -Wextra -Wconversion -O2 -g` | **0 条** |
| 追加 `-funsigned-char`（模拟 `char` 无符号的平台） | 0 条 |

### 5.2 单元测试

`make test`，全部用例只用内存中的数据，不依赖任何外部文件，可以在任何目录下直接运行：

```
wordcount 1.0.0 单元测试

ok    空输入（NULL + size 0）
ok    空输入的结果
ok    只有分隔符
ok    单个单词，无结尾换行
ok    两个单词
ok    标点作为分隔符
ok    数字计入单词
ok    三行，行尾都有换行
ok    三行，最后一行无换行
ok    三个空行
ok    基准：整体送入
ok    逐字节送入（每段 1 字节）
ok    跨块边界：每段 1 字节
...
ok    跨块边界：每段 11 字节
ok    在词中间切断仍算一个词
ok    在分隔符处切断算两个词
ok    第一次使用
ok    重置后复用
ok    重置后状态确实归零
ok    out 为 NULL
ok    计数器为 NULL
ok    data 为 NULL 但 size 非 0
ok    流为 NULL
ok    路径为 NULL
ok    wc_counter_reset(NULL) 不崩溃
ok    wc_status_string 已知值非空
ok    wc_status_string 未知值

40 项检查，0 项失败
```

`main` 的返回值是失败项数，因此可以直接接进 `make` 之外的其他构建系统：

```c
return (g_failures == 0) ? 0 : 1;
```

### 5.3 交叉验证

`make check` 用与模块完全无关的路径独立算一遍，逐项比对：

```
与 wc / 独立方法交叉验证

          行          词         字节  文件
---------------------------------------------------------------
           2           14            118  testdata/a.txt
        1762       114588         617637  testdata/b.txt

全部一致（2 个文件，6 项比对）
```

三个计数的验证方式不同：

- **行数、字节数**：直接与 GNU `wc` 比对。这两个量的定义没有歧义，应当完全一致。
- **单词数**：`wc -w` 的口径不同（按空白切分），不能直接比。改用等价变换
  `tr -c '[:alnum:]' '\n' | grep -c '[[:alnum:]]'`——把所有非字母数字的字节换成换行，
  再数非空行数，即"最长连续字母数字段"的个数。这条命令与模块的实现没有任何共享代码，
  是真正独立的第二条路径。

脚本里固定了 `LC_ALL=C`，保证 `tr` 与 `grep` 的字符类按字节处理，不受调用环境的区域设置影响。
不加这一句，脚本在不同机器上的行为会不一致。

### 5.4 消毒剂

`make sanitize`，用 `-fsanitize=address,undefined -fno-omit-frame-pointer -O1` 重新构建：

```
--- 单元测试（ASan + UBSan）---
40 项检查，0 项失败

--- 命令行工具（ASan + UBSan）---
       2       14      118 testdata/a.txt
    1762   114588   617637 testdata/b.txt
    1764   114602   617755 total

--- 错误路径 ---
wordcount: testdata/nonexistent.txt: cannot open file: No such file or directory
```

没有任何 ASan 或 UBSan 报告。`sanitize` 目标的产物落在 `bin/sanitize/`，
与普通构建的 `bin/` 分开存放，所以不需要先 `make clean`，也不会误拿到上一次的
非消毒剂产物。这样分开是因为 **make 只比较时间戳、不比较编译选项**：
共用同一个目录时，先跑 `make sanitize` 再跑 `make`，普通构建会认为目标已是最新，
于是留下一个插桩过的可执行文件。

### 5.5 端到端

| 场景 | 命令 | 结果 |
|---|---|---|
| 标准输入 | `printf "hello world\nfoo\n" \| ./bin/wordcount` | `2 3 16`，退出码 0 |
| 单文件 | `./bin/wordcount testdata/a.txt` | 行 2、词 14、字节 118 |
| 多文件 | `./bin/wordcount testdata/a.txt testdata/b.txt` | 逐行输出，末尾补 `total` 行 |
| 空输入 | `: \| ./bin/wordcount` | `0 0 0`，退出码 0 |
| 文件不存在 | `./bin/wordcount testdata/nonexistent.txt` | 报错到 stderr，退出码 1 |

退出码是关键：脚本里用 `if ./bin/wordcount "$f"; then` 或 `&&` 串联时，
只有退出码正确，失败才能被检测到。多个文件时只要有一个失败，最终退出码就是 1，
但其余文件仍然会被继续处理，不会因为第一个错误就中断整批任务。

### 5.6 内存与资源

| 验证项 | 结果 |
|---|---|
| ASan（越界、泄漏） | 无报告 |
| 文件描述符泄漏 | 连续 500 次 `wc_count_file`，`/proc/self/fd` 计数 6 → 6，**净增 0** |
| 非法参数（各接口传 NULL） | 全部返回 `WC_ERR_PARAM`，不崩溃 |
| `wc_counter_reset(NULL)` / `wc_counter_get(NULL, &st)` | 直接返回，不崩溃 |

文件描述符那一项值得单看：`ulimit -n` 是 1024，如果每次调用泄漏一个描述符，
同一个进程里累积约 1020 次就会让 `fopen` 因 `EMFILE` 失败。一次性程序看不出这个问题——
`main` 调用完就退出，进程结束时内核统一回收；但只要把它放进长期运行的循环，
泄漏立刻变成硬故障。这也是"资源所有权必须跟着作用域走"最实际的理由。

---

## 6 性能

测试数据：`testdata/b.txt` 拼接 20 份，共 **12 352 740 字节**（11.78 MiB）、35 240 行、
2 291 760 词。各跑 20 轮取总时间，`-O2` 编译。

### 6.1 与单文件实现、GNU `wc` 的对比

| 实现 | 20 轮总耗时 | 单轮 | 吞吐 |
|---|---|---|---|
| 单文件完善版（`fgetc` 逐字符扫描，提交 `cdb73f0`） | 1517 ~ 1537 ms | ~76 ms | ~162 MB/s |
| 本模块（`fread`，16 KiB 块） | 531 ~ 538 ms | ~27 ms | ~465 MB/s |
| `LC_ALL=C wc -l -w -c`（参考） | 1534 ~ 1545 ms | ~77 ms | ~161 MB/s |

**按块读取带来约 2.9 倍提升。** 差距全部来自 I/O 方式：`fgetc` 每次调用都要进入 stdio 内部
逻辑（检查缓冲区、必要时加锁），对每个字节都走一遍；`fread` 一次把 16 KiB 搬进用户缓冲区，
之后是纯内存的顺序扫描。

顺带说明为什么模块比 `wc` 快：GNU `wc` 的 `-w` 用的是逐字节的状态判断加 `iswspace`
一类的多字节处理路径，在 C locale 下反而更重。这不代表本模块"比 `wc` 好"——
`wc` 要处理的是任意区域设置下的多字节字符，代价花在了本模块明确不支持的场景上。

把 `wc` 的三个统计分开测，可以确认瓶颈在哪里：

| 命令 | 20 轮总耗时 | 单轮 |
|---|---|---|
| `wc -c` | 15 ms | ~0.8 ms |
| `wc -l` | 53 ms | ~2.7 ms |
| `wc -w` | 1578 ms | ~79 ms |
| `wc -l -w -c` | 1534 ms | ~77 ms |

字节数和行数几乎是纯内存带宽（`-c` 甚至可以用 `fstat` 拿到大小，不需要读内容），
耗时完全在词数统计上。三个一起做和只做 `-w` 的时间基本相同，说明**一遍扫描的时间由最重的那一步决定**，
多统计几项几乎不增加成本——这也是本模块把三个计数放在同一次扫描里完成的原因。

### 6.2 缓冲区大小的取舍

同一输入，20 轮，重复测三次：

| 块大小 | 第一次 | 第二次 | 第三次 | 均值 |
|---|---|---|---|---|
| 256 B | 577 ms | 565 ms | 564 ms | 569 ms |
| 1 KiB | 562 ms | 555 ms | 558 ms | 558 ms |
| 4 KiB | 558 ms | 547 ms | 552 ms | 552 ms |
| 16 KiB | 515 ms | 534 ms | 531 ms | 527 ms |
| 64 KiB | 517 ms | 537 ms | 531 ms | 528 ms |

结论：

1. **256 B 到 16 KiB 只差约 8%**，说明块大小只要不极端偏小，就对吞吐影响有限。
2. **超过 4 KiB 收益就很小。** 4 KiB 是常见文件系统的页大小，也是 stdio 默认缓冲区量级的取值。
3. **16 KiB 与 64 KiB 基本持平**，但 64 KiB 的栈占用是 16 KiB 的四倍。`wc_count_stream`
   把缓冲区放在栈上（不是 `malloc`），因此在栈空间受限的环境（线程栈默认往往只有几百 KB 到几 MB）
   上，64 KiB 是个不划算的赌注。

默认取 **16 KiB**，是在"吞吐已经不差"和"栈占用仍然安全"之间的折中。
需要更极端的取值时，通过 `-DWC_BUFFER_SIZE=n` 覆盖，不必改头文件。

顺带一个反面结论：模块早期版本试过 `getc_unlocked`，实测只快约 2%，落在测量噪声里。
它是 POSIX 扩展，去掉它等于去掉一个平台依赖，代价是那 2%——不值得。**先测量，再决定要不要换。**

---

## 7 实现记录：从单文件 demo 到模块

这个主题从一份 52 行的教学代码 `count.c` 开始，经过两轮改造才成为模块：

| 轮次 | 产物 | 规模 | 做了什么 |
|---|---|---|---|
| 起点 | 原版 `count.c` | 52 行 | 能跑，但有多处缺陷，且完全是 demo 形态 |
| 第一轮 | 单文件完善版 | 106 行 | 修掉全部缺陷，仍是单文件 |
| 第二轮 | **本模块** | 头文件 + 实现 + 驱动 + 测试 | 接口与实现分离，状态外置，可被独立链接 |

第一轮的成果保留在 git 历史里（提交 `cdb73f0`），第 6 节的性能对比仍会用到它。

### 7.1 起点：原版 `count.c`

```c
#define OUT     0
#define IN      1
#define INIT    OUT

int splite(char c) {                /* 手写字符表，只列了 9 个字符 */
    if ((' ' == c) || ('\n' == c) || ('\t' == c) ||
            ('\"' == c) || ('\'' == c) || ('+' == c) ||
            (',' == c) || ';' == c || '.' == c)
        return 1;
    else
        return 0;
}

int count_word(char *filename) {
    int status = INIT;
    int word = 0;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) return -1;

    char c;                          /* 用 char 接收 fgetc 的返回值 */
    while ((c = fgetc(fp)) != EOF) {
        if (splite(c)) {
            status = OUT;
        } else if (OUT == status) {
            status = IN;
            word ++;
        }
    }

    return word;                     /* fp 从未关闭 */
}

int main(int argc, char *argv[]) {
    if (argc < 2) return -1;         /* 静默退出，不提示 */
    printf("word: %d\n", count_word(argv[1]));
}
```

### 7.2 第一轮：修掉的缺陷

| # | 缺陷 | 依据与后果 | 处理 |
|---|---|---|---|
| 1 | `char c` 接收 `fgetc` 的返回值 | C11 7.21.7.1 规定 `fgetc` 返回 `int`，EOF 是负值；C11 6.2.5p15 规定 `char` 是否带符号由实现决定。x86 上 `char` 有符号、恰好存得下 -1，行为正确；ARM 等平台 `char` 无符号，EOF(-1) 截断成 255，与 EOF 永不相等 → **死循环** | 改用 `int` 接收；字节数据在扫描时显式转 `unsigned char` |
| 2 | 任何返回路径都不 `fclose` | 每次调用泄漏一个文件描述符。一次性程序看不出（进程退出时内核统一回收），放进长期运行的循环就是硬故障 | 由 `fopen` 所在的函数在每条返回路径上关闭 |
| 3 | 不检查返回值 | 文件打不开时返回 -1，被当作词数打印成 `word: -1`；`main` 永远返回 0，脚本无法检测失败 | 引入分类状态码，输出具体原因，退出码反映失败 |
| 4 | 只看 `EOF` 不看 `ferror` | `fgetc` 返回 `EOF` 有两种可能：正常读完、发生读错误。只看 `EOF` 会把读错误静默当成正常结束 | 检查 `ferror`，命中时报读错误 |
| 5 | 词数判据用手写字符表 | 原表只列了 9 个字符，`! ? - ( ) [ ] { } : / = < > \| @ # $ % ^ & * ~` 以及反引号全部漏掉，会把 `!`、`{-`、`}` 连成的串当成词 | 改为 `isalnum`，语义是"最长连续字母数字段"，定义完整无遗漏 |
| 6 | 计数器用 `int` | 常见平台上限 2147483647，处理语料库、日志聚合级别的输入会溢出 | 改用 `unsigned long`（LP64 下 64 位） |
| 7 | 只能处理单个文件，`argc < 2` 时静默 `return -1` | 用户看不到任何提示，不知道是参数没给还是文件有问题；不支持批量，也没有总计 | 遍历 `argv[1..argc-1]`，输出用法提示，多个文件时补 `total` 行 |
| 8 | `splite` 拼写错误，参数用 `char`；`OUT`/`IN` 用 `#define` | 函数名与被判断的语义不符（它判断的正是"是不是分隔符"）；`#define` 在预处理阶段无差别替换，没有类型、调试器看不见 | 改名 `is_separator`；常量改为 `enum`；内部函数保持 `static` |
| — | `main` 走到 `}` 结束，没有 `return` | **不构成缺陷。** C11 5.1.2.2.3 专门规定：`main` 执行到终结的 `}` 等价于 `return 0;`。这是标准为 `main` 开的特例，其他函数不享有 | 模块的 `main` 仍显式 `return failed;`，因为它的返回值承载了"是否有文件处理失败"的信息 |

第 1 条最需要留意：它在开发机上**完全看不出来**。x86 的 `char` 有符号，`-Wall -Wextra`
不报任何警告，程序行为也正确；只有 `-Wconversion` 能发现 `int` → `char` 的窄化转换。
同一份代码在 ARM 目标板上直接死循环。这正是 `-Wconversion` 值得长期开着的理由——
它抓的不是风格问题，而是开发机与目标平台的 ABI 差异。

关于第 8 行那条"不是缺陷"，有一个可观测的旁证：把 `main` 改名之后，GCC 立刻开始报警告——
`warning: control reaches end of non-void function [-Wreturn-type]`。因为在它眼里这已经
不是 `main` 了，特例不再适用。这也解释了原版在 `-Wall -Wextra` 下**完全零警告**的部分原因。

第 2 条和第 4 条在一次性小程序里都不会暴露。这是"能跑的代码"与"能用的模块"之间最典型的差距：
前者只需要在一条路径上正确，后者需要所有路径都正确。

### 7.3 第二轮：结构上的改变

第一轮结束时，代码是正确的，但仍然是 demo——要复用只能把整个 `count.c` 拷走。
第二轮解决的是"能不能被别人拿去用"：

| 单文件完善版 | 本模块 |
|---|---|
| 一个 `count.c` 包含全部逻辑 | `include/`（接口）+ `src/`（实现）+ `src/main.c`（驱动） |
| 只能统计单个文件，一次一个结果 | `wc_counter` 对象，可同时持有任意多个独立计数器 |
| 状态是 `count_word` 的局部变量，数据必须整体到手 | 状态存在对象里，可按任意粒度分块送入，结果不变 |
| 内部函数全部 `static`，外部无法复用 | 公开接口 + `libwordcount.a`，`main.c` 只链接不重编 |
| 手写测试，无自动化 | `make test`（40 项检查）+ `make check`（交叉验证）+ `make sanitize` |
| 手动 `gcc` 一行命令 | `Makefile`，目标拆分，零警告构建 |
| 错误只能用 `errno` 的原始值表达 | `wc_status` 枚举给错误分类，附 `wc_status_string` |

最关键的一步是**把扫描状态从函数局部变量提升为对象成员**。这不是"重构"式的整理，
而是能力上的变化：状态在局部变量里，函数就只能处理"已经完整到手"的数据；状态在对象里，
函数才能处理任意分段的流。后面所有的接口设计（`wc_counter_update` 的分块语义、
可重入性、多实例并存）都是从这一个决定推出来的。

---

## 8 词的定义与口径

同一份 `b.txt`，按不同的"单词"定义会得到不同的数字。列出这组数据是为了说明
**词数不是一个唯一定义的量**，任何一个数都得先说清楚口径：

| 定义 | 实现方式 | `b.txt` 结果 |
|---|---|---|
| 非空白段 | `wc -w`，`isspace` 判据 | 113550 |
| 非（空白或标点）段 | `isspace \|\| ispunct` 判据 | 115426 |
| 字母数字段 | 本模块，`isalnum` 判据 | **114588** |

第二行比第三行多出 **838** 个"词"。原因在字节口径：`b.txt` 含 8757 个非 ASCII 字节
（正文用了 UTF-8 弯引号），而在 C locale 下 `ispunct` 对 `0x80–0xFF` 一律返回 0——
这些字节既不是空白也不是标点，于是被当成"词字符"，一个孤零零的弯引号就成了一"个单词"。

以 `isalnum` 为判据时，`0x80–0xFF` 全部落在分隔符一侧，这 838 个幽灵词就消失了。
代价是弯撇号会把词切开：`don’t` 写作弯撇号时会被算成 2 个词。

本模块选择的口径是 **单词 = 最长的连续字母数字段**。它有两个好处：ASCII 下定义完整、
没有遗漏（第 7 节第 7 条缺陷正出自旧判据的遗漏）；高位字节的处理方向是"更保守"
（少算而不是多算），不会凭空造出词来。

---

## 9 已知局限

### 9.1 非 ASCII 字节一律当作分隔符

本模块是**字节导向**的，并且工作在 C locale 下。`isalnum` 对 `0x80–0xFF` 全部返回 0，
因此这些字节全部被当作分隔符。后果是双向的：

- 好的一面：孤立的 UTF-8 标点不会被误计成单词（第 8 节的 838 个幽灵词）。
- 不好的一面：带弯撇号的词会被切开，`don’t` 算 2 个词。

**严格的 UTF-8 处理需要另一套做法**：`setlocale(LC_ALL, "")` 打开区域设置，
用 `mbrtowc` 把字节流转换为宽字符，再用 `iswalpha` 判断——这是"按字符"而非"按字节"扫描，
代码量和复杂度都会明显上升，且引入对区域设置数据的运行时依赖。
本主题定位是字节流扫描的基础实现，不展开。

### 9.2 只统计总量，没有词频

当前只输出三个总数。若要统计每个词的出现频次（`sort | uniq -c` 那类场景），
需要一个"单词 → 计数"的映射表。C 里通常用哈希表实现，涉及哈希函数设计、冲突处理、
内存管理、大小写归一，是另一个量级的话题。

### 9.3 不预判输入类型

`fopen` 在 Linux 上也能打开目录，随后的读取会失败并设置 `errno = EISDIR`。
本模块会通过 `ferror` 报出 `Cannot read stream: Is a directory`，
但不会在打开之前把目录识别出来并给出更准确的提示。

### 9.4 大文件的 `bytes` 计数依赖平台

`unsigned long` 在 LP64 平台是 64 位，在 32 位平台（ILP32）上上限约 42.9 亿字节，
即单个文件超过 4 GiB 时 `bytes` 会回绕。需要跨平台一致的行为时应改用 `uintmax_t`。
`lines` / `words` 在 32 位平台上的上限同样是 42.9 亿，对绝大多数文本足够。

---

## 10 待办

- [ ] 增加词频统计模式（哈希表 + 大小写归一）
- [ ] 增加 `-h` / `--help` 与短选项解析，支持 `-` 作为"读标准输入"的显式文件名
- [ ] 若要正确处理 UTF-8，改用 `mbrtowc` + `iswalpha` 做宽字符扫描（见 9.1）
- [ ] 跨平台一致的计数宽度：改用 `uintmax_t` 或在头文件里做编译期检查（见 9.4）
- [ ] 用 `perf` 对比 `fread` 路径与 `mmap` + `wc_count_buffer` 路径的差异，
      确认块读取在大文件上是否仍是瓶颈
- [ ] 为 `wc_count_stream` 补一个 `EINTR` 重试用例（当前假设 stdio 已处理中断）

---

## 测试数据

| 文件 | 大小 | 说明 |
|---|---|---|
| `a.txt` | 118 字节 | 人工构造，故意塞入 `' " ; , . ! { -+ }` 等标点，用于检验分隔符判据 |
| `b.txt` | 617637 字节，1762 行 | 英文文本，含 UTF-8 弯引号，用作较大规模的输入 |
