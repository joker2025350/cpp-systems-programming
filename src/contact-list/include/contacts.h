/*
 * contacts.h —— 通讯录模块的公开接口
 *
 * 模块职责：用双向链表保存联系人的插入顺序，用哈希索引加速按姓名定位，
 * 提供增删查、遍历与文件存取。
 *
 * 设计上的三条约定：
 *   1. name 是主键。同一个通讯录里不允许出现重名，因此 contacts_remove
 *      与 contacts_find 的结果都是唯一确定的。
 *   2. 结构体成员是公开的，调用方可以在栈上直接分配 contact_book，
 *      不需要额外的构造函数；但约定不直接读写内部指针与计数，
 *      一律通过下面的函数访问。
 *   3. 所有函数都不使用全局状态，也不使用内部静态缓冲区，
 *      因此同一个进程里可以同时持有任意多个互相独立的通讯录。
 *
 * 线程模型：不同通讯录之间的操作互不影响；同一个 contact_book 上的
 * 并发调用需要调用方自己加锁，模块内部不做同步。
 */

#ifndef CONTACTS_H
#define CONTACTS_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 版本信息
 * ------------------------------------------------------------------
 * 1.1.0：加入哈希索引，contact 与 contact_book 各多出字段，
 *        ABI 不兼容（调用方必须与库一起重编），源码级兼容。
 * 1.2.0：删除时按装载因子收缩桶表；新增重分配计数。
 *        ABI 不兼容：contact_book 末尾多一个 size_t，contacts_index_stats
 *        末尾也多一个；contact 本身没变。contact_book 由调用方在栈上分配，
 *        因此库与调用方必须一起重编。
 * 1.3.0：新增 contacts_order（按姓名升序取指针数组）。
 *        只增加一个函数，结构体一个字节没动，因此 ABI 兼容；
 *        用旧的库配新的头文件编译出的程序仍然能跑（不要反过来）。
 */

#define CONTACTS_VERSION_MAJOR 1
#define CONTACTS_VERSION_MINOR 3
#define CONTACTS_VERSION_PATCH 0
#define CONTACTS_VERSION_STRING "1.3.0"

/* ------------------------------------------------------------------
 * 字段容量
 * ------------------------------------------------------------------
 * 两个值都包含结尾的 '\0'，因此姓名的实际可用长度是 63 字节。
 *
 * 这两个宏是公开的，调用方可以直接用 CONTACT_NAME_MAX 声明临时缓冲区，
 * 不必硬编码一个数字——硬编码的后果是模块扩容后调用方的缓冲区悄悄变小。
 *
 * 用 #ifndef 包一层，允许在编译命令上用 -DCONTACT_NAME_MAX=n 覆盖。
 * 注意：库与调用方必须用同一个取值编译，否则 ABI 不一致。
 */
#ifndef CONTACT_NAME_MAX
#define CONTACT_NAME_MAX  64
#endif

#ifndef CONTACT_PHONE_MAX
#define CONTACT_PHONE_MAX 64
#endif

/* ------------------------------------------------------------------
 * 数据结构
 * ------------------------------------------------------------------ */

/*
 * 一个联系人节点。
 *
 * prev/next 是链表指针，hash_next 是哈希索引的桶链指针，三者都由模块维护。
 * 调用方通过 contacts_first / contacts_next 遍历，不要自己沿着
 * next 或 hash_next 走——那样写出来的代码会依赖"节点在堆上、
 * 链表双向、索引用分离链接"这些实现细节。
 */
typedef struct contact {
	char name[CONTACT_NAME_MAX];
	char phone[CONTACT_PHONE_MAX];
	struct contact *prev;
	struct contact *next;
	struct contact *hash_next;   /* 索引桶链，模块内部维护 */
} contact;

