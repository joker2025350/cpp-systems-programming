# http-client —— 手写 HTTP/1.1 请求，把应答的原始字节取回

用 TCP 直接跟 Web 服务器对话：自己拼请求行与头部、自己 `select` 等可读、自己把
应答字节拼成一个字符串，中间不经过任何 HTTP 库。代码来自
`D:\7. http客户端请求\code\httprequest.c`（2019 年的课程示例），本轮只做到
"入库 + 可构建 + 可运行"，**逻辑与原始示例一致，缺陷一个没修**。

当前状态一句话：**回环上能取回一份完整应答（头部与正文都对），但收尾路径一步走错
就是段错误，已知缺陷 15 条**（第 9 节）。第二阶段按第 10 节的顺序逐日推进，
时间窗是 2026-12-05 ~ 2026-12-12。

## 目录结构

```
src/http-client/
├── Makefile                 all / test / sanitize / clean
├── README.md                本文件
├── src/httprequest.c        全部实现 + main（单文件程序）
└── tests/smoke.sh           离线可判定的 5 项检查
```

少了 `include/`、`lib`、`cli`、`check`、`bench`、`tsan`，原因写在"对外接口"一节。

## 对外接口

**没有。** 这是一个单文件程序：`host_to_ip` / `http_create_socket` /
`http_send_request` 全部是 `static`，外部只能通过 `main` 使用它。

这不是设计选择，是记账。第二阶段的第一个任务就是把它拆成
`include/httpclient.h` + `libhttpclient.a` + 薄驱动，理由是**测不了**：
"请求拼得对不对"取决于 `sprintf` 那一行的输出字节，"应答收得对不对"取决于
`recv` 循环怎么处理 0、正数、-1 三种返回值。这两件事现在都在 `http_send_request`
内部，中间没有任何可单独调用的函数，也没有"喂一段字节、断言我怎么处理"的入口。
第 9 节里至少 6 条缺陷（第 1、3、4、5、11、13 条）在拆分之前只能靠真实网络
碰运气复现——第 3、4 条我在第 5 节是用"临时副本 + 假服务器"复现的，那是验证手段，
不是用例。

## 设计要点

**请求的字节形态**（RFC 9112 §3、RFC 9110 §7.2）：请求行是
`method SP request-target SP HTTP-version CRLF`，随后若干
`field-name ":" OWS field-value CRLF`，最后以一个空行（单独的 CRLF）结束头部。
程序拼出来的完整请求是：

```
GET <resource> HTTP/1.1\r\n
Host: <hostname>\r\n
Connection: close\r\n
\r\n
```

两处值得说：

- `Host` 在 HTTP/1.1 里是必需字段，缺了服务器应当回 400（RFC 9110 §7.2 规定
  客户端"必须"发送，服务器"必须"拒绝缺失的请求）。
- `Connection: close` 把连接的生命周期钉成"一次请求一次连接"，于是"`recv` 返回 0"
  就成了天然的结束条件——收数据的循环不需要知道 `Content-Length`，也不需要认
  chunked 编码。这是整段代码里唯一正确的设计判断，也解释了它为什么能在回环上
  跑通：Python 的 `http.server` 正好也按 `HTTP/1.0` + 关闭连接回。

**收数据的结构**：`connect` 成功后立刻把 socket 切成 `O_NONBLOCK`，然后
`select` 等可读 → `recv` 一次最多 4096 字节 → 追加到堆上的字符串，直到
`select` 超时或 `recv` 返回 0。所以它是"非阻塞读 + 事件等待"，但全程只有一条连接、
一个描述符。

`select` 的语义有两处容易记错：第一个参数是"**关心的最大描述符 + 1**"，不是描述符
个数；要关心的描述符用 `FD_SET` 记进一个 `fd_set` 位图。这两点在 dns-client 里
没有出现（那边是阻塞的 `recvfrom`），是本模块与它最实质的差别。

**应答不解析。** 拿到的字节流原样返回，`main` 用 `printf("%s")` 打出来。状态行、
头部字段、`Content-Length`、chunked 解码一概不处理（第 9 节第 13 条）。

## 构建

```bash
cd src/http-client
make              # bin/http-client
make test         # tests/smoke.sh（5 项，离线）
make sanitize     # ASan + UBSan 重建并跑同一套检查，产物在 bin/sanitize/
make clean
```

用法：

```bash
./bin/http-client example.com /
```

两个参数是主机名与 request-target（要带前导 `/`）。端口硬编码在
`src/httprequest.c` 的 `htons((uint16_t)80)` 里，改端口要改源码重编。

