# dns-client —— 手写 DNS 查询报文并解析应答

用 UDP 直接跟 DNS 服务器对话：自己拼查询报文、自己解析应答，中间不经过
`getaddrinfo` 这类解析库。代码来自 `D:\6. DNS协议与请求\code\dns.c`（2019 年的
课程示例）。

2026-11-23 以"入库态"进仓库（单文件、五个函数全 `static`），2026-11-27 拆成
`include/dnsclient.h` + `libdnsclient.a` + 薄驱动。**拆分只改链接属性与文件位置，
函数体一行没动，已知缺陷 12 条一条没修**（第 9 节），按第 10 节的顺序逐日处理。

当前状态一句话：报文构造有 22 项离线断言钉住了字节，解析与连接管理仍然只有
"能查出结果"这一个证据，缺陷 12 条里已修 0 条。

## 目录结构

```
src/dns-client/
├── Makefile              all / lib / cli / test / check / sanitize / bench / clean
├── README.md             本文件
├── include/dnsclient.h   对外接口：3 个结构体、5 个函数、2 个常量
├── src/dnsclient.c       实现（is_pointer / dns_parse_name 仍是文件内 static）
├── src/main.c            薄驱动，15 行：看参数、调库、返回
└── tests/
    ├── test_message.c    22 项：报文构造的逐字节断言（链接 libdnsclient.a）
    └── smoke.sh          3 项：命令行行为（缺参数、退出码、无输出）
```

`testdata/` 还没有：今天的用例用编译期常量当输入（QNAME 的期望编码直接写成
数组），不需要外部文件。等解析用例进来——"一段固定的应答字节"用 `.bin` 存更
清楚，那时再加，并按仓库约定在 `.gitattributes` 的 `**/testdata/** -text` 下
保持字节不变。

## 对外接口

`include/dnsclient.h`，按一次查询的三个阶段分三组：

| 函数 | 作用 | 返回 |
|---|---|---|
| `dns_create_header` | 填 12 字节报文头：ID 取 `random()`，Flags = `htons(0x0100)`（只置 RD），QDCOUNT = 1 | 0 / -1（参数为 NULL） |
| `dns_create_question` | 把点分域名编码成 QNAME，填 qtype = 1(A)、qclass = 1(IN) | 0 / -1（NULL）/ -2（分配失败） |
| `dns_build_request` | header → QNAME → QTYPE → QCLASS 顺序写入 `request` | **写入的字节数** / -1 |
| `dns_parse_response` | 解析应答，把 A 记录收进 `struct dns_item` 数组 | 记录条数 / -1（calloc 失败） |
| `dns_client_commit` | 建 socket、发请求、收应答、交给解析 | `recvfrom` 的字节数 / -1 |

三个结构体（`dns_header` / `dns_question` / `dns_item`）在头文件里是可见的，
没做不透明句柄：报文是**按字段直接写内存**的，调用方必须先有一块可写的
`struct dns_header`。要封成句柄就得同时引入一对构造/析构函数，那已经属于接口
设计，会和"先修哪条缺陷"纠缠在一起，单独排一次更清楚。

内存归属写在头文件的注释里，这里重复一遍，因为它是最容易漏的地方：

- `dns_create_question` 分配的 `question->name` 由**调用方** `free`；
- `dns_parse_response` 分配的数组本身、以及每条记录的 `domain` / `ip`，
  都由调用方 `free`。而 `dns_client_commit` 只 `free` 了数组（第 9 节第 6 条）。

调用 `dns_parse_response` 之前必须先确认真实收到的字节数：它**拿不到 buffer
的长度**，只按报文内部的长度字段推进指针（第 4 条缺陷）。

另外记一笔：`dns_parse_response` 会把每条 A 记录 `printf` 到 stdout。
这段打印在拆分前就在 `main` 所在的同一个文件里，拆分时跟着解析一起走。
库函数写 stdout 是接口缺陷（第 13 条），列进了待办。

