# epoll-server —— 多端口监听的 epoll 服务端 + 多连接压测客户端

一个模块两个可执行文件，因为源素材就是两个配套的程序：服务端在
`port..port+99` 这 100 个端口上各建一个监听 socket，全部以 `EPOLLIN`（水平触发）
注册进一个 epoll 实例；`accept` 出来的连接切成非阻塞，以 `EPOLLIN | EPOLLET`
（边缘触发）注册。客户端按同一个端口范围轮转 `connect`，每个连接发一行打招呼，
目标 340000 个连接。

代码来自 `D:\9. 百万并发的服务器\`（2019 年课程示例）下的
`tcp_server.c` 与 `mul_port_client_epoll.c`。本轮只做到
**入库 + 可构建 + 可运行**：**逻辑与原始示例一致，缺陷一个没修**。
已知局限 35 条，分服务端 18 条、客户端 17 条，每条都给标准依据。

当前状态一句话：服务端能接连接、能读、能打屏，但一次事件只取 1024 字节，
其余永久滞留（16384 字节实测只读到 2048）；客户端能建连接（实测 ~900/s），
但读写落不到真正就绪的 fd 上，回声服务器回了 2923 行、客户端打印 0 行。
第二阶段的时间窗是 2026-12-20 ~ 2026-12-26。

## 目录结构

```
src/epoll-server/
├── Makefile                        all / test / sanitize / clean（产出两个二进制）
├── README.md                       本文件
├── src/tcp_server.c                服务端：全部实现 + main
├── src/mul_port_client_epoll.c     客户端：全部实现 + main
└── tests/smoke.sh                  离线可判定的 11 项检查
```

少了 `include/`、`lib`、`cli`、`check`、`bench`、`tsan`，原因写在「对外接口」。

## 对外接口

**没有。** 两个文件都是单文件程序：服务端把 `socket` / `bind` / `listen` /
`epoll_ctl` / `accept` / `recv` / `close` 全写在 `main` 里，客户端把
`connect` / `send` / `epoll_wait` 全写在 `main` 里。外部只能通过命令行使用它们。

这不是设计选择，是记账。第二阶段第一个任务就是拆成
`include/epollserver.h` + `libepollserver.a` + 两个薄驱动，理由是**测不了**：
"这个事件该不该走 accept 分支"取决于 `islistenfd` 的判定结果，
"这次读该不该读第二次"取决于 fd 的阻塞模式与 `recv` 的返回值，
"这条应答该发回哪个连接"取决于 `events[i].data.fd`——这三件事现在都在同一个
`while` 循环体内，中间没有可单独调用的函数。

所以本轮的 11 项检查只能钉三类与网络无关的事实：二进制能起、缺参数时的退出码
与输出、启动路径是否真的把 100 个端口挂上去了。剩下的靠第 5 节的实测数据，
那是验证手段，不是用例。

## 设计要点

### 为什么是两个可执行文件

源素材是一个服务端加一个压测客户端，两者互相依赖：客户端没有对端只能得到
`connect` 失败，服务端没有客户端就只能空转。它们的角色不同、参数不同、退出
路径也不同，合成一个二进制只会得到 `argv[1]` 分派，掩盖"这是两件事"。
Makefile 因此有两条构建规则，`all` 产出 `bin/epoll-server` 与 `bin/mul-port-client`。

### 多端口：把"连接来自哪个端口"变成事件分类

服务端先建 100 个监听 socket（`port + i`，i 从 0 到 99），每个都
`EPOLL_CTL_ADD` 到同一个 epoll 实例，`data.fd` 存监听 fd 本身。主循环拿到事件后，
用 `islistenfd(events[k].data.fd, sockfds)` 在 100 个监听 fd 里线性查找：

- 查到了 → 这个事件属于监听 socket，走 `accept`，把新连接注册进同一个实例；
- 返回 0 → 这个事件属于某个已建立的连接，走 `recv`。

也就是说"事件源是谁"不靠 `data.ptr` 打标记，而是靠一次 100 次的扫描。这带来两个
后果：单事件成本随端口数线性增长（第 8 条），以及用 0 当"不是监听 fd"的哨兵值
（fd 0 是合法 fd，第 8 条）。

### 服务端的两种触发模式混用

监听 fd 用 `EPOLLIN`（水平触发，LT）：只要还有连接没被 `accept` 走，下次
`epoll_wait` 还会报。这是对的——`accept` 一次只取一个连接，剩在队列里的连接
需要在下一轮继续被发现。

连接 fd 用 `EPOLLIN | EPOLLET`（边缘触发，ET）：只在"不可读 → 可读"的那一刻
报一次。ET 的硬性约束是**收到事件后循环 `recv` 直到返回 `EAGAIN`**，把这次
状态变化对应的数据全部取走（`epoll(7)` 的 "Level-triggered and edge-triggered"
一节），并且必须配非阻塞 fd，否则最后那次读会永久阻塞。

这段代码把 fd 切成了非阻塞（`fcntl(clientfd, F_SETFL, O_NONBLOCK)`，比
`tcp-server` 模块多做了这一步），但**没有循环读**：一次事件只 `recv` 一次。
所以行为是"每次状态变化最多取 1024 字节"，剩下的字节要等下一次状态变化。

实测（第 5 节）：一次写入 16384 字节、之后对端静默，服务端输出 2 行 × 1024 字节，
剩 14336 字节再也不返回；65536 字节同样是 2 行；13 KB 连续写入只读到 1024。
之所以是 2 行不是 1 行，是因为内核按 MSS 把数据切成两段，每段到达各构成一次
"空 → 非空"的变化——ET 按**状态变化**计数，不按数据量计数。

### 客户端：一个循环干三件事

客户端的 `while (1)` 每轮最多做三件事：

1. 端口号 `index` 轮转（0..99），在一个新 fd 上 `connect` + `send` + `epoll_ctl(ADD)`；
2. 每攒到 999 个连接（或到 340000）调一次 `epoll_wait`，处理事件；
3. `usleep(1 ms)`。

第 2 步的"每 1000 个连接 poll 一次"决定了事件处理是连接建立的附属品，而不是
驱动；第 3 步决定了连接速率的上限——实测 1000 个连接用 1.1 秒，约 900/s，
与 `usleep(1000)` 加上阻塞 `connect` 的往返吻合。

注册的事件是 `EPOLLIN | EPOLLOUT`，水平触发。客户端 socket 建立后发送缓冲区
几乎总是空的，所以**每个连接每次 `epoll_wait` 都会报 `EPOLLOUT`**，事件列表里
几乎只有 `EPOLLOUT`；而处理分支先判 `EPOLLOUT`、`else if` 才判 `EPOLLIN`（第 3 条）。

## 构建

```bash
cd src/epoll-server
make              # bin/epoll-server 与 bin/mul-port-client
make test         # tests/smoke.sh（11 项，离线）
make sanitize     # ASan + UBSan 重建并跑同一套检查，产物在 bin/sanitize/
make clean
```

用法：

```bash
./bin/epoll-server 9000                  # 监听 9000..9099
./bin/mul-port-client 127.0.0.1 9000     # 连 9000..9099，轮转
```

服务端缺参数时打印 `Param Error` 并 `return -1`（退出状态 255）；`bind` 失败退出 2，
`listen` 失败退出 3。客户端缺参数时打印用法后 **`exit(0)`**——退出码 0 表示成功，
这是缺陷（客户端第 1 条），smoke 检查钉住的是当前行为。

**两个目标给不同的 feature test macro，是分别实测的结果。** gcc 11.4.0 /
glibc 2.35 上：

| 文件 | 不给宏 | `_POSIX_C_SOURCE=200809L` | `_DEFAULT_SOURCE` |
|---|---|---|---|
| `src/tcp_server.c` | 0 警告 | 0 警告 | 0 警告 |
| `src/mul_port_client_epoll.c` | **1 警告**（`usleep` 隐式声明） | **1 警告**（同上） | 0 警告 |

`usleep` 在 glibc 的 `<unistd.h>` 里藏在 `__USE_MISC` 之后（BSD/XSI 遗留接口），
而 `-std=c11` 定义的 `__STRICT_ANSI__` 会把 `__USE_MISC` 关掉；只给
`_POSIX_C_SOURCE` 不够，这一点是实测否掉的，不是推的。服务端用到的
socket 一族、`epoll_*`、`close` 都是无条件声明，所以不给宏。

必须补的头文件三个，都是编译错误而不是警告：`<unistd.h>`（`close`、`usleep`
原本完全无声明，`-std=c11` 下隐式函数声明从 C11 6.5.2.2p6 起不被允许）、
`<sys/socket.h>`（服务端的 socket 一族原先是靠 `<arpa/inet.h>` →
`<netinet/in.h>` 这条间接包含链碰巧拿到声明）、`<sys/time.h>`
（客户端的 `gettimeofday` 原本无声明）。

## 测试与验证

环境：内网 Ubuntu，Linux 6.8.0-149 / gcc 11.4.0。2026-11-26 测。

| 项目 | 结果 |
|---|---|
| 默认构建 | 0 条警告（`-std=c11 -Wall -Wextra -Wconversion -O2 -g`），两个文件都是 |
| 加严构建 | 0 条警告（再叠加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wsign-conversion`，合计十二个 `-W`） |
| `make test` | **11 项**，0 失败 |
| `make sanitize` | 0 条警告，11 项全过，无 ASan/UBSan 报告 |
| 100 个端口 | 启动输出 100 行，`ss -ltn` 在范围内计到 100 个 LISTEN |
| 一次写 16384 字节 | 收到 2 行 × 1024，剩 14336 永久滞留 |
| 一次写 65536 字节 | 收到 2 行 × 1024，剩 63488 永久滞留 |
| 空转 | `epoll_wait` 2 秒 386 次、5 秒 937 次（≈190/秒）；20 秒空载 CPU 8 个 tick（80 ms，约 0.4%） |
| 重启 | 关闭连接后杀掉服务端，立刻重启成功；`TIME-WAIT` 落在客户端端口（`127.0.0.1:52182 → 127.0.0.1:25037`） |
| 端口被占 | 第二个实例 `bind` 失败，stderr `bind: Address already in use`，退出码 2 |
| `./bin/epoll-server 70000` | 打印 `listen on port : 70000`，实际监听 4464..4563（`ss` 计到 100 个） |
| 客户端连接速率 | 1000 个连接 1.1 秒（约 900/s），`ulimit -n 4096` 下 |
| 客户端 fd 上限 | 默认上限（1024）在第 999 个连接后 `socket: Too many open files`；4096 时约 4000 个后同样失败，退出码仍是 0 |
| 客户端 vs 回声服务器 | 服务端为 2923 个连接各回 `pong`，客户端 `RecvBuffer` 打印 **0** 行 |
| 客户端 vs RST 服务器 | 第 189 个连接处 `send` 触发 SIGPIPE，退出状态 **141**（128+13） |
| 客户端 999 次 `EPOLLOUT` | 999 条 `data from N` 全部从同一个连接发出：服务端只有一个 fd 收到它们，且一次 `recv` 只读到 1024 字节（其中 77 条） |

