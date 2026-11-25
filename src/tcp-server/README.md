# tcp-server —— epoll 边缘触发的 TCP 服务端

用 `epoll` 单线程接连接、读数据、打屏。没有线程、没有连接池、没有协议解析：
`accept` 之后把新 fd 以 `EPOLLET` 注册进 epoll 集合，来事件就 `recv` 一次，
把收到的字节用 `printf("%s")` 打到标准输出。代码来自
`D:\8. TCP服务器\tcp_server.c`（2019 年的课程示例），本轮只做到
"入库 + 可构建 + 可运行"，**逻辑与原始示例一致，缺陷一个没修**。

当前状态一句话：**能接连接、能读到数据、能打印，但一次事件只取 1024 字节，
超出的部分永久留在内核缓冲区**——一次写入 16384 字节实测只读到 2048，
剩余 14336 字节再也不会被读走（第 5 节有实测记录）。已知缺陷 18 条（「已知局限」）。
第二阶段按「待办」的顺序逐日推进，时间窗是 2026-12-13 ~ 2026-12-19。

## 目录结构

```
src/tcp-server/
├── Makefile                 all / test / sanitize / clean
├── README.md                本文件
├── src/tcp_server.c         全部实现 + main（单文件程序）
└── tests/smoke.sh           离线可判定的 5 项检查
```

少了 `include/`、`lib`、`cli`、`check`、`bench`、`tsan`，原因写在「对外接口」一节。

## 对外接口

**没有。** 这是一个单文件程序：`accept`、`epoll_ctl(ADD)`、`recv`、`close` 全部
写死在 `main` 的 `for` 循环里，外部只能通过 `main` 使用它，而 `main` 唯一的
交互方式是"启动后监听某个端口、把收到的字节打到标准输出"。

这不是设计选择，是记账。第二阶段的第一个任务就是把它拆成
`include/tcpserver.h` + `libtcpserver.a` + 薄驱动，理由是**测不了**：
"新连接该不该注册"取决于 `epoll_ctl` 的返回值和 `ev` 的构造，
"这次读该不该读第二次"取决于 `recv` 的返回值和 fd 的阻塞模式，
这两件事现在都在同一个 `for` 循环体内，中间没有任何可单独调用的函数。
「已知局限」里至少 7 条（第 2、3、6、7、8、9、10 条）在拆分之前只能靠
起真实对端碰运气复现——第 5 节的实测是验证手段，不是用例。

## 设计要点

**epoll 的两种触发模式。** 先给定义，因为整个模块的正确性都压在这一点上。

`epoll` 是 Linux 提供的 I/O 多路复用接口：把一个或多个文件描述符注册到一个
内核对象（epoll 实例）里，然后调用 `epoll_wait` 阻塞等待，直到其中至少一个
"就绪"。**就绪（readiness）** 指 fd 上的某个操作现在可以不阻塞地完成——
对读来说就是接收缓冲区非空，对写来说就是发送缓冲区有空间。

触发模式决定"同一个就绪状态要报告几次"：

- **水平触发（level-triggered，LT）**：默认模式。只要条件成立（接收缓冲区非空），
  每次 `epoll_wait` 都报告这个 fd。没读完不要紧，下次还会报。
- **边缘触发（edge-triggered，ET）**：只在条件从**不成立变为成立**的那一刻报告一次。
  上报之后，即使缓冲区里还有数据，只要没有新的数据到达，就不会再报。

于是 ET 的使用约束是硬性的：**收到事件后必须循环 `recv` 直到返回 `EAGAIN`**，
把这一次"变为可读"所对应的数据全部取走。`epoll(7)` 的
"Level-triggered and edge-triggered" 一节把这个约束写在明面上，并且要求
配合非阻塞 fd——否则循环会在最后那次读上永久阻塞。

这段代码两条都没做：`EPOLLET` 注册了，但每次事件只 `recv` 一次
（`src/tcp_server.c` 的 else 分支），clientfd 也从未切成 `O_NONBLOCK`
（文件包了 `<fcntl.h>`，一次都没用）。所以它的行为是"每次状态变化最多取 1024 字节"。

这不是理论推断。一次写入 16384 字节（对端之后不再发任何字节），服务端只输出
两行、每行 1024 字节，剩余 14336 字节永久滞留：

