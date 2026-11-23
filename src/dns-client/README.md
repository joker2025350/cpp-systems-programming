# dns-client —— 手写 DNS 查询报文并解析应答

用 UDP 直接跟 DNS 服务器对话：自己拼查询报文、自己解析应答，中间不经过
`getaddrinfo` 这类解析库。代码来自 `D:\6. DNS协议与请求\code\dns.c`（2019 年的
课程示例），本轮只做到"入库 + 可构建 + 可运行"，**逻辑与原始示例一致，缺陷一个没修**。

当前状态一句话：**能查出结果，但报文级的正确性没被验证过，已知缺陷 12 条**（第 9 节）。
第二阶段按第 10 节的顺序逐日推进。

## 目录结构

```
src/dns-client/
├── Makefile              all / test / sanitize / clean
├── README.md             本文件
├── src/dns.c             全部实现 + main（单文件程序）
└── tests/smoke.sh        离线可判定的 3 项检查
```

少了 `include/`、`lib`、`cli`、`check`、`bench`、`tsan`，原因写在"对外接口"一节。

## 对外接口

**没有。** 这是一个单文件程序：`dns_create_header` / `dns_create_question` /
`dns_build_request` / `dns_parse_response` / `dns_client_commit` 全部是 `static`，
外部只能通过 `main` 使用它。

这不是设计选择，是记账：第二阶段的第一个任务就是把它拆成
`include/dnsclient.h` + `libdnsclient.a` + 薄驱动，让"报文构造"和"应答解析"
各自成为能单独调用的函数。不先拆，第 9 节的缺陷里有一半根本没法写用例——
比如 TTL 取错字段，现在只能靠"和 `dig` 的输出对不上"这种端到端比对发现，
拆出来之后输入一段固定的应答字节就能断言。

## 设计要点

DNS 查询报文（RFC 1035 §4.1）分三段，这个程序三段都手写：

1. **头部 12 字节定长**：ID(2) + Flags(2) + QDCOUNT(2) + ANCOUNT(2) + NSCOUNT(2) +
   ARCOUNT(2)。Flags 的 `0x0100` 表示 RD（Recursion Desired，请求递归解析）；
   `0x8000` 是 QR 位，查询报文必须为 0，应答里由服务器置 1。
2. **QNAME 不是点分字符串**：每段前面加一个长度字节，末尾一个 `0x00`。
   `www.0voice.com` 在报文里是 `03 77 77 77 07 30 76 6f 69 63 65 03 63 6f 6d 00`。
   `dns_create_question` 做的就是这件事：`strtok` 按 `.` 切开，逐段写长度再拷内容。
   所以分配的长度是 `strlen(hostname) + 2`——多出来的 1 字节是开头那个长度字节，
   另 1 字节是末尾的根标签。
3. **应答里的资源记录（RR）**：NAME（可能是压缩指针）+ TYPE(2) + CLASS(2) + TTL(4) +
   RDLENGTH(2) + RDATA。A 记录的 RDATA 是 4 字节 IPv4 地址；CNAME 的 RDATA 是另一个域名。

**域名压缩**是这段代码里唯一有点绕的地方：报文重复出现的域名后缀，第二次起用
2 字节指针代替，高 2 位是 `11`（所以 `(byte & 0xC0) == 0xC0` 就是"这是指针"），
低 14 位是"从报文起始算起的偏移"。`dns_parse_name` 遇到指针就跳到 `chunk + n`
继续读，读完 break——这也是它没有防环的原因（第 9 节第 5 条）。

## 构建

```bash
cd src/dns-client
make              # bin/dns-client
make test         # tests/smoke.sh（3 项，离线）
make sanitize     # ASan + UBSan 重建并跑同一套检查，产物在 bin/sanitize/
make clean
```

用法：

```bash
./bin/dns-client www.baidu.com
```

