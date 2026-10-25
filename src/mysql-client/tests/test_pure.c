/*
 * test_pure.c —— 纯逻辑单元测试
 *
 * "纯逻辑"指不访问数据库的那一层：状态码映射、连接参数校验、文件读写。
 * 这个测试**不链接 libmysqlclient**：它只引用 options.o / status.o / file.o
 * 里的符号，链接器按需抽取静态库成员，mysqlclient.o 不会被拉进来，
 * 那些 mysql_* 也就不会成为未定义符号。因此没有装 MySQL 客户端库的机器
 * 也能跑这一半测试。
 *
 * 用法：sh 里由 make test 直接执行；退出码 = 失败项数。
 */

#include "mysqlclient.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   /* stat / chmod / umask：POSIX */
#include <unistd.h>   /* rmdir：POSIX，声明在 unistd.h 而不是 stdio.h */

static int g_checks = 0;
static int g_fails  = 0;

static void ok(const char *name) {
	g_checks++;
	(void)printf("ok    %s\n", name);
}

static void fail(const char *name, const char *detail) {
	g_checks++;
	g_fails++;
	(void)printf("FAIL  %-48s %s\n", name, detail);
}

static void check_long(const char *name, long actual, long expected) {
	char detail[96];

	if (actual == expected) {
		ok(name);
		return;
	}

	(void)snprintf(detail, sizeof detail, "实际 %ld，期望 %ld", actual, expected);
	fail(name, detail);
}

static void check_true(const char *name, int cond) {
	if (cond) ok(name);
	else      fail(name, "条件不成立");
}

/* ------------------------------------------------------------------
 * 1. 状态码映射
 * ------------------------------------------------------------------ */

static void test_status_string(void) {

	/*
	 * 逐个枚举值检查"能返回非 NULL"。这条性质被驱动层直接依赖：
	 * 出错路径上 printf("%s", mc_status_string(st)) 不允许出现 (null)，
	 * 而这种崩溃只在出错时才出现，正常路径完全测不到。
	 */
	check_true("MC_OK 有描述",             mc_status_string(MC_OK) != NULL);
	check_true("MC_ERR_PARAM 有描述",      mc_status_string(MC_ERR_PARAM) != NULL);
	check_true("MC_ERR_NOMEM 有描述",      mc_status_string(MC_ERR_NOMEM) != NULL);
	check_true("MC_ERR_FILE_OPEN 有描述",  mc_status_string(MC_ERR_FILE_OPEN) != NULL);
	check_true("MC_ERR_FILE_READ 有描述",  mc_status_string(MC_ERR_FILE_READ) != NULL);
	check_true("MC_ERR_FILE_WRITE 有描述", mc_status_string(MC_ERR_FILE_WRITE) != NULL);
	check_true("MC_ERR_CONNECT 有描述",    mc_status_string(MC_ERR_CONNECT) != NULL);
	check_true("MC_ERR_QUERY 有描述",      mc_status_string(MC_ERR_QUERY) != NULL);
	check_true("MC_ERR_RESULT 有描述",     mc_status_string(MC_ERR_RESULT) != NULL);
	check_true("MC_ERR_STMT 有描述",       mc_status_string(MC_ERR_STMT) != NULL);
	check_true("MC_ERR_TOOBIG 有描述",     mc_status_string(MC_ERR_TOOBIG) != NULL);
	check_true("MC_ERR_NODATA 有描述",     mc_status_string(MC_ERR_NODATA) != NULL);

	/* 越界的整数值走不到 switch 的任何一个 case，必须有兜底字符串 */
	check_true("未知状态码返回 unknown status",
	           strcmp(mc_status_string((mc_status)9999), "unknown status") == 0);
	check_true("MC_OK 的描述是 ok",
	           strcmp(mc_status_string(MC_OK), "ok") == 0);
}

/* ------------------------------------------------------------------
 * 2. 连接参数
 * ------------------------------------------------------------------ */