```
$ ./bin/tcp-server 19004 &          # 服务端
$ python3 -c '...sendall(b"X"*16384); sleep(3); close()'
=== send 16384 字节，服务端收到行数：2 ===
收到 1024 字节
收到 1024 字节
```

为什么是 2 行而不是 1 行：内核把这 16384 字节按 MSS 分成了两个报文段，
每段到达都构成一次"空 → 非空"的变化（上一段已经被读走一部分但没读完，
内核仍会为新到达的段再触发一次回调）。这恰好说明 ET 的语义是
"按状态变化计数"，不是"按数据量计数"——第一次变化给 1024，第二次变化再给 1024，
之后没有第三次变化，剩下的 14336 永远读不到。换成 65536 字节，结果同样是 2 行、
共 2048 字节。

**5 毫秒超时。** `epoll_wait(epfd, events, EPOLL_SIZE, 5)` 的第四个参数是超时
（毫秒），5 表示"最多等 5 毫秒就返回，即使没有事件"。这个程序没有任何定时任务，
所以超时的唯一作用是让主循环每 5 毫秒空转一圈。实测 2 秒内 `epoll_wait` 被调用
391 次（约 195 次/秒），与 1000/5 吻合（第 5 节）。

**`listen` 的 backlog 是 5。** `listen(fd, backlog)` 的第二个参数限制的是
"已完成三次握手、等待 `accept` 取走"的连接队列长度（Linux 上取
`min(backlog, net.core.somaxconn)`，`listen(2)` 的 NOTES 一节说明了这一点）。
backlog 取 5 意味着同时有 6 个连接在握手完成后来不及被 `accept`，第 7 个的 SYN
就可能被丢弃、由对端重传。这个数值与"百万并发"的标题是矛盾的。

**单线程、无连接状态。** 每个连接除了 fd 号之外没有任何状态：不记录对端地址、
不记录已读字节数、不记录最后一次活动时间。所以既做不了空闲回收，也没法在
一次读不完时暂存半条消息。这个"无状态"是后面所有容量类缺陷的根源。

## 构建

```bash
cd src/tcp-server
make              # bin/tcp-server
make test         # tests/smoke.sh（5 项，离线）
make sanitize     # ASan + UBSan 重建并跑同一套检查，产物在 bin/sanitize/
make clean
```

用法：

```bash
./bin/tcp-server 9000
```

唯一的参数是监听端口，绑 `INADDR_ANY`。缺参数时打印 `Param Error` 并
`return -1`（退出状态 255）。`bind` 失败退出 2，`listen` 失败退出 3。

**这里没有 feature test macro，是实测的结果而不是照搬。** gcc 11.4.0 /
glibc 2.35 上三种组合都是零警告：不给任何宏、只给 `-D_POSIX_C_SOURCE=200809L`、
给 `-D_DEFAULT_SOURCE`。原因是本文件用到的东西——`socket` / `bind` / `listen` /
`accept` / `recv`（`<sys/socket.h>`）、`close`（`<unistd.h>`）、`epoll_*`
（`<sys/epoll.h>`）、`htons`（`<arpa/inet.h>`）——在 glibc 的头文件里都是
无条件声明，不受 `__USE_MISC` 控制。dns-client 必须给 `-D_DEFAULT_SOURCE`，
是因为它用的 `random` / `srandom` / `bzero` 确实藏在 `__USE_MISC` 之后；
**那条经验不能整体照搬**，这次是编译一遍确认的。

必须补的头文件有两个，都是编译错误而不是警告：`<unistd.h>`（`close` 原本
完全没有声明，`-std=c11` 下是隐式函数声明，C11 6.5.2.2p6 起不允许）和
`<sys/socket.h>`（原本只是靠 `<arpa/inet.h>` → `<netinet/in.h>` →
`<sys/socket.h>` 这条间接包含链碰巧拿到声明）。

## 测试与验证

环境：内网 Ubuntu，Linux 6.8.0-149 / gcc 11.4.0。2026-11-25 测。