/*
 * 一个通讯录。
 *
 * 内部同时维护两份结构：
 *   - 双向链表（head / tail），保存插入顺序，遍历与保存靠它；
 *   - 哈希索引（buckets / nbuckets），保存"姓名 -> 节点"的映射，
 *     查找、查重、删除靠它。
 *
 * 两份结构的一致性由 contacts_add / contacts_remove / contacts_clear
 * 三个入口维护，调用方不需要（也不应该）碰后面两个字段。
 * 保留 head 与 tail 两个指针，使追加是 O(1)，
 * 且遍历顺序与插入顺序一致。只保留 head 也能工作，
 * 但追加就要么遍历到表尾（O(n)），要么头插（顺序反转）。
 */
typedef struct {
	contact *head;
	contact *tail;
	size_t   count;
	contact **buckets;    /* 桶数组，长度是 2 的幂；NULL 表示还没有索引 */
	size_t   nbuckets;    /* 桶数量，0 表示还没有索引 */
	size_t   rehashes;    /* 桶表重分配次数（扩容 + 收缩），从 contacts_init 起累计 */
} contact_book;

/* ------------------------------------------------------------------
 * 状态码
 * ------------------------------------------------------------------
 * 沿用 C 语言的惯例：0 表示成功，非 0 表示失败。
 * CONTACTS_OK 的值固定为 0，因此可以直接用 if (status) 判断失败。
 *
 * 返回 CONTACTS_ERR_OPEN / READ / WRITE / CLOSE 时，errno 保留
 * 导致失败的系统错误号。其余状态码下 errno 无意义。
 */
typedef enum {
	CONTACTS_OK             = 0,   /* 成功 */
	CONTACTS_ERR_PARAM      = 1,   /* 参数非法，例如传入了 NULL */
	CONTACTS_ERR_NOMEM      = 2,   /* 内存分配失败 */
	CONTACTS_ERR_OPEN       = 3,   /* 无法打开文件 */
	CONTACTS_ERR_READ       = 4,   /* 读取过程中出错 */
	CONTACTS_ERR_WRITE      = 5,   /* 写入或改名失败 */
	CONTACTS_ERR_CLOSE      = 6,   /* 关闭文件时出错 */
	CONTACTS_ERR_FORMAT     = 7,   /* 字段内容不符合格式约定，或行无法解析 */
	CONTACTS_ERR_TOOLONG    = 8,   /* 字段超出容量 */
	CONTACTS_ERR_NOTFOUND   = 9,   /* 没有这个联系人 */
	CONTACTS_ERR_DUPLICATE  = 10   /* 姓名已存在 */
} contacts_status;

/* 载入模式 */
typedef enum {
	CONTACTS_LOAD_APPEND  = 0,     /* 追加到现有内容之后 */
	CONTACTS_LOAD_REPLACE = 1      /* 先清空再载入 */
} contacts_load_mode;

/*
 * 载入结果的统计。
 *
 * 载入不会因为某一行有问题就整体失败——一个格式不对的行不应该让
 * 另外 999 行都读不进来。坏行被跳过并记在这里，由调用方决定是
 * 忽略还是报警。
 *
 * skipped 只统计"有内容但不合法"的行；空行被静默忽略，不计入。
 * first_bad_line 是第一个被跳过的行的行号（从 1 开始），
 * 0 表示没有任何行被跳过。
 */
typedef struct {
	size_t loaded;
	size_t skipped;
	size_t first_bad_line;
} contacts_load_result;

/* ------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------ */

/*
 * 把通讯录初始化为空。contact_book 在栈上分配之后必须先调用它，
 * 否则 head/tail/count 是未初始化的值，后续操作会解引用野指针。
 */
void contacts_init(contact_book *book);

/*
 * 释放全部节点与哈希索引的桶数组，回到空状态。可以重复调用。
 * 只是释放内容、不清零结构体本身，因此之后还能继续使用这本书——
 * 下一次 contacts_add 会重新建索引（可能因此返回 CONTACTS_ERR_NOMEM）。
 */
void contacts_clear(contact_book *book);

/* contacts_clear 的别名，用名字表达"这本书到此为止"的语义。 */
void contacts_destroy(contact_book *book);