**这里没有 feature test macro，与 dns-client 不同，是实测的结果。** 三种组合在
gcc 11.4.0 / glibc 2.35 上都是零警告：不给任何宏、只给 `-D_POSIX_C_SOURCE=200809L`、
给 `-D_DEFAULT_SOURCE`。原因是本文件真正用到的东西分两类：`select` / `fd_set` /
`struct timeval` 由 `<sys/select.h>` 提供（头文件体里无条件声明），
`gethostbyname` / `inet_addr` / `inet_ntoa` 在 glibc 的头文件里也是无条件声明的，
不受 `__USE_MISC` 控制。dns-client 那边必须给 `-D_DEFAULT_SOURCE`，是因为它用的
`random` / `srandom` / `bzero` 确实藏在 `__USE_MISC` 之后——**那条经验不能整体照搬**，
这次是编译一遍确认的，不是推断的。

真正必须补的头文件只有一个：`<sys/select.h>`。原文只包了 `<sys/socket.h>`，
在 `-std=c11` 下 `fd_set` 直接是未知类型、`struct timeval` 的 storage size 未知，
这是编译错误而不是警告。

## 测试与验证

环境：内网 Ubuntu，Linux 6.8.0-149 / gcc 11.4.0。2026-11-24 测。

| 项目 | 结果 |
|---|---|
| 默认构建 | 0 条警告（`-std=c11 -Wall -Wextra -Wconversion -O2 -g`） |
| 加严构建 | 0 条警告（在默认三项之上再加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes`，合计十一个 `-W`） |
| `make test` | **5 项**，0 失败 |
| `make sanitize` | 0 条警告，5 项全过，无 ASan/UBSan 报告 |
| 真实请求（回环） | 服务端日志 `"GET /doc.txt HTTP/1.1" 200 -`；客户端拿到的 `Content-Length: 31` 与正文 31 字节一致 |
| 整仓 `SKIP=mysql-client` | 构建 0 警告；`make test` / `make check` 全过；`dns-client` 3 项与本模块 5 项都在输出里 |

5 项检查钉的是与网络无关的事实：二进制能起、无参数时退出码非 0（实测 255）、
无参数时无输出、**只给一个参数时同样拒绝且不碰网络**、重复调用退出码一致。
最后多出来的那一项是 dns-client 没有的：本程序的判据是 `argc < 3`，
"少一个参数"是它自己的边界，写错成 `argc < 2` 就会拿 `argv[2]`（空指针）去
`sprintf`。**这 5 项不验证请求字节与应答处理**，理由见 `tests/smoke.sh` 文件头。

第 5 节的两条实测是用 `/tmp` 里的临时副本做的，仓库源码没改：

**一、域名解析失败时直接段错误。** 把主机名换成一个不存在的域名，`gethostbyname`
返回 NULL，`http_create_socket` 拿这个 NULL 去喂 `inet_addr`：

```
$ ./bin/sanitize/http-client nonexistent.invalid /
AddressSanitizer: SEGV on unknown address 0x000000000000
    #1 inet_addr
    #2 http_create_socket src/httprequest.c:84
    #3 http_send_request  src/httprequest.c:103
```

对应第 9 节第 2 条。`inet_addr`（POSIX 对 `inet_addr` 的说明、glibc 的实现）要求
参数是有效的以 NUL 结尾的字符串，传 NULL 是未定义行为，实际落在 `strtoul` 里解引用
空指针。

**二、对端用 RST 断开时也段错误，而且机制完全不同。** 用一个 accept 之后立刻
`SO_LINGER(1,0)` 关闭的假服务器，让 `recv` 返回 -1 / ECONNRESET 而不是 0：

```
runtime error: null pointer passed as argument 1, which is declared to never be null
AddressSanitizer: SEGV on unknown address 0x000000000000
    #2 strncat
    #3 http_send_request /tmp/hc8080.c:149