| 项目 | 结果 |
|---|---|
| 默认构建 | 0 条警告（`-std=c11 -Wall -Wextra -Wconversion -O2 -g`） |
| 加严构建 | 0 条警告（再叠加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wsign-conversion`，合计十二个 `-W`） |
| `make test` | **5 项**，0 失败 |
| `make sanitize` | 0 条警告，5 项全过，无 ASan/UBSan 报告 |
| 一次写 16384 字节 | 收到 2 行 × 1024 字节，剩 14336 永久滞留 |
| 一次写 65536 字节 | 收到 2 行 × 1024 字节，剩 63488 永久滞留 |
| 空转计数 | 2 秒内 `epoll_wait` 391 次（≈195/秒） |
| 端口被占 | 第二个实例 `bind` 失败，stderr `bind: Address already in use`，退出码 2 |

5 项检查钉的是与网络无关的事实：二进制能起、缺参数退出码非 0（实测 255）、
缺参数输出恰为 `Param Error`、重复调用退出码一致、给了端口后进入监听循环而不是
自己退出（用端口 0 让内核分配，因此不会有"端口被占"的偶发失败；`timeout 1` 的
退出码 124 表示到点被杀，也就是进程一直活着）。**这 5 项不验证 accept、不验证
读写、不验证连接关闭**，理由见 `tests/smoke.sh` 文件头。

第 2 节那些实测数据都是用 `/tmp` 里的临时副本和 Python 客户端做的，仓库源码没改。
客户端只做三件事：`connect`、`sendall` 固定长度的同一字节、`sleep` 后 `close`。
服务端输出要先 `stdbuf -o0`——重定向到文件时 `stdout` 是全缓冲，被 `SIGTERM`
杀掉时缓冲区里的内容会丢掉，这一步不做会看到"服务端什么都没收到"的假象。

## 性能

能直接量出来的只有空转成本：`epoll_wait` 超时 5 毫秒，无事件时每秒唤醒 195 次
（实测 391 次 / 2 秒）。每次唤醒的代价是两次系统调用（`epoll_wait` 返回 0、
`for` 循环不进入、再调一次），空载时 CPU 占用不是 0，而是一个可以算出来的常数。
把超时改成 -1（永久阻塞）在"没有定时任务"的前提下是等价且更省的正确做法。

吞吐方面没有可量性可言：单个连接每次事件最多取 1024 字节，而每个事件至少要
一次 `epoll_wait` 返回、一次 `recv`、一次 `printf`。`printf` 走 stdio 的锁与
格式化，比 `recv` 本身贵一个数量级。所以这里测出来的"吞吐"主要是
`printf` 的速度，与网络栈无关——真正要量吞吐，得先把读与应用层处理分开
（第二阶段拆库之后才有可量的对象）。

## 实现记录

### 2026-11-25 入库

从课程示例搬进仓库，只做让它在零警告前提下构建所需的编译层改动，
运行行为与原始示例相同（逐条写在 `src/tcp_server.c` 的文件头）：

| 改动 | 原因 |
|---|---|
| 补 `<unistd.h>` 与 `<sys/socket.h>` | `close` 原本无声明，`-std=c11` 下隐式声明是错误（C11 6.5.2.2p6）；`socket` 等的声明原先是靠间接包含链碰巧拿到的 |
| 用宏 `TCP_SERVER_PTHREAD`（默认 0）同时管住 `client_routine` 的定义与 `main` 里的 pthread 分支 | 原文把定义留在 `#if 0` 块外、调用关在块内，函数没有调用点：不加 `static` 触发 `-Wmissing-prototypes`，加了 `static` 触发 `-Wunused-function`，两条路都过不去。与原文的 `#if 0` 等价，那段代码依旧不参与构建 |
| 显式转换五处：`htons((uint16_t)port)`、两处 `(socklen_t)sizeof(...)`、两处 `(int)recv(..., (size_t)BUFFER_LENGTH, 0)` | `-Wconversion` / `-Wsign-conversion`。转换不改变任何一处的结果（`recv` 的返回值域是 [-1, 1024]，落在 `int` 里） |

刻意**没有**动的：

- `main` 的 `return -1`。从 `main` 返回 -1 的退出状态是 255——POSIX 只取低 8 位，
  所以 -1 与 255 是同一个状态。这是风格问题，不是缺陷。
- `#if 0` 里那段 pthread 代码本身。它不参与构建，改它没有意义，但它的问题记在
  「已知局限」最后三条，因为第二阶段一旦启用就是真缺陷。
