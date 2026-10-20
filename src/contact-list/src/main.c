/*
 * main.c —— 命令行驱动
 *
 * 这一层只负责参数解析、输入读入、输出排版与退出码，
 * 所有通讯录逻辑都在 contacts 模块里。把它单独分出来，
 * 是为了让模块可以被别的程序链接使用，而不必连带一个 main。
 *
 * 两种使用方式：
 *   子命令模式   contacts [-f FILE] add alice 13900001111
 *   菜单模式     contacts [-f FILE] menu
 *
 * 子命令模式的意义在于可以脚本化：没有它，任何端到端测试都只能靠
 * 往标准输入里灌一串数字，一旦菜单顺序改了测试全废。
 */

#include "contacts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_BUF 256

/* 退出码 */
#define EXIT_USAGE 2

typedef struct {
	contact_book book;
	const char  *file;     /* -f 指定的工作文件，NULL 表示只在内存里 */
} app;

/* ------------------------------------------------------------------
 * 通用小工具
 * ------------------------------------------------------------------ */

static void usage(FILE *out, const char *prog) {
	(void)fprintf(out,
	    "Usage: %s [-f FILE] COMMAND [ARGS...]\n"
	    "\n"
	    "Commands:\n"
	    "  add <name> <phone>    添加联系人\n"
	    "  del <name>            按姓名删除\n"
	    "  find <name>           按姓名查找\n"
	    "  list                  列出全部联系人（顺序即插入顺序）\n"
	    "  count                 输出联系人数量\n"
	    "  save <file>           保存到文件\n"
	    "  load <file> [-r]      从文件载入；-r 表示先清空\n"
	    "  menu                  交互式菜单\n"
	    "  help                  显示本说明\n"
	    "\n"
	    "Options:\n"
	    "  -f FILE   把 FILE 当作工作文件：启动时载入，改动后自动写回\n",
	    prog);
}

/* 读一行到 buf，去掉行尾换行；超长的一行会被截断并丢弃剩余部分。 */
static int read_line(char *buf, size_t size) {
	size_t len;

	if (fgets(buf, (int)size, stdin) == NULL) {
		return -1;
	}

	len = strlen(buf);
	if (len == size - 1 && buf[len - 1] != '\n') {
		int ch;

		while ((ch = getchar()) != EOF && ch != '\n') {
			/* 丢弃超长行的剩余部分 */
		}
	}

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
		buf[--len] = '\0';
	}

	return 0;
}

/*
 * 判断一个状态码是否伴随有意义的 errno。
 * 只有文件级的错误才有；参数、重名、格式这几类的 errno 是上一次系统调用
 * 的残留值，直接 strerror 会打印出与本次失败无关的内容。
 */
static int has_errno(contacts_status status) {
	return status == CONTACTS_ERR_OPEN
	    || status == CONTACTS_ERR_READ
	    || status == CONTACTS_ERR_WRITE
	    || status == CONTACTS_ERR_CLOSE;
}

static void print_status(const char *subject, contacts_status status) {
	if (has_errno(status)) {
		(void)fprintf(stderr, "contacts: %s: %s: %s\n",
		              subject, contacts_status_string(status), strerror(errno));
	} else {
		(void)fprintf(stderr, "contacts: %s: %s\n",
		              subject, contacts_status_string(status));
	}
}

/* 一行输出。格式与保存格式完全一致，因此 list 的输出可以直接当输入文件用。 */
static void print_contact(const contact *c) {
	(void)printf("name: %s, phone: %s\n", c->name, c->phone);
}

