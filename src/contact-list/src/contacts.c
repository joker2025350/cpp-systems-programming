/*
 * contacts.c —— 通讯录模块的实现
 *
 * 分四块：
 *   1. 双向链表、节点生命周期与哈希索引
 *   2. 一行的解析与一张表的序列化
 *   3. 文件级的读写，含错误分类与 errno 保存
 *   4. 辅助
 *
 * 两份结构描述同一批节点：双向链表保存插入顺序（遍历、保存都靠它），
 * 哈希索引保存"姓名 -> 节点"的映射（查找、查重、删除都靠它）。
 * 两份结构的一致性由本文件的三个入口维护：contacts_add、contacts_remove、
 * contacts_clear。除此之外没有任何代码改链表指针或桶链指针。
 */

#include "contacts.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * 读文件时单行的缓冲区大小。
 *
 * 写出的最长一行是 "name: " + 63 + ", phone: " + 63 + '\n' = 146 字节，
 * 512 有足够余量。超出这个长度的行会被判为过长并整行丢弃，
 * 而不是截断成两条记录——截断会把一行坏数据变成两行"看起来正常"的数据，
 * 比直接报错更难排查。
 */
#define CONTACTS_LINE_MAX 512

/* ------------------------------------------------------------------
 * 1. 链表、节点生命周期与哈希索引
 * ------------------------------------------------------------------ */

/*
 * 初始桶数。桶数始终是 2 的幂，取下标时用按位与代替取模：
 * 取模在 x86 上是 20~40 个周期的整除指令，只在除数是编译期常量时
 * 才被优化成乘法；桶数在运行期变化，编译器替代不了，按位与没有这个问题。
 */
#define CONTACTS_BUCKETS_MIN ((size_t)16)

/*
 * 收缩阈值：装载因子低于 1/CONTACTS_SHRINK_DIV（即 1/4）时桶数减半。
 *
 * 取 1/4 的理由是让扩容与收缩把表都留在"半满"状态，两个方向各有 2 倍余量：
 *   - 扩容发生在装载因子刚超过 1.0，翻倍后回到 0.5 附近；
 *   - 收缩取 1/4 时，减半后装载因子升到 0.5 附近。
 *
 * 若收缩也取 1/2，减半后装载因子会回到 1.0——表是满的。这不至于造成
 * 无限抖动（本实现的扩容判据是"插入后条数 > 桶数"，减半后 count <= nbuckets-1，
 * 要连插两条才会再扩容，实测 2000 轮"删一条加一条"只有 1 次重分配），
 * 但代价是刚还回去的内存立刻又处在"下一条记录就要拿回来"的状态，
 * 而插入路径上的最坏延迟正好落在内存最紧张的时刻。取 1/4 就没有这个窗口。
 * 这段推理的实测过程写在 README 3.11：先按教科书假设"1/2 会抖动"，
 * 写测试一量，发现抖动并不发生，于是把理由换成了实际成立的那一条。
 */
#define CONTACTS_SHRINK_DIV ((size_t)4)

/*
 * FNV-1a 64 位哈希。
 *
 * 定义（可独立复现）：h = 0xcbf29ce484222325，对输入字节流逐个执行
 *     h ^= byte;  h *= 0x100000001b3;   （乘数与偏移基都是 FNV 规范给的）
 *
 * 选它的理由不是"最快"，而是三条实际约束：
 *   1. 逐字节处理，不需要预知长度，也不依赖对齐——姓名是 char[64]，
 *      长度随内容变化，逐字节循环不必先 strlen。
 *   2. 不引入运行期随机种子，因此同一批姓名每次都落在同一组桶里，
 *      测试与基准的结果可复现（代价见 README 9.7 的哈希洪泛）。
 *   3. 乘法每步都作用于 h 的全部 64 位，输入的低位变化会扩散到高位，
 *      因此下面用低若干位做桶下标不会丢掉熵。
 *
 * 注意 0x100000001b3 的低字节是 0xb3（奇数）。若乘数是偶数，
 * 每步乘法都会把 h 的最低位置 0，低位的熵会被逐位清空，
 * 用低 b 位取下标就会退化——这是选乘数时必须满足的整除性要求。
 */
