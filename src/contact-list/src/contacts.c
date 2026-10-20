/*
 * contacts.c —— 通讯录模块的实现
 *
 * 分三块：
 *   1. 双向链表与节点生命周期
 *   2. 一行的解析与一张表的序列化
 *   3. 文件级的读写，含错误分类与 errno 保存
 */

#include "contacts.h"

#include <errno.h>
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
 * 1. 链表与节点
 * ------------------------------------------------------------------ */

void contacts_init(contact_book *book) {
	if (book == NULL) {
		return;
	}
	book->head  = NULL;
	book->tail  = NULL;
	book->count = 0;
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

	for (item = book->head; item != NULL; item = item->next) {
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
	contact *item;

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

	return CONTACTS_OK;
}

contacts_status contacts_remove(contact_book *book, const char *name) {
	contact *item;

	if (book == NULL || name == NULL) {
		return CONTACTS_ERR_PARAM;
	}

	for (item = book->head; item != NULL; item = item->next) {
		if (strcmp(item->name, name) != 0) {
			continue;
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
		return CONTACTS_OK;
	}

	return CONTACTS_ERR_NOTFOUND;
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
		if (contacts_add(book, name, phone) == CONTACTS_OK) {
			local.loaded++;
		} else {
			local.skipped++;
			if (local.first_bad_line == 0) {
				local.first_bad_line = lineno;
			}
		}
	}

	/* fgets 返回 NULL 有两种可能：读完，或出错。ferror 用来区分。 */
	if (ferror(fp)) {
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
 * 辅助
 * ------------------------------------------------------------------ */

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