11 项检查的内容：服务端缺参数退出码非 0（实测 255）、缺参数输出恰为
`Param Error`、重复调用一致、给端口后进入监听循环（`timeout 1` 的 124）、
启动输出 100 行、首尾端口为 `BASE` 与 `BASE+99`、端口被占时退出码 2 与 stderr 文案；
客户端缺参数退出码为 0（钉住缺陷）、用法行字面量、重复调用一致。

两个与网络无关的坑，写在 `tests/smoke.sh` 里也值得记一笔：

- **基端口不能从 0 开始。** `./bin/epoll-server 0` 的第一次 `bind` 拿到的是内核
  分配的临时端口，`i = 1` 就撞到 1 号端口，非 root 时立刻 `bind: Permission denied`
  退出 2。所以 smoke 取 20000 起、按 `$$` 打散的基端口，避开特权端口与临时端口段
  （32768 起）。
- **数启动输出必须借 pty。** stdout 重定向到文件时是全缓冲，进程被 `SIGTERM`
  杀掉会整块丢掉缓冲区，只能读到 0 行。`script -q -e -c` 给子进程一个 pty，
  stdout 变行缓冲，100 行才数得到；顺带 pty 会把行尾换成 CRLF，比较端口号时
  要先去 `\r`。

第 5 节那些行为数据用 `/tmp` 里的临时副本、Python 客户端/假服务器做的，仓库源码
没改。服务端输出一律先 `stdbuf -o0`。假服务器三种形态：`echo`（accept 后立即回
一行 `pong`）、`rst`（`SO_LINGER(1, 0)` 后 `close`，让对端拿到 RST）、`sink`
（只 accept 不写）。