/* 当前联系人数量。book 为 NULL 时返回 0。 */
size_t contacts_count(const contact_book *book);

/* ------------------------------------------------------------------
 * 增删查
 * ------------------------------------------------------------------ */

/*
 * 追加一个联系人。
 *
 * 姓名字段需要满足：非空、长度小于 CONTACT_NAME_MAX、
 * 不含逗号与换行符。电话号码的要求相同。
 * 这些限制不是随意定的：保存格式用逗号分隔两个字段，
 * 字段里出现逗号会让"保存再读取"无法还原原来的值。
 * 与其在存取环节做转义，不如在入口处禁止——通讯录的字段
 * 本来就不需要逗号。
 *
 * 返回：
 *   CONTACTS_OK               成功，新节点追加到表尾
 *   CONTACTS_ERR_PARAM        book/name/phone 为 NULL
 *   CONTACTS_ERR_FORMAT       字段为空或含逗号、换行符
 *   CONTACTS_ERR_TOOLONG      字段超出容量
 *   CONTACTS_ERR_DUPLICATE    同名联系人已存在
 *   CONTACTS_ERR_NOMEM        首次建立索引或分配节点时内存不足
 *
 * 失败时通讯录保持调用前的状态：不会留下"节点已挂上但索引里没有"的记录。
 * 索引扩容失败不进这个分支——那种情况只降级为链更长的查找，仍然返回成功。
 */
contacts_status contacts_add(contact_book *book, const char *name, const char *phone);

/*
 * 按姓名删除。找到并释放节点后返回 CONTACTS_OK，
 * 没有这个姓名时返回 CONTACTS_ERR_NOTFOUND。
 *
 * 删除后指针不再有效，调用方不要在删除之后继续使用之前拿到的
 * const contact *——这是释放后使用，ASan 会报 heap-use-after-free。
 *
 * 删除可能顺带把桶表减半（装载因子低于 1/4 时），这一次重挂是 O(n)，
 * 但均摊到导致它发生的那些删除上仍是 O(1)。重挂只改桶链指针，不移动节点，
 * 因此本次删除之外的其它 const contact * 仍然有效——不过这个保证不建议依赖：
 * 需要跨删除持有指针的调用方应当假定指针已失效。
 */
contacts_status contacts_remove(contact_book *book, const char *name);

/*
 * 按姓名查找。找不到返回 NULL。
 *
 * 平均 O(1)：内部有哈希索引，按姓名直接定位到桶链，链长在装载因子
 * 不超过 1.0 的前提下平均 1 条多一点。最坏情形退化为 O(n)，
 * 条件是大量姓名落在同一个桶上（哈希洪泛，见 README 9.7）。
 * 索引是内部结构，不影响任何返回值。
 *
 * 返回的指针在下一次 contacts_add / contacts_remove / contacts_clear
 * 之前保持有效。注意 contacts_add 可能触发 malloc，也可能触发索引扩容
 * 并在内部重排桶链，但两者都不会移动已有节点，因此已有的指针在追加之后
 * 仍然有效；只有删除和清空才会让它失效。
 */
const contact *contacts_find(const contact_book *book, const char *name);

/* ------------------------------------------------------------------
 * 遍历
 * ------------------------------------------------------------------
 * 游标式遍历，而不是"传一个回调函数进去"：
 *   - 调用方可以在任意位置提前结束，不需要靠回调的返回值表达"停下来"
 *   - 不需要为每种用途定义一个回调类型
 *
 * 用法：
 *     for (const contact *c = contacts_first(book); c != NULL; c = contacts_next(c)) {
 *         ...
 *     }
 *
 * 遍历顺序是插入顺序。遍历期间不要增删节点，否则游标会失效。
 */

const contact *contacts_first(const contact_book *book);
const contact *contacts_next(const contact *c);