服务器地址与端口硬编码在 `src/dns.c` 的两个宏里：`DNS_SERVER_IP` 为
`114.114.114.114`，`DNS_SERVER_PORT` 为 53。改服务器要改源码重编——这本身是待办之一。

`-D_DEFAULT_SOURCE` 必须跟 `-D_POSIX_C_SOURCE=200809L` 一起给：`random` / `srandom` /
`bzero` 是 BSD/SVID 遗留接口，glibc 把它们藏在 `__USE_MISC` 之后，而只开
`_POSIX_C_SOURCE` 时 `__USE_MISC` 是关的。原始代码没给这两个宏，靠隐式声明编过去——
按 C11 6.5.2.2p6 隐式函数声明在 C99 之后已被移除，这里已经补上。

## 测试与验证

环境：内网 Ubuntu，Linux 6.8.0-149 / gcc 11.4.0。2026-11-23 测。

| 项目 | 结果 |
|---|---|
| 默认构建 | 0 条警告（`-std=c11 -Wall -Wextra -Wconversion -O2 -g`） |
| 加严构建 | 0 条警告（再加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow -Wstrict-prototypes -Wmissing-prototypes`） |
| `make test` | **3 项**，0 失败 |
| `make sanitize` | 0 条警告，3 项全过，无 ASan/UBSan 报告 |
| 真实查询 | `www.baidu.com` → `www.a.shifen.com` / `183.2.172.177`，与 `dig @114.114.114.114` 的应答集一致 |

3 项检查钉的是与网络无关的事实：二进制能起、缺参数时退出码非 0（实测 255）、
缺参数时没有输出。**它不验证报文对不对**，理由见 `tests/smoke.sh` 的文件头。

真实查询这一行同时暴露了第一处缺陷，也是最直观的一处：

```
$ ./bin/dns-client www.baidu.com
connect : 0
www.a.shifen.com has address 183.2.172.177
	Time to live: 0 minutes , 0 seconds