## 性能

**空转成本。** `epoll_wait` 超时 5 毫秒、没有定时任务，实测 ≈190 次/秒唤醒；
20 秒空载消耗 8 个 CPU tick（80 ms），约 0.4%，折合每次循环 20 µs。改成 -1
（永久阻塞）在"没有定时任务"的前提下等价且更省。

**连接建立速率。** 客户端 1000 个连接 1.1 秒，约 900/s。这个数不是网络的上限，
是 `usleep(1 ms)` 加阻塞 `connect` 往返的上限：`usleep` 占 1 ms，往返占 0.1 ms。
按这个速率，340000 个连接至少要 340 秒，而且每个连接占一个 fd。

**吞吐。** 服务端单连接每次事件最多取 1024 字节，路径上有一次 `epoll_wait` 返回、
一次 `recv`、一次 `printf`；`printf` 走 stdio 的锁与格式化，比 `recv` 本身贵一个
数量级。所以这里能测到的"吞吐"主要是 `printf` 的速度，与网络栈无关——要量吞吐，
得先把读与应用层处理分开（第二阶段拆库之后才有可量的对象）。

## 实现记录

### 2026-11-26 入库

从课程示例的两个文件搬进仓库，只做让它们在零警告前提下构建所需的编译层改动，
运行行为与原始示例相同（逐条写在两个源文件的文件头）：