static void test_options(void) {
	mc_options opt;
	mc_options bad;

	mc_options_init(&opt);
	check_true("init 之后 host 为空", opt.host[0] == '\0');
	check_true("init 之后 user 为空", opt.user[0] == '\0');
	check_true("init 之后密码为空", opt.password[0] == '\0');
	check_true("init 之后库名为空", opt.database[0] == '\0');
	check_long("init 之后端口为 3306", (long)opt.port, 3306);
	check_long("init 之后超时为 0", (long)opt.connect_timeout, 0);

	check_long("NULL 参数被拒绝", mc_options_check(NULL), MC_ERR_PARAM);

	/* 主机的两类非法情形 */
	check_long("空 host 被拒绝", mc_options_check(&opt), MC_ERR_PARAM);

	(void)snprintf(opt.host, sizeof opt.host, "127.0.0.1");
	check_long("只有 host 没有 user 被拒绝", mc_options_check(&opt), MC_ERR_PARAM);

	(void)snprintf(opt.user, sizeof opt.user, "root");
	check_long("host + user 可通过", mc_options_check(&opt), MC_OK);
	check_long("库名为空仍可通过（不指定库是合法的）",
	           mc_options_check(&opt), MC_OK);

	/*
	 * 没有 '\0' 结尾的字段。memset 把整个数组填满，strlen 会一路读出去，
	 * 而选项校验必须在这里挡住——放过去就是把一次越界读交给 mysql_real_connect。
	 */
	bad = opt;
	(void)memset(bad.host, 'a', sizeof bad.host);
	check_long("host 未以 '\\0' 结尾被拒绝", mc_options_check(&bad), MC_ERR_PARAM);

	bad = opt;
	(void)memset(bad.user, 'a', sizeof bad.user);
	check_long("user 未以 '\\0' 结尾被拒绝", mc_options_check(&bad), MC_ERR_PARAM);

	bad = opt;
	(void)memset(bad.password, 'a', sizeof bad.password);
	check_long("password 未以 '\\0' 结尾被拒绝", mc_options_check(&bad), MC_ERR_PARAM);

	bad = opt;
	(void)memset(bad.database, 'a', sizeof bad.database);
	check_long("database 未以 '\\0' 结尾被拒绝", mc_options_check(&bad), MC_ERR_PARAM);

	/* 端口边界：0 与 65536 都不可能是一个有效端口 */
	bad         = opt;
	bad.port    = 0;
	check_long("端口 0 被拒绝", mc_options_check(&bad), MC_ERR_PARAM);

	bad      = opt;
	bad.port = 65535;
	check_long("端口 65535 被接受", mc_options_check(&bad), MC_OK);

	bad      = opt;
	bad.port = 65536;
	check_long("端口 65536 被拒绝", mc_options_check(&bad), MC_ERR_PARAM);
}

/* ------------------------------------------------------------------
 * 3. 文件读写
 * ------------------------------------------------------------------ */

#define BLOB_SIZE 1024

static void fill_pattern(unsigned char *buf, size_t n) {
	size_t i;

	/* 含 0x00 的图案：任何"当成字符串处理"的实现都会在这里暴露 */
	for (i = 0; i < n; i++) {
		buf[i] = (unsigned char)((i * 7u) & 0xffu);
	}
}