- `errno.h` / `fcntl.h` 两个用不到的包含。留着是因为它们标记了作者原本的意图
  （`fcntl` 本该用来设 `O_NONBLOCK`，见「已知局限」第 7 条）。

根 `Makefile` 自动发现本模块（`make list` 里能看到 `tcp-server`），不需要改根构建。
根 `README` 的模块表新增一行。

## 已知局限

按"触发了会怎样"分组：第一组丢数据或让连接失控，第二组把错误状态当成正常状态，
第三组是容量与协议覆盖面的缺口，第四组在不参与构建的代码里。每条都给标准依据。

**丢数据 / 连接失控**

1. **`EPOLLET` 配"每次事件只 `recv` 一次"，超出的数据永久滞留。** 这是本模块的
   核心缺陷。`epoll(7)` 的 "Level-triggered and edge-triggered" 一节要求 ET 模式下
   循环读直到 `EAGAIN`；代码只读一次，缓冲区里剩下的字节要等**下一次状态变化**
   才有机会被读，而对端不再发数据时就没有下一次。**已实测**：16384 字节只读到
   2048，剩 14336（第 5 节）。这不是"慢"，是数据丢失——TCP 已经确认过的字节
   被应用层丢在内核缓冲区里，直到连接关闭。
2. **clientfd 没有切成 `O_NONBLOCK`。** ET 的正确写法必须是非阻塞 fd，否则"循环读到
   `EAGAIN`"这一条无法实现（最后那次读会永久阻塞）。文件包了 `<fcntl.h>` 却一次没用，
   说明作者知道该做但没做。当前代码因为只读一次而不会阻塞，代价就是第 1 条。
3. **`accept` 的返回值不检查。** `accept` 失败返回 -1 并置 `errno`
   （POSIX `accept()`），典型原因是 `EMFILE` / `ENFILE`（描述符耗尽）、
   `ECONNABORTED`（握手后被对端复位）、`EINTR`（被信号打断）。代码把这个 -1
   直接交给 `epoll_ctl(ADD, -1)`，后者返回 `EBADF`，而返回值又不检查（第 4 条）。
   结果是：一次暂时性的 `ECONNABORTED` 之后，这个连接被静默丢弃，对端等到超时。
4. **三处 `epoll_ctl` 的返回值全不检查。** `ADD` 失败（`ENOMEM`、`ENOSPC`——内核
   `max_user_watches` 到顶）时 fd 没有进集合，此后永远收不到事件，连接"活着但
   不被服务"，对端看到的是连接建立成功然后永远没有响应。`DEL` 失败同理。
5. **`close` 在 `epoll_ctl(DEL)` 之前。** `close` 会把 fd 从所有 epoll 集合里移除
   （`epoll(7)` 的 "Q6: Will closing a file descriptor cause it to be removed from all
   epoll sets?" 一节：只要没有别的 fd 指向同一个打开文件描述，就移除）。所以
   `DEL` 是多余动作。更实际的危险在另一头：POSIX `close()` 之后该 fd 号立即可被
   复用，`accept` 拿到的下一个连接很可能就是同一个号——此时 `DEL` 删掉的是
   **新连接**的注册。正确顺序是 `DEL` 然后 `close`，或者只 `close`。
6. **`recv` 返回 -1 时一律当断开。** `len < 0` 与 `len == 0` 走同一个分支，但这两件事
   含义完全不同：`0` 是对端发了 FIN（POSIX `recv()` 的返回值语义），`-1` 是出错，
   其中 `EAGAIN` / `EWOULDBLOCK` 表示"现在没数据，不是错误"，`EINTR` 表示"被信号
   打断，重试即可"。当前代码把 `EAGAIN` 也当断开、直接 `close`。现在因为 fd 是
   阻塞的所以遇不到 `EAGAIN`，但第 1、2 条一改，这里必须同时改，否则"正确地读到
   `EAGAIN`"会被"错误地关掉连接"抵消掉。