| 改动 | 原因 |
|---|---|
| 服务端补 `<unistd.h>`、`<sys/socket.h>` | `close` 原本无声明；socket 一族的声明原先靠间接包含链碰巧拿到。`-std=c11` 下隐式声明是 C11 6.5.2.2p6 的错误 |
| 服务端用宏 `EPOLL_SERVER_PTHREAD`（默认 0）同时管住 `client_routine` 的定义与 `main` 里的 pthread 分支 | 原文函数定义在 `#if 0` 块外、调用在块内 → 无调用点：不加 `static` 触发 `-Wmissing-prototypes`，加了 `static` 触发 `-Wunused-function`。与原文的 `#if 0` 等价，那段代码依旧不参与构建 |
| 服务端 `islistenfd` 加 `static` | 同上，非 static 且无原型声明触发 `-Wmissing-prototypes` |
| 服务端内层事件循环计数 `i` → `k` | 它与端口循环的 `i` 同名，构成变量遮蔽，触发 `-Wshadow`。只改名，作用域与取值不变 |
| 客户端补 `<unistd.h>`、`<sys/time.h>` | `close` / `usleep` / `gettimeofday` 原本无声明 |
| 客户端给 `-D_DEFAULT_SOURCE` | `usleep` 藏在 `__USE_MISC` 后；只给 `_POSIX_C_SOURCE` 实测仍是 1 条警告 |
| 显式转换：服务端 7 处、客户端 4 处（`htons((uint16_t)(...))`、`(socklen_t)sizeof(...)`、`(int)recv(...)`、`(size_t)BUFFER_LENGTH`、`(int)TIME_SUB_MS(...)`、`(useconds_t)(1 * 1000)`） | `-Wconversion` / `-Wsign-conversion`。都不改变结果（`recv` 的返回值域是 [-1, 1024]，`sizeof` 的结果域远小于 `socklen_t`） |

刻意**没有**动的：`main` 的 `return -1`（从 main 返回 -1 的退出状态是 255，POSIX
只取低 8 位，-1 与 255 是同一个状态，属风格而非缺陷）；客户端的 `exit(0)`
（缺陷，见第 1 条）；`strcpy` 那行死代码（见第 12 条）；`#if` 里那段 pthread
代码本身（不参与构建，问题记在第 17 条）。

根 `Makefile` 自动发现本模块（`make list` 里能看到 `epoll-server`），不需要改根
构建。根 `README` 的模块表新增一行，并把"前三个"改成四个都到齐。

## 已知局限

按文件分组：服务端 S1–S18，客户端 C1–C17。每条给标准依据；标"实测"的都在第 5 节
有数据。

### 服务端（`src/tcp_server.c`）

**丢数据 / 连接失控**

1. **`EPOLLET` 配"每次事件只 `recv` 一次"，超出的数据永久滞留。** 核心缺陷。
   `epoll(7)` 的 "Level-triggered and edge-triggered" 一节要求 ET 下循环读到
   `EAGAIN`；这里只读一次，缓冲区里剩下的字节要等下一次**状态变化**才有机会被读，
   对端不再发数据时就没有下一次。**实测**：16384 字节只读到 2048，剩 14336；
   65536 字节同样只读到 2048；13 KB 连续写入只读到 1024。这不是"慢"，是数据丢失
   ——TCP 已经确认过的字节被应用层丢在内核缓冲区里，直到连接关闭。
2. **没有按长度处理，也没有半包暂存。** `recv` 的返回值只用来判断 `>0 / =0 / <0`
   与打印，读到的字节被当成"一条消息"。TCP 提供的是字节流、不保留消息边界
   （RFC 9293 §3.7.4 的 "TCP Connection" 一节把字节流语义写明了），所以一次
   `recv` 可能只拿到半条应用消息，也可能拿到一条半。服务端既没有定长/分隔符
   切分，也没有把剩余字节留到下次拼接。