## 设计要点

DNS 查询报文（RFC 1035 §4.1）分三段，这个程序三段都手写：

1. **头部 12 字节定长**：ID(2) + Flags(2) + QDCOUNT(2) + ANCOUNT(2) + NSCOUNT(2) +
   ARCOUNT(2)。Flags 的 `0x0100` 表示 RD（Recursion Desired，请求递归解析）；
   `0x8000` 是 QR 位，查询报文必须为 0，应答里由服务器置 1。
2. **QNAME 不是点分字符串**：每段前面加一个长度字节，末尾一个 `0x00`。
   `www.0voice.com` 在报文里是 16 字节：
   `03 77 77 77 | 06 30 76 6f 69 63 65 | 03 63 6f 6d | 00`
   ——`www` 三字符配长度 `03`，`0voice` 六字符配长度 `06`，`com` 三字符配 `03`，
   最后是表示根标签的 `00`。
   `dns_create_question` 做的就是这件事：`strtok` 按 `.` 切开，逐段写长度再拷内容。
   所以分配的长度是 `strlen(hostname) + 2`——14 个字符的域名编码后是 16 字节：
   每个 label 一个长度字节（共 3 个），加末尾根标签 1 个，
   正好抵掉 3 个点号再多 1 字节。
3. **应答里的资源记录（RR）**：NAME（可能是压缩指针）+ TYPE(2) + CLASS(2) + TTL(4) +
   RDLENGTH(2) + RDATA。A 记录的 RDATA 是 4 字节 IPv4 地址；CNAME 的 RDATA 是另一个域名。

**域名压缩**是这段代码里唯一有点绕的地方：报文重复出现的域名后缀，第二次起用
2 字节指针代替，高 2 位是 `11`（所以 `(byte & 0xC0) == 0xC0` 就是"这是指针"），
低 14 位是"从报文起始算起的偏移"。`dns_parse_name` 遇到指针就跳到 `chunk + n`
继续读，读完 break——这也是它没有防环的原因（第 9 节第 5 条）。

## 构建

```bash
cd src/dns-client
make              # bin/libdnsclient.a + bin/dns-client
make lib          # 只建静态库
make cli          # 只建命令行工具（链接 libdnsclient.a）
make test         # 22 项报文构造用例
make check        # 3 项命令行行为检查
make sanitize     # ASan + UBSan 重建并跑 test + check，产物在 bin/sanitize/
make bench        # 打印"暂无可量的吞吐"后正常退出
make clean
```

用法：

```bash
./bin/dns-client www.baidu.com
```

服务器地址与端口硬编码在 `src/dnsclient.c` 的两个宏里：`DNS_SERVER_IP` 为
`114.114.114.114`，`DNS_SERVER_PORT` 为 53。改服务器要改源码重编——这本身是待办之一。

`-D_DEFAULT_SOURCE` 必须跟 `-D_POSIX_C_SOURCE=200809L` 一起给：`random` / `srandom` /
`bzero` 是 BSD/SVID 遗留接口，glibc 把它们藏在 `__USE_MISC` 之后，而只开
`_POSIX_C_SOURCE` 时 `__USE_MISC` 是关的。原始代码没给这两个宏，靠隐式声明编过去——
按 C11 6.5.2.2p6 隐式函数声明在 C99 之后已被移除，这里已经补上。

`test` 与 `check` 的分工：`test` 是库的单元用例（进程内，不碰网络）；
`check` 是命令行这个交付物的端到端行为。入库阶段两者混在一个 `smoke.sh` 里，
拆库之后各归各位。

## 测试与验证

环境：WSL2 里的 Ubuntu 22.04.5（内核 6.18.58.1-microsoft-standard-WSL2），
gcc 11.4.0（Ubuntu 11.4.0-1ubuntu1~22.04.3），2026-11-27 测。