static void test_files(void) {
	char          dir[] = "bin/tXXXXXX";
	char          path[256];
	char          bad_path[256];
	char          empty_path[256];
	char          tmp_path[300];
	unsigned char data[BLOB_SIZE];
	unsigned char back[BLOB_SIZE + 64];
	size_t        len = 0;
	mc_status     st;

	if (mkdtemp(dir) == NULL) {
		fail("创建临时目录", "mkdtemp 失败");
		return;
	}
	ok("创建临时目录");

	(void)snprintf(path, sizeof path, "%s/blob.bin", dir);
	(void)snprintf(bad_path, sizeof bad_path, "%s/no-such-dir/x.bin", dir);
	(void)snprintf(empty_path, sizeof empty_path, "%s/empty.bin", dir);

	(void)memset(data, 0, sizeof data);
	(void)memset(back, 0, sizeof back);
	fill_pattern(data, sizeof data);

	/* ---- 参数检查 ---- */
	check_long("NULL 路径被拒绝", mc_file_read(NULL, back, sizeof back, &len),
	           MC_ERR_PARAM);
	check_long("NULL 缓冲区被拒绝", mc_file_read(path, NULL, 16, &len),
	           MC_ERR_PARAM);
	check_long("NULL 长度指针被拒绝", mc_file_read(path, back, sizeof back, NULL),
	           MC_ERR_PARAM);
	check_long("容量 0 被拒绝（与空文件无法区分）",
	           mc_file_read(path, back, 0, &len), MC_ERR_PARAM);

	/* ---- 往返 ---- */
	st = mc_file_write(path, data, sizeof data);
	check_long("写入 1024 字节", st, MC_OK);

	st = mc_file_read(path, back, sizeof back, &len);
	check_long("读回 1024 字节", st, MC_OK);
	check_long("读回长度为 1024", (long)len, BLOB_SIZE);
	check_true("内容逐字节相同（含内嵌 '\\0'）",
	           memcmp(data, back, sizeof data) == 0);

	/* 容量恰好等于文件长度：不能误判为超限 */
	(void)memset(back, 0, sizeof back);
	st = mc_file_read(path, back, BLOB_SIZE, &len);
	check_long("容量恰好等于长度时成功", st, MC_OK);
	check_long("容量恰好等于长度时长度正确", (long)len, BLOB_SIZE);

	/* 容量小一字节：必须报 TOOBIG，而不是截断成 1023 字节 */
	len = 12345;
	st  = mc_file_read(path, back, BLOB_SIZE - 1, &len);
	check_long("容量少一字节时报 TOOBIG", st, MC_ERR_TOOBIG);
	check_long("失败时不写 out_len", (long)len, 12345);

	/* ---- 错误路径 ---- */
	(void)snprintf(path, sizeof path, "%s/nonexistent.bin", dir);
	check_long("读不存在的文件返回 FILE_OPEN",
	           mc_file_read(path, back, sizeof back, &len), MC_ERR_FILE_OPEN);

	/* 目录：fopen 在 Linux 上能成功，fread 会以 EISDIR 失败；
	 * 某些 libc 在 fopen 阶段就拒绝。两种都不算错，差别只在这条路径
	 * 走到哪一步，因此这里接受任一结果——要求的是"不能静默成功"。 */
	st = mc_file_read(dir, back, sizeof back, &len);
	check_true("读目录不会静默成功",
	           st == MC_ERR_FILE_READ || st == MC_ERR_FILE_OPEN);

	/*
	 * /proc 下的伪文件：可 seek，但 lstat 报告的长度是 0。
	 * 以 ftell 的结果为准的实现在这里会读出 0 字节（旧实现就是这样），
	 * 而以"读到 EOF"为准的实现能读出真实内容。这一条是回归测试。
	 */
	st = mc_file_read("/proc/version", back, sizeof back, &len);
	check_long("/proc/version 读取成功", st, MC_OK);
	check_true("/proc/version 读到了内容（长度大于 0）", len > 0);
	check_true("/proc/version 内容以 'Linux' 开头",
	           len > 5 && memcmp(back, "Linux", 5) == 0);

	(void)snprintf(path, sizeof path, "%s/blob.bin", dir);
	check_long("写到不存在的目录返回 FILE_OPEN",
	           mc_file_write(bad_path, data, 16), MC_ERR_FILE_OPEN);

	check_long("写入 NULL 路径被拒绝", mc_file_write(NULL, data, 16), MC_ERR_PARAM);
	check_long("写入 NULL 缓冲区被拒绝", mc_file_write(path, NULL, 16), MC_ERR_PARAM);

	/* 长度 0：创建一个空文件，随后读回长度为 0 */
	check_long("写入 0 字节成功", mc_file_write(empty_path, data, 0), MC_OK);
	st = mc_file_read(empty_path, back, sizeof back, &len);
	check_long("读空文件成功", st, MC_OK);
	check_long("空文件长度为 0", (long)len, 0);

	/* 覆盖写：第二次写入更短的内容，文件必须变成新长度而不是残留旧尾部 */
	check_long("覆盖写入 10 字节", mc_file_write(path, data, 10), MC_OK);
	st = mc_file_read(path, back, sizeof back, &len);
	check_long("覆盖写之后读取成功", st, MC_OK);
	check_long("覆盖写之后长度为 10", (long)len, 10);

	/*
	 * 原子写入的副产物：成功之后临时文件必须已经消失。
	 * 残留的 .tmp 不只是脏，它还说明 rename 没走到，
	 * 也就是目标文件可能根本没更新——所以这一条要单独验。
	 */
	(void)snprintf(tmp_path, sizeof tmp_path, "%s.tmp", path);
	check_true("写入成功后不残留 .tmp 文件", access(tmp_path, F_OK) != 0);

	/* 失败路径同样不能留下残片 */
	(void)snprintf(tmp_path, sizeof tmp_path, "%s.tmp", bad_path);
	check_long("写入失败返回 FILE_OPEN", mc_file_write(bad_path, data, 16),
	           MC_ERR_FILE_OPEN);
	check_true("写入失败后不残留 .tmp 文件", access(tmp_path, F_OK) != 0);

	/* 清理：只删本次创建的文件，不碰目录之外的东西 */
	(void)remove(empty_path);
	(void)remove(path);
	(void)rmdir(dir);
}