static uint64_t hash_name(const char *name) {
	const unsigned char *p = (const unsigned char *)name;
	uint64_t             h = 0xcbf29ce484222325u;

	while (*p != '\0') {
		h ^= (uint64_t)(*p);
		h *= 0x100000001b3u;
		p++;
	}
	return h;
}

/* 给定桶数求下标。nbuckets 必须是 2 的幂且非 0。 */
static size_t bucket_index(size_t nbuckets, const char *name) {
	return (size_t)(hash_name(name) & (uint64_t)(nbuckets - 1u));
}

/*
 * 分配并清零一个桶数组。
 *
 * 用 calloc 而不是 malloc + memset：桶数组的初值必须是 NULL 指针，
 * 全零位模式在 C 标准里不保证等于空指针（C11 6.3.2.3p3 只规定了
 * 空指针常量转成指针的结果，没规定其位模式），但 POSIX 与所有
 * 实际平台都满足，且 calloc 是获得"零位模式内存"的唯一标准途径。
 * contacts_init 里对 head/tail 显式赋 NULL，这里依赖 calloc 的零值。
 */
static contact **buckets_alloc(size_t n) {
	return (contact **)calloc(n, sizeof(contact *));
}

/*
 * 把全部节点重新挂到 want 个桶的新表上，并释放旧表。want 必须是 2 的幂。
 * 返回 0 表示成功，-1 表示新表分配失败——此时旧表原样保留，调用方不需要
 * 做任何清理。
 *
 * 扩容与收缩共用这一段，因为它们要做的事完全相同：换一张表，按新桶数重算
 * 每个节点的下标。区别只在 want 是 nbuckets * 2 还是 nbuckets / 2。
 *
 * 重挂时只改 hash_next，不动 prev / next。
 * 若这里连 prev/next 一起重排，链表顺序就变成"按桶分组"，
 * 遍历顺序不再等于插入顺序，save 写出的文件也跟着变——
 * 那是一条会破坏文件契约的捷径。
 */
static int index_rehash(contact_book *book, size_t want) {
	contact **fresh = buckets_alloc(want);
	contact  *item;

	if (fresh == NULL) {
		return -1;
	}

	for (item = book->head; item != NULL; item = item->next) {
		size_t i = bucket_index(want, item->name);

		item->hash_next = fresh[i];
		fresh[i] = item;
	}

	free(book->buckets);
	book->buckets  = fresh;
	book->nbuckets = want;
	book->rehashes++;
	return 0;
}

/*
 * 为"即将插入一个节点"准备索引。返回 0 表示可用，-1 表示内存不足。
 *
 * 三种情况：
 *   1. 还没有索引：按 CONTACTS_BUCKETS_MIN 建一个空表；
 *   2. 现有桶数装得下（插入后 count <= nbuckets，即装载因子 <= 1.0）：什么都不做；
 *   3. 装不下：桶数翻倍，把已有节点重新挂到新表上。
 *
 * 第 3 步扩容失败时**不返回错误**，继续用旧表。理由：桶数不足只是链变长、
 * 查找常数变大，结果仍然正确；为了让"扩容"这种纯性能优化去否决一次
 * 合法的插入，是把可用性问题变成了正确性问题。
 *
 * 而第 1 步失败必须上报：没有索引也仍然能用线性扫描工作，
 * 但每次插入都要 O(n) 查重，把 O(n²) 的行为悄悄还给调用方，
 * 比直接返回 CONTACTS_ERR_NOMEM 更难排查。
 */
