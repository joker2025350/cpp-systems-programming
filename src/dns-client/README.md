# dns-client —— 手写 DNS 查询报文并解析应答

用 UDP 直接跟 DNS 服务器对话：自己拼查询报文、自己解析应答，中间不经过
`getaddrinfo` 这类解析库。代码来自 `D:\6. DNS协议与请求\code\dns.c`（2019 年的
课程示例）。

2026-11-23 以"入库态"进仓库（单文件、五个函数全 `static`），2026-11-27 拆成
`include/dnsclient.h` + `libdnsclient.a` + 薄驱动。拆分只改链接属性与文件位置，
函数体一行没动。2026-11-28 起按第 9 节的顺序逐条修缺陷，当天动的是解析侧。

当前状态一句话：报文构造与应答解析各有一批离线断言钉住了字节（22 + 30 项），
连接管理仍然只有"能查出结果"这一个证据；第 8 节列的 14 条里已修 2 条
（第 4、5 条，2026-11-28），其余 12 条原样。

## 目录结构

```
src/dns-client/
├── Makefile              all / lib / cli / test / check / sanitize / bench / clean
├── README.md             本文件
├── include/dnsclient.h   对外接口：3 个结构体、5 个函数、2 个常量
├── src/dnsclient.c       实现（is_pointer / read_u16 / dns_name_read 仍是文件内 static）
├── src/main.c            薄驱动，15 行：看参数、调库、返回
├── testdata/             7 份固定应答字节，字节数本身就是文档语义
│   ├── a-simple.bin          48  一条 A 记录，NAME 用压缩指针
│   ├── cname-chain.bin       74  CNAME + A，第二条的 NAME 指向前一条的 RDATA
│   ├── uncompressed-name.bin 62  一条 A 记录，NAME 完整写出而非指针
│   ├── nxdomain.bin          33  ANCOUNT=0、RCODE=3
│   ├── truncated-question.bin 40  截在记录中间（TTL 都不完整）
│   ├── truncated-rdata.bin   46  截在 RDATA 里，只剩 2 字节
│   └── bad-pointer.bin       28  NAME 压缩指针指向自己
└── tests/
    ├── test_message.c    22 项：报文构造的逐字节断言（链接 libdnsclient.a）
    ├── test_response.c   30 项：解析固定应答、拒绝坏报文
    └── smoke.sh          3 项：命令行行为（缺参数、退出码、无输出）
```

`testdata/*.bin` 按字节原样存放，`.gitattributes` 的 `**/testdata/** -text` 保证
换行符不被转换。手工按 RFC 1035 §4.1 编码，每份的字段偏移写在"测试与验证"里。
它们不是从程序输出里截下来的：抄输出只能证明"改动前后一样"，不能证明"对"。

## 对外接口

`include/dnsclient.h`，按一次查询的三个阶段分三组：

| 函数 | 作用 | 返回 |
|---|---|---|
| `dns_create_header` | 填 12 字节报文头：ID 取 `random()`，Flags = `htons(0x0100)`（只置 RD），QDCOUNT = 1 | 0 / -1（参数为 NULL） |
| `dns_create_question` | 把点分域名编码成 QNAME，填 qtype = 1(A)、qclass = 1(IN) | 0 / -1（NULL）/ -2（分配失败） |
| `dns_build_request` | header → QNAME → QTYPE → QCLASS 顺序写入 `request` | **写入的字节数** / -1 |
| `dns_parse_response` | 解析应答，把 A 记录收进 `struct dns_item` 数组 | 记录条数（可为 0）/ -1（参数、报文或分配失败） |
| `dns_client_commit` | 建 socket、发请求、收应答、交给解析 | `recvfrom` 的字节数 / -1 |

三个结构体（`dns_header` / `dns_question` / `dns_item`）在头文件里是可见的，
没做不透明句柄：报文是**按字段直接写内存**的，调用方必须先有一块可写的
`struct dns_header`。要封成句柄就得同时引入一对构造/析构函数，那已经属于接口
设计，会和"先修哪条缺陷"纠缠在一起，单独排一次更清楚。