/* ------------------------------------------------------------------
 * 5. 覆盖写的权限位
 *
 * "覆盖同名文件"的预期是：内容换掉，其余不变。权限位属于"其余"。
 *
 * 这一类缺陷事后极难发现：函数返回成功、内容也对，只有 stat 之后才知道
 * 权限已经被换成了 0666 & ~umask。实测（umask 002）一个 0600 的文件
 * 被写一次就变成 0664——原本只有文件所有者能读的导出内容，
 * 同组用户从此也能读，而程序没有任何提示。
 * ------------------------------------------------------------------ */

/* 权限位按八进制比较：八进制是权限位的书写惯例，写成十进制没人看得懂
 * （0755 会显示成 493）。 */
static void check_mode(const char *name, mode_t actual, mode_t expected) {
	char detail[96];

	if (actual == expected) {
		ok(name);
		return;
	}

	(void)snprintf(detail, sizeof detail, "实际 %04o，期望 %04o",
	               (unsigned)actual, (unsigned)expected);
	fail(name, detail);
}

/* 取文件的权限位（含 setuid / setgid / sticky），失败返回 0。 */
static mode_t mode_of(const char *path) {
	struct stat st;

	if (stat(path, &st) != 0) return (mode_t)0;
	return (mode_t)(st.st_mode & 07777);
}