static int index_prepare(contact_book *book) {
	if (book->buckets == NULL) {
		return (index_rehash(book, CONTACTS_BUCKETS_MIN) == 0) ? 0 : -1;
	}

	if (book->count + 1u <= book->nbuckets) {
		return 0;
	}

	(void)index_rehash(book, book->nbuckets * 2u);   /* 失败就是降级，不影响返回值 */
	return 0;
}

/*
 * 按需收缩桶表。调用点在 contacts_remove 的末尾，也就是"删除已经完成、
 * count 已经减一"之后。
 *
 * 三种情形：
 *   1. 还没有索引（从未插入过，或上一次删空已经把表释放）：什么都不做；
 *   2. count 归零：直接释放桶表，回到与 contacts_init 完全一致的状态。
 *      头文件里 buckets == 0 的定义就是"还没有索引"，空表留着 64K 个桶
 *      与这个约定不符，也与 contacts_clear 的语义不符；
 *   3. 装载因子低于 1/4：桶数减半，直到条件不再成立或触到 CONTACTS_BUCKETS_MIN。
 *
 * 用 while 而不是 if，是因为一次删除可能跨过好几档：桶表 256 个桶、只剩
 * 2 条记录时，减半到 128 之后 2 < 32 仍然成立，继续减半才对。正常使用时
 * 每次删除只会减一档（装载因子从 1/4 降到 1/4 以下，减半后回到 1/2），
 * 因此循环绝大多数情况下只跑一次判断。
 *
 * 减半的分配失败按降级处理：桶表保持原样，删除已经成功，语义不受影响——
 * 与扩容失败同一套理由。因此本函数没有返回值。
 */
static void index_shrink(contact_book *book) {
	if (book->buckets == NULL) {
		return;
	}

	if (book->count == 0) {
		free(book->buckets);
		book->buckets  = NULL;
		book->nbuckets = 0;
		return;
	}

	while (book->nbuckets > CONTACTS_BUCKETS_MIN
	       && book->count < book->nbuckets / CONTACTS_SHRINK_DIV) {
		if (index_rehash(book, book->nbuckets / 2u) != 0) {
			return;     /* 降级：继续用当前这张表 */
		}
	}
}

void contacts_init(contact_book *book) {
	if (book == NULL) {
		return;
	}
	book->head     = NULL;
	book->tail     = NULL;
	book->count    = 0;
	book->buckets  = NULL;
	book->nbuckets = 0;
	book->rehashes = 0;
}

void contacts_clear(contact_book *book) {
	contact *item;

	if (book == NULL) {
		return;
	}

	item = book->head;
	while (item != NULL) {
		/*
		 * next 必须在 free 之前取出来。写成
		 *     free(item); item = item->next;
		 * 是在已经释放的内存上读 item->next，典型的释放后使用。
		 */
		contact *next = item->next;
		free(item);
		item = next;
	}

	/*
	 * 桶数组与节点一起释放。不保留空表来"省一次分配"，是因为
	 * clear 的语义就是"释放全部内容"：留着 64 万条的桶数组
	 * （512 KB）而对外报告"清空了"，是与语义不符的内存占用。
	 * 代价是 clear 之后第一次 add 要重新分配一次，可忽略。
	 */
	free(book->buckets);
	book->buckets  = NULL;
	book->nbuckets = 0;

	book->head  = NULL;
	book->tail  = NULL;
	book->count = 0;
}

void contacts_destroy(contact_book *book) {
	contacts_clear(book);
}

size_t contacts_count(const contact_book *book) {
	return (book == NULL) ? 0u : book->count;
}

const contact *contacts_first(const contact_book *book) {
	return (book == NULL) ? NULL : book->head;
}

const contact *contacts_next(const contact *c) {
	return (c == NULL) ? NULL : c->next;
}