> 换环境的原因如实记一笔：此前一直用的内网 Ubuntu 是 `192.168.31.192`，
> 今天本机在 `192.168.1.109` 网段，那台机器不可达（`ssh Ubuntu` 在 banner
> exchange 阶段超时，`192.168.1.0/24` 上没有开 22 端口的主机）。本机 WSL 没有
> 装发行版，于是把 `ubuntu-base-22.04.5` 的 rootfs 导进来当验证机——gcc 版本与
> 文档记录的一致，所以"零警告"这个结论的前提没变。

| 项目 | 结果 |
|---|---|
| 默认构建 | 0 条警告（`-std=c11 -Wall -Wextra -Wconversion -O2 -g`） |
| 加严构建 | 0 条警告（再加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes`） |
| `make test` | **22 项**，0 失败（默认构建与加严构建各跑一次） |
| `make check` | **3 项**，0 失败 |
| `make sanitize` | 0 条警告，22 + 3 项全过，无 ASan/UBSan/LSan 报告 |
| 整仓 `SKIP=mysql-client` | 构建 0 警告；test / check / sanitize 全过（dns-client 之外 8 个模块的数字见各自 README） |
| 真实查询 | `www.baidu.com` → `198.18.1.188`，与 `dig @114.114.114.114 +noall +answer` 的同一条记录一致 |

22 项检查覆盖的是报文构造这一半，具体断言：

- 报文头：`Flags` 的 RD 位（比较前 `ntohs` 回主机序，不然就变成"在小端上断言
  0x0001"，把实现细节当契约）、QDCOUNT = 1、其余三个计数为 0；`NULL` 入参返回 -1。
- QNAME：`www.0voice.com` 编码成 `03 www 06 0voice 03 com 00` 共 16 字节，
  `length == 16`、qtype/qclass 为 1/1；单 label 的 `localhost` 编码成 `09 localhost 00`
  共 11 字节；`NULL` 入参返回 -1。
- 整条报文：返回值 32 = 12 + 16 + 4；前 12 字节与 `struct dns_header` 逐字节相同；
  QNAME 紧跟其后；尾部 4 字节为 `00 01 00 01`；返回值之后直到缓冲区末尾全是
  `memset` 过的 0（缓冲区先填 `0xA5`，所以"没写"和"写 0"能区分开）。

**没有**断言的：TTL（属于解析）、`rlen` 不足时的行为（第 4 条，现在会越界写，
ASan 下直接报错，不该写成"通过"）、带结尾点的域名（长度字段与实际写入字节数
不一致，第 9 条的另一面，另案处理）。

真实查询这一行同时暴露了第一处缺陷，也是最直观的一处：

```
$ ./bin/dns-client www.baidu.com
connect : 0
www.baidu.com has address 198.18.1.188
	Time to live: 0 minutes , 0 seconds