3. **`recv` 返回 -1 一律当断开。** 与 `tcp-server` 模块不同，这里的 clientfd
   **已经**是 `O_NONBLOCK`，所以 `EAGAIN` 在这条路径上可能出现：按
   `epoll(7)`，ET 事件不保证调用 `recv` 时仍有数据（数据可能已被别的读者取走），
   那时 `recv` 返回 -1 并置 `EAGAIN`，而代码会直接 `close`。本轮没有构造出可达
   路径（唯一读者、每事件只读一次），但第 1 条一改，`EAGAIN` 立刻成为循环的正常
   终止条件，这里必须同时改，否则"正确地读到 `EAGAIN`"会被"错误地关掉连接"抵消。
   同时 `EINTR`（被信号打断，POSIX `recv()` 语义要求重试）也会被当成断开。
4. **`close` 在 `epoll_ctl(DEL)` 之前。** `close` 已经把这个 fd 从所有 epoll 集合
   里移除（`epoll(7)` 的 "Q6: Will closing a file descriptor cause it to be removed
   from all epoll sets?"），所以 `DEL` 是多余动作；真正的危险在另一头：POSIX
   `close()` 之后该 fd 号立即可被复用，同一个 `for` 循环里紧接着的 `accept`
   很可能拿到同一个号——此时 `DEL` 删掉的是**新连接**的注册。
5. **`accept` 的返回值不检查。** 失败返回 -1 并置 `errno`（POSIX `accept()`），
   典型原因是 `EMFILE` / `ENFILE`（描述符耗尽）、`ECONNABORTED`（握手后被复位）、
   `EINTR`。代码把这个 -1 直接交给 `fcntl` 和 `epoll_ctl(ADD, -1)`，后者返回
   `EBADF`，而返回值又不检查（第 6 条）。结果是一次暂时性的 `ECONNABORTED` 之后
   连接被静默丢弃，对端等到超时。
6. **`fcntl`、三处 `epoll_ctl`、`setsockopt` 的返回值全不检查。** `ADD` 失败
   （`ENOMEM`、`ENOSPC`——内核 `max_user_watches` 到顶）时 fd 没进集合，此后永远
   收不到事件，连接"活着但不被服务"。`fcntl` 失败意味着 fd 仍是阻塞的，而 ET 要求
   非阻塞——两者叠加就是第 3 条的坑。

**把错误当正常**

7. **`SO_REUSEADDR` 设在 `accept` 之后的连接 fd 上，而不是 `bind` 之前的监听 fd 上。**
   `socket(7)` 的 SO_REUSEADDR 一节说明它影响的是 `bind`，对一个已经 `connect`/
   `accept` 完的 socket 没有意义。**实测**：客户端关闭连接后杀掉服务端、立刻重启是
   成功的，`TIME-WAIT` 落在客户端端口那一侧（`127.0.0.1:52182 → 127.0.0.1:25037`）
   ——因为 `TIME_WAIT` 由主动关闭方进入（RFC 9293 §3.5），而本模块只在 `recv`
   返回 0/-1 后 `close`，永远是被动关闭方。**所以这条现在只是隐患**：第二阶段一旦
   加上"服务端主动关闭空闲连接"，`TIME_WAIT` 就落到服务端，那时缺 `SO_REUSEADDR`
   会让重启直接 `bind` 失败。
8. **`islistenfd` 用 0 当"不是监听 fd"的哨兵，且每个事件扫 100 次。** fd 0 是
   合法描述符（stdin 关掉时 `accept` 可能返回 0），那时它会被判成监听 fd 并走进
   `accept(0, ...)`。用 `data.ptr` 指向一个带类型标记的结构体是常规做法，这里不用，
   于是每次事件多 100 次比较、且分类依据是一个可能合法也可能不合法的哨兵值。
9. **`epoll_wait` 超时 5 毫秒导致空转。** 没有定时任务却每秒醒 190 次（**实测**
   2 秒 386 次、5 秒 937 次）。改成 -1 就好；将来要做空闲连接回收，超时应该取
   "下一个连接到期还有多久"，而不是一个常数。
10. **`epoll_wait` 返回 -1 时无条件 `continue`，不看 `errno`。** `EINTR` 时
    `continue` 是对的；`EBADF`（epfd 被关）、`EINVAL`（`maxevents` 非法）这类错误
    会让循环变成纯忙等：立即返回 -1、立即再调，CPU 占满。