$ dig @114.114.114.114 +noall +answer www.baidu.com
www.baidu.com.		854	IN	CNAME	www.a.shifen.com.
www.a.shifen.com.	606	IN	A	183.2.172.177
```

同一个服务器、同一条记录，`dig` 报 606 秒，本程序报 0 分 0 秒。原因在
`ttl = htons(*(unsigned short *)ptr); ptr += 4;`：TTL 是 4 字节字段，
这里按 2 字节读、读到的还是高 2 字节（TTL 小于 65536 时恒为 0）。

## 性能

没有测量，也不打算在现阶段测。单次查询的墙钟时间几乎全是到 DNS 服务器的 RTT
（同一台服务器上实测几次都在毫秒级抖动），程序本身的开销只有一次 `malloc`
和两次 `memcpy`，量它得到的是网络噪声。有意义的是**批量查询的吞吐**，
但那要先有"复用一条 socket、并发多个 in-flight 查询"的结构，
属于第二阶段之后的事。

## 实现记录

### 2026-11-23 入库

从课程示例搬进仓库，只做让它在零警告前提下构建所需的五处编译层改动，
语义一行没变（逐条写在 `src/dns.c` 的文件头）：

| 改动 | 原因 |
|---|---|
| 补 `<time.h>` / `<arpa/inet.h>` / `<strings.h>`，并给 `-D_DEFAULT_SOURCE` | 原文靠隐式声明编过；C11 6.5.2.2p6 之后隐式声明不是合法写法 |
| 五个函数改 `static` | 只在本文件用且无对外声明，留着外部链接会触发 `-Wmissing-prototypes` |
| `dns_create_question` 补 `return 0` | 声明返回 `int` 却从末尾掉出去，按 C11 6.9.1p12 返回值未指定 |
| 显式转换（`size_t` → `int`、`char *` ↔ `unsigned char *`） | `-Wconversion` 与 `-Wincompatible-pointer-types` |
| `strncpy(qname, token, len+1)` → `memcpy(...)`，删掉从未使用的 `alen` | 拷贝长度正好是 `len` 个字符加 NUL，两者逐字节相同；`strncpy` 这种用法会被静态检查判为"可能无终止符" |

根 `Makefile` 自动发现本模块（`make list` 可见），不需要改根构建。
根 `README` 的模块表新增一行，并标注它目前是"入库态"而不是库。

## 已知局限

按"改起来要动哪里"排序，第一组是**会算错结果**的，第二组是**会读越界**的，
第三组是功能和工程上的缺口。每条都给了标准依据，不是"这样不好"。

**会算错**

1. **TTL 读错字段。** `ttl = htons(*(unsigned short *)ptr)` 把一个 4 字节字段
   （RFC 1035 §4.1.3：TTL 是 32 位无符号整数）当 2 字节读，读的还是高 2 字节。
   实测 `dig` 给 606 秒、本程序给 0（见"测试与验证"）。
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
   的其他变量。这是唯一一条"能打出来就是安全漏洞"的缺陷。
5. **压缩指针没有防环。** `dns_parse_name` 遇到指针即递归，深度不受限。
   指向自身的指针（偏移指回自己）会让它无限递归，栈溢出。

**功能与工程缺口**

6. **内存泄漏两处**：`list[cnt].domain` / `.ip` 各 `calloc` 一次，
   `dns_client_commit` 只 `free(dns_domain)`；`question->name` 也从未释放。
7. **没有超时。** `recvfrom` 是阻塞调用，服务器不回包就永久挂起。把
   `DNS_SERVER_IP` 改成不可达地址即可复现。修法：`SO_RCVTIMEO` 或 `poll`。
8. **只支持 A 记录**（`qtype` 硬编码 1）、只解析应答里的 A 与 CNAME，
   MX/TXT/AAAA 一律跳过；也不处理 TC 标志（应答被截断时应改用 TCP 重试）。
9. **不做输入校验**：域名超 253 字节或单个 label 超 63 字节时，
   `(unsigned char)len` 会静默截断，发出去的报文与输入不再对应。
10. **服务器与端口硬编码**，无法命令行指定；`inet_addr` 失败返回
    `INADDR_NONE`（`255.255.255.255`），与合法地址不可区分，且不支持 IPv6。
11. **`recvfrom` 用 1024 字节缓冲区**，而 `DNS_SERVER_IP` 那台服务器对大应答会置 TC 位。
    缓冲区与"最大 UDP 应答"之间没有关系说明。
12. **一次只查一个域名，串行。** 没有"批量 + 并发在飞（in-flight）"的结构，
    所以第 6 节说的吞吐量现在测不了。

## 待办

- [ ] 拆成 `include/dnsclient.h` + `libdnsclient.a` + 薄驱动（第二阶段第一件）
- [ ] 补"输入一段固定应答字节 → 断言解析结果"的用例：第 1、2、4、5 条都要靠它
- [ ] 修 TTL（4 字节 + `ntohl`）与 `type`（`ntohs`），并把两处取值改成
      `memcpy` 到局部变量再转换，消掉未对齐解引用（C11 6.3.2.3p7）
- [ ] 解析加边界检查：所有 `ptr` 前进都对照"已收到的字节数"核对
- [ ] `dns_parse_name` 加递归深度/偏移回退检查，防压缩指针成环
- [ ] 加超时（`SO_RCVTIMEO` 或 `poll`），把"服务器不回包"变成可判定的失败
- [ ] 释放 `question->name` 与 `list[i].domain` / `.ip`，并在 ASan/LSan 下跑一次真实查询
- [ ] 服务器地址与端口改成参数；`inet_addr` 换成 `inet_pton` + 返回值判断
- [ ] 支持 AAAA 与 MX，处理 TC 标志与 TCP 回退
- [ ] 事务 ID 改用 `/dev/urandom` 或 `getrandom`，并加源端口随机化说明
