/*
 * test_contacts.c —— contacts 模块的单元测试
 *
 * 绝大多数用例只用内存中的数据；涉及文件的用例需要写一个临时文件，
 * 默认写在工作目录下的 bin/ 里（make 之后必然存在），跑完删除。
 *
 * 用法：
 *   make test
 *   ./bin/test_contacts [testdata 目录]      默认 testdata
 */

#include "contacts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TMP_FILE   "bin/tmp_test_contacts.txt"
#define TMP_FILE2  "bin/tmp_test_contacts2.txt"
#define TMP_LONG   "bin/tmp_test_longline.txt"

static int g_checks   = 0;
static int g_failures = 0;

static const char *g_testdata = "testdata";

/* ---------------------------------------------------------------- */

static void expect_int(const char *name, long got, long want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-46s 实际 %ld，期望 %ld\n", name, got, want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

static void expect_str(const char *name, const char *got, const char *want) {
	g_checks++;

	if (got == NULL || want == NULL || strcmp(got, want) != 0) {
		g_failures++;
		(void)printf("FAIL  %-46s 实际 \"%s\"，期望 \"%s\"\n",
		             name, (got != NULL) ? got : "(null)",
		             (want != NULL) ? want : "(null)");
		return;
	}
	(void)printf("ok    %s\n", name);
}

static void expect_status(const char *name, contacts_status got, contacts_status want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-46s 实际 %d，期望 %d\n",
		             name, (int)got, (int)want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

/* 把 testdata 下的文件名拼成完整路径 */
static const char *data_path(char *buf, size_t size, const char *name) {
	(void)snprintf(buf, size, "%s/%s", g_testdata, name);
	return buf;
}

/* ---------------------------------------------------------------- */

static void test_init_and_empty(void) {
	contact_book book;

	contacts_init(&book);

	expect_int("空通讯录数量为 0", (long)contacts_count(&book), 0);
	expect_int("空通讯录 first 为 NULL", contacts_first(&book) == NULL, 1);
	expect_int("空通讯录 find 为 NULL",
	           contacts_find(&book, "nobody") == NULL, 1);

	contacts_destroy(&book);

	/* NULL 参数不能崩 */
	contacts_init(NULL);
	contacts_clear(NULL);
	contacts_destroy(NULL);
	expect_int("contacts_count(NULL) 返回 0", (long)contacts_count(NULL), 0);
	expect_int("contacts_first(NULL) 返回 NULL", contacts_first(NULL) == NULL, 1);
	expect_int("contacts_next(NULL) 返回 NULL", contacts_next(NULL) == NULL, 1);
	expect_int("contacts_find(NULL) 返回 NULL",
	           contacts_find(NULL, "x") == NULL, 1);
	expect_status("contacts_add(NULL) 返回 PARAM",
	              contacts_add(NULL, "a", "1"), CONTACTS_ERR_PARAM);
	expect_status("contacts_remove(NULL) 返回 PARAM",
	              contacts_remove(NULL, "a"), CONTACTS_ERR_PARAM);
}

static void test_add_and_find(void) {
	contact_book  book;
	const contact *found;

	contacts_init(&book);

	expect_status("添加 alice",
	              contacts_add(&book, "alice", "13900001111"), CONTACTS_OK);
	expect_int("数量为 1", (long)contacts_count(&book), 1);

	found = contacts_find(&book, "alice");
	expect_int("能找到 alice", found != NULL, 1);
	if (found != NULL) {
		expect_str("alice 的电话", found->phone, "13900001111");
	}

	expect_int("找不到 bob", contacts_find(&book, "bob") == NULL, 1);

	contacts_destroy(&book);
}

static void test_add_validation(void) {
	contact_book book;

	contacts_init(&book);

	expect_status("name 为 NULL", contacts_add(&book, NULL, "1"),
	              CONTACTS_ERR_PARAM);
	expect_status("phone 为 NULL", contacts_add(&book, "a", NULL),
	              CONTACTS_ERR_PARAM);
	expect_status("name 为空串", contacts_add(&book, "", "1"),
	              CONTACTS_ERR_FORMAT);
	expect_status("phone 为空串", contacts_add(&book, "a", ""),
	              CONTACTS_ERR_FORMAT);

	/* 逗号是保存格式的字段分隔符，进不来 */
	expect_status("name 含逗号", contacts_add(&book, "a,b", "1"),
	              CONTACTS_ERR_FORMAT);
	expect_status("phone 含逗号", contacts_add(&book, "a", "1,2"),
	              CONTACTS_ERR_FORMAT);

	/* 换行是记录分隔符，同样进不来 */
	expect_status("name 含换行", contacts_add(&book, "a\nb", "1"),
	              CONTACTS_ERR_FORMAT);

	/* 长度恰好等于容量时没有位置放 '\0'，必须拒绝 */
	{
		char name[CONTACT_NAME_MAX + 8];

		memset(name, 'x', sizeof name - 1);
		name[sizeof name - 1] = '\0';

		expect_status("name 长度 = 容量", contacts_add(&book, name, "1"),
		              CONTACTS_ERR_TOOLONG);

		name[CONTACT_NAME_MAX - 1] = '\0';
		expect_status("name 长度 = 容量 - 1",
		              contacts_add(&book, name, "1"), CONTACTS_OK);
		name[CONTACT_NAME_MAX - 1] = 'x';
	}

	/* 只有那一次合法添加成功，失败的都不能留下节点 */
	expect_int("只有一次添加成功，数量为 1", (long)contacts_count(&book), 1);

	contacts_destroy(&book);
}

static void test_duplicate(void) {
	contact_book book;

	contacts_init(&book);

	expect_status("首次添加", contacts_add(&book, "alice", "111"), CONTACTS_OK);
	expect_status("重名被拒绝",
	              contacts_add(&book, "alice", "222"), CONTACTS_ERR_DUPLICATE);
	expect_int("重名不增加数量", (long)contacts_count(&book), 1);

	/* 原来的号码不能被第二次添加覆盖 */
	{
		const contact *again = contacts_find(&book, "alice");

		expect_int("重名后仍能找到 alice", again != NULL, 1);
		if (again != NULL) {
			expect_str("重名时保留原号码", again->phone, "111");
		}
	}

	contacts_destroy(&book);
}

/*
 * 顺序必须是插入顺序。
 * 旧实现用头插法，打印出来是倒序，且 save 写出的文件也是倒序——
 * 存到文件再读回来，顺序又反一次。这里盯住的就是这一点。
 */
static void test_insert_order(void) {
	contact_book  book;
	const contact *item;
	const char   *want[] = {"c", "a", "b"};

	contacts_init(&book);

	(void)contacts_add(&book, "c", "3");
	(void)contacts_add(&book, "a", "1");
	(void)contacts_add(&book, "b", "2");

	item = contacts_first(&book);
	for (int i = 0; i < 3; i++) {
		char label[64];

		(void)snprintf(label, sizeof label, "第 %d 个节点存在", i);
		expect_int(label, item != NULL, 1);
		if (item != NULL) {
			(void)snprintf(label, sizeof label, "第 %d 个是 %s", i, want[i]);
			expect_str(label, item->name, want[i]);
			item = contacts_next(item);
		}
	}
	expect_int("遍历到末尾", item == NULL, 1);

	contacts_destroy(&book);
}

static void test_remove_positions(void) {
	contact_book book;

	/* 表头 */
	contacts_init(&book);
	(void)contacts_add(&book, "a", "1");
	(void)contacts_add(&book, "b", "2");
	(void)contacts_add(&book, "c", "3");
	expect_status("删除表头 a", contacts_remove(&book, "a"), CONTACTS_OK);
	expect_int("删表头后数量为 2", (long)contacts_count(&book), 2);
	expect_str("删表头后第一个是 b", contacts_first(&book)->name, "b");
	expect_int("表头 prev 仍为 NULL", contacts_first(&book)->prev == NULL, 1);
	contacts_destroy(&book);

	/* 表中 */
	contacts_init(&book);
	(void)contacts_add(&book, "a", "1");
	(void)contacts_add(&book, "b", "2");
	(void)contacts_add(&book, "c", "3");
	expect_status("删除表中 b", contacts_remove(&book, "b"), CONTACTS_OK);
	expect_int("删表中后数量为 2", (long)contacts_count(&book), 2);
	expect_str("a 的 next 指向 c", contacts_next(contacts_first(&book))->name, "c");
	contacts_destroy(&book);

	/* 表尾：删完之后必须能从头遍历到 a，且不再出现 c */
	contacts_init(&book);
	(void)contacts_add(&book, "a", "1");
	(void)contacts_add(&book, "b", "2");
	(void)contacts_add(&book, "c", "3");
	expect_status("删除表尾 c", contacts_remove(&book, "c"), CONTACTS_OK);
	expect_int("删表尾后数量为 2", (long)contacts_count(&book), 2);
	expect_int("遍历只有 a 与 b", contacts_next(contacts_next(contacts_first(&book))) == NULL, 1);
	contacts_destroy(&book);

	/* 唯一节点：删完 head 与 tail 都必须回到 NULL */
	contacts_init(&book);
	(void)contacts_add(&book, "only", "1");
	expect_status("删除唯一节点", contacts_remove(&book, "only"), CONTACTS_OK);
	expect_int("删唯一节点后数量为 0", (long)contacts_count(&book), 0);
	expect_int("删唯一节点后 first 为 NULL", contacts_first(&book) == NULL, 1);
	/* 再删一次必须报 NOTFOUND，而不是崩在野指针上 */
	expect_status("空表再删返回 NOTFOUND",
	              contacts_remove(&book, "only"), CONTACTS_ERR_NOTFOUND);
	contacts_destroy(&book);

	/* 删除后仍可继续添加，tail 没有悬空 */
	contacts_init(&book);
	(void)contacts_add(&book, "a", "1");
	(void)contacts_remove(&book, "a");
	expect_status("清空后还能添加", contacts_add(&book, "z", "9"), CONTACTS_OK);
	expect_str("新添加的节点是表头", contacts_first(&book)->name, "z");
	contacts_destroy(&book);
}

static void test_remove_notfound(void) {
	contact_book book;

	contacts_init(&book);
	(void)contacts_add(&book, "a", "1");

	expect_status("删除不存在的名字",
	              contacts_remove(&book, "ghost"), CONTACTS_ERR_NOTFOUND);
	expect_int("失败不改变数量", (long)contacts_count(&book), 1);
	expect_status("name 为 NULL 删除", contacts_remove(&book, NULL),
	              CONTACTS_ERR_PARAM);

	contacts_destroy(&book);
}

static void test_clear_and_reuse(void) {
	contact_book book;

	contacts_init(&book);
	(void)contacts_add(&book, "a", "1");
	(void)contacts_add(&book, "b", "2");

	contacts_clear(&book);
	expect_int("clear 后数量为 0", (long)contacts_count(&book), 0);
	expect_int("clear 后 first 为 NULL", contacts_first(&book) == NULL, 1);

	/* 清空后必须能继续用，且新插入的节点能正常遍历 */
	expect_status("clear 后添加", contacts_add(&book, "z", "9"), CONTACTS_OK);
	expect_int("clear 后数量为 1", (long)contacts_count(&book), 1);
	expect_str("clear 后第一个是 z", contacts_first(&book)->name, "z");
	expect_int("clear 后只有一个节点",
	           contacts_next(contacts_first(&book)) == NULL, 1);

	contacts_destroy(&book);

	/* 重复 destroy 不应崩溃 */
	contacts_destroy(&book);
	expect_int("重复 destroy 后数量为 0", (long)contacts_count(&book), 0);
}

static void test_check_field(void) {
	expect_status("check_field 正常值",
	              contacts_check_field("alice", 16), CONTACTS_OK);
	expect_status("check_field NULL",
	              contacts_check_field(NULL, 16), CONTACTS_ERR_PARAM);
	expect_status("check_field 空串",
	              contacts_check_field("", 16), CONTACTS_ERR_FORMAT);
	expect_status("check_field 含逗号",
	              contacts_check_field("a,b", 16), CONTACTS_ERR_FORMAT);
	expect_status("check_field 含回车",
	              contacts_check_field("a\rb", 16), CONTACTS_ERR_FORMAT);
	expect_status("check_field 恰好等于容量",
	              contacts_check_field("1234567890123456", 16), CONTACTS_ERR_TOOLONG);
	expect_status("check_field 容量减一",
	              contacts_check_field("123456789012345", 16), CONTACTS_OK);
	expect_status("check_field 容量为 0",
	              contacts_check_field("a", 0), CONTACTS_ERR_TOOLONG);
}

/* ---------------------------------------------------------------- */
/* 文件相关                                                          */
/* ---------------------------------------------------------------- */

static int make_library(contact_book *book) {
	contacts_init(book);
	if (contacts_add(book, "alice", "13900001111") != CONTACTS_OK) return -1;
	if (contacts_add(book, "Zhang San", "+8613800002222") != CONTACTS_OK) return -1;
	if (contacts_add(book, "bob", "010-12345678") != CONTACTS_OK) return -1;
	return 0;
}

/*
 * 存取互逆：保存再读回来，每一条的每个字段都必须与原来完全相同。
 * 这是旧实现最严重的一处缺陷——它写出的格式自己读不回来，
 * 每往返一次，电话号码前面就多一个 "phone:"。
 */
static void test_roundtrip(void) {
	contact_book src;
	contact_book dst;
	const contact *a;
	const contact *b;
	int i = 0;

	if (make_library(&src) != 0) {
		g_checks++;
		g_failures++;
		(void)printf("FAIL  构造测试数据失败\n");
		return;
	}

	expect_status("保存到文件", contacts_save(&src, TMP_FILE), CONTACTS_OK);

	contacts_init(&dst);
	expect_status("读回文件",
	              contacts_load(&dst, TMP_FILE, CONTACTS_LOAD_REPLACE, NULL),
	              CONTACTS_OK);

	expect_int("往返后数量一致",
	           (long)contacts_count(&dst), (long)contacts_count(&src));

	for (a = contacts_first(&src), b = contacts_first(&dst);
	     a != NULL && b != NULL;
	     a = contacts_next(a), b = contacts_next(b)) {
		char label[64];

		(void)snprintf(label, sizeof label, "第 %d 条姓名一致", i);
		expect_str(label, b->name, a->name);
		(void)snprintf(label, sizeof label, "第 %d 条电话一致", i);
		expect_str(label, b->phone, a->phone);
		i++;
	}

	contacts_destroy(&src);
	contacts_destroy(&dst);
	(void)remove(TMP_FILE);
}

/* 写出的文件内容必须就是约定的那一行格式 */
static void test_saved_format(void) {
	contact_book src;
	FILE        *fp;
	char         buf[256];

	contacts_init(&src);
	(void)contacts_add(&src, "alice", "13900001111");
	(void)contacts_add(&src, "bob", "222");

	expect_status("保存两条", contacts_save(&src, TMP_FILE), CONTACTS_OK);

	fp = fopen(TMP_FILE, "r");
	g_checks++;
	if (fp == NULL) {
		g_failures++;
		(void)printf("FAIL  %-46s 打不开刚写出的文件\n", "保存格式");
	} else {
		int ok = 1;

		if (fgets(buf, (int)sizeof buf, fp) == NULL
		    || strcmp(buf, "name: alice, phone: 13900001111\n") != 0) {
			ok = 0;
		}
		if (fgets(buf, (int)sizeof buf, fp) == NULL
		    || strcmp(buf, "name: bob, phone: 222\n") != 0) {
			ok = 0;
		}
		if (fgets(buf, (int)sizeof buf, fp) != NULL) {
			ok = 0;       /* 不该有多余的行 */
		}
		(void)fclose(fp);

		if (ok) {
			(void)printf("ok    %s\n", "保存格式为 name: X, phone: Y");
		} else {
			g_failures++;
			(void)printf("FAIL  %-46s 写出的内容与约定不符\n", "保存格式");
		}
	}

	contacts_destroy(&src);
	(void)remove(TMP_FILE);
}

/*
 * 三份历史样例文件：a.txt 与 b.txt 是逗号后无空格的紧凑写法，
 * c.txt 是逗号后有空格、且字段标签不叫 telephone 的写法。
 * 三种都必须被正确解析——"按标签冒号取值"的设计不看空格位置。
 */
static void test_legacy_files(void) {
	char          path[512];
	contact_book  book;
	const contact *item;

	/* a.txt：4 条 */
	data_path(path, sizeof path, "a.txt");
	contacts_init(&book);
	expect_status("载入 a.txt",
	              contacts_load(&book, path, CONTACTS_LOAD_REPLACE, NULL),
	              CONTACTS_OK);
	expect_int("a.txt 条数", (long)contacts_count(&book), 4);

	item = contacts_first(&book);
	expect_str("a.txt 第 1 条姓名", item->name, "liaoqf");
	expect_str("a.txt 第 1 条电话", item->phone, "23456543789");

	item = contacts_next(item);
	expect_str("a.txt 第 2 条姓名", item->name, "zhaomeiping");
	expect_str("a.txt 第 2 条电话", item->phone, "14583943453");

	item = contacts_next(item);
	expect_str("a.txt 第 3 条姓名", item->name, "wangbojing");
	expect_str("a.txt 第 3 条电话", item->phone, "15889650380");

	item = contacts_next(item);
	expect_str("a.txt 第 4 条姓名", item->name, "qiuxiang");
	expect_str("a.txt 第 4 条电话", item->phone, "43563456123");

	expect_int("a.txt 恰好 4 条",
	           contacts_next(item) == NULL, 1);
	contacts_destroy(&book);

	/* b.txt：4 条 */
	data_path(path, sizeof path, "b.txt");
	contacts_init(&book);
	expect_status("载入 b.txt",
	              contacts_load(&book, path, CONTACTS_LOAD_REPLACE, NULL),
	              CONTACTS_OK);
	expect_int("b.txt 条数", (long)contacts_count(&book), 4);
	item = contacts_first(&book);
	expect_str("b.txt 第 1 条电话", item->phone, "98765678123");
	contacts_destroy(&book);

	/*
	 * c.txt：2 条。旧实现会解析成 "phone:344567754"，
	 * 因为它把逗号后的空格当成了字段名与值的分隔符。
	 */
	data_path(path, sizeof path, "c.txt");
	contacts_init(&book);
	expect_status("载入 c.txt",
	              contacts_load(&book, path, CONTACTS_LOAD_REPLACE, NULL),
	              CONTACTS_OK);
	expect_int("c.txt 条数", (long)contacts_count(&book), 2);

	item = contacts_first(&book);
	expect_str("c.txt 第 1 条姓名", item->name, "king");
	expect_str("c.txt 第 1 条电话", item->phone, "344567754");

	item = contacts_next(item);
	expect_str("c.txt 第 2 条姓名", item->name, "wbj");
	expect_str("c.txt 第 2 条电话", item->phone, "1234567865");
	contacts_destroy(&book);
}

static void test_load_append_vs_replace(void) {
	char         path[512];
	contact_book book;

	data_path(path, sizeof path, "c.txt");

	contacts_init(&book);
	(void)contacts_add(&book, "keep", "1");

	expect_status("追加载入",
	              contacts_load(&book, path, CONTACTS_LOAD_APPEND, NULL),
	              CONTACTS_OK);
	expect_int("追加后条数 1+2=3", (long)contacts_count(&book), 3);
	expect_str("已有的记录还在", contacts_first(&book)->name, "keep");

	expect_status("替换载入",
	              contacts_load(&book, path, CONTACTS_LOAD_REPLACE, NULL),
	              CONTACTS_OK);
	expect_int("替换后条数 2", (long)contacts_count(&book), 2);
	expect_int("原有记录被清掉", contacts_find(&book, "keep") == NULL, 1);

	/* 同一个文件追加两次会全部撞重名，只计入 skipped，不算失败 */
	contacts_load_result result = {0, 0, 0};

	expect_status("重复追加同一文件",
	              contacts_load(&book, path, CONTACTS_LOAD_APPEND, &result),
	              CONTACTS_OK);
	expect_int("重复追加 loaded 为 0", (long)result.loaded, 0);
	expect_int("重复追加 skipped 为 2", (long)result.skipped, 2);
	expect_int("重复追加不增加条数", (long)contacts_count(&book), 2);

	contacts_destroy(&book);
}

/* 坏行只跳过，不中断整个载入 */
static void test_load_bad_lines(void) {
	contact_book         book;
	contacts_load_result result = {0, 0, 0};
	FILE                *fp;

	fp = fopen(TMP_FILE2, "w");
	if (fp == NULL) {
		g_checks++;
		g_failures++;
		(void)printf("FAIL  无法创建临时文件\n");
		return;
	}
	(void)fputs("name: good, phone: 111\n", fp);        /* 1 正常 */
	(void)fputs("\n", fp);                              /* 2 空行，静默忽略 */
	(void)fputs("this line has no colon\n", fp);        /* 3 无标签 */
	(void)fputs("name: good2, phone: 222\n", fp);       /* 4 正常 */
	(void)fputs("name: , phone: 333\n", fp);            /* 5 空姓名 */
	(void)fputs("name: x, phone: y, z\n", fp);          /* 6 电话含逗号 */
	(void)fclose(fp);

	contacts_init(&book);
	expect_status("载入含坏行的文件",
	              contacts_load(&book, TMP_FILE2, CONTACTS_LOAD_REPLACE, &result),
	              CONTACTS_OK);

	expect_int("好行载入 2 条", (long)result.loaded, 2);
	expect_int("坏行跳过 3 条", (long)result.skipped, 3);
	expect_int("第一个坏行是第 3 行", (long)result.first_bad_line, 3);
	expect_int("书里确实只有 2 条", (long)contacts_count(&book), 2);

	contacts_destroy(&book);
	(void)remove(TMP_FILE2);
}

/* 超长的一行必须整行作废，不能截成两条记录 */
static void test_load_long_line(void) {
	contact_book         book;
	contacts_load_result result = {0, 0, 0};
	FILE                *fp;

	fp = fopen(TMP_LONG, "w");
	if (fp == NULL) {
		g_checks++;
		g_failures++;
		(void)printf("FAIL  无法创建临时文件\n");
		return;
	}
	(void)fputs("name: first, phone: 111\n", fp);
	/* 一行 900 个字符：里面有逗号和冒号，截断后看起来会像合法记录 */
	(void)fputs("name: ", fp);
	for (int i = 0; i < 900; i++) {
		(void)fputc('a', fp);
	}
	(void)fputs(", phone: 999\n", fp);
	(void)fputs("name: last, phone: 222\n", fp);
	(void)fclose(fp);

	contacts_init(&book);
	expect_status("载入含超长行的文件",
	              contacts_load(&book, TMP_LONG, CONTACTS_LOAD_REPLACE, &result),
	              CONTACTS_OK);

	expect_int("超长行整行作废，只载入 2 条", (long)result.loaded, 2);
	expect_int("超长行计为 1 条跳过", (long)result.skipped, 1);
	expect_int("超长行是第 2 行", (long)result.first_bad_line, 2);

	/* 关键：不能出现一条名字是一长串 a 的记录 */
	expect_int("没有产生截断出的记录",
	           contacts_find(&book, "last") != NULL, 1);
	contacts_destroy(&book);
	(void)remove(TMP_LONG);
}

static void test_load_missing(void) {
	contact_book book;

	contacts_init(&book);
	expect_status("载入不存在的文件",
	              contacts_load(&book, "bin/definitely_not_here.txt",
	                            CONTACTS_LOAD_REPLACE, NULL),
	              CONTACTS_ERR_OPEN);
	expect_int("打开失败不改变内容", (long)contacts_count(&book), 0);

	expect_status("path 为 NULL",
	              contacts_load(&book, NULL, CONTACTS_LOAD_REPLACE, NULL),
	              CONTACTS_ERR_PARAM);
	expect_status("保存到不可写的目录",
	              contacts_save(&book, "bin/no_such_dir/x.txt"),
	              CONTACTS_ERR_OPEN);

	contacts_destroy(&book);
}

static void test_status_string(void) {
	const char *s;

	for (int i = 0; i <= 10; i++) {
		g_checks++;
		s = contacts_status_string((contacts_status)i);
		if (s == NULL || s[0] == '\0') {
			g_failures++;
			(void)printf("FAIL  %-46s 返回空描述\n", "状态码描述非空");
		}
	}
	(void)printf("ok    %s\n", "状态码描述非空");

	g_checks++;
	s = contacts_status_string((contacts_status)999);
	if (s == NULL || strcmp(s, "unknown status") != 0) {
		g_failures++;
		(void)printf("FAIL  %-46s 期望 \"unknown status\"\n", "未知状态码");
	} else {
		(void)printf("ok    %s\n", "未知状态码");
	}
}

/* ---------------------------------------------------------------- */
/* 哈希索引                                                          */
/* ---------------------------------------------------------------- */

static int is_pow2(size_t v) {
	return (v != 0 && (v & (v - 1u)) == 0) ? 1 : 0;
}

/*
 * 索引与链表是两个独立的结构，最容易出错的地方不是"某个函数算错了"，
 * 而是"某条改动路径只维护了其中一个"。这一组用例盯的就是这件事：
 * 每次结构变动之后，都用 contacts_index_get 把两份结构的规模对上。
 */
static void test_index_lifecycle(void) {
	contact_book         book;
	contacts_index_stats st;
	char                 name[32];

	contacts_init(&book);

	(void)memset(&st, 0xff, sizeof st);
	expect_status("空表取索引统计", contacts_index_get(&book, &st), CONTACTS_OK);
	expect_int("空表桶数为 0", (long)st.buckets, 0);
	expect_int("空表 entries 为 0", (long)st.entries, 0);

	(void)contacts_add(&book, "alice", "1");
	expect_status("插入后取索引统计", contacts_index_get(&book, &st), CONTACTS_OK);
	expect_int("首次插入建立索引，桶数 16", (long)st.buckets, 16);
	expect_int("索引 entries 等于条数", (long)st.entries, 1);

	/* 补到 16 条：装载因子正好 1.0，此时还不扩容 */
	for (int i = 0; i < 15; i++) {
		(void)snprintf(name, sizeof name, "u%02d", i);
		(void)contacts_add(&book, name, "0");
	}
	(void)contacts_index_get(&book, &st);
	expect_int("16 条时桶数仍为 16", (long)st.buckets, 16);
	expect_int("16 条时 entries 为 16", (long)st.entries, 16);

	/* 第 17 条：插入后条数超过桶数，必须扩容到 32 */
	(void)contacts_add(&book, "u15", "0");
	(void)contacts_index_get(&book, &st);
	expect_int("第 17 条触发扩容，桶数 32", (long)st.buckets, 32);
	expect_int("扩容后 entries 仍等于条数", (long)st.entries, 17);
	expect_int("扩容后老记录仍能找到",
	           contacts_find(&book, "alice") != NULL, 1);

	(void)contacts_clear(&book);
	(void)contacts_index_get(&book, &st);
	expect_int("clear 后桶数归 0", (long)st.buckets, 0);
	expect_int("clear 后 entries 归 0", (long)st.entries, 0);

	expect_status("index_get(NULL, &st)",
	              contacts_index_get(NULL, &st), CONTACTS_ERR_PARAM);
	expect_status("index_get(&book, NULL)",
	              contacts_index_get(&book, NULL), CONTACTS_ERR_PARAM);

	contacts_destroy(&book);
}

#define INDEX_N 2000

/*
 * 规模化：两千条的插入、查找、删除、再插入。
 *
 * 检查的重点不是"能不能查到"，而是三份事实互相印证：
 *   - contacts_count 的计数
 *   - 沿 next 遍历出来的节点数
 *   - 索引里的 entries
 * 三者必须始终相等。任何一条路径漏维护了一份结构，这里就会不等。
 */
static void test_index_scale(void) {
	contact_book         book;
	contacts_index_stats st;
	char                 name[32];
	char                 phone[32];
	long                 added  = 0;
	long                 found  = 0;
	long                 absent = 0;

	contacts_init(&book);

	for (int i = 0; i < INDEX_N; i++) {
		(void)snprintf(name, sizeof name, "user%04d", i);
		(void)snprintf(phone, sizeof phone, "138%08d", i);
		if (contacts_add(&book, name, phone) == CONTACTS_OK) {
			added++;
		}
	}
	expect_int("两千条全部插入成功", added, INDEX_N);
	expect_int("两千条时 count 正确", (long)contacts_count(&book), INDEX_N);

	(void)contacts_index_get(&book, &st);
	expect_int("entries 等于 count", (long)st.entries, INDEX_N);
	expect_int("桶数是 2 的幂", is_pow2(st.buckets), 1);
	expect_int("装载因子不超过 1.0",
	           (long)(st.entries <= st.buckets), 1);
	/*
	 * 分布检查。装载因子 1.0 时，随机均匀散列的最长链期望在
	 * ln(n)/ln(ln(n)) 附近（n=2000 时约 4~6），20 是一个宽松的上界。
	 * 真正要挡的是"哈希函数只用了少数字节，导致大量姓名挤进同一个桶"
	 * 这类退化——那会让最长链达到几十上百。
	 */
	expect_int("最长链不超过 20", (long)(st.max_chain <= 20), 1);
	expect_int("非空桶超过一半", (long)(st.used * 2u > st.buckets), 1);

	for (int i = 0; i < INDEX_N; i++) {
		(void)snprintf(name, sizeof name, "user%04d", i);
		(void)snprintf(phone, sizeof phone, "138%08d", i);

		const contact *c = contacts_find(&book, name);
		if (c != NULL && strcmp(c->phone, phone) == 0) {
			found++;
		}
	}
	expect_int("两千条全部能查到且电话正确", found, INDEX_N);

	/* 不存在的姓名不能误命中：用插入范围之外的序号 */
	for (int i = 0; i < 500; i++) {
		(void)snprintf(name, sizeof name, "user%04d", INDEX_N + i);
		if (contacts_find(&book, name) == NULL) {
			absent++;
		}
	}
	expect_int("五百个不存在的姓名全部落空", absent, 500);

	/*
	 * 删除一半，按插入序号倒序删。桶链是头插，因此倒序删除
	 * 先删到的是链首，正序删除先删到的是链尾——两种位置都要覆盖，
	 * 拆链写错时通常只错其中一种。
	 */
	{
		long removed = 0;

		for (int i = INDEX_N - 1; i >= 0; i -= 2) {
			(void)snprintf(name, sizeof name, "user%04d", i);
			if (contacts_remove(&book, name) == CONTACTS_OK) {
				removed++;
			}
		}
		expect_int("倒序删除一千条", removed, INDEX_N / 2);
	}

	expect_int("删除后 count 为一半",
	           (long)contacts_count(&book), INDEX_N / 2);
	(void)contacts_index_get(&book, &st);
	expect_int("删除后 entries 等于 count",
	           (long)st.entries, INDEX_N / 2);

	{
		long still_there = 0;
		long gone        = 0;

		for (int i = 0; i < INDEX_N; i++) {
			(void)snprintf(name, sizeof name, "user%04d", i);
			if ((i % 2) == 0) {
				if (contacts_find(&book, name) != NULL) {
					still_there++;
				}
			} else {
				if (contacts_find(&book, name) == NULL) {
					gone++;
				}
			}
		}
		expect_int("保留项全部仍在", still_there, INDEX_N / 2);
		expect_int("删除项全部消失", gone, INDEX_N / 2);
	}

	/* 删掉的名字可以重新加入，且不破坏索引 */
	(void)snprintf(name, sizeof name, "user%04d", INDEX_N - 1);
	expect_status("删除后可重新添加",
	              contacts_add(&book, name, "139"), CONTACTS_OK);
	{
		const contact *again = contacts_find(&book, name);

		expect_int("重新添加后能找到", again != NULL, 1);
		if (again != NULL) {
			expect_str("重新添加后电话是新的", again->phone, "139");
		}
	}
	expect_status("重新添加同一个名字仍然重名",
	              contacts_add(&book, name, "140"), CONTACTS_ERR_DUPLICATE);

	(void)contacts_index_get(&book, &st);
	expect_int("重名被拒后 entries 不变",
	           (long)st.entries, INDEX_N / 2 + 1);
	expect_int("重名被拒后 count 不变",
	           (long)contacts_count(&book), INDEX_N / 2 + 1);

	/* 全部删完：链表与索引都要回到空状态 */
	{
		long left = 0;

		for (int i = 0; i < INDEX_N; i++) {
			(void)snprintf(name, sizeof name, "user%04d", i);
			(void)contacts_remove(&book, name);
		}
		expect_int("全删后 count 为 0", (long)contacts_count(&book), 0);

		for (int i = 0; i < INDEX_N; i++) {
			(void)snprintf(name, sizeof name, "user%04d", i);
			if (contacts_find(&book, name) != NULL) {
				left++;
			}
		}
		expect_int("全删后没有任何残留命中", left, 0);
	}

	contacts_destroy(&book);
}

/* ---------------------------------------------------------------- */

/*
 * 从尾部按名字删掉 user[to, from) 这一段，返回实际删除条数。
 * 倒序删：桶链是头插，倒序删除先删到链首，与正序删除覆盖的位置不同。
 */
static long remove_names(contact_book *book, long from, long to) {
	char name[32];
	long removed = 0;

	for (long i = from - 1; i >= to; i--) {
		(void)snprintf(name, sizeof name, "user%04ld", i);
		if (contacts_remove(book, name) == CONTACTS_OK) {
			removed++;
		}
	}
	return removed;
}

static void add_names(contact_book *book, long n) {
	char name[32];

	for (long i = 0; i < n; i++) {
		(void)snprintf(name, sizeof name, "user%04ld", i);
		(void)contacts_add(book, name, "138");
	}
}

/*
 * 收缩：删除把装载因子压到 1/4 以下时，桶数减半。
 *
 * 这一组用例盯四个数：
 *   1. 阈值的**边界**——256/1024 恰好等于 1/4 时不收缩，255 才收缩。
 *      差一条的边界是最容易写错的地方（`<` 写成 `<=` 就会在 1/4 处多缩一次）。
 *   2. 减半之后索引与链表仍然一致：entries、count、遍历长度三者相等。
 *   3. 遍历顺序不受影响——重挂只改 hash_next，链表的 prev/next 一根不动，
 *      因此 save 写出的文件与收缩前逐字节相同。
 *   4. 空表回到 buckets == 0，与新建的表状态一致；再插入能重新建索引。
 */
static void test_index_shrink(void) {
	contact_book         book;
	contacts_index_stats st;
	char                 path[] = "bin/tmp_test_shrink.txt";
	const contact       *item;
	char                 name[32];
	long                 i;
	long                 in_order = 0;

	contacts_init(&book);
	add_names(&book, 1024);

	(void)contacts_index_get(&book, &st);
	expect_int("收缩：1024 条时桶数 1024", (long)st.buckets, 1024);
	/* 建表 1 次 + 扩容 6 次（16→32→64→128→256→512→1024） */
	expect_int("收缩：建 1024 条共重分配 7 次", (long)st.rehashes, 7);

	/* 删到 256 条：装载因子正好 1/4，判据是"低于"，此处不该收缩 */
	expect_int("收缩：删掉 768 条", remove_names(&book, 1024, 256), 768);
	expect_int("收缩：剩余 256 条", (long)contacts_count(&book), 256);
	(void)contacts_index_get(&book, &st);
	expect_int("收缩：装载因子 1/4 时不收缩", (long)st.buckets, 1024);
	expect_int("收缩：不收缩时 entries 等于 count", (long)st.entries, 256);

	/* 再删一条：255 < 1024/4，桶数减半 */
	expect_int("收缩：删掉第 769 条", remove_names(&book, 256, 255), 1);
	(void)contacts_index_get(&book, &st);
	expect_int("收缩：装载因子低于 1/4 时桶数减半", (long)st.buckets, 512);
	expect_int("收缩后 entries 等于 count", (long)st.entries, 255);
	expect_int("收缩后桶数仍是 2 的幂", is_pow2(st.buckets), 1);
	expect_int("收缩后装载因子回到 1/2 附近",
	           (long)(st.entries * 4u >= st.buckets), 1);

	/* 继续删到 127：跨过 512/4 = 128 这条线，桶数再减半 */
	expect_int("收缩：删到 127 条", remove_names(&book, 255, 127), 128);
	(void)contacts_index_get(&book, &st);
	expect_int("收缩：127 条时桶数 256", (long)st.buckets, 256);
	expect_int("收缩：127 条时 entries 为 127", (long)st.entries, 127);

	/* 顺序：剩下的必须是 user0000..user0126，且按插入顺序排列 */
	for (item = contacts_first(&book), i = 0;
	     item != NULL;
	     item = contacts_next(item), i++) {
		(void)snprintf(name, sizeof name, "user%04ld", i);
		if (strcmp(item->name, name) == 0) {
			in_order++;
		}
	}
	expect_int("收缩后遍历顺序仍是插入顺序", in_order, 127);
	expect_int("收缩后遍历长度是 127", i, 127);

	/*
	 * 文件契约：收缩只重挂桶链，链表一根不动，因此写出的内容必须与
	 * "插入了 127 条、从未删过"的表逐字节相同。
	 */
	expect_status("收缩后保存", contacts_save(&book, path), CONTACTS_OK);
	{
		FILE *fp = fopen(path, "r");
		long  lines = 0;
		int   first_ok = 0;

		if (fp != NULL) {
			char line[256];

			while (fgets(line, (int)sizeof line, fp) != NULL) {
				if (lines == 0
				    && strcmp(line, "name: user0000, phone: 138\n") == 0) {
					first_ok = 1;
				}
				lines++;
			}
			(void)fclose(fp);
		}
		expect_int("收缩后保存的行数是 127", lines, 127);
		expect_int("收缩后保存的第一行不变", first_ok, 1);
	}
	(void)remove(path);

	/* 继续删到 3 条：一路减半到下限 16 */
	expect_int("收缩：删到 3 条", remove_names(&book, 127, 3), 124);
	(void)contacts_index_get(&book, &st);
	expect_int("收缩：3 条时触到下限 16", (long)st.buckets, 16);
	expect_int("收缩：3 条时 entries 为 3", (long)st.entries, 3);
	/* 扩容 6 次 + 建表 1 次 + 收缩 6 次（1024→512→256→128→64→32→16） */
	expect_int("收缩：累计重分配 13 次", (long)st.rehashes, 13);
	expect_int("收缩：3 条时仍能查到 user0002",
	           contacts_find(&book, "user0002") != NULL, 1);

	/* 下限之下不再收缩：1 条时仍是 16 个桶 */
	expect_int("收缩：删到 1 条", remove_names(&book, 3, 1), 2);
	(void)contacts_index_get(&book, &st);
	expect_int("收缩：1 条时桶数不低于下限", (long)st.buckets, 16);

	/* 删空：桶表整块释放，回到 buckets == 0，与新建的表一致 */
	expect_int("收缩：删掉最后一条", remove_names(&book, 1, 0), 1);
	(void)contacts_index_get(&book, &st);
	expect_int("删空后桶数归 0", (long)st.buckets, 0);
	expect_int("删空后 entries 归 0", (long)st.entries, 0);
	expect_int("删空后 used 归 0", (long)st.used, 0);
	expect_int("删空后 count 归 0", (long)contacts_count(&book), 0);
	expect_int("删空后 find 落空",
	           contacts_find(&book, "user0000") == NULL, 1);

	/*
	 * 删空之后再插入：重建索引，条数为 1、桶数为 16。
	 * 这条用例挡的是"删空时忘了把 nbuckets 清零"——那种实现会让下一次
	 * 插入用到一张已释放的桶表，ASan 会直接报 heap-use-after-free。
	 */
	expect_status("删空后重新插入",
	              contacts_add(&book, "user0000", "139"), CONTACTS_OK);
	(void)contacts_index_get(&book, &st);
	expect_int("删空后重新插入，桶数 16", (long)st.buckets, 16);
	expect_int("删空后重新插入，entries 为 1", (long)st.entries, 1);

	contacts_destroy(&book);
}

/* ---------------------------------------------------------------- */

/*
 * contacts_order：按姓名升序取指针数组。
 *
 * 盯四类东西：
 *   1. 参数合同——book 为 NULL、cap 不足、out 为 NULL、空表；
 *   2. 顺序本身，包括"字节序而不是大小写不敏感"这一点；
 *   3. 排序是只读的：链表顺序、数量、索引统计一个都不许动；
 *   4. 规模大一点时（200 条，倒序插入）仍然严格递增——只测 3 条的话，
 *      把比较函数写成"只比较第一个字节"也能过。
 */
static void test_order_by_name(void) {
	contact_book         book;
	contacts_index_stats st;
	const contact       *arr[4];
	contacts_status      status;
	size_t               i;

	contacts_init(&book);

	/* ---- 参数合同 ---- */
	expect_status("排序：空表返回 OK", contacts_order(&book, NULL, 0), CONTACTS_OK);
	expect_status("排序：book 为 NULL 被拒绝",
	              contacts_order(NULL, arr, 4), CONTACTS_ERR_PARAM);

	expect_status("排序：插入 carol", contacts_add(&book, "carol", "138"), CONTACTS_OK);
	expect_status("排序：插入 alice", contacts_add(&book, "alice", "139"), CONTACTS_OK);
	expect_status("排序：插入 bob",   contacts_add(&book, "bob",   "137"), CONTACTS_OK);

	for (i = 0; i < 4; i++) {
		arr[i] = NULL;
	}
	expect_status("排序：cap 小于条数被拒绝",
	              contacts_order(&book, arr, 2), CONTACTS_ERR_PARAM);
	expect_int("排序：cap 不足时不写数组", arr[0] == NULL, 1);
	expect_status("排序：out 为 NULL 被拒绝",
	              contacts_order(&book, NULL, 3), CONTACTS_ERR_PARAM);

	/* ---- 顺序 ---- */
	status = contacts_order(&book, arr, 3);
	expect_status("排序：三条返回 OK", status, CONTACTS_OK);
	expect_str("排序：第 1 条是 alice", arr[0]->name, "alice");
	expect_str("排序：第 2 条是 bob",   arr[1]->name, "bob");
	expect_str("排序：第 3 条是 carol", arr[2]->name, "carol");

	/* 填进去的是节点指针，不是副本：电话必须与原记录一致 */
	expect_str("排序：第 2 条电话仍是原值", arr[1]->phone, "137");
	expect_int("排序：拿到的是同一个对象",
	           arr[1] == contacts_find(&book, "bob"), 1);

	/* ---- 只读 ---- */
	expect_int("排序后数量不变", (long)contacts_count(&book), 3);
	expect_str("排序后 first 仍是插入的第一条",
	           contacts_first(&book)->name, "carol");
	expect_str("排序后遍历仍是插入顺序",
	           contacts_next(contacts_first(&book))->name, "alice");
	(void)contacts_index_get(&book, &st);
	expect_int("排序后索引 entries 不变", (long)st.entries, 3);
	expect_int("排序后桶数不变", (long)st.buckets, 16);

	contacts_destroy(&book);

	/*
	 * ---- 字节序 ----
	 * '0'=0x30 < 'Z'=0x5A < 'a'=0x61 < 0xE5（"张"的 UTF-8 首字节）。
	 * 最后一条是关键：若实现把字节按 signed char 比较，0xE5 是负数，
	 * "张三"会被排到最前面——这是 C 里逐字节比较最常见的错法。
	 */
	contacts_init(&book);
	expect_status("排序：插入 alice（字节序用例）",
	              contacts_add(&book, "alice", "1"), CONTACTS_OK);
	expect_status("排序：插入 Zoe",
	              contacts_add(&book, "Zoe", "2"), CONTACTS_OK);
	expect_status("排序：插入 007",
	              contacts_add(&book, "007", "3"), CONTACTS_OK);
	expect_status("排序：插入 张三",
	              contacts_add(&book, "张三", "4"), CONTACTS_OK);

	status = contacts_order(&book, arr, 4);
	expect_status("排序：字节序用例返回 OK", status, CONTACTS_OK);
	expect_str("字节序：数字在最前", arr[0]->name, "007");
	expect_str("字节序：大写在小写之前", arr[1]->name, "Zoe");
	expect_str("字节序：小写随后", arr[2]->name, "alice");
	expect_str("字节序：非 ASCII 在最后（按无符号字节比较）",
	           arr[3]->name, "张三");
	contacts_destroy(&book);

	/* ---- 规模：200 条倒序插入 ---- */
	contacts_init(&book);
	for (i = 200; i > 0; i--) {
		char name[CONTACT_NAME_MAX];

		(void)snprintf(name, sizeof name, "n%03lu", (unsigned long)i);
		if (contacts_add(&book, name, "13800000000") != CONTACTS_OK) {
			expect_int("排序：200 条全部插入成功", 0, 1);
			contacts_destroy(&book);
			return;
		}
	}

	{
		const contact **big = (const contact **)malloc(200 * sizeof *big);
		int             ok  = 1;

		if (big == NULL) {
			expect_int("排序：为 200 个指针分配内存", 0, 1);
			contacts_destroy(&book);
			return;
		}

		status = contacts_order(&book, big, 200);
		expect_status("排序：200 条返回 OK", status, CONTACTS_OK);
		expect_str("排序：200 条的首条是 n001", big[0]->name, "n001");
		expect_str("排序：200 条的末条是 n200", big[199]->name, "n200");

		for (i = 0; i + 1 < 200; i++) {
			if (strcmp(big[i]->name, big[i + 1]->name) >= 0) {
				ok = 0;
			}
		}
		expect_int("排序：200 条严格递增", ok, 1);

		(void)contacts_index_get(&book, &st);
		expect_int("排序：200 条时索引 entries 仍为 200", (long)st.entries, 200);
		free(big);
	}

	contacts_destroy(&book);
}

/* ---------------------------------------------------------------- */

int main(int argc, char *argv[]) {
	if (argc > 1) {
		g_testdata = argv[1];
	}

	(void)printf("contacts %s 单元测试（testdata = %s）\n\n",
	             CONTACTS_VERSION_STRING, g_testdata);

	test_init_and_empty();
	test_add_and_find();
	test_add_validation();
	test_duplicate();
	test_insert_order();
	test_order_by_name();
	test_remove_positions();
	test_remove_notfound();
	test_clear_and_reuse();
	test_check_field();
	test_roundtrip();
	test_saved_format();
	test_legacy_files();
	test_load_append_vs_replace();
	test_load_bad_lines();
	test_load_long_line();
	test_load_missing();
	test_status_string();
	test_index_lifecycle();
	test_index_scale();
	test_index_shrink();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_failures);
	return (g_failures == 0) ? 0 : 1;
}