内存归属写在头文件的注释里，这里重复一遍，因为它是最容易漏的地方：

- `dns_create_question` 分配的 `question->name` 由**调用方** `free`；
- `dns_parse_response` 分配的数组本身、以及每条记录的 `domain` / `ip`，
  都由调用方 `free`。而 `dns_client_commit` 只 `free` 了数组（第 8 节第 6 条）。

`dns_parse_response` 的第二个形参是**实际收到的字节数**，直接把 `recvfrom` 的
返回值传进去即可（两者类型都是能装下字节数的整型，这里统一用 `size_t`）。
2026-11-28 之前它没有这个形参，只能按报文内部自称的长度推进指针——见第 8 节
第 4 条（已修）。

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
   所以分配的长度是 `strlen(hostname) + 2`。这个 `+2` 的来历：`www.0voice.com`
   有 14 个字符，其中 2 个是点号；编码后内容是 12 字节 label、3 个长度字节、
   1 个根标签，合计 16——比原串多 2，正好是"点号被长度字节替换，再加末尾那个 0"。
3. **应答里的资源记录（RR）**：NAME（可能是压缩指针）+ TYPE(2) + CLASS(2) + TTL(4) +
   RDLENGTH(2) + RDATA。A 记录的 RDATA 是 4 字节 IPv4 地址；CNAME 的 RDATA 是另一个域名。

**域名压缩**是这段代码里唯一有点绕的地方：报文重复出现的域名后缀，第二次起用
2 字节指针代替，高 2 位是 `11`（所以 `(byte & 0xC0) == 0xC0` 就是"这是指针"），
低 14 位是"从报文起始算起的偏移"。`dns_name_read` 遇到指针就跳到 `msg + target`
继续读，同时记住"这个名字在记录里只占 2 字节"。

这里有一条容易被当成实现细节、其实是协议要求的约束：**指针只能往回指**。
RFC 1035 §4.1.4 的原文是"指向报文里先前出现过的域名"，所以目标偏移必然严格
小于当前偏移。按这条检查，指向自己（成环）和指向后面（读到还没解析的字节）
两种坏指针在第一跳就被拒掉，同时也顺带得到"跳转必然终止"这个性质——
不需要额外的深度计数器（`dns_name_read` 里那个上限只是保险，不是防线）。

## 构建

```bash
cd src/dns-client
make              # bin/libdnsclient.a + bin/dns-client
make lib          # 只建静态库
make cli          # 只建命令行工具（链接 libdnsclient.a）
make test         # 22 项报文构造 + 30 项应答解析（都离线，链接 libdnsclient.a）
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
gcc 11.4.0（Ubuntu 11.4.0-1ubuntu1~22.04.3），2026-11-27 与 11-28 测。

> 换环境的原因如实记一笔：此前一直用的内网 Ubuntu 是 `192.168.31.192`，
> 本机换到 `192.168.1.109` 网段之后那台机器不可达（`ssh Ubuntu` 在 banner
> exchange 阶段超时，`192.168.1.0/24` 上没有开 22 端口的主机）。本机 WSL 没有
> 装发行版，于是把 `ubuntu-base-22.04.5` 的 rootfs 导进来当验证机——gcc 版本与
> 文档记录的一致，所以"零警告"这个结论的前提没变。

| 项目 | 结果 |
|---|---|
| 默认构建 | 0 条警告（`-std=c11 -Wall -Wextra -Wconversion -O2 -g`） |
| 加严构建 | 0 条警告（再加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes`） |
| `make test` | **22 + 30 项**，0 失败（默认构建与加严构建各跑一次） |
| `make check` | **3 项**，0 失败 |
| `make sanitize` | 0 条警告，22 + 30 + 3 项全过，无 ASan/UBSan/LSan 报告 |
| 整仓 `SKIP=mysql-client` | 构建 0 警告；test / check / sanitize 全过（dns-client 之外 8 个模块的数字见各自 README） |
| 真实查询 | `www.baidu.com` → `198.18.1.188`，与 `dig @114.114.114.114 +noall +answer` 的同一条记录一致 |

### 报文构造的 22 项