const contact *contacts_find(const contact_book *book, const char *name) {
	const contact *item;

	if (book == NULL || name == NULL) {
		return NULL;
	}

	/*
	 * 没有索引时退回线性扫描。这条路径只在"空表"与"首次插入失败"两种
	 * 情形下走到，结果必须与有索引时完全一致——索引是加速手段，
	 * 不参与定义语义。
	 */
	if (book->buckets == NULL) {
		for (item = book->head; item != NULL; item = item->next) {
			if (strcmp(item->name, name) == 0) {
				return item;
			}
		}
		return NULL;
	}

	for (item = book->buckets[bucket_index(book->nbuckets, name)];
	     item != NULL;
	     item = item->hash_next) {
		if (strcmp(item->name, name) == 0) {
			return item;
		}
	}

	return NULL;
}

contacts_status contacts_check_field(const char *value, size_t capacity) {
	size_t len;
	size_t i;

	if (value == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	len = strlen(value);
	if (len == 0) {
		return CONTACTS_ERR_FORMAT;
	}
	if (capacity == 0 || len >= capacity) {
		return CONTACTS_ERR_TOOLONG;
	}

	/*
	 * 逗号是保存格式里的字段分隔符，换行是记录分隔符。
	 * 字段里出现这两个字符，写出去的行就再也无法唯一地解析回来，
	 * 所以必须在入口处拒绝，而不是在存取环节做转义。
	 */
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)value[i];
		if (c == ',' || c == '\n' || c == '\r') {
			return CONTACTS_ERR_FORMAT;
		}
	}

	return CONTACTS_OK;
}