11. **从不读 `events[k].events`，因此 `EPOLLERR` / `EPOLLHUP` 没被显式处理。**
    `epoll(7)` 规定这两个事件总会被报告、不受 `events` 掩码影响，所以它们确实出现
    在 `events[k].events` 里——但代码只看 `data.fd`。现在的容错完全依赖"`recv`
    会返回 0 或 -1"这条间接路径，行为能对，依据是别的机制。
12. **`socket()` 的返回值不检查。** 失败返回 -1 并置 `errno`（POSIX
    `<sys/socket.h>`），代码拿 -1 去 `bind`，`bind` 返回 `EBADF`，`perror("bind")`
    打出 `bind: Bad file descriptor`，退出码 2。于是"描述符耗尽"被伪装成"绑定失败"。
13. **三个失败点各写各的退出码（2、3），没有统一的错误出口。** 第二阶段拆库时
    需要收敛成一套错误码，否则调用方只能看 stderr 文本。

**容量与协议覆盖面**

14. **`listen(sockfd, 5)` 的 backlog 太小，而且 100 个端口都是 5。** `listen(2)`
    说明 backlog 上限受 `net.core.somaxconn` 约束；取 5 时，握手完成但尚未
    `accept` 的连接超过 5 个就会被丢弃或复位（取决于 `tcp_abort_on_overflow`），
    对端只能靠 SYN 重传。与目录名"百万并发的服务器"直接矛盾。
15. **端口参数不校验，`htons` 静默截断。** `atoi` 可以返回 0、负数、70000；
    `htons` 的原型是 `uint16_t htons(uint16_t)`（POSIX `<arpa/inet.h>`），
    `(uint16_t)70000` 是 4464。**实测**：`./bin/epoll-server 70000` 打印
    `tcp server listen on port : 70000`（打印用的是未经截断的 `int`），实际监听
    4464..4563；`-1` 打印 -1，实际先监听 65535、随后撞上 0 号端口。
16. **100 个端口必须同时可用，任意一个被占就整体退出 2。** **实测**：先起一个实例
    占住基端口，第二个实例打印 `bind: Address already in use` 并退出 2。没有
    "跳过这个端口"或"换一段端口"的退路，一台机器上与该范围重叠的服务会让整个
    程序起不来。
17. **没有连接上限，也没有 `EMFILE` 的处理。** `EPOLL_SIZE`（1024）只是
    `epoll_wait` 单次返回的事件数上限，不是连接数上限——两个数在代码里长得一样，
    很容易被读成"最多 1024 个连接"。真实上限是 `RLIMIT_NOFILE`，到顶之后
    `accept` 返回 `EMFILE`，落进第 5 条。
18. **`epoll_create(1)` 的参数与 `EPOLL_SIZE` 不一致。** 该参数自 Linux 2.6.8 起
    被忽略，但必须大于 0（`epoll_create(2)`）；写 1 而事件数组是 1024 条，容易
    被读成"这个实例只支持 1 个 fd"。另外 `printf("%s", buffer)` 依赖 NUL 结尾，
    而 `recv` 不追加终止符：`buffer` 1024 字节被填满时 `%s` 会越过数组末尾读到
    0 字节为止，C11 7.21.6.1p8 要求 `%s` 的参数指向以空字符结尾的字符数组，越界
    读是未定义行为。**ASan 拦不住这条**（glibc 的 `vfprintf` 用内部扫描而不是
    libc 的 `strlen`），本轮以整 1024 字节输入跑 ASan 构建没有报告，只能给标准
    依据、不声称已复现。

### 客户端（`src/mul_port_client_epoll.c`）

**错误路径与退出状态**

1. **参数错误时 `exit(0)`。** 用法错误却返回 0（成功），调用方无法把"参数写错了"
   和"跑完了"区分开。**实测** rc=0；`tests/smoke.sh` 第 9 项钉的就是这个当前
   行为，标题里标明是缺陷，第二阶段改成非 0 时这条检查要跟着改。
2. **连接失败的错误路径也 `return 0`。** `goto err` 打印 `error : <strerror>` 之后
   返回 0。**实测**：默认 fd 上限下第 999 个连接后 `socket: Too many open files`，
   进程以 0 退出。
3. **`isContinue` 只被写成 0，从没写成 1。** 收到 `quit` 时置 0，而这个变量本来就是
   0，等于什么都没做；`!isContinue` 恒为真。加上 `connections < 340000`，程序没有
   正常退出路径——只能被信号杀死或走 `goto err`。