7. **`printf("%s", buffer)` 依赖 `buffer` 以 NUL 结尾，而 `recv` 不追加终止符。**
   `buffer` 是 1024 字节，`recv` 最多写满 1024 字节，一个终止符都不留。填满时
   `%s` 的转换会越过数组末尾继续读，直到碰到某个 0 字节——C11 7.21.6.1p8 要求
   `%s` 对应的参数是指向"以空字符结尾的字符数组"的指针，越界读是未定义行为。
   本次入库实测里没有出问题：以正好 1024 字节输入跑 ASan 构建，没有报告，输出
   每行恰好 1044 字符（`Recv: ` 6 + 1024 + `, 1024 byte(s)` 14），说明这次栈布局里
   `buffer[1024]` 恰好是 0。**这是侥幸，不是安全**——换编译器、换优化等级、
   在 `buffer` 前多一个局部变量都会改变结果。另外，ASan 拦不住这条：
   glibc 的 `vfprintf` 用自己内部的字符扫描而不是 libc 的 `strlen`，不经过 ASan 的
   拦截点，所以"ASan 没报"不能当作证据。要检测得靠 Valgrind memcheck 或人工审查。

**把错误当正常**

8. **`epoll_wait` 返回 -1 时无条件 `continue`，不看 `errno`。** 被信号打断
   （`EINTR`）时 `continue` 是对的；但 `EBADF`（epfd 被关）、`EINVAL`（EPOLL_SIZE
   非法）这类错误会让循环变成纯空转的忙等——`epoll_wait` 立即返回 -1，`continue`
   立即再调，CPU 占满。
9. **没有注册 `EPOLLERR` / `EPOLLHUP`，也没有主动处理它们。** `epoll(7)` 规定这两个
   事件总是会被报告，不受 `events` 掩码影响，所以它们在 `events[i].events` 里会
   出现——但代码从不读 `events[i].events`，只看 `data.fd`。现在的容错完全依赖
   "`recv` 会返回 0 或 -1"这条间接路径，行为能对，但依据是别的机制。
10. **`socket()` 的返回值不检查。** 失败返回 -1（POSIX `<sys/socket.h>`，并置
    `errno`），代码拿 -1 去 `bind`，`bind` 返回 `EBADF`，`perror("bind")` 打出
    `bind: Bad file descriptor`，退出码 2。于是"描述符耗尽"被伪装成"绑定失败"，
    而退出码 2 在调用方看来是端口问题。
11. **`listen` 失败与 `bind` 失败用不同的退出码（3 与 2），但没有文档也没有统一的
    错误出口。** 三个失败点各写各的，第二阶段拆库时需要收敛成一套错误码，
    否则调用方仍然只能看 stderr 的文本。

**容量与协议覆盖面**

12. **`listen(sockfd, 5)` 的 backlog 太小。** `listen(2)` 说明 backlog 上限受
    `net.core.somaxconn` 约束；取 5 时，握手完成但尚未 `accept` 的连接超过 5 个，
    Linux 会按 `tcp_abort_on_overflow` 的取值丢弃或复位新连接，对端只能靠 SYN 重传
    重来。这个值在单机回环上几乎碰不到，在真实负载下是第一批暴露的问题。
13. **`epoll_wait` 超时 5 毫秒导致空转。** 没有定时任务却每秒醒 195 次（实测）。
    改成 -1 就好；如果将来要做空闲连接回收，超时才应该取"下一个连接到期还有多久"，
    而不是一个常数。
14. **端口号不校验，`htons` 的参数被静默截断。** `atoi` 可以返回 0、负数、70000；
    `htons` 的原型是 `uint16_t htons(uint16_t)`（POSIX `<arpa/inet.h>`），
    `(uint16_t)70000` 是 4464，`(uint16_t)-1` 是 65535。于是 `./bin/tcp-server 70000`
    监听在 4464 上，`./bin/tcp-server -1` 监听在 65535 上，两者都不报错。
15. **没有 `SO_REUSEADDR`，但当前代码路径下它还没有造成重启失败。** 这一条要
    说准确：`SO_REUSEADDR`（`socket(7)` 的 SO_REUSEADDR 一节）的作用是允许
    `bind` 到存在 `TIME_WAIT` 连接的本地端口。`TIME_WAIT` 由**主动关闭方**进入
    （RFC 9293 §3.5，Closing a Connection）。而本模块只在 `recv` 返回 0 或 -1 后
    `close`——那时对端已经发过 FIN，所以服务端永远是**被动关闭方**，不进 `TIME_WAIT`。
    实测确认：客户端 `shutdown(SHUT_WR)` 之后 `ss -tan` 里的 `TIME-WAIT` 出现在
    客户端临时端口那一侧，杀掉服务端后立刻重启是成功的。**所以这条现在只是隐患**：
    第二阶段一旦加上"空闲连接由服务端主动关闭"，`TIME_WAIT` 就落到服务端，
    那时缺 `SO_REUSEADDR` 会让重启直接 `bind` 失败。