- 报文头：`Flags` 的 RD 位（比较前 `ntohs` 回主机序，不然就变成"在小端上断言
  0x0001"，把实现细节当契约）、QDCOUNT = 1、其余三个计数为 0；`NULL` 入参返回 -1。
- QNAME：`www.0voice.com` 编码成 `03 www 06 0voice 03 com 00` 共 16 字节，
  `length == 16`、qtype/qclass 为 1/1；单 label 的 `localhost` 编码成 `09 localhost 00`
  共 11 字节；`NULL` 入参返回 -1。
- 整条报文：返回值 32 = 12 + 16 + 4；前 12 字节与 `struct dns_header` 逐字节相同；
  QNAME 紧跟其后；尾部 4 字节为 `00 01 00 01`；返回值之后直到缓冲区末尾全是
  `memset` 过的 0（缓冲区先填 `0xA5`，所以"没写"和"写 0"能区分开）。

### 应答解析的 30 项

输入是 `testdata/` 的七份固定字节，每份先钉住文件字节数（字节数变了就说明
有人动过数据），再断言解析结果。以 `a-simple.bin`（48 字节）为例，偏移是：

| 偏移 | 字节 | 含义 |
|---|---|---|
| 0–11 | `12 34 81 80 00 01 00 01 00 00 00 00` | ID=0x1234、QR=1/RD=1/RA=1、QD=1、AN=1 |
| 12–27 | `03 www 06 0voice 03 com 00` | 问题名（QNAME），16 字节 |
| 28–31 | `00 01 00 01` | QTYPE=A、QCLASS=IN |
| 32–33 | `c0 0c` | 应答记录的 NAME，压缩指针指回偏移 12 |
| 34–37 | `00 01 00 01` | TYPE=A、CLASS=IN |
| 38–41 | `00 00 01 2c` | TTL = 300 秒 |
| 42–43 | `00 04` | RDLENGTH = 4 |
| 44–47 | `5d b8 d8 22` | RDATA = 93.184.216.34 |

七份文件覆盖的路径与断言：

| 文件 | 覆盖 | 断言 |
|---|---|---|
| `a-simple.bin` | 压缩指针形式的 NAME | 1 条记录，`www.0voice.com` → `93.184.216.34` |
| `cname-chain.bin` | CNAME 后跟 A；第二条的 NAME 用指针指向第一条 RDATA 里的域名（RFC 1035 §4.1.4 允许） | 1 条记录（只收 A），`example.com` → `93.184.216.34` |
| `uncompressed-name.bin` | NAME 完整写出 16 字节而非 2 字节指针 | 1 条记录，`www.0voice.com` → `192.0.2.1` |
| `nxdomain.bin` | ANCOUNT=0、RCODE=3 | 返回 0，`*domains` 为 NULL |
| `truncated-question.bin` | 截在 TTL 中间（len=40） | 返回 -1，`*domains` 为 NULL |
| `truncated-rdata.bin` | 截在 RDATA 里，只剩 2 字节（len=46） | 返回 -1，`*domains` 为 NULL |
| `bad-pointer.bin` | NAME 压缩指针指向自己 | 返回 -1，`*domains` 为 NULL |

另有 6 项钉住长度形参本身：`len=11`（报文头不完整）、`len=12`（自称 1 条记录
但一个字节的数据都没有）、`buffer` 为 NULL、`domains` 为 NULL，各断言返回 -1；
两条失败用例还同时断言 `*domains` 被置 NULL（调用前把它指向一个栈上的假结构，
所以"函数有没有真的清掉它"才是被验证的事情）。

**同一批字节喂给改动前的实现**（`git show HEAD:src/dns-client/src/dnsclient.c`
导出、函数改个名字避免与新声明冲突、用 ASan 编译），差别如下：

| 固定字节 | 改动前 | 改动后 |
|---|---|---|
| `a-simple.bin` | rc=1，结果正确 | 同 |
| `cname-chain.bin` | rc=1，结果正确 | 同 |
| `uncompressed-name.bin` | rc=0，**漏掉这条记录** | rc=1，正确 |
| `nxdomain.bin` | rc=0 | rc=0（现在显式置 `*domains = NULL`） |
| `truncated-question.bin` | rc=0，把截断报文当成"没有记录" | rc=-1 |
| `truncated-rdata.bin` | rc=1，地址被读成 `93.184.0.0` | rc=-1 |
| `bad-pointer.bin` | **栈溢出崩溃**（ASan: stack-overflow，`dns_parse_name` 自身递归） | rc=-1 |