4. **`send` 的返回值不检查，也没有 `MSG_NOSIGNAL`。** 对端复位后写入会得到
   `EPIPE` 并同时收到 `SIGPIPE`（`send(2)` 的 EPIPE 一节；`signal(7)` 的默认动作
   是终止进程）。**实测**：RST 服务器下第 189 个连接处进程被 SIGPIPE 杀死，退出
   状态 141（128+13）。短写（`send` 只发出一部分）同样被静默忽略。

**读写落错对象**

5. **`EPOLLOUT` 分支 `send(sockfd, ...)`、`EPOLLIN` 分支 `recv(sockfd, ...)`
   用的都不是 `events[i].data.fd`。** `sockfd` 是**本轮循环**里新建的那个 fd（也是
   上一次 `epoll_wait` 之前最后建的那个），对所有事件都一样；`clientfd` 虽然被正确
   取出，却只用在打印和 `close` 上。**实测**：一次 `epoll_wait` 拿到 999 个
   `EPOLLOUT` 事件，999 条 `data from N` 全部从同一个连接发出——服务端只有一个 fd
   收到它们，且因为服务端第 1 条，一次 `recv` 只读到 1024 字节（其中含 77 条），
   其余约 12 KB 滞留。
6. **先判 `EPOLLOUT`、`else if` 才判 `EPOLLIN`，而客户端 socket 一直可写。**
   水平触发下可写就是每次 `epoll_wait` 都报，所以 `EPOLLIN` 分支几乎不可能执行。
   **实测**：回声服务器为 2923 个连接各回一行 `pong`，客户端 `RecvBuffer` 打印
   **0** 行。两个分支都该按 `events[i].events` 逐位判断，而不是 `if / else if`。
7. **每个连接都注册 `EPOLLIN | EPOLLOUT`，且是水平触发。** 客户端 socket 的
   发送缓冲区几乎总是空的，于是每个 fd 每次 `epoll_wait` 都报 `EPOLLOUT`，
   事件列表被无意义的事件填满，真正可读的 fd 反而排不上。
8. **`epoll_wait` 的 `maxevents` 传的是 `connections`。** `connections == 0` 时是
   `EINVAL`（`epoll_wait(2)` 要求 `maxevents > 0`）；返回值也不检查，`-1` 时
   `for (i = 0; i < nfds; i++)` 直接不执行，错误被吞掉。

**容量与速率**

9. **`connect` 是阻塞的，且每轮 `usleep(1 ms)`。** 连接速率因此被钉在 ~900/s
   （**实测** 1000 个连接 1.1 秒），与 340000 的目标组合意味着至少 340 秒；
   非阻塞 `connect` + `EPOLLOUT` 是常规做法，但那样就得先修第 5、6 条。
10. **340000 这个目标数与 fd 上限无关。** 每个连接占一个 fd，默认
    `RLIMIT_NOFILE` 是 1024：**实测**默认上限下第 999 个连接后 `EMFILE`；
    `ulimit -n 4096` 下约 4000 个后同样失败。要跑到 340000 得先把
    `RLIMIT_NOFILE` 与 `net.ipv4.ip_local_port_range`、`fs.file-max` 一起调，
    代码里没有任何提示。
11. **端口号不校验，`htons` 静默截断。** 与服务端第 15 条同源：`port + index`
    超过 65535 会绕回，`atoi` 的 0 与负数也不拦。
12. **`struct epoll_event events[384 * 1024]` 在栈上。** 393216 × 12 =
    4 718 592 字节（4.5 MiB），占默认 8 MiB 栈的 56%；容器或线程栈更小时会直接
    `SIGSEGV`。这个数组只在"等事件"时用，没有任何理由不放到静态区或堆上。

**无意义的动作与遗留**

13. **`ntySetReUseAddr(sockfd)` 在 `connect` 之后调用，作用在已连接的客户端 socket 上。**
    `SO_REUSEADDR` 影响的是 `bind`（`socket(7)`），对客户端连接没有任何效果；
    源码里被注释掉的那次调用（connect 之前）也一样无意义，但至少顺序是对的。
14. **`strcpy(buffer, " Data From MulClient\n")` 紧接着被 `sprintf` 覆盖。** 死代码。
15. **`sprintf` 无长度上限。** 当前格式串最长 `"Hello Server: client --> 339999\n"`
    31 字节 + NUL，128 字节够用，所以现在不会溢出；但 C11 7.21.6.1 的 `sprintf`
    语义里没有任何上限，缓冲区不足即未定义行为。同样危险的是第 6 条里的
    `sprintf(buffer, "data from %d\n", clientfd)`——这条路径上缓冲区是被重用的。