```

链条是：`len == -1` → `realloc(result, strlen + (size_t)(-1) + 1)` = `realloc(result, 0)`
→ glibc 释放并返回 NULL → `strncat(NULL, buffer, SIZE_MAX)`。对应第 9 节第 4 条，
这是全篇最严重的一处。

## 性能

没有测量，现阶段也不该测。单次请求的墙钟时间由两段构成：DNS 解析与 TCP 握手
往返（都是到对端的 RTT），加上对端生成应答的时间；程序自身的开销只是几次
`memcpy` 与 `realloc`，量它得到的是网络噪声。

可以从代码直接读出的复杂度倒是有意义的一处：**收应答是 O(n²)**。
每一轮循环都做 `strlen(result)`（从头扫一遍已收的字节）和
`strncat(result, ...)`（为了找结尾再从头部扫一遍），再加一次 `realloc`。
一个 1 MiB 的应答按 4096 字节分块是 256 轮，累计扫描量约
`256 × 1 MiB / 2 ≈ 128 MiB`。这个量级无关痛痒，但它是"边收边拼字符串"这种写法的
固有代价：正确做法是维护 `(buf, len, cap)` 三元组，追加时只写 `buf + len`，
扩容按 2 倍增长。要做这件事必须先有个"收应答"的函数，所以它排在第二阶段，
和拆分一起做；同时才有可测的对象——喂一段固定长度的应答，量拷贝字节数。

## 实现记录

### 2026-11-24 入库

从课程示例搬进仓库，只做让它在零警告前提下构建所需的四处编译层改动，
输出字节与原始示例完全相同（逐条写在 `src/httprequest.c` 的文件头）：

| 改动 | 原因 |
|---|---|
| 补 `<sys/select.h>` 与 `<stdint.h>` | 前者缺失是**编译错误**：`-std=c11` 下 `fd_set` 是未知类型、`struct timeval` 的 storage size 未知（POSIX 把 `select` / `fd_set` / `FD_*` 都声明在这个头里）；后者只为 `(uint16_t)` 显式转换 |
| 三个函数改 `static` | 只在本文件用且无对外声明，留着外部链接会触发 `-Wmissing-prototypes` |
| 显式转换：`htons((uint16_t)80)`、`(socklen_t)sizeof(...)`、`(size_t)BUFFER_SIZE`、`(int)recv(...)`、`strncat` 的 `(size_t)len` | `-Wconversion` / `-Wsign-conversion`。转换不改变行为，但把"`len` 可能是 -1"显式化了，见第 9 节第 4 条 |
| 请求格式串从"行尾反斜杠续行"改成"相邻字符串字面量" | 逐字节相同。继续用反斜杠时，行尾多一个空格会被悄悄拼进请求，肉眼看不出来 |

刻意**没有**动的两处：

- `main` 末尾没有 `return`。按 C11 5.1.2.2.3p1，从 `main` 掉出去等价于 `return 0`，
  不是缺陷，也不产生警告。dns-client 里补的那个 `return` 是因为那个函数不是 `main`，
  按 C11 6.9.1p12 返回值未指定。
- 宏名 `CONNETION_TYPE` 少一个 `C`，沿用原文。它是本文件内的标识符，
  改名只会让"与原始示例对照"变难。

根 `Makefile` 自动发现本模块（`make list` 里能看到 `http-client`），不需要改根构建。
根 `README` 的模块表新增一行，并把 "Layout" 一节里漏掉的 `dns-client` 一并补上。

## 已知局限

按"触发了会怎样"分组：第一组是**会崩或写坏内存**的，第二组是**语义错了但当前
不崩**的，第三组是资源与协议覆盖面的缺口。每条都给标准依据。

**会崩 / 会写坏内存**

1. **`socket()` 的返回值不检查。** 失败时返回 -1（POSIX `<sys/socket.h>` 的规定，
   并置 `errno`），代码把 -1 当描述符继续用：`connect(-1, ...)` 得到 EBADF，
   于是"描述符耗尽"这件事被伪装成"连不上服务器"，`fcntl(-1, ...)` 同样失败、
   同样不检查。
2. **`gethostbyname` 返回 NULL 时不终止。** `host_to_ip` 返回 NULL，
   `http_create_socket(NULL)` 到 `inet_addr(NULL)` 解引用空指针。**已实测崩**
   （第 5 节），ASan 报在 `inet_addr` 内部。依据：`inet_addr` 的参数必须是有效的
   以 NUL 结尾的字符串。
3. **`sprintf` 拼请求没有长度上界。** `buffer` 是 4096 字节的栈数组，格式串里两个
   `%s` 直接来自命令行，长度任意；超出时按 C11 6.5.2.1 是未定义行为，实际表现是
   覆盖同一栈帧里的 `sockfd` / `fdread` / `tv`。HTTP 层的惯例上限是请求行加头部
   8 KiB（RFC 9112 §3 允许实现自行设定上限，主流实现取 8 KiB），代码里一条检查都
   没有。
4. **`recv` 返回 -1 时仍走"追加"分支。** 只有 `len == 0` 被当作断开处理；
   `len == -1`（EAGAIN / EINTR / ECONNRESET）继续往下：
   `realloc(result, strlen + (size_t)len + 1)` 里 `(size_t)-1` 是 `SIZE_MAX`，
   长度变成 0，`realloc` 释放原块并返回 NULL，接着
   `strncat(NULL, buffer, SIZE_MAX)`。**已实测崩**（第 5 节）。
   若 `strlen(result)` 不为 0，则退化成"追加 0 字节然后继续循环"，
   同样没有退出条件。
5. **`result` 的初始分配与用途不匹配。** `malloc(sizeof(int))` 给 4 字节，用途是
   "存一个空字符串"，而空字符串只要 1 字节。在 x86-64 上 `sizeof(int) == 4 >= 1`，
   所以"恰好够用"是巧合：换成更窄的整型或别的 ABI 就变成"分配 2 字节、
   `strlen` 读 4 字节"的越界读。

**语义错误，当前不崩**

6. **`fcntl(sockfd, F_SETFL, O_NONBLOCK)` 覆盖整个标志字。** POSIX 规定 `F_SETFL`
   把文件状态标志**设为**第三个参数的值，不是按位或；此前生效的 `O_ASYNC` 等会被
   清掉。正确写法是先 `F_GETFL` 再 `| O_NONBLOCK`。
7. **`select` 的 `timeval` 在同一轮循环里被复用。** Linux 会把 `timeval` 改写成
   "剩余时间"（POSIX 说该参数返回后内容未指定），于是第二轮迭代的 5 秒是"上一轮
   剩下的"，几轮之后变成 0，`select` 退化成纯轮询。`select(2)` 的 BUGS 一节专门写了
   这一条，可移植代码必须每轮重新赋值。
8. **`fd_set` 在循环里不重建。** POSIX 规定 `select` 返回后 `fd_set` 内容未指定
   （Linux 只保留就绪位，BSD 会清空）。代码只在进循环前 `FD_ZERO` / `FD_SET` 一次，
   在当前 Linux 上因为 `sockfd` 一直就绪而侥幸正确。
9. **阻塞 `connect` 没有超时，`O_NONBLOCK` 设得比它晚。** SYN 得不到应答时
   `connect` 按 TCP 重传表退避（Linux 默认 `net.ipv4.tcp_syn_retries = 6`，
   约两分钟），这段时间程序没有任何输出也无法中断。把 socket 切成非阻塞的动作
   发生在 `connect` **之后**，对它不起作用。

**资源与覆盖面的缺口**

10. **`sockfd` 从不 `close`。** 每次调用泄漏一个描述符，`connect` 失败的分支同样
    泄漏。
11. **`send` 的返回值不检查。** `send` 允许短写（POSIX：返回实际写出的字节数，
    可能小于请求值），剩余字节被丢弃，服务器收到半个请求。回环上 4 KiB 以内几乎
    不会发生，但对端窗口小、或请求被 `sprintf` 拼到接近 4096 字节时会。
12. **只支持 HTTP，端口硬编码 80，不解析 URL。** `resource` 要调用方自己给
    （含前导 `/`）。输入 `https://...` 会静默连到 80 端口；TLS 是另一个层次的事，
    不在本模块范围内。