`truncated-rdata.bin` 那一行值得单独说：旧实现把 RDATA 的前 2 个字节读完之后
继续读，读到的是同一个 512 字节缓冲区里 `fread` 没写过的位置（恰好是 0），
于是拼出一个 `93.184.0.0`。ASan **没有**报警——被读的地址仍然落在那个数组对象
内部，不算越界。这说明"加长度核对"和"开 ASan"是两道独立的防线，前者拦不住的
是"读到缓冲区里不属于这条报文的数据"，后者拦不住这种读，两者不能互相替代。

**没有**断言的：TTL（第 1、2 条未修，现在恒为 0，断言它等于 0 等于把缺陷写成
契约）、`dns_build_request` 的 `rlen` 不足时的行为（会越界写，见第 8 节）、
带结尾点的域名（长度字段与实际写入字节数不一致，第 9 条的另一面，另案处理）。

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
`ttl = htons(raw)` 那一行：TTL 是 4 字节字段（RFC 1035 §4.1.3），这里读的是
高 2 字节、且用了方向相反的转换函数（TTL 小于 65536 时高 2 字节恒为 0）。
这一条 2026-11-28 没有修——修它必须同时把 TTL 放进 `struct dns_item`，那和
"把 `printf` 搬回驱动"是同一件事（第 13 条）。

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

### 2026-11-28 解析侧加边界：长度形参 + 全程核对 + 指针必须回退

昨天拆库之后，"解析一个坏报文会怎样"这件事第一次变得可以问：以前它在 `main`
所在的文件里，喂进去的字节由 `recvfrom` 决定；现在任何调用方都能拿任意字节调它。
这一天的改动就是让它在坏输入上给出确定结果，而不是读越界或栈溢出。

三处改动，都在同一条线上（游标推进必须与真实字节数一致）：

| 改动 | 依据 | 影响 |
|---|---|---|
| `dns_parse_response` 加 `size_t len` 形参 | 函数无法知道缓冲区里有多少字节，边界就无从核对 | 签名变化；驱动把 `recvfrom` 的返回值直接传进来，并且只在 `n > 0` 时调用 |
| 指针推进改成 `size_t off` + 全程核对 | 原实现只按报文自称的长度 `ptr += datalen`，从没和"收到多少"比较 | 第 4 条关闭 |
| `dns_parse_name` → `dns_name_read`：不递归、指针必须目标 < 当前 | RFC 1035 §4.1.4：指针指向"先前出现过的"域名 | 第 5 条关闭，循环代替递归 |

顺带被这件事带出来的三处，属于"不改就没法改边界"：

1. **字段读取改用 `memcpy`。** 原写法 `*(unsigned short *)ptr` 在 `ptr` 未按 2 字节
   对齐时行为未定义（C11 6.3.2.3p7）；缓冲区是 `char` 数组，对齐要求为 1，
   除偏移 0 外都不保证对齐。现在统一走 `read_u16`，用 `memcpy` 取两字节。
2. **NAME 之后的游标按实际消耗推进。** 原来固定 `ptr += 2`，只有在 NAME 是压缩
   指针时才碰巧正确；`uncompressed-name.bin` 那份固定字节就是反例（旧实现返回
   0 条记录，见"测试与验证"的对照表）。
3. **失败路径自己收尾。** 新增了七八个 `goto bad`，已经 `calloc` 出来的
   `domain` / `ip` 必须在里面释放；不释放就是这次改动**新造**的一条泄漏，
   与第 6 条（正常路径漏掉的两处）不是同一件事。

`dns_name_read` 的返回值是"这个名字在记录里占多少字节"，不是文本长度——
这是它同时解决"边界"与"游标"两件事的关键：压缩指针只算 2 字节，
完整写出的名字算 `1 + len` 累加，调用方一句 `off += adv` 就对。

