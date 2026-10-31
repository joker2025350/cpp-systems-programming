/*
 * test_save_fail.c —— contacts_save 写失败路径的注入测试
 *
 * 模块 README 9.3 说"保存是安全的：临时文件 + rename，失败时目标文件保持原样"。
 * 这句话在 1.3.0 之前只有实现代码支撑，没有任何测试：正常单元测试只会走
 * "目录可写、磁盘没满、rename 必然成功"这一条路，而上面那句话的全部价值
 * 恰好在另外几条路上。
 *
 * 难点是"造失败"。三条路都不需要 root：
 *
 *   1. 目录不可写：chmod 0500 去掉目录的写权限位。open(dir "/x", O_CREAT)
 *      由内核按目录的权限位检查（POSIX.1-2017 open()，[EACCES]），与文件
 *      自身权限无关。root 具备 CAP_DAC_OVERRIDE，权限位对它不生效，所以
 *      用 geteuid() 判断并跳过。
 *
 *   2. 磁盘满：用 RLIMIT_FSIZE 把"单个文件能长到多大"限死。POSIX.1-2017
 *      write() 规定，写入会使文件超过该资源的软限制时返回 -1 并置 errno
 *      为 EFBIG，同时向进程发 SIGXFSZ。SIGXFSZ 的默认动作是终止进程
 *      （signal.h 规定的 Term 动作），因此必须先把处置改成 SIG_IGN，
 *      否则测试进程在断言之前就被信号杀了。EFBIG 是"磁盘满"在
 *      errno 层面的同类：都是"写不下去"，只是触发条件可控。
 *      不需要 tmpfs，也不需要 root。
 *
 *   3. rename 失败：把目标路径造成一个目录。POSIX.1-2017 rename() 规定，
 *      目标存在且是目录、而源不是目录时返回 -1 并置 errno 为 EISDIR。
 *      这条专打"写完 tmp 之后才失败"的路径——它是唯一会走到 remove(tmp)
 *      那一步的分支。
 *
 * 每个场景都核对同样四件事，不是只核对返回码：
 *   a) 返回码落在契约允许的集合里；
 *   b) errno 保留真正的失败点（不是后续调用留下的残留值）；
 *   c) 目标文件逐字节不变，且 inode 没变（说明它根本未被重写）；
 *   d) 目录里不留 *.tmp。
 *
 * c) 里的 inode 比内容相等更强：内容相等也可能是"先截断再写回同样内容"，
 * 而实现用的是 tmp + rename，失败时目标 inode 必须与失败前相同。
 *
 * 边界要说清楚：这里注入的是**系统调用层面**的失败，覆盖不到
 * "fprintf 自身返回负数"那一支。stdio 默认全缓冲（BUFSIZ，通常 4096 字节），
 * 只要总量没超过缓冲区，fprintf 一律返回成功，真正的 write 发生在
 * fclose 里。所以第二个场景的返回码既可能是 CONTACTS_ERR_CLOSE
 * （错误在关闭阶段暴露）也可能是 CONTACTS_ERR_WRITE（缓冲途中就刷过一次），
 * 断言写成"两者之一"，不假装能固定到某个分支。
 *
 * 用法：
 *   make test-save-fail
 */

#define _POSIX_C_SOURCE 200809L

#include "contacts.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORKDIR "bin/savefail"

/* 目标文件里原本的内容。保存失败之后它必须一个字都不差。 */
static const char ORIGINAL[] = "name: keeper, phone: 13000000000\n";

static int g_checks;
static int g_failures;
static int g_skips;

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */

static void expect_int(const char *name, long got, long want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-48s 实际 %ld，期望 %ld\n", name, got, want);
		return;
	}
	(void)printf("ok    %s\n", name);
}

static void expect_status(const char *name, contacts_status got, contacts_status want) {
	g_checks++;

	if (got != want) {
		g_failures++;
		(void)printf("FAIL  %-48s 实际 %d（%s），期望 %d（%s）\n",
		             name, (int)got, contacts_status_string(got),
		             (int)want, contacts_status_string(want));
		return;
	}
	(void)printf("ok    %s\n", name);
}

/*
 * 环境不具备注入条件时如实跳过，不计入检查数。
 * 这一类跳过必须显式打印：悄悄少跑一段，与"全部通过"看起来一模一样。
 */
static void skip(const char *name, const char *why) {
	g_skips++;
	(void)printf("skip  %-48s %s\n", name, why);
}

/* 只说明情况、不判定对错的附加信息 */
static void info(const char *text) {
	(void)printf("       %s\n", text);
}