contacts_status contacts_add(contact_book *book, const char *name, const char *phone) {
	contacts_status status;
	contact        *item;
	size_t          bucket;

	if (book == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	status = contacts_check_field(name, CONTACT_NAME_MAX);
	if (status != CONTACTS_OK) {
		return status;
	}
	status = contacts_check_field(phone, CONTACT_PHONE_MAX);
	if (status != CONTACTS_OK) {
		return status;
	}

	/* name 是主键，重名会让查找与删除的结果变得不确定 */
	if (contacts_find(book, name) != NULL) {
		return CONTACTS_ERR_DUPLICATE;
	}

	/*
	 * 先备好索引，再分配节点。顺序反过来也能工作，但会产生一个
	 * "节点已分配、索引建不起来"的状态，那时只能把节点释放掉再返回失败；
	 * 而现在索引先行，节点分配失败时桶表是一个空表，仍然自洽。
	 */
	if (index_prepare(book) != 0) {
		return CONTACTS_ERR_NOMEM;
	}

	item = (contact *)malloc(sizeof *item);
	if (item == NULL) {
		return CONTACTS_ERR_NOMEM;
	}

	/*
	 * 先全零，再拷贝字段。
	 * contacts_check_field 已经保证字段长度小于容量，所以拷贝之后
	 * 缓冲区末尾的 '\0' 天然存在，不需要再手动补。
	 */
	memset(item, 0, sizeof *item);
	memcpy(item->name, name, strlen(name));
	memcpy(item->phone, phone, strlen(phone));

	/* 追加到表尾：维护 tail 指针，使插入是 O(1) 且保持插入顺序 */
	item->prev = book->tail;
	item->next = NULL;

	if (book->tail != NULL) {
		book->tail->next = item;
	} else {
		book->head = item;      /* 空表时新节点同时是表头 */
	}
	book->tail = item;
	book->count++;

	/* 头插进桶链：新节点成为链首，插入是 O(1) */
	bucket = bucket_index(book->nbuckets, item->name);
	item->hash_next = book->buckets[bucket];
	book->buckets[bucket] = item;

	return CONTACTS_OK;
}

contacts_status contacts_remove(contact_book *book, const char *name) {
	contact  *item;
	contact **link;

	if (book == NULL || name == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	if (book->buckets == NULL) {
		/* 没有索引就只可能是空表；仍然走一遍线性查找以保持语义一致 */
		for (item = book->head; item != NULL; item = item->next) {
			if (strcmp(item->name, name) == 0) {
				break;
			}
		}
		if (item == NULL) {
			return CONTACTS_ERR_NOTFOUND;
		}
	} else {
		/*
		 * link 存的是"指向本节点的那个指针"的地址，可能是桶数组里的
		 * 某一格，也可能是前驱的 hash_next。用二级指针拆链，
		 * 链首与非链首两种情形就不需要各写一遍。
		 */
		link = &book->buckets[bucket_index(book->nbuckets, name)];

		while ((item = *link) != NULL && strcmp(item->name, name) != 0) {
			link = &item->hash_next;
		}
		if (item == NULL) {
			return CONTACTS_ERR_NOTFOUND;
		}
		*link = item->hash_next;
	}

	/* 四种位置：表头、表中、表尾、唯一节点。下面五行覆盖全部情形。 */
	if (item->prev != NULL) {
		item->prev->next = item->next;
	} else {
		book->head = item->next;
	}
	if (item->next != NULL) {
		item->next->prev = item->prev;
	} else {
		book->tail = item->prev;
	}

	free(item);
	book->count--;

	/*
	 * 删除会让装载因子下降，因此这里要给它一次收缩的机会。
	 * 顺序不能反：先减 count 再判断，判据才是"删除之后"的真实状态。
	 *
	 * 收缩失败不回滚、也不报错：删除本身已经完成且语义正确，
	 * 桶表偏大只是多占内存，不该让一次成功的删除变成失败。
	 */
	index_shrink(book);
	return CONTACTS_OK;
}

/* ------------------------------------------------------------------
 * 2. 行解析与序列化
 * ------------------------------------------------------------------ */

static int is_space(char c) {
	return (c == ' ' || c == '\t') ? 1 : 0;
}

static int is_eol(char c) {
	return (c == ' ' || c == '\t' || c == '\r' || c == '\n') ? 1 : 0;
}

/* 去掉尾部空白（含 CR/LF）。原地修改，不改变长度上限。 */
static void rtrim(char *s) {
	size_t len = strlen(s);

	while (len > 0 && is_eol(s[len - 1])) {
		s[--len] = '\0';
	}
}

/*
 * 解析一行。成功时 name/phone 被写成 '\0' 结尾的字符串。
 *
 * 规则：field1 = 行首到第一个逗号之前，field2 = 第一个逗号之后。
 * 每个字段先找到自己的标签冒号，取冒号后面的内容并去掉两端空白。
 * 标签本身不参与语义，因此 name/telephone/phone 都行。
 *
 * 这个写法与"找到第一个空格就当作字段名与值的分隔"的区别在于：
 * 逗号后面有没有空格不再影响结果。旧实现用"第一个空格"定位，
 * 于是 `name: king, phone: 344567754` 会把 `phone:` 一起吞进值里。
 */
static contacts_status parse_line(const char *line, char *name, char *phone) {
	const char *p;
	const char *field;
	size_t      len;

	/* 第一个字段的标签：冒号之前不能先出现逗号，否则说明没有标签 */
	p = line;
	while (*p != '\0' && *p != ':' && *p != ',') {
		p++;
	}
	if (*p != ':') {
		return CONTACTS_ERR_FORMAT;
	}

	/* 第一个字段的值：冒号之后到第一个逗号 */
	field = p + 1;
	while (*field != '\0' && *field != ',') {
		field++;          /* 占位，真正的切片在下面用指针区间算 */
	}
	{
		const char *start = p + 1;
		const char *end   = field;      /* 指向逗号或 '\0' */

		if (*end != ',') {
			return CONTACTS_ERR_FORMAT; /* 只有一个字段，缺少第二个 */
		}

		while (start < end && is_space(*start)) {
			start++;
		}
		while (end > start && is_space(end[-1])) {
			end--;
		}

		len = (size_t)(end - start);
		if (len == 0 || len >= CONTACT_NAME_MAX) {
			return CONTACTS_ERR_FORMAT;
		}
		memcpy(name, start, len);
		name[len] = '\0';

		p = field + 1;      /* 跳过逗号 */
	}

	/* 第二个字段：跳过空白，找标签冒号 */
	while (is_space(*p)) {
		p++;
	}
	while (*p != '\0' && *p != ':') {
		p++;
	}
	if (*p != ':') {
		return CONTACTS_ERR_FORMAT;
	}

	{
		const char *start = p + 1;
		const char *end;

		while (is_space(*start)) {
			start++;
		}

		end = start + strlen(start);
		while (end > start && is_space(end[-1])) {
			end--;
		}

		len = (size_t)(end - start);
		if (len == 0 || len >= CONTACT_PHONE_MAX) {
			return CONTACTS_ERR_FORMAT;
		}
		memcpy(phone, start, len);
		phone[len] = '\0';
	}

	return CONTACTS_OK;
}

/* ------------------------------------------------------------------
 * 3. 文件存取
 * ------------------------------------------------------------------ */

contacts_status contacts_save(const contact_book *book, const char *path) {
	contacts_status status = CONTACTS_OK;
	const contact  *item;
	FILE           *fp;
	char           *tmp;
	size_t          n;
	int             saved;

	if (book == NULL || path == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	/* 临时文件名 = 目标名 + ".tmp"，因此与目标在同一目录、同一文件系统 */
	n = strlen(path) + sizeof ".tmp";
	tmp = (char *)malloc(n);
	if (tmp == NULL) {
		return CONTACTS_ERR_NOMEM;
	}
	(void)snprintf(tmp, n, "%s.tmp", path);

	fp = fopen(tmp, "w");
	if (fp == NULL) {
		free(tmp);
		return CONTACTS_ERR_OPEN;
	}

	for (item = book->head; item != NULL; item = item->next) {
		if (fprintf(fp, "name: %s, phone: %s\n", item->name, item->phone) < 0) {
			status = CONTACTS_ERR_WRITE;
			break;
		}
	}

	/*
	 * errno 在 fclose 与 rename 里都可能被改写，必须存下来再恢复，
	 * 否则调用方拿到的错误号可能来自关闭阶段而不是真正的失败点。
	 */
	saved = errno;
	if (fclose(fp) != 0 && status == CONTACTS_OK) {
		status = CONTACTS_ERR_CLOSE;
		saved  = errno;
	}
	if (status == CONTACTS_OK && rename(tmp, path) != 0) {
		status = CONTACTS_ERR_WRITE;
		saved  = errno;
	}
	if (status != CONTACTS_OK) {
		(void)remove(tmp);      /* 写失败时不留半成品 */
	}
	errno = saved;

	free(tmp);
	return status;
}

contacts_status contacts_load(contact_book *book, const char *path,
                              contacts_load_mode mode,
                              contacts_load_result *result) {
	contacts_status      status = CONTACTS_OK;
	contacts_load_result local  = {0, 0, 0};
	char                 line[CONTACTS_LINE_MAX];
	FILE                *fp;
	size_t               lineno = 0;
	int                  saved;

	if (book == NULL || path == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	fp = fopen(path, "r");
	if (fp == NULL) {
		return CONTACTS_ERR_OPEN;
	}

	if (mode == CONTACTS_LOAD_REPLACE) {
		contacts_clear(book);
	}

	while (fgets(line, (int)sizeof line, fp) != NULL) {
		char            name[CONTACT_NAME_MAX];
		char            phone[CONTACT_PHONE_MAX];
		contacts_status parsed;
		size_t          len;

		lineno++;
		len = strlen(line);

		/*
		 * fgets 读满缓冲区时不会带 '\n'。如果既读满了又没有换行符，
		 * 说明这一行还没结束：把剩余字节吃掉，整行作废。
		 * 不能让它留到下一轮当作新的一行——那样一行坏数据会变成两行。
		 */
		if (len == sizeof line - 1 && line[len - 1] != '\n') {
			int ch;

			while ((ch = fgetc(fp)) != EOF && ch != '\n') {
				/* 丢弃该行剩余内容 */
			}
			local.skipped++;
			if (local.first_bad_line == 0) {
				local.first_bad_line = lineno;
			}
			continue;
		}

		rtrim(line);
		if (line[0] == '\0') {
			continue;       /* 空行静默忽略，不计入 skipped */
		}

		memset(name, 0, sizeof name);
		memset(phone, 0, sizeof phone);

		parsed = parse_line(line, name, phone);
		if (parsed != CONTACTS_OK
		    || contacts_check_field(name, CONTACT_NAME_MAX) != CONTACTS_OK
		    || contacts_check_field(phone, CONTACT_PHONE_MAX) != CONTACTS_OK) {
			local.skipped++;
			if (local.first_bad_line == 0) {
				local.first_bad_line = lineno;
			}
			continue;
		}

		/*
		 * contacts_add 会重新校验并查重，这里不重复判断。
		 * 重名在追加载入时是常见情况（同一个文件读两遍），
		 * 计入 skipped 而不是让整个载入失败。
		 */
		parsed = contacts_add(book, name, phone);
		if (parsed == CONTACTS_OK) {
			local.loaded++;
		} else if (parsed == CONTACTS_ERR_NOMEM) {
			/*
			 * 内存不足与"这一行有问题"是两回事：前者与数据内容无关，
			 * 继续读下去每一行都会失败，而把剩下的行全部计入 skipped
			 * 再返回 CONTACTS_OK，等于对外宣布"载入成功，只是跳过了一部分"，
			 * 调用方无从区分"文件里有坏行"与"进程快没内存了"。
			 * 因此立刻中止，返回 CONTACTS_ERR_NOMEM，已读入的部分保留。
			 */
			status = CONTACTS_ERR_NOMEM;
			break;
		} else {
			local.skipped++;
			if (local.first_bad_line == 0) {
				local.first_bad_line = lineno;
			}
		}
	}

	/* fgets 返回 NULL 有两种可能：读完，或出错。ferror 用来区分。 */
	if (status == CONTACTS_OK && ferror(fp)) {
		status = CONTACTS_ERR_READ;
	}

	saved = errno;
	if (fclose(fp) != 0 && status == CONTACTS_OK) {
		status = CONTACTS_ERR_CLOSE;
		saved  = errno;
	}
	errno = saved;

	if (result != NULL) {
		*result = local;
	}

	return status;
}

/* ------------------------------------------------------------------
 * 4. 辅助
 * ------------------------------------------------------------------ */

contacts_status contacts_index_get(const contact_book *book,
                                   contacts_index_stats *out) {
	size_t i;

	if (book == NULL || out == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	out->buckets   = 0;
	out->entries   = 0;
	out->used      = 0;
	out->max_chain = 0;
	out->rehashes  = book->rehashes;

	if (book->buckets == NULL) {
		return CONTACTS_OK;     /* 还没有索引，前四个值都是 0 */
	}

	out->buckets = book->nbuckets;

	for (i = 0; i < book->nbuckets; i++) {
		const contact *item = book->buckets[i];
		size_t         len  = 0;

		/*
		 * 链长不可能超过节点总数。加上这个上界是为了让"索引结构被破坏
		 * （出现环）"这种情形表现为 entries 与 count 不等、断言失败，
		 * 而不是让诊断函数自己挂在这里。
		 */
		while (item != NULL && len <= book->count) {
			len++;
			item = item->hash_next;
		}

		if (len > 0) {
			out->used++;
		}
		if (len > out->max_chain) {
			out->max_chain = len;
		}
		out->entries += len;
	}

	return CONTACTS_OK;
}

/* ------------------------------------------------------------------
 * 5. 排序
 * ------------------------------------------------------------------ */

/*
 * qsort 的比较函数。
 *
 * 形参是"指向被比较元素的指针"：qsort 比较的是数组元素，而这里的元素
 * 本身是 const contact *，所以 a、b 的类型是 const contact *const *。
 * 写成两级指针不是笔误——少一级就会把节点的首字节当成指针读，
 * 在 LP64 上表现为拿 name 的前 8 个字节当地址，崩溃点离出错处很远。
 *
 * 二级比较：先比姓名，相同再比电话。姓名是主键（见 contacts_add 的查重），
 * 正常情况下走不到第二级。保留它是为了让比较函数成为全序：
 * qsort 不保证相等元素的相对顺序，一旦不变量被破坏（例如有人直接改了
 * 链表节点的 name），没有兜底键的话同样的输入可能给出不同顺序，
 * "排序结果"就没法写成断言。
 */
static int cmp_by_name(const void *a, const void *b) {
	const contact *const *pa = (const contact *const *)a;
	const contact *const *pb = (const contact *const *)b;
	int                   r;

	r = strcmp((*pa)->name, (*pb)->name);
	if (r != 0) {
		return r;
	}
	return strcmp((*pa)->phone, (*pb)->phone);
}

contacts_status contacts_order(const contact_book *book,
                               const contact **out, size_t cap) {
	const contact *item;
	size_t         i = 0;

	if (book == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	/*
	 * 先判容量再判 out：count 为 0 时调用方不必为一块空数组去 malloc，
	 * 传 NULL 也合法。这条顺序写反了会让"空表查询"变成参数错误。
	 */
	if (cap < book->count) {
		return CONTACTS_ERR_PARAM;
	}
	if (book->count == 0) {
		return CONTACTS_OK;
	}
	if (out == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	/*
	 * 遍历用的是 next 而不是索引的桶链：桶链的顺序是"同桶内最近插入在前、
	 * 跨桶按下标"，与插入顺序、与字典序都没有关系，收进来排序只会更慢。
	 *
	 * 上界写 book->count 而不是"直到 next 为 NULL"：cap 只保证不小于 count，
	 * 若链比 count 长（不变量被破坏），照 next 走会写出 out 的边界，
	 * 那是 C11 6.5.6p8 意义上的未定义行为。宁可按上界截断，再由下面的
	 * 长度核对把它暴露成错误。
	 */
	for (item = book->head; item != NULL && i < book->count; item = item->next) {
		out[i] = item;
		i++;
	}

	/*
	 * 走到这里 count 与实际节点数应当相等。不等说明内存被非法改写、
	 * 链表有环或节点被提前释放——这不是调用方的参数错误，
	 * 而是内部不变量已经破坏，此时 out 的内容不可用，直接报错，
	 * 不要把残缺的数组交给 qsort 之后的调用方。
	 */
	if (i != book->count) {
		return CONTACTS_ERR_PARAM;
	}

	qsort(out, book->count, sizeof *out, cmp_by_name);
	return CONTACTS_OK;
}

const char *contacts_status_string(contacts_status status) {
	switch (status) {
	case CONTACTS_OK:            return "success";
	case CONTACTS_ERR_PARAM:     return "invalid argument";
	case CONTACTS_ERR_NOMEM:     return "out of memory";
	case CONTACTS_ERR_OPEN:      return "cannot open file";
	case CONTACTS_ERR_READ:      return "cannot read file";
	case CONTACTS_ERR_WRITE:     return "cannot write file";
	case CONTACTS_ERR_CLOSE:     return "cannot close file";
	case CONTACTS_ERR_FORMAT:    return "malformed field or line";
	case CONTACTS_ERR_TOOLONG:   return "field too long";
	case CONTACTS_ERR_NOTFOUND:  return "contact not found";
	case CONTACTS_ERR_DUPLICATE: return "duplicate name";
	default:                     return "unknown status";
	}
}