没有动的：TTL（第 1 条）、`type` 的 `htons`（第 2 条）、`printf`（第 13 条）。
前两条要等 TTL 进 `struct dns_item` 才能被断言，那和搬 `printf` 是同一件事。

### 2026-11-27 拆库：`dnsclient.h` + `libdnsclient.a` + 薄驱动

拆之前的状态：`dns_create_header` / `dns_create_question` / `dns_build_request` /
`dns_parse_response` / `dns_client_commit` 全是 `static`，外部只能通过 `main`
使用它。"发出去的字节对不对"没法断言，只能连真实服务器再人工比对 `dig` 的输出——
那既依赖网络，也把"服务器今天回什么"变成断言的输入。

拆之后的分界，只按"功能边界"切，不按"文件好不好看"切：

| 文件 | 内容 | 为什么在这儿 |
|---|---|---|
| `include/dnsclient.h` | 3 个结构体、5 个函数声明、`DNS_HEADER_SIZE` 与 `DNS_FLAGS_RD` | 调用方要用报文段就必须能看见结构体 |
| `src/dnsclient.c` | 实现；`is_pointer` 与 `dns_parse_name` 仍是文件内 `static` | 前者只判压缩指针的高 2 位，后者是 `dns_parse_response` 的递归辅助，都没有独立意义（2026-11-28 起后者换成 `dns_name_read`，同上） |
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

**编号是固定的，修完也不重新编号。** 源码注释、头文件、提交信息里都用
"第 8 节第 N 条"引用，重排会让这些引用悄悄指错对象——那种错不会触发任何检查。
已修的条目保留编号、标上"已修"并指向"实现记录"，新发现的缺陷续在后面编号。

**会算错**

1. **TTL 读错字段。** `ttl = htons(raw)` 把一个 4 字节字段
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

**会读越界（两条都已修，2026-11-28）**

4. **解析全程没有边界检查。** ~~只用报文内部的长度字段推进指针（`ptr += datalen`、
   `ptr += flag + 1`），从不与 `recvfrom` 的返回值比较；一份损坏或恶意的应答可以
   让 `ptr` 越过 1024 字节缓冲区。~~ **已修**：加 `size_t len` 形参，每一步推进
   都核对；三条截断用例钉住了它。见"实现记录 2026-11-28"。
5. **压缩指针没有防环。** ~~`dns_parse_name` 遇到指针即递归，深度不受限；
   指向自身的指针会让它无限递归，栈溢出。~~ **已修**：`dns_name_read` 用循环，
   并要求指针目标严格小于当前位置（RFC 1035 §4.1.4）；`bad-pointer.bin`
   在旧实现上是 ASan 报的 stack-overflow，现在返回 -1。

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
- [x] 补解析侧用例：`testdata/` 七份固定应答 → 断言域名、地址、条数（2026-11-28 完成，30 项）
- [x] 解析加边界检查：`size_t len` 形参，所有游标前进都对照真实字节数（2026-11-28 完成）
- [x] `dns_name_read` 要求压缩指针回退，循环代替递归，防环（2026-11-28 完成）
- [ ] 修 TTL（4 字节 + `ntohl`）与 `type`（`ntohs`）——要等 TTL 进 `struct dns_item`
      才有可断言的地方，与下面那条一起做（`memcpy` 取值已在 2026-11-28 完成）
- [ ] 把三条 `printf` 从库里搬回驱动（第 13 条），顺便让解析结果能被调用方
      自己格式化——这也是"加 TTL 到 `struct dns_item`"的前置
- [ ] 加超时（`SO_RCVTIMEO` 或 `poll`），把"服务器不回包"变成可判定的失败
- [ ] 释放 `question->name` 与 `list[i].domain` / `.ip`，并在 ASan/LSan 下跑一次真实查询
- [ ] 服务器地址与端口改成参数；`inet_addr` 换成 `inet_pton` + 返回值判断
- [ ] 支持 AAAA 与 MX，处理 TC 标志与 TCP 回退
- [ ] 事务 ID 改用 `/dev/urandom` 或 `getrandom`，并加源端口随机化说明
