/*
 * dnsclient.h —— dns-client 模块的对外接口
 *
 * 接口分三组，对应一次 DNS 查询的三个阶段：
 *
 *   报文构造   dns_create_header / dns_create_question / dns_build_request
 *   应答解析   dns_parse_response
 *   连接管理   dns_client_commit（建 socket、发请求、收应答、交给解析）
 *
 * 2026-11-27 从单文件程序拆出。拆分只动了链接属性与文件位置：函数体逐行沿用
 * 2026-11-23 入库的那份（原始素材 D:\6. DNS协议与请求\code\dns.c），
 * 已知缺陷一条没修——它们仍在 README 第 9 节，按第 10 节的顺序逐日处理。
 *
 * 为什么结构体是可见的、不做成不透明句柄：报文是按字段直接写内存的，
 * 调用方必须先有一块可写的 struct dns_header / struct dns_question。
 * 改成不透明句柄要同时引入一对构造/析构函数，那已经属于接口设计，
 * 会和"先修哪条缺陷"纠缠在一起，单独排一次做更清楚。
 *
 * 字节序：报文里所有 16 位、32 位字段都是网络字节序（大端，RFC 1035 §2.3.2
 * 规定传输格式为 big-endian）。本头文件里凡是 unsigned short 成员，
 * 存进去之前都要 htons，读出来之后都要 ntohs。struct dns_header 的成员名
 * 用的是报文里的字段名，不是主机序的语义。
 */

#ifndef DNSCLIENT_H
#define DNSCLIENT_H

/* 报文头定长（RFC 1035 §4.1.1）：
 * ID(2) + Flags(2) + QDCOUNT(2) + ANCOUNT(2) + NSCOUNT(2) + ARCOUNT(2)。 */
#define DNS_HEADER_SIZE		12

/* 查询报文的 Flags 取值：0x0100 只置了 RD 位（Recursion Desired，请求递归解析），
 * QR 位为 0（这是查询而非应答）。应答里服务器把 QR 置 1 并填 RA、RCODE。
 * 完整位图见 RFC 1035 §4.1.1。 */
#define DNS_FLAGS_RD		0x0100

struct dns_header {

	unsigned short id;			/* 事务 ID。RFC 1035 §4.1.1 只要求应答原样带回；
								 * 取值的不可预测性见 RFC 5452 §9.2 */
	unsigned short flags;

	unsigned short questions;	/* QDCOUNT，本模块恒为 1 */
	unsigned short answer;		/* ANCOUNT */

	unsigned short authority;	/* NSCOUNT */
	unsigned short additional;	/* ARCOUNT */

};

/* 一条待查询的问题（RFC 1035 §4.1.2：QNAME + QTYPE + QCLASS）。
 *
 * name 存的是 QNAME 的**线上编码**，不是点分字符串：每个 label 前一个长度字节，
 * 末尾一个 0x00 表示根。www.0voice.com 编码后是 16 字节
 *   03 'w' 'w' 'w' 07 '0' 'v' 'o' 'i' 'c' 'e' 03 'c' 'o' 'm' 00
 * length 是这 16 字节的长度，不是 hostname 的字符数。 */
struct dns_question {
	int length;
	unsigned short qtype;		/* 1 = A，5 = CNAME（RFC 1035 §3.2.2） */
	unsigned short qclass;		/* 1 = IN */
	unsigned char *name;		/* malloc 分配，长度 length，调用方负责 free */
};

/* 一条解析出来的 A 记录。domain 与 ip 都是 calloc 分配的 NUL 结尾字符串，
 * 调用方逐个 free。 */
struct dns_item {
	char *domain;
	char *ip;
};

/* 把 header 填成一份 A 记录递归查询的报文头：ID 取 random()，
 * Flags = htons(DNS_FLAGS_RD)，QDCOUNT = htons(1)，其余计数为 0。
 *
 * 返回 0；header 为 NULL 返回 -1。
 *
 * 缺陷（README 第 9 节第 3 条）：srandom(time(NULL)) 每次调用都重置种子，
 * 同一秒内的两次查询拿到同一个 ID；random() 本身是可预测的线性同余序列。
 * RFC 5452 §9.2 要求事务 ID 不可预测。 */
int dns_create_header(struct dns_header *header);

/* 把点分域名编码成 QNAME 写进 question，并填 qtype = htons(1)（A）、
 * qclass = htons(1)（IN）。name 由 malloc 分配，长度 strlen(hostname) + 2
 * （每个 label 一个长度字节，加末尾的根标签 0x00）。
 *
 * 返回 0；参数为 NULL 返回 -1；分配 name 失败返回 -2。
 *
 * 缺陷（第 9 条）：label 长度按 (unsigned char) 截断，不检查单 label ≤ 63 字节、
 * 整个域名 ≤ 253 字节的上限（RFC 1035 §2.3.4）。 */
int dns_create_question(struct dns_question *question, const char *hostname);

/* 按 header → QNAME → QTYPE → QCLASS 的顺序把查询报文写进 request，
 * 写入前把 request 的前 rlen 字节清零。
 *
 * 返回值是**写入的字节数**（合法输入下为 12 + question->length + 4），
 * 不是 0/-1 那种错误码风格；参数为 NULL 时返回 -1，两者靠符号区分。
 *
 * 缺陷（第 9 节第 4、11 条）：不检查 rlen 够不够，也不检查 header 里的
 * 计数字段与实际问题是否一致——报文长度由调用方自己记住。 */
int dns_build_request(struct dns_header *header, struct dns_question *question,
                      char *request, int rlen);

/* 解析一份应答报文，把其中的 A 记录收进 *domains。
 *
 * 返回值：写进 *domains 的记录条数；calloc 失败返回 -1。
 * *domains 是一个 answers 长度的数组（按 ANCOUNT 分配，按实际 A 记录数使用），
 * 每条的 domain / ip 各自 calloc，数组本身 calloc——两者都要 free。
 *
 * 两条必须先读的前提：
 *   1. 它拿不到 buffer 的长度，只按报文内部的长度字段推进指针
 *      （第 9 节第 4 条）。喂进来的字节数小于报文自称的长度就会读越界，
 *      所以调用方必须先确认真实收到的字节数。
 *   2. 它会把每条 A 记录的域名、地址、TTL 直接 printf 到 stdout。
 *      这段打印在拆分前就在 main 所在的同一个文件里，拆分时跟着解析一起走；
 *      库函数写 stdout 是接口上的缺陷，已列进待办（把打印搬回驱动）。
 *
 * 第 1 条缺陷的另一面：TTL 按 2 字节读且用错了字节序转换函数，
 * 打印出来的 TTL 恒为 0（第 9 节第 1、2 条）。 */
int dns_parse_response(char *buffer, struct dns_item **domains);

/* 向 DNS_SERVER_IP:DNS_SERVER_PORT 发一次 A 记录查询，收到应答后交给
 * dns_parse_response。
 *
 * 返回值是 recvfrom 的字节数（失败为 -1），不是解析出来的记录数——
 * 原始代码就是这个语义，拆分时没有改。解析结果在函数内部被丢弃
 * （只 free 了数组，没 free 每条的 domain / ip，第 9 节第 6 条）。
 *
 * 缺陷（第 7、10 条）：recvfrom 是阻塞调用且没有超时；服务器地址与端口是
 * 编译期常量，不能由调用方指定。 */
int dns_client_commit(const char *domain);

#endif /* DNSCLIENT_H */