/* ------------------------------------------------------------------
 * 文件系统小工具
 * ------------------------------------------------------------------ */

/* mkdir -p 的单层版本；已存在视为成功 */
static int make_dir(const char *path, mode_t mode) {
	if (mkdir(path, mode) == 0) {
		return 0;
	}
	return (errno == EEXIST) ? 0 : -1;
}

static int write_text(const char *path, const char *text) {
	FILE *fp = fopen(path, "w");
	size_t len = strlen(text);

	if (fp == NULL) {
		return -1;
	}
	if (fwrite(text, 1, len, fp) != len) {
		(void)fclose(fp);
		return -1;
	}
	return (fclose(fp) == 0) ? 0 : -1;
}

/* 读回整个文件并与 want 逐字节比较；打不开返回 0 */
static int file_equals(const char *path, const char *want) {
	char  buf[512];
	FILE *fp = fopen(path, "r");
	size_t got;

	if (fp == NULL) {
		return 0;
	}
	got = fread(buf, 1, sizeof buf, fp);
	(void)fclose(fp);

	if (got != strlen(want)) {
		return 0;
	}
	return (memcmp(buf, want, got) == 0) ? 1 : 0;
}

/* 打不开返回 0。inode 号不会真的等于 0，可以拿 0 当哨兵。 */
static unsigned long long file_inode(const char *path) {
	struct stat st;

	if (stat(path, &st) != 0) {
		return 0;
	}
	return (unsigned long long)st.st_ino;
}

static long long file_size(const char *path) {
	struct stat st;

	if (stat(path, &st) != 0) {
		return -1;
	}
	return (long long)st.st_size;
}

static int path_exists(const char *path) {
	struct stat st;

	return (stat(path, &st) == 0) ? 1 : 0;
}