/* ------------------------------------------------------------------
 * 排序
 * ------------------------------------------------------------------
 * 遍历给的是插入顺序，那是"用户先后录入了什么"；按姓名排序是另一个
 * 需求，两者不能同时由同一条链表满足，所以排序做成一次性取一批指针：
 * 调用方给一块数组，模块把全部节点的指针按姓名升序填进去。
 *
 * 为什么由调用方给数组，而不是模块返回一个数组：
 *   - 返回 malloc 出来的数组，就把释放责任推给调用方，还得再定义并导出
 *     一个释放函数，否则调用方无从知道该用 free 还是别的释放方式；
 *   - 调用方自己给缓冲区，模块内部不产生新的分配点，也就不引入
 *     "取指针数组时内存不足"这条新的失败路径（分配失败在调用方那一侧，
 *     它有完整的上下文：可以退化成按插入顺序输出，或者少排几条）。
 * 代价是调用方要知道元素个数，也就是先调一次 contacts_count。
 *
 * 排序键是姓名的字节序，即 strcmp 的语义。按 C11 7.24.4.4，
 * strcmp 把两个字符串的字节当作 unsigned char 逐个比较，与 LC_COLLATE
 * 无关。具体后果："Zoe" 排在 "alice" 之前（'Z'=0x5A < 'a'=0x61），
 * 中文按 UTF-8 编码字节排序，而不是拼音序。选字节序的理由写在 README 3.12：
 * 排序结果必须只取决于数据本身，不能取决于进程启动时恰好设了什么 locale。
 *
 * 这个函数是 const 的：不改链表、不改索引、不移动节点，
 * 因此返回的指针与 contacts_find 拿到的是同一批对象。
 * 但这些指针同样在下一次 contacts_remove / contacts_clear 之后失效。
 *
 * 返回：
 *   CONTACTS_OK         成功；out[0..count-1] 是按姓名升序的全部指针
 *   CONTACTS_ERR_PARAM  book 为 NULL；cap 小于 contacts_count；
 *                       或者 count 大于 0 而 out 为 NULL
 * 空表（count 为 0）返回 OK，此时 out 可以是 NULL，cap 也不看。
 * 参数类失败一个元素都不写，调用方看到的仍是调用前的值。
 *
 * 还有一个同样返回 PARAM 的情形：count 与实际遍历出的节点数不一致。
 * 参数并没有错，是模块内部不变量（见 contacts_index_get 的说明）被破坏，
 * 通常意味着内存被非法改写。此时 out 的内容不可用——正常调用方不会遇到，
 * 但如果看到了，应当按"数据损坏"处理，而不是当成参数写错了。
 *
 * 复杂度：O(n log n) 次比较 + O(n) 次写指针。额外内存全在调用方的数组里，
 * 即 n * sizeof(const contact *)（64 位下 8 字节/条）。
 */
contacts_status contacts_order(const contact_book *book,
                               const contact **out, size_t cap);

/* ------------------------------------------------------------------
 * 文件存取
 * ------------------------------------------------------------------
 * 写出的每一行长这样：
 *
 *     name: <姓名>, phone: <电话>
 *
 * 这是模块唯一规定的格式。读取时按"标签 + 冒号"识别字段，
 * 标签本身被忽略，因此下面两种历史写法也能正确读入：
 *
 *     name: liaoqf,telephone: 23456543789      （逗号后无空格，标签不同）
 *     name: king, phone: 344567754             （逗号后有空格）
 *
 * 两个字段都是"取标签冒号之后、到下一个分隔符之前的全部内容并去掉
 * 两端空白"，不依赖"第一个空格在哪"这种偶然位置。
 * 因此写出去的内容一定读得回来——存取互逆。
 */

/*
 * 把整本书写到 path。
 *
 * 实现上先写 path.tmp，关闭成功后再 rename 到 path。这样写到一半
 * 失败（磁盘满、进程被杀）不会破坏原来的文件内容。代价是要求
 * path 所在目录可写——不只是文件本身可写。
 *
 * POSIX 的 rename 在目标已存在时原子替换；Windows 上同名 rename
 * 会失败，需要先用 ReplaceFile 之类的接口，本模块不处理这种差异。
 *
 * book 为空时写出的文件是 0 字节，不是错误。
 */