static int save_working_file(app *a) {
	contacts_status status;

	if (a->file == NULL) {
		return 0;
	}

	status = contacts_save(&a->book, a->file);
	if (status != CONTACTS_OK) {
		print_status(a->file, status);
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------
 * 子命令
 * ------------------------------------------------------------------ */

static int cmd_add(app *a, int argc, char **argv) {
	contacts_status status;

	if (argc != 3) {
		(void)fprintf(stderr, "contacts: add 需要 <name> <phone> 两个参数\n");
		return EXIT_USAGE;
	}

	status = contacts_add(&a->book, argv[1], argv[2]);
	if (status != CONTACTS_OK) {
		print_status(argv[1], status);
		return 1;
	}

	return save_working_file(a);
}

static int cmd_del(app *a, int argc, char **argv) {
	contacts_status status;

	if (argc != 2) {
		(void)fprintf(stderr, "contacts: del 需要 <name> 一个参数\n");
		return EXIT_USAGE;
	}

	status = contacts_remove(&a->book, argv[1]);
	if (status != CONTACTS_OK) {
		print_status(argv[1], status);
		return 1;
	}

	return save_working_file(a);
}

static int cmd_find(app *a, int argc, char **argv) {
	const contact *found;

	if (argc != 2) {
		(void)fprintf(stderr, "contacts: find 需要 <name> 一个参数\n");
		return EXIT_USAGE;
	}

	found = contacts_find(&a->book, argv[1]);
	if (found == NULL) {
		(void)fprintf(stderr, "contacts: %s: %s\n",
		              argv[1], contacts_status_string(CONTACTS_ERR_NOTFOUND));
		return 1;
	}

	print_contact(found);
	return 0;
}

static int cmd_list(app *a) {
	const contact *item;

	for (item = contacts_first(&a->book); item != NULL; item = contacts_next(item)) {
		print_contact(item);
	}
	return 0;
}

static int cmd_count(app *a) {
	(void)printf("%lu\n", (unsigned long)contacts_count(&a->book));
	return 0;
}

static int cmd_save(app *a, int argc, char **argv) {
	contacts_status status;

	if (argc != 2) {
		(void)fprintf(stderr, "contacts: save 需要 <file> 一个参数\n");
		return EXIT_USAGE;
	}

	status = contacts_save(&a->book, argv[1]);
	if (status != CONTACTS_OK) {
		print_status(argv[1], status);
		return 1;
	}
	return 0;
}

static int cmd_load(app *a, int argc, char **argv) {
	contacts_load_result result = {0, 0, 0};
	contacts_load_mode   mode   = CONTACTS_LOAD_APPEND;
	contacts_status      status;

	if (argc < 2 || argc > 3) {
		(void)fprintf(stderr, "contacts: load 需要 <file> [-r]\n");
		return EXIT_USAGE;
	}
	if (argc == 3) {
		if (strcmp(argv[2], "-r") != 0) {
			(void)fprintf(stderr, "contacts: 未知选项 %s\n", argv[2]);
			return EXIT_USAGE;
		}
		mode = CONTACTS_LOAD_REPLACE;
	}

	status = contacts_load(&a->book, argv[1], mode, &result);
	if (status != CONTACTS_OK) {
		print_status(argv[1], status);
		return 1;
	}

	/* 统计写 stderr：stdout 留给数据，脚本重定向时不会被污染 */
	(void)fprintf(stderr, "contacts: loaded %lu, skipped %lu",
	              (unsigned long)result.loaded, (unsigned long)result.skipped);
	if (result.skipped > 0) {
		(void)fprintf(stderr, ", first bad line %lu",
		              (unsigned long)result.first_bad_line);
	}
	(void)fprintf(stderr, "\n");

	return save_working_file(a);
}

static int cmd_menu(app *a, int argc, char **argv);

/* ------------------------------------------------------------------
 * 交互式菜单（保留原程序的使用方式）
 * ------------------------------------------------------------------ */

static void menu_print(void) {
	(void)printf("\n"
	    "  1) 添加    2) 列出    3) 删除\n"
	    "  4) 查找    5) 保存    6) 载入\n"
	    "  0) 退出\n"
	    "> ");
	(void)fflush(stdout);
}

static void menu_add(app *a) {
	char name[LINE_BUF];
	char phone[LINE_BUF];
	contacts_status status;

	(void)printf("姓名：");
	(void)fflush(stdout);
	if (read_line(name, sizeof name) != 0) {
		return;
	}
	(void)printf("电话：");
	(void)fflush(stdout);
	if (read_line(phone, sizeof phone) != 0) {
		return;
	}

	status = contacts_add(&a->book, name, phone);
	if (status != CONTACTS_OK) {
		(void)printf("失败：%s\n", contacts_status_string(status));
		return;
	}
	(void)printf("已添加 %s\n", name);
}

static void menu_del(app *a) {
	char name[LINE_BUF];

	(void)printf("姓名：");
	(void)fflush(stdout);
	if (read_line(name, sizeof name) != 0) {
		return;
	}

	if (contacts_remove(&a->book, name) != CONTACTS_OK) {
		(void)printf("没有这个人：%s\n", name);
		return;
	}
	(void)printf("已删除 %s\n", name);
}

static void menu_find(app *a) {
	char           name[LINE_BUF];
	const contact *found;

	(void)printf("姓名：");
	(void)fflush(stdout);
	if (read_line(name, sizeof name) != 0) {
		return;
	}

	found = contacts_find(&a->book, name);
	if (found == NULL) {
		(void)printf("没有这个人：%s\n", name);
		return;
	}
	print_contact(found);
}

static void menu_save(app *a) {
	char           path[LINE_BUF];
	contacts_status status;

	(void)printf("保存到文件：");
	(void)fflush(stdout);
	if (read_line(path, sizeof path) != 0 || path[0] == '\0') {
		return;
	}

	status = contacts_save(&a->book, path);
	if (status != CONTACTS_OK) {
		(void)printf("失败：%s\n", contacts_status_string(status));
		return;
	}
	(void)printf("已保存 %lu 条到 %s\n",
	             (unsigned long)contacts_count(&a->book), path);
}

static void menu_load(app *a) {
	contacts_load_result result = {0, 0, 0};
	char                 path[LINE_BUF];
	contacts_status      status;

	(void)printf("从文件载入（会追加到现有内容）：");
	(void)fflush(stdout);
	if (read_line(path, sizeof path) != 0 || path[0] == '\0') {
		return;
	}

	status = contacts_load(&a->book, path, CONTACTS_LOAD_APPEND, &result);
	if (status != CONTACTS_OK) {
		(void)printf("失败：%s\n", contacts_status_string(status));
		return;
	}
	(void)printf("载入 %lu 条，跳过 %lu 条\n",
	             (unsigned long)result.loaded, (unsigned long)result.skipped);
}

static int cmd_menu(app *a, int argc, char **argv) {
	char line[LINE_BUF];

	(void)argc;
	(void)argv;

	for (;;) {
		int choice;

		menu_print();
		if (read_line(line, sizeof line) != 0) {
			break;                  /* 标准输入结束 */
		}
		if (sscanf(line, "%d", &choice) != 1) {
			break;                  /* 输入的不是数字，退出 */
		}

		switch (choice) {
		case 1: menu_add(a);  break;
		case 2: cmd_list(a);  break;
		case 3: menu_del(a);  break;
		case 4: menu_find(a); break;
		case 5: menu_save(a); break;
		case 6: menu_load(a); break;
		case 0: goto done;
		default:
			(void)printf("没有这个选项\n");
			break;
		}
	}

done:
	return save_working_file(a);
}

/* ------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
	app          a;
	const char  *prog = argv[0];
	int          i;
	int          rc;

	contacts_init(&a.book);
	a.file = NULL;

	/* 全局选项必须在命令之前 */
	i = 1;
	while (i < argc && argv[i][0] == '-') {
		if (strcmp(argv[i], "-f") == 0) {
			if (i + 1 >= argc) {
				(void)fprintf(stderr, "contacts: -f 需要一个文件名\n");
				usage(stderr, prog);
				return EXIT_USAGE;
			}
			a.file = argv[i + 1];
			i += 2;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout, prog);
			return 0;
		} else {
			(void)fprintf(stderr, "contacts: 未知选项 %s\n", argv[i]);
			usage(stderr, prog);
			return EXIT_USAGE;
		}
	}

	if (i >= argc) {
		usage(stderr, prog);
		return EXIT_USAGE;
	}

	/*
	 * 工作文件：存在就载入，不存在就当作空表。
	 * 不把"文件不存在"当错误，是因为 add 到一个新文件是很自然的用法；
	 * 但如果文件存在却打不开（权限、是目录），那就是真的错误，要报出来。
	 */
	if (a.file != NULL) {
		contacts_load_result result = {0, 0, 0};
		contacts_status      status;

		status = contacts_load(&a.book, a.file, CONTACTS_LOAD_REPLACE, &result);
		if (status != CONTACTS_OK && !(status == CONTACTS_ERR_OPEN && errno == ENOENT)) {
			print_status(a.file, status);
			contacts_destroy(&a.book);
			return 1;
		}
	}

	if (strcmp(argv[i], "add") == 0) {
		rc = cmd_add(&a, argc - i, argv + i);
	} else if (strcmp(argv[i], "del") == 0) {
		rc = cmd_del(&a, argc - i, argv + i);
	} else if (strcmp(argv[i], "find") == 0) {
		rc = cmd_find(&a, argc - i, argv + i);
	} else if (strcmp(argv[i], "list") == 0) {
		rc = cmd_list(&a);
	} else if (strcmp(argv[i], "count") == 0) {
		rc = cmd_count(&a);
	} else if (strcmp(argv[i], "save") == 0) {
		rc = cmd_save(&a, argc - i, argv + i);
	} else if (strcmp(argv[i], "load") == 0) {
		rc = cmd_load(&a, argc - i, argv + i);
	} else if (strcmp(argv[i], "menu") == 0) {
		rc = cmd_menu(&a, argc - i, argv + i);
	} else if (strcmp(argv[i], "help") == 0) {
		usage(stdout, prog);
		rc = 0;
	} else {
		(void)fprintf(stderr, "contacts: 未知命令 %s\n", argv[i]);
		usage(stderr, prog);
		rc = EXIT_USAGE;
	}

	/* 每条退出路径都走到这里，保证整条链表被释放 */
	contacts_destroy(&a.book);
	return rc;
}