/* 填一本书：每行约 30 字节，20 行约 600 字节，足够顶穿下面 128 字节的限制 */
static int fill_book(contact_book *book, int count) {
	char name[32];
	char phone[32];
	int  i;

	for (i = 0; i < count; i++) {
		(void)snprintf(name, sizeof name, "person%03d", i);
		(void)snprintf(phone, sizeof phone, "1390000%04d", i);
		if (contacts_add(book, name, phone) != CONTACTS_OK) {
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------
 * 场景一：目录不可写 —— fopen 阶段就失败
 * ------------------------------------------------------------------ */

static void test_readonly_dir(contact_book *book) {
	const char     *dir  = WORKDIR "/ro";
	const char     *path = WORKDIR "/ro/book.txt";
	const char     *tmp  = WORKDIR "/ro/book.txt.tmp";
	contacts_status status;
	int             saved_errno;

	(void)chmod(dir, 0700);
	if (make_dir(dir, 0700) != 0 || write_text(path, ORIGINAL) != 0) {
		skip("只读目录：无法准备测试目录", "mkdir 或写初始文件失败");
		(void)chmod(dir, 0700);
		return;
	}

	if (chmod(dir, 0500) != 0) {
		skip("只读目录：chmod 失败", strerror(errno));
		(void)chmod(dir, 0700);
		return;
	}

	/*
	 * 探针：先确认这个环境真的会因为权限位拒绝创建文件。
	 * root 有 CAP_DAC_OVERRIDE，权限位对它无效；某些容器/文件系统
	 * 也有例外（比如以 root 身份挂载的 overlayfs）。探针能过就说明
	 * 本场景在此环境下没有意义，跳过比给出一个假"通过"好。
	 */
	{
		FILE *probe = fopen(WORKDIR "/ro/probe.tmp", "w");

		if (probe != NULL) {
			(void)fclose(probe);
			(void)remove(WORKDIR "/ro/probe.tmp");
			(void)chmod(dir, 0700);
			skip("只读目录：环境忽略目录权限位", "以 root 运行或文件系统不检查权限");
			return;
		}
	}

	errno  = 0;
	status = contacts_save(book, path);
	saved_errno = errno;           /* 先存下来：后面的 chmod 也是系统调用 */

	(void)chmod(dir, 0700);

	expect_status("只读目录：返回 CONTACTS_ERR_OPEN", status, CONTACTS_ERR_OPEN);
	expect_int("只读目录：errno 为 EACCES", (saved_errno == EACCES) ? 1 : 0, 1);
	expect_int("只读目录：目标文件逐字节不变", file_equals(path, ORIGINAL), 1);
	expect_int("只读目录：不留 .tmp", path_exists(tmp), 0);
}

/* ------------------------------------------------------------------
 * 场景二：达到 RLIMIT_FSIZE —— 写到一半失败
 * ------------------------------------------------------------------ */

static void test_fsize_limit(contact_book *book) {
	const char     *path = WORKDIR "/full/book.txt";
	const char     *tmp  = WORKDIR "/full/book.txt.tmp";
	struct rlimit   old;
	struct rlimit   lim;
	void          (*old_handler)(int);
	contacts_status status;
	int             saved_errno;
	unsigned long long ino_before;
	long long       size_before;
	rlim_t          want = 128;

	if (make_dir(WORKDIR "/full", 0700) != 0 || write_text(path, ORIGINAL) != 0) {
		skip("磁盘满：无法准备测试目录", "mkdir 或写初始文件失败");
		return;
	}
	ino_before  = file_inode(path);
	size_before = file_size(path);

	if (getrlimit(RLIMIT_FSIZE, &old) != 0) {
		skip("磁盘满（RLIMIT_FSIZE）", strerror(errno));
		return;
	}

	/*
	 * 软限制不能超过硬限制，否则 setrlimit 返回 EINVAL。
	 * 硬限制很少设成有限值，但代码不该假设这一点。
	 */
	if (old.rlim_max != RLIM_INFINITY && want > old.rlim_max) {
		want = old.rlim_max;
	}
	lim      = old;
	lim.rlim_cur = want;

	if (want < (rlim_t)strlen(ORIGINAL)) {
		skip("磁盘满（RLIMIT_FSIZE）", "硬限制比一行记录还小，无法准备初始文件");
		return;
	}
	if (setrlimit(RLIMIT_FSIZE, &lim) != 0) {
		skip("磁盘满（RLIMIT_FSIZE）", strerror(errno));
		return;
	}

	/* SIGXFSZ 默认动作是终止进程（signal.h 的 Term），必须先忽略 */
	old_handler = signal(SIGXFSZ, SIG_IGN);
	if (old_handler == SIG_ERR) {
		(void)setrlimit(RLIMIT_FSIZE, &old);
		skip("磁盘满（RLIMIT_FSIZE）", "signal(SIGXFSZ, SIG_IGN) 失败");
		return;
	}

	errno  = 0;
	status = contacts_save(book, path);
	saved_errno = errno;

	/*
	 * 先恢复限制再打印：stdout 若被重定向到普通文件（`make test > log`），
	 * RLIMIT_FSIZE 同样会作用在它上面，128 字节之后连结果都打不出来。
	 */
	(void)signal(SIGXFSZ, old_handler);
	(void)setrlimit(RLIMIT_FSIZE, &old);

	/*
	 * 全缓冲的 stdio 会把错误推到 fclose：总量没超过 BUFSIZ 时
	 * fprintf 一路成功，write 只发生在关闭阶段，因此两种返回码都可能。
	 * 这里不做"应该只有一种"的假设。
	 */
	expect_int("磁盘满：返回 WRITE 或 CLOSE",
	           (status == CONTACTS_ERR_WRITE || status == CONTACTS_ERR_CLOSE) ? 1 : 0, 1);
	{
		char note[128];

		(void)snprintf(note, sizeof note, "实际返回 %s（全缓冲下错误由 fclose 暴露）",
		               contacts_status_string(status));
		info(note);
	}
	expect_int("磁盘满：errno 保留 EFBIG", (saved_errno == EFBIG) ? 1 : 0, 1);
	expect_int("磁盘满：目标文件逐字节不变", file_equals(path, ORIGINAL), 1);
	expect_int("磁盘满：目标 inode 不变",
	           (file_inode(path) == ino_before) ? 1 : 0, 1);
	expect_int("磁盘满：目标大小不变",
	           (file_size(path) == size_before) ? 1 : 0, 1);
	expect_int("磁盘满：不留 .tmp", path_exists(tmp), 0);
}

/* ------------------------------------------------------------------
 * 场景三：目标是目录 —— rename 阶段才失败
 * ------------------------------------------------------------------ */

static void test_rename_to_dir(contact_book *book) {
	const char     *target = WORKDIR "/isdir/book.txt";
	const char     *tmp    = WORKDIR "/isdir/book.txt.tmp";
	contacts_status status;
	int             saved_errno;

	if (make_dir(WORKDIR "/isdir", 0700) != 0 || make_dir(target, 0700) != 0) {
		skip("目标是目录：无法准备测试目录", "mkdir 失败");
		return;
	}

	errno  = 0;
	status = contacts_save(book, target);
	saved_errno = errno;

	expect_status("目标是目录：返回 CONTACTS_ERR_WRITE", status, CONTACTS_ERR_WRITE);
	expect_int("目标是目录：errno 为 EISDIR", (saved_errno == EISDIR) ? 1 : 0, 1);
	expect_int("目标是目录：目录本身未被替换", (path_exists(target) == 1) ? 1 : 0, 1);
	expect_int("目标是目录：不留 .tmp", path_exists(tmp), 0);

	(void)rmdir(target);
}

/* ------------------------------------------------------------------
 * 场景四：父目录不存在 —— 最简单的 OPEN 失败
 * ------------------------------------------------------------------ */

static void test_missing_dir(contact_book *book) {
	contacts_status status;
	int             saved_errno;

	errno  = 0;
	status = contacts_save(book, WORKDIR "/nodir/book.txt");
	saved_errno = errno;

	expect_status("父目录不存在：返回 CONTACTS_ERR_OPEN", status, CONTACTS_ERR_OPEN);
	expect_int("父目录不存在：errno 为 ENOENT", (saved_errno == ENOENT) ? 1 : 0, 1);
}

/* ------------------------------------------------------------------
 * 场景五：成功路径的对照 —— 内容确实换了，但换的是 inode
 * ------------------------------------------------------------------ */

static void test_success_replaces_inode(void) {
	const char     *path = WORKDIR "/ok/book.txt";
	const char     *tmp  = WORKDIR "/ok/book.txt.tmp";
	const char     *want =
		"name: alice, phone: 13900001111\n"
		"name: bob, phone: 13900002222\n";
	contact_book    small;
	contacts_status status;
	unsigned long long ino_before;

	if (make_dir(WORKDIR "/ok", 0700) != 0 || write_text(path, ORIGINAL) != 0) {
		skip("成功路径：无法准备测试目录", "mkdir 或写初始文件失败");
		return;
	}
	ino_before = file_inode(path);

	contacts_init(&small);
	if (contacts_add(&small, "alice", "13900001111") != CONTACTS_OK ||
	    contacts_add(&small, "bob", "13900002222") != CONTACTS_OK) {
		contacts_destroy(&small);
		skip("成功路径：无法构造两行的书", "contacts_add 失败");
		return;
	}

	status = contacts_save(&small, path);

	expect_status("成功路径：返回 CONTACTS_OK", status, CONTACTS_OK);
	expect_int("成功路径：内容为新的两行", file_equals(path, want), 1);
	/*
	 * 成功时 inode 必然改变：新内容写在新 inode 上，rename 把它换到
	 * 目标名下。失败时 inode 不变（见场景二）——两件事同源。
	 */
	expect_int("成功路径：inode 已更换",
	           (file_inode(path) != ino_before && ino_before != 0) ? 1 : 0, 1);
	expect_int("成功路径：不留 .tmp", path_exists(tmp), 0);

	(void)remove(path);
	contacts_destroy(&small);
}

/* ------------------------------------------------------------------ */

int main(void) {
	contact_book book;
	int          rc = 0;

	(void)printf("contacts %s 保存失败注入测试\n\n", CONTACTS_VERSION_STRING);

	if (make_dir("bin", 0700) != 0 || make_dir(WORKDIR, 0700) != 0) {
		(void)fprintf(stderr, "无法创建 %s：%s（先执行 make）\n", WORKDIR, strerror(errno));
		return 1;
	}

	contacts_init(&book);
	if (fill_book(&book, 20) != 0) {
		(void)fprintf(stderr, "构造测试数据失败\n");
		return 1;
	}

	(void)printf("-- 场景一：目标目录不可写（chmod 0500）\n");
	test_readonly_dir(&book);

	(void)printf("\n-- 场景二：写到一半超出 RLIMIT_FSIZE\n");
	test_fsize_limit(&book);

	(void)printf("\n-- 场景三：目标路径是个目录（rename 失败）\n");
	test_rename_to_dir(&book);

	(void)printf("\n-- 场景四：父目录不存在\n");
	test_missing_dir(&book);

	(void)printf("\n-- 场景五：成功路径的对照\n");
	test_success_replaces_inode();

	(void)printf("\n%d 项检查，%d 项失败", g_checks, g_failures);
	if (g_skips > 0) {
		(void)printf("，%d 项跳过", g_skips);
	}
	(void)printf("\n");

	if (g_failures != 0) {
		rc = 1;
	}

	contacts_destroy(&book);
	return rc;
}