16. **没有连接上限，也没有 `EMFILE` 的处理。** `EPOLL_SIZE`（1024）只是
    `epoll_wait` 单次返回的事件数上限，不是连接数上限——这两个数在代码里长得一样，
    很容易被读成"最多 1024 个连接"。真实的连接上限是 `RLIMIT_NOFILE`，到顶之后
    `accept` 返回 `EMFILE`，然后落进第 3 条。
17. **每个连接没有任何状态，所以做不了空闲回收、半包暂存、按连接限速。** 第 1 条
    的修复会立刻要求"半包暂存"（一次读到 `EAGAIN` 可能跨多个 TCP 段），
    所以这条与第 1 条是同一个改动。
18. **单线程、每条事件最多 1024 字节、`printf` 在关键路径上。** 与目录名
    "百万并发的服务器"不符：这是"单线程 + 每事件一次 1 KiB 读 + 同步格式化输出"。
    要谈并发，得先把读与应用层处理分开、把输出改成有界缓冲。

**在不参与构建的代码里（`#if TCP_SERVER_PTHREAD`）**

19. **`pthread_create(..., &clientfd)` 传的是循环内局部变量的地址。** `clientfd`
    每轮 `accept` 都被重新赋值，新线程读到的可能是下一轮的 fd（甚至是被复用的同一个
    栈槽），这是两个线程对同一对象的无同步读写，按 C11 5.1.2.4 的定义是数据竞争，
    竞争导致未定义行为。正确做法是把 fd 值本身传进去（`(void *)(intptr_t)fd`）
    或者在堆上分配。
20. **`thread_id` 从不 `pthread_join`。** 线程结束后资源不回收（POSIX
    `pthread_join()`），长时间运行会耗尽线程资源；主循环也没有退出路径。
21. **`client_routine` 没有 `return`，而它返回 `void *`。** C11 6.9.1p12：到达函数
    末尾的 `}` 而调用方使用了返回值，行为未定义。`pthread_join` 会把这个不确定值
    交给回收者。

## 待办

第二阶段（2026-12-13 ~ 2026-12-19）按这个顺序推进，一天一条：

- [ ] 拆成 `include/tcpserver.h` + `libtcpserver.a` + 薄驱动（第一件）
- [ ] 把"接一个新连接"与"读一次/读到底"各暴露成一个函数，补可离线判定的用例：
      回环起真实对端 → 断言注册的 `events` 掩码；固定字节 → 断言读到的长度序列
- [ ] 修第 1、2、6、17 条（一组）：clientfd 设 `O_NONBLOCK`，循环 `recv` 到
      `EAGAIN`，区分 `EAGAIN` / `EINTR` / 真错误，为半包留状态。验证方式：
      一次写 65536 字节，断言全部读出
- [ ] 修第 3、4、5 条：`accept` 检查 `EMFILE` 并有退避、`epoll_ctl` 全部检查、
      调换 `DEL` 与 `close` 的顺序
- [ ] 修第 7 条：`recv` 之后按返回长度补 NUL，或者用 `%.*s` / `fwrite` 按长度输出
- [ ] 修第 8、9 条：`epoll_wait` 按 `errno` 分支；显式处理 `EPOLLERR` / `EPOLLHUP`
- [ ] 修第 10、11、14 条：`socket` 检查、错误码收敛、端口范围校验（1..65535）
- [ ] 修第 13 条：超时改 -1，或将超时与"下一个空闲连接到期时间"绑定
- [ ] 加 `SO_REUSEADDR`，并加上空闲连接回收（两条一起做，第 15 条才会被真正触发）
- [ ] 加连接上限与 `EMFILE` 退避，把 `EPOLL_SIZE` 改名成 `EPOLL_EVENTS_MAX`
- [ ] 输出改成有界缓冲，`printf` 移出读路径
- [ ] 补 `check`（真实回环端到端）、`bench`（单连接吞吐 / 多连接 accept 速率）、
      `tsan`（启用 pthread 分支之后）