contacts_status contacts_save(const contact_book *book, const char *path);

/*
 * 从 path 载入。mode 决定是追加还是先清空。
 *
 * 单行超过约 510 字节、字段无法解析、字段内容不合法、与已有记录重名，
 * 这些情况都只跳过该行并计入 result->skipped，不中断整个载入过程。
 *
 * 例外是内存不足：它与数据内容无关，继续读下去每一行都会失败。
 * 此时立即中止，返回 CONTACTS_ERR_NOMEM，已经载入的部分保留，
 * result 报告中止时已载入与已跳过的条数——而不是把剩下的行
 * 全部记成"格式坏行"再返回成功。
 *
 * 只有文件级别的错误（打不开、读出错、关闭出错）与内存不足才返回非 0。
 *
 * result 可以为 NULL，表示不关心统计结果。
 *
 * 注意：返回非 0 时，mode 为 REPLACE 的情况下原内容已经被清空。
 * 需要"失败时保持原样"的调用方应当先载入到另一本书再交换，
 * 本模块不提供事务语义。
 */
contacts_status contacts_load(contact_book *book, const char *path,
                              contacts_load_mode mode,
                              contacts_load_result *result);

/* ------------------------------------------------------------------
 * 辅助
 * ------------------------------------------------------------------ */

/*
 * 把状态码转成一句可读的英文描述。
 * 传入未知值时返回 "unknown status"，不会返回 NULL——
 * 调用方 printf("%s", ...) 时不必再判空。
 */
const char *contacts_status_string(contacts_status status);

/*
 * 校验一个字段值是否满足 contacts_add 的要求。
 * 供命令行层在调用 contacts_add 之前给出具体提示用；
 * 单独暴露是因为"字段不合法"和"重名"要给用户不同的指引。
 *
 * capacity 传入目标字段的容量（含 '\0'），例如 CONTACT_NAME_MAX。
 */
contacts_status contacts_check_field(const char *value, size_t capacity);

/* ------------------------------------------------------------------
 * 诊断
 * ------------------------------------------------------------------
 * 索引是内部实现，正常调用方不需要关心。这组接口存在的目的是让
 * "索引内部状态是否自洽""桶的分布是否合理"可以被测试与基准测量，
 * 而不是只能靠"跑得快所以大概是对的"来推断。
 */

/*
 * 索引的当前统计。
 *
 * buckets   桶数量，0 表示还没有索引（空表、删空之后，或首次插入就失败）
 * entries   索引里挂着的节点总数，正常情况下等于 contacts_count
 * used      非空桶的数量
 * max_chain 最长的一条桶链上的节点数
 * rehashes  桶表重分配的累计次数（扩容 + 收缩），从 contacts_init 起算
 *
 * entries 与 contacts_count 不等，说明索引与链表已经不一致——
 * 这是断言用的不变量，不是可以忽略的统计噪声。
 *
 * 桶数随插入与删除变化：装载因子超过 1.0 时翻倍，低于 1/4 时减半，
 * 下限 16。因此"桶数"不是稳定的量，只有"桶数是 2 的幂""装载因子落在
 * 1/4 到 1.0 之间"这两个性质可以断言。
 *
 * rehashes 用来回答"这套阈值会不会在边界上反复抖动"：抖动就是同一批数据
 * 上重分配次数不断上涨。它是确定值，不像耗时那样受调度与机器频率影响，
 * 因此适合写成断言而不是基准。
 *
 * 空表调用返回 CONTACTS_OK，五个字段全为 0（rehashes 只在 contacts_init
 * 之后归零；contacts_clear 不重新分配，所以不清零）。
 */
typedef struct {
	size_t buckets;
	size_t entries;
	size_t used;
	size_t max_chain;
	size_t rehashes;
} contacts_index_stats;

contacts_status contacts_index_get(const contact_book *book,
                                   contacts_index_stats *out);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* CONTACTS_H */