13. **完全不解析应答。** 返回原始字节流，`printf("%s")` 直接打印：没有状态行、
    没有头部字段、没有 chunked 解码（RFC 9112 §7.1）、没有 `Content-Length` 校验。
    头部值里出现 `\0` 时 `printf` 会截断。响应码 404 与 200 在程序看来没有区别。
14. **`gethostbyname` 与 `inet_ntoa` 都返回静态缓冲区。** POSIX 对这两个接口的说明里
    明确：返回值可能指向静态存储，下一次调用会覆盖。因此本模块不可重入、不能并发
    调用；`h_addr_list` 也直接按 `struct in_addr` 使用，没有看 `h_addrtype`
    （POSIX 要求先确认是 `AF_INET`）。
15. **只取 `h_addr_list[0]`。** 一个主机名解析出多个地址时只用第一个，
    第一个不可达不会尝试下一个。

## 待办

第二阶段（2026-12-05 ~ 2026-12-12）按这个顺序推进，一天一条：

- [ ] 拆成 `include/httpclient.h` + `libhttpclient.a` + 薄驱动（第一件）
- [ ] 把"拼请求"与"收应答"各暴露成一个函数，补固定输入的用例：
      给定 host 与 resource → 断言请求字节；给定一段应答字节 → 断言收尾行为
- [ ] 修第 4 条（`len < 0` 分支）与第 2 条（NULL 检查），这两条已能稳定复现
- [ ] 修第 3 条：算出可用长度再 `snprintf`，超长返回错误而不是截断
- [ ] 修第 5 条：初值改成 `malloc(1)` + `result[0] = '\0'`
- [ ] 加 `close`，把第 10 条的泄漏消掉（LSan 下跑一次真实请求）
- [ ] 修第 6、7、8 条：`F_GETFL | O_NONBLOCK`、每轮重设 `timeval`、
      每轮重建 `fd_set`
- [ ] 收应答改成 `(buf, len, cap)` + 2 倍增长，并用固定长度应答量拷贝字节数
- [ ] 加超时：`connect` 用非阻塞 + `poll`，或 `SO_SNDTIMEO` / `SO_RCVTIMEO`
- [ ] 端口改成参数、加 URL 解析（scheme / host / port / path）
- [ ] 解析状态行与头部，认 `Content-Length` 与 chunked
- [ ] 主机名解析换 `getaddrinfo`（可重入、支持 IPv6、多地址回退）