static void test_file_mode(void) {

	char      dir[] = "bin/mXXXXXX";
	char      path[256];
	char      ro_dir[] = "bin/rXXXXXX";
	char      ro_path[256];
	char      back[32];
	size_t    len = 0;
	mc_status st;
	mode_t    mask;
	mode_t    listed[4];
	size_t    i;

	if (mkdtemp(dir) == NULL) {
		fail("创建权限测试目录", "mkdtemp 失败");
		return;
	}

	(void)snprintf(path, sizeof path, "%s/out.bin", dir);

	/* ---- 新建：权限由 umask 决定 ---- */

	/*
	 * umask 没有"只读"接口，取当前值的写法就是先设成 0 再设回去。
	 * 这里读一次，是为了把"新建文件应当是 0666 & ~umask"写成期望值，
	 * 而不是硬编码 0664——测试机 umask 不同时，硬编码会让这条用例
	 * 变成环境相关。
	 */
	mask = umask(0);
	(void)umask(mask);

	st = mc_file_write(path, "abc", 3);
	check_long("新建文件写入成功", st, MC_OK);
	check_mode("新建文件的权限是 0666 & ~umask", mode_of(path),
	           (mode_t)(0666 & ~mask));

	/* ---- 覆盖：保持目标文件原有的权限位 ---- */

	listed[0] = 0600;
	listed[1] = 0640;
	listed[2] = 0444;
	listed[3] = 0755;

	for (i = 0; i < 4; i++) {
		char name[64];

		/*
		 * 0444 这一条同时说明 rename 与 fopen("wb") 的差别：
		 * 目标文件不可写，但目录可写，覆盖仍然成功——rename 改的是
		 * 目录项，不要求对目标文件本身有写权限。若用 fopen(path, "wb")，
		 * 这里会以 EACCES 失败。行为变了，权限位就必须跟着保持，
		 * 否则"覆盖成功"这个结论只对了一半。
		 */
		check_long("设置目标文件权限成功", chmod(path, listed[i]), 0);

		st = mc_file_write(path, "xyz", 3);
		check_long("覆盖写入成功", st, MC_OK);

		(void)snprintf(name, sizeof name, "%04o 覆盖后仍是 %04o",
		               (unsigned)listed[i], (unsigned)listed[i]);
		check_mode(name, mode_of(path), listed[i]);

		(void)memset(back, 0, sizeof back);
		st = mc_file_read(path, back, sizeof back, &len);
		check_long("覆盖后读回成功", st, MC_OK);
		check_long("覆盖后长度为 3", (long)len, 3);
		check_true("覆盖后内容正确", memcmp(back, "xyz", 3) == 0);
	}

	/* ---- setuid / setgid 不跟着搬 ---- */

	/*
	 * 04755 -> 0755：普通权限位保持，setuid 位被丢掉。
	 * 依据是这两个位的作用——execve(2) 下它让进程以文件所有者（或所属组）
	 * 的身份运行；而这里写进去的是一份全新的、未经审查的内容。
	 * 内核在 write(2) 写已有文件时会清掉 setuid，这里换的是新 inode，
	 * 内核不会替我们清，只能由实现主动不带。
	 */
	check_long("设置 setuid 位成功", chmod(path, 04755), 0);
	st = mc_file_write(path, "s", 1);
	check_long("覆盖 setuid 文件成功", st, MC_OK);
	check_mode("setuid 位不被继承（04755 -> 0755）", mode_of(path), 0755);
	check_true("setuid 位确实已清除", (mode_of(path) & S_ISUID) == 0);

	/* ---- 失败路径不动目标文件 ---- */

	/*
	 * 目录不可写：临时文件创建不出来，写入必须以 FILE_OPEN 失败，
	 * 且目标文件的内容与权限都不变。
	 *
	 * 若实现"先 chmod 目标再写内容"，权限在这里就已经被改掉了。
	 * root 不受目录权限限制，这条用例对 root 没有意义，跳过。
	 */
	if (mkdtemp(ro_dir) == NULL) {
		fail("创建只读目录", "mkdtemp 失败");
	} else {
		(void)snprintf(ro_path, sizeof ro_path, "%s/keep.bin", ro_dir);
		st = mc_file_write(ro_path, "old", 3);
		check_long("在只读目录里先建一个文件", st, MC_OK);
		check_long("把它改成 0600", chmod(ro_path, 0600), 0);
		check_long("目录改成 0500", chmod(ro_dir, 0500), 0);

		if (geteuid() == 0) {
			ok("以 root 运行，跳过只读目录用例");
		} else {
			st = mc_file_write(ro_path, "new", 3);
			check_long("目录不可写时返回 FILE_OPEN", st, MC_ERR_FILE_OPEN);
			check_mode("失败后目标权限不变", mode_of(ro_path), 0600);

			(void)memset(back, 0, sizeof back);
			st = mc_file_read(ro_path, back, sizeof back, &len);
			check_long("失败后目标仍可读", st, MC_OK);
			check_true("失败后目标内容不变（仍是 old）",
			           len == 3 && memcmp(back, "old", 3) == 0);
		}

		(void)chmod(ro_dir, 0700);
		(void)remove(ro_path);
		(void)rmdir(ro_dir);
	}

	(void)remove(path);
	(void)rmdir(dir);
}

int main(void) {

	(void)printf("mysqlclient 纯逻辑单元测试\n\n");

	(void)printf("--- 状态码 ---\n");
	test_status_string();

	(void)printf("\n--- 连接参数 ---\n");
	test_options();

	(void)printf("\n--- 文件读写 ---\n");
	test_files();

	(void)printf("\n--- 覆盖写的权限位 ---\n");
	test_file_mode();

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_fails);

	return g_fails;
}