16. **`TIME_SUB_MS` 用 `gettimeofday` 计时，且宏参数没有括号包裹。** `gettimeofday`
    读的是 wall clock，系统时间被 NTP 调整或手工 `date` 时会跳变，算出的间隔可能
    为负；POSIX 建议时间间隔用 `CLOCK_MONOTONIC`。宏体里 `tv1.tv_sec - tv2.tv_sec`
    没有把参数括起来，`TIME_SUB_MS(a+1, b)` 会被展开成错误的表达式（当前两个调用点
    都是简单标识符，所以没暴露）。
17. **`inet_addr(ip)` 不检查返回值。** 非法点分地址返回 `INADDR_NONE`
    （`0xFFFFFFFF`），与 `255.255.255.255` 无法区分（`inet_addr(3)` 的 BUGS 一节
    明确建议改用 `inet_aton`），随后会去连广播地址；而且整个过程没有 `getaddrinfo`，
    主机名根本不能用。

另外：服务端 `#if EPOLL_SERVER_PTHREAD` 那段代码（默认不编译，等价于原文的
`#if 0`）如果打开，有四个独立问题——引用的 `sockfd` 是端口循环里的变量，已出作用域
（编译不过）；`pthread_create(..., &clientfd)` 传的是循环内局部变量的地址，新线程
与下一轮 `accept` 对同一个对象无同步读写，按 C11 5.1.2.4 是数据竞争、未定义行为；
`client_routine` 返回 `void *` 却走到函数末尾没有 `return`（C11 6.9.1p12：调用方
使用返回值时行为未定义）；线程从不 `pthread_join`（POSIX `pthread_join()`），资源
不回收。它不参与构建，所以没编号。

## 待办

第二阶段（2026-12-20 ~ 2026-12-26）按这个顺序推进，一天一条：

- [ ] 拆成 `include/epollserver.h` + `libepollserver.a` + 两个薄驱动（第一件）。
      第一刀切在 `server_setup_ports()`（建 100 个监听 fd 并注册）、
      `server_accept_one()`、`server_read_once()`、`client_connect_one()` 上
- [ ] 补可离线判定的用例：固定字节 → 断言读到的长度序列；回环假服务器 →
      断言 `client_connect_one` 发出的字节与注册的 `events` 掩码
- [ ] 修服务端 S1、S2、S3（一组）：clientfd 已是非阻塞，改成循环 `recv` 到 `EAGAIN`，
      区分 `EAGAIN` / `EINTR` / 真错误，为半包留状态。验证方式：一次写 65536 字节，
      断言全部读出
- [ ] 修服务端 S4、S5、S6：`DEL` 在 `close` 之前、`accept` 检查 `EMFILE` 并退避、
      `epoll_ctl` / `fcntl` 全部检查
- [ ] 修服务端 S8：用 `data.ptr` 带类型标记替代 `islistenfd` 的 100 次扫描
- [ ] 修服务端 S10、S11：`epoll_wait` 按 `errno` 分支；显式处理 `EPOLLERR` / `EPOLLHUP`
- [ ] 修服务端 S9：超时改 -1，或绑到"下一个空闲连接到期时间"
- [ ] 修服务端 S7、S12、S13、S15、S16：`SO_REUSEADDR` 挪到 `bind` 之前、`socket`
      检查、错误码收敛、端口范围校验、`%s` 改成按长度输出、`epoll_create` 参数
- [ ] 修客户端 C5、C6、7、8：`send` / `recv` 改用 `events[i].data.fd`，按
      `events` 逐位判断，只注册需要的事件，`maxevents` 与 `connections` 解耦
- [ ] 修客户端 C4：`send` 加 `MSG_NOSIGNAL` 或忽略 `SIGPIPE`
- [ ] 修客户端 C1、2、3：用法错误与错误路径返回非 0，`isContinue` 收敛成一个
      真正能停下的状态机
- [ ] 修客户端 C9、10、11、12：非阻塞 `connect`、fd 上限检测、端口校验、
      事件数组移出栈
- [ ] 补 `check`（真实回环端到端：服务端 100 端口 + 客户端 N 连接 + 断言收到
      N 行问候）、`bench`（连接建立速率、单连接吞吐）、`tsan`（启用 pthread 分支之后）
