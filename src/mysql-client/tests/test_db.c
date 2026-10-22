/*
 * test_db.c —— 接口层测试（不要求运行中的数据库）
 *
 * 这个文件验证的是"动网络之前"和"动网络之后失败"这两类行为：
 * 参数校验、句柄生命周期、错误信息、连不上的时候返回什么。
 *
 * 为什么不测成功路径：那需要一台运行中的 MySQL，还要预置库、表和存储过程。
 * 把这类测试塞进 make test，结果是"没装数据库的机器上全红"，等于没有测试。
 * 成功路径的验证记录在 README 的"测试与验证"一节，用真实服务器跑。
 *
 * 它必须链接 libmysqlclient：一旦引用 mc_open，链接器就会把 mysqlclient.o
 * 拉进来，mysql_* 符号随之成为未定义符号。
 */

#include "mysqlclient.h"

#include <stdio.h>
#include <string.h>

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

int main(void) {

	mc_options  opt;
	mc_conn    *conn = (mc_conn *)1;   /* 故意给一个非 NULL 的脏值 */
	mc_rows     set;
	mc_status   st;
	unsigned long long affected = 0;
	char        buf[16];
	size_t      len = 0;

	(void)printf("mysqlclient 接口层测试\n\n");

	/*
	 * 1. 句柄为空时每个入口都必须返回 PARAM，而不是解引用空指针。
	 *    这类检查的意义在于：模块是给别人用的，使用者漏检一次返回值
	 *    不应该直接段错误。
	 */
	check_long("mc_ping(NULL)",   mc_ping(NULL),   MC_ERR_PARAM);
	check_long("mc_exec(NULL)",   mc_exec(NULL, "SELECT 1", &affected), MC_ERR_PARAM);
	check_long("mc_query(NULL)",  mc_query(NULL, "SELECT 1", &set),    MC_ERR_PARAM);
	check_long("mc_exec 空 SQL",  mc_exec(NULL, "", NULL),             MC_ERR_PARAM);
	check_long("mc_blob_write(NULL)",
	           mc_blob_write(NULL, "x", buf, 1), MC_ERR_PARAM);
	check_long("mc_blob_read(NULL)",
	           mc_blob_read(NULL, "x", buf, sizeof buf, &len), MC_ERR_PARAM);

	/* 空操作必须安全 */
	mc_close(NULL);
	ok("mc_close(NULL) 不崩溃");
	mc_rows_free(NULL);
	ok("mc_rows_free(NULL) 不崩溃");

	/* mc_error 保证不返回 NULL，调用方才能直接 printf("%s") */
	check_true("mc_error(NULL) 非 NULL", mc_error(NULL) != NULL);

	/* 2. 建连参数 */
	check_long("mc_open 的 out 为 NULL", mc_open(NULL, NULL), MC_ERR_PARAM);

	mc_options_init(&opt);
	check_long("空参数建连被拒绝", mc_open(&opt, &conn), MC_ERR_PARAM);
	check_true("失败时 out 被置为 NULL", conn == NULL);

	(void)snprintf(opt.host, sizeof opt.host, "127.0.0.1");
	(void)snprintf(opt.user, sizeof opt.user, "nobody");
	check_long("参数合法但 out 为 NULL 被拒绝", mc_open(&opt, NULL), MC_ERR_PARAM);

	/*
	 * 3. 连接一个必然失败的端点。
	 *
	 * 127.0.0.1:1 是保留的 tcpmux 端口，本机不会有服务监听：
	 * 内核直接回 RST，connect(2) 立刻返回 ECONNREFUSED，
	 * 因此这条用例毫秒级完成，不依赖任何外部环境。
	 * 用 192.0.2.0/24 这类"必然不可达"的地址会退化成超时，
	 * 让整个测试卡在 TCP 重传上，因此不能这么写。
	 */
	opt.port = 1;
	conn     = (mc_conn *)1;
	st       = mc_open(&opt, &conn);
	check_long("连接 127.0.0.1:1 返回 CONNECT", st, MC_ERR_CONNECT);
	check_true("连接失败时句柄为 NULL", conn == NULL);
	check_true("连接失败后错误信息非空", mc_error(NULL)[0] != '\0');
	check_true("错误信息带 connect 前缀",
	           strncmp(mc_error(NULL), "connect:", 8) == 0);

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_fails);

	return g_fails;
}