$ dig @114.114.114.114 +noall +answer www.baidu.com
www.baidu.com.		1	IN	A	198.18.1.188
```

地址两边一致，TTL 是 0 而 `dig` 给 1。原因在
`ttl = htons(*(unsigned short *)ptr); ptr += 4;`：TTL 是 4 字节字段，
这里按 2 字节读、读到的还是高 2 字节（TTL 小于 65536 时恒为 0）。

地址是 `198.18.1.188` 而不是公网地址，是因为这台机器的出口被代理接管，
`dig` 与 `dns-client` 拿到的是同一条 fake-IP 记录。这里比对的是"同一台服务器、
同一条记录"的一致性，不是公网真实地址。

## 性能

没有测量，也不打算在现阶段测。单次查询的墙钟时间几乎全是到 DNS 服务器的 RTT
（同一台服务器上实测几次都在毫秒级抖动），程序本身的开销只有一次 `malloc`
和两次 `memcpy`，量它得到的是网络噪声。有意义的是**批量查询的吞吐**，
但那要先有"复用一条 socket、并发多个 in-flight 查询"的结构，
属于第二阶段之后的事。`make bench` 因此只打印这个结论后正常退出——
留着这个目标是为了让根 `Makefile` 的 `make bench` 不必写模块例外。

## 实现记录

### 2026-11-27 拆库：`dnsclient.h` + `libdnsclient.a` + 薄驱动

拆之前的状态：`dns_create_header` / `dns_create_question` / `dns_build_request` /
`dns_parse_response` / `dns_client_commit` 全是 `static`，外部只能通过 `main`
使用它。"发出去的字节对不对"没法断言，只能连真实服务器再人工比对 `dig` 的输出——
那既依赖网络，也把"服务器今天回什么"变成断言的输入。

拆之后的分界，只按"功能边界"切，不按"文件好不好看"切：

| 文件 | 内容 | 为什么在这儿 |
|---|---|---|
| `include/dnsclient.h` | 3 个结构体、5 个函数声明、`DNS_HEADER_SIZE` 与 `DNS_FLAGS_RD` | 调用方要用报文段就必须能看见结构体 |
| `src/dnsclient.c` | 实现；`is_pointer` 与 `dns_parse_name` 仍是文件内 `static` | 前者只判压缩指针的高 2 位，后者是 `dns_parse_response` 的递归辅助，都没有独立意义 |
| `src/main.c` | 15 行：`argc < 2` 返回 -1，否则调 `dns_client_commit` 后返回 0 | 驱动只链接 `libdnsclient.a`，不重复编译模块源码 |

`src/main.c` 的返回值照搬入库态的行为（少参数直接 `return -1`，退出状态 255；
正常路径丢弃查询结果，进程以 0 退出）。这不是"正确"的用法，是记账：
"打印用法并返回 2"和"返回码反映查询结果"要跟超时、错误码一起重新定义 CLI 契约，
属于另一件事。

`Makefile` 同时补齐 `lib` / `cli` / `test` / `check` / `bench`，并把
`test` 与 `check` 分开：`test` 跑单元用例，`check` 跑命令行行为。
入库阶段这两件事混在一个 `smoke.sh` 里（当时没别的地方可放）。

### 2026-11-27 新增 `tests/test_message.c`（22 项）

拆分本身不产生任何新行为，能证明它成立的只有一件事：**构造过程真的能被单独
调用，且发出去的字节与 RFC 1035 §4.1.2 的编码逐字节相符**。所以同一天写了这批用例——
不是"顺手加测试"，是拆分的验收条件。

期望值不是从程序输出里抄回来的（那样只能证明"改动前后一样"，不能证明"对"），
而是按 §4.1.2 手写的常量数组：每个 label 前一个长度字节、末尾一个 `00`。
写的时候立刻对上了一处**文档错误**：11-23 入库时 README 的"设计要点"把
`0voice` 的长度字节写成了 `07`，实际是 `06`（`0voice` 六个字符）。
按 07 编码整条 QNAME 会是 17 字节，与 `strlen + 2 = 16` 自相矛盾。已改正。

### 2026-11-23 入库

从课程示例搬进仓库，只做让它在零警告前提下构建所需的五处编译层改动，
语义一行没变（逐条写在 `src/dnsclient.c` 的文件头）：

| 改动 | 原因 |
|---|---|
| 补 `<time.h>` / `<arpa/inet.h>` / `<strings.h>`，并给 `-D_DEFAULT_SOURCE` | 原文靠隐式声明编过；C11 6.5.2.2p6 之后隐式声明不是合法写法 |
| 五个函数改 `static` | 只在本文件用且无对外声明，留着外部链接会触发 `-Wmissing-prototypes`（2026-11-27 拆库后又变回外部链接，这次有头文件里的声明，同一警告不再触发） |
| `dns_create_question` 补 `return 0` | 声明返回 `int` 却从末尾掉出去，按 C11 6.9.1p12 返回值未指定 |
| 显式转换（`size_t` → `int`、`char *` ↔ `unsigned char *`） | `-Wconversion` 与 `-Wincompatible-pointer-types` |
| `strncpy(qname, token, len+1)` → `memcpy(...)`，删掉从未使用的 `alen` | 拷贝长度正好是 `len` 个字符加 NUL，两者逐字节相同；`strncpy` 这种用法会被静态检查判为"可能无终止符" |

## 已知局限

按"改起来要动哪里"排序，前两组是**会算错结果**和**会读越界**的，
第三组是功能与工程上的缺口。每条都给了标准依据，不是"这样不好"。

**会算错**

1. **TTL 读错字段。** `ttl = htons(*(unsigned short *)ptr)` 把一个 4 字节字段
   （RFC 1035 §4.1.3：TTL 是 32 位无符号整数）当 2 字节读，读的还是高 2 字节。
   实测 `dig` 给 1 秒、本程序给 0（见"测试与验证"）。
2. **`type` 用 `htons` 而不是 `ntohs`。** 这一处在当前平台上不影响结果——
   16 位字段上 `htons` 与 `ntohs` 都是"字节序转换"，小端交换、大端恒等，
   两者数值永远相同。但语义是错的：`htons` 的契约是"主机序 → 网络序"，
   用在"从报文里取出的字段"上方向反了。**两个错互相抵消**的写法不该留，
   因为下一个改这段的人会照着它继续写（TTL 那处就是照抄出来的）。
3. **事务 ID 可预测。** `srandom(time(NULL))` 每次调用都重置种子，同一秒内的
   两次查询会拿到同一个 ID；`random()` 本身是可预测的线性同余序列。
   RFC 5452 §9.2 要求事务 ID 与源端口不可预测，否则缓存投毒的门槛从
   "猜 16 位"降到"猜时间"。

**会读越界**

4. **解析全程没有边界检查。** `dns_parse_response` 只用报文内部的长度字段推进
   指针（`ptr += datalen`、`ptr += flag + 1`），从不与 `recvfrom` 的返回值比较。
   一份损坏或恶意的应答可以让 `ptr` 越过 1024 字节缓冲区，之后读到的是同一栈帧上
   的其他变量。这是唯一一条"能打出来就是安全漏洞"的缺陷。拆库之后这条更显眼了：
   头文件里必须明写"调用方先确认真实字节数"，而那本该是函数自己做的事。
5. **压缩指针没有防环。** `dns_parse_name` 遇到指针即递归，深度不受限。
   指向自身的指针（偏移指回自己）会让它无限递归，栈溢出。

**功能与工程缺口**

6. **内存泄漏两处**：`list[cnt].domain` / `.ip` 各 `calloc` 一次，
   `dns_client_commit` 只 `free(dns_domain)`，`question->name` 也从未释放。
   后者会在 `dns_client_commit` 返回时变成不可达块，前者在数组被 `free` 之后
   同样不可达——所以只要在 ASan/LSan 下跑一次真实查询就会报出来。现在**没有**
   这样的用例（`dns_client_commit` 要连网，用例不调它），这条因此没有被覆盖。
7. **没有超时。** `recvfrom` 是阻塞调用，服务器不回包就永久挂起。把
   `DNS_SERVER_IP` 改成不可达地址即可复现。修法：`SO_RCVTIMEO` 或 `poll`。
8. **只支持 A 记录**（`qtype` 硬编码 1）、只解析应答里的 A 与 CNAME，
   MX/TXT/AAAA 一律跳过；也不处理 TC 标志（应答被截断时应改用 TCP 重试，
   RFC 1035 §4.2.1）。
9. **不做输入校验**：域名超 253 字节或单个 label 超 63 字节时，
   `(unsigned char)len` 会静默截断，发出去的报文与输入不再对应
   （RFC 1035 §2.3.4 规定 label 上限 63 字节、域名上限 255 字节）。
   另一面是**带结尾点的域名**（`www.0voice.com.`）：`strtok` 不会产出空 token，
   实际只写 `strlen` 字节，而 `length` 记的是 `strlen + 2`，
   `dns_build_request` 会把多出来的那个未初始化字节一起发出去。
10. **服务器与端口硬编码**，无法命令行指定；`inet_addr` 失败返回
    `INADDR_NONE`（`255.255.255.255`），与合法地址不可区分，且不支持 IPv6
    （POSIX `inet_addr(3p)` 的返回值语义）。
11. **`recvfrom` 用 1024 字节缓冲区**，而 `DNS_SERVER_IP` 那台服务器对大应答会置 TC 位
    （RFC 1035 §4.2.1：UDP 应答超过 512 字节时截断并把 TC 置 1）。
    缓冲区与"最大 UDP 应答"之间没有关系说明。
12. **一次只查一个域名，串行。** 没有"批量 + 并发在飞（in-flight）"的结构，
    所以第 6 节说的吞吐量现在测不了。
13. **库函数写 stdout。** `dns_parse_response` 里两条 `printf`、
    `dns_client_commit` 里一条 `printf("connect : %d")`。C11 7.21.3 把 `stdout`
    定义为程序启动时预定义的流，库无法知道它是否被重定向、是否已关闭；
    调用方因此没法把输出接到自己的日志或缓冲里，而 `printf` 的返回值
    也从未检查（POSIX `printf(3)` 失败时返回负值并置 `errno`）。
    这是拆库之后才浮出来的——拆分前这段打印和 `main` 在同一个文件里，
    还算"程序的输出"；拆分后它就是库在替调用方决定往哪写。
14. **错误码不成体系。** `dns_create_question` 用 0 / -1 / -2，`dns_build_request`
    用"正数表示字节数、-1 表示错误"，`dns_client_commit` 直接透传 `recvfrom`
    的返回值。调用方无法区分"参数错""网络错""服务器拒绝"（RCODE 字段
    ——RFC 1035 §4.1.1 的 4 位应答码——从来没有被读过）。今天的拆分刻意
    没有动这些签名：改返回类型会同时改动驱动与全部用例，和"修 TTL"是两件事。

## 待办

- [x] 拆成 `include/dnsclient.h` + `libdnsclient.a` + 薄驱动（2026-11-27 完成）
- [x] 第一批"固定输入 → 断言输出"的用例：报文构造 22 项（2026-11-27 完成）
- [ ] 补解析侧用例：固定应答字节（放进 `testdata/`）→ 断言域名、地址、条数。
      第 1、2、4、5 条都靠它，且要先给解析加边界检查，否则用例本身就是越界读
- [ ] 修 TTL（4 字节 + `ntohl`）与 `type`（`ntohs`），并把两处取值改成
      `memcpy` 到局部变量再转换，消掉未对齐解引用（C11 6.3.2.3p7）
- [ ] 解析加边界检查：所有 `ptr` 前进都对照"已收到的字节数"核对，
      接口上得把长度一起传进来（签名要变）
- [ ] `dns_parse_name` 加递归深度/偏移回退检查，防压缩指针成环
- [ ] 把三条 `printf` 从库里搬回驱动（第 13 条），顺便让解析结果能被调用方
      自己格式化——这也是"加 TTL 到 `struct dns_item`"的前置
- [ ] 加超时（`SO_RCVTIMEO` 或 `poll`），把"服务器不回包"变成可判定的失败
- [ ] 释放 `question->name` 与 `list[i].domain` / `.ip`，并在 ASan/LSan 下跑一次真实查询
- [ ] 服务器地址与端口改成参数；`inet_addr` 换成 `inet_pton` + 返回值判断
- [ ] 支持 AAAA 与 MX，处理 TC 标志与 TCP 回退
- [ ] 事务 ID 改用 `/dev/urandom` 或 `getrandom`，并加源端口随机化说明
