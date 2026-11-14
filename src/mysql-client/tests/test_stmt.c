/*
 * test_stmt.c —— 语句对象：正确性、复用、状态约束
 *
 * 需要一台运行中的 MySQL：语句对象的语义大半落在协议上（什么时候丢弃
 * 上一次的结果集、结果集能读几遍、复用之后服务端还认不认这条语句），
 * 本地没有可查的对照物。连接参数走环境变量，与前两个测试程序一致：
 *   MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root MC_PASSWORD= ./test_stmt
 *
 * 自建自删一个库（mc_stmt_db）与一张表，不碰其它数据。
 *
 * 覆盖五类东西：
 *   1. 生命周期与状态：未执行就取数、结果集取两遍、错误之后对象还能不能用；
 *   2. 参数：六种 kind、长字段分块、参数个数不符，以及"本地检查先于网络"；
 *   3. 结果集：与 mc_query 的物化结果逐格比对（含 NULL、空结果集），
 *      以及 mc_stmt_each 与 mc_stmt_rows 交付的内容一致；
 *   4. 复用：同一条语句执行 N 次（批量写入的实际形状），
 *      以及"上次没读完就再执行"这条丢弃路径；
 *   5. 性能模式：`./test_stmt --bench N` 对比"每次 prepare"与"prepare 一次"
 *      的墙面时间，N 次单行 INSERT。数字进 README 的性能一节。
 */

#include "mysqlclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_checks = 0;
static int g_fails  = 0;

static void ok(const char *name) {
	g_checks++;
	(void)printf("ok    %s\n", name);
}

static void fail(const char *name, const char *detail) {
	g_checks++;
	g_fails++;
	(void)printf("FAIL  %-56s %s\n", name, detail);
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
 * 建连：与驱动层一致的两层来源（环境变量 + 内置默认值）
 * ------------------------------------------------------------------ */

static mc_status open_from_env(mc_conn **out, const char **why) {
	mc_options opt;
	const char    *v;
	mc_status      st;

	mc_options_init(&opt);

	(void)snprintf(opt.host, sizeof opt.host, "127.0.0.1");
	(void)snprintf(opt.user, sizeof opt.user, "root");
	opt.port = 3306;

	if ((v = getenv("MC_HOST")) != NULL && v[0] != '\0') {
		(void)snprintf(opt.host, sizeof opt.host, "%s", v);
	}
	if ((v = getenv("MC_PORT")) != NULL && v[0] != '\0') {
		opt.port = (unsigned int)strtoul(v, NULL, 10);
	}
	if ((v = getenv("MC_USER")) != NULL && v[0] != '\0') {
		(void)snprintf(opt.user, sizeof opt.user, "%s", v);
	}
	if ((v = getenv("MC_PASSWORD")) != NULL) {
		(void)snprintf(opt.password, sizeof opt.password, "%s", v);
	}

	st = mc_open(&opt, out);
	if (st != MC_OK) *why = mc_error(NULL);
	return st;
}

#define DB    "mc_stmt_db"
#define TBL   DB ".s"

/* ------------------------------------------------------------------
 * 参数构造
 * ------------------------------------------------------------------ */

static mc_param p_null(void) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_NULL;
	return p;
}

static mc_param p_int(long long v) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_INT;
	p.v.i  = v;
	return p;
}

static mc_param p_uint(unsigned long long v) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_UINT;
	p.v.u  = v;
	return p;
}

static mc_param p_double(double v) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_DOUBLE;
	p.v.d  = v;
	return p;
}

static mc_param p_text(const char *s) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind         = MC_PARAM_TEXT;
	p.v.bytes.data = s;
	p.v.bytes.len  = (s != NULL) ? strlen(s) : 0;
	return p;
}

static mc_param p_blob(const void *data, size_t len) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind         = MC_PARAM_BLOB;
	p.v.bytes.data = data;
	p.v.bytes.len  = len;
	return p;
}

/* ------------------------------------------------------------------
 * 结果集比较
 * ------------------------------------------------------------------ */

/* 把两个物化结果集逐格比对。NULL 与空串必须区分开，长度也要比，
 * 因为 BLOB 列的内容可以含 '\0'，strcmp 在那里会提前结束。 */
static void rows_equal(const char *name, const mc_rows *a, const mc_rows *b) {
	size_t i;
	size_t j;
	char   detail[160];

	if (a->rows != b->rows || a->cols != b->cols) {
		(void)snprintf(detail, sizeof detail, "形状 %lu×%lu 与 %lu×%lu 不同",
		               (unsigned long)a->rows, (unsigned long)a->cols,
		               (unsigned long)b->rows, (unsigned long)b->cols);
		fail(name, detail);
		return;
	}

	for (j = 0; j < a->cols; j++) {
		if (a->names[j] == NULL || b->names[j] == NULL ||
		    strcmp(a->names[j], b->names[j]) != 0) {
			(void)snprintf(detail, sizeof detail, "第 %lu 列列名不同",
			               (unsigned long)j);
			fail(name, detail);
			return;
		}
	}

	for (i = 0; i < a->rows; i++) {
		for (j = 0; j < a->cols; j++) {
			size_t      k = i * a->cols + j;
			const char *ca = a->cell[k];
			const char *cb = b->cell[k];

			if ((ca == NULL) != (cb == NULL)) {
				(void)snprintf(detail, sizeof detail,
				               "第 %lu 行第 %lu 列一个 NULL 一个非 NULL",
				               (unsigned long)i, (unsigned long)j);
				fail(name, detail);
				return;
			}
			if (ca == NULL) continue;

			if (a->length[k] != b->length[k] ||
			    memcmp(ca, cb, a->length[k]) != 0) {
				(void)snprintf(detail, sizeof detail,
				               "第 %lu 行第 %lu 列内容不同（%lu 与 %lu 字节）",
				               (unsigned long)i, (unsigned long)j,
				               (unsigned long)a->length[k],
				               (unsigned long)b->length[k]);
				fail(name, detail);
				return;
			}
		}
	}

	ok(name);
}

/* 单行单列的文本值，取不到就算失败并返回 NULL */
static const char *cell_text(const mc_rows *set, const char *name) {
	if (set->rows != 1 || set->cols != 1 || set->cell[0] == NULL) {
		fail(name, "期望单行单列的非 NULL 结果");
		return NULL;
	}
	return set->cell[0];
}

/* mc_stmt_each 的收集器：把交付的行按 "a|b|c" 拼起来，
 * 与 mc_stmt_rows / mc_query 的结果做字符串级对照。 */
typedef struct {
	char  *buf;
	size_t len;
	size_t cap;
	size_t rows;
	int    overflow;
} echo_state;

static int echo_row(void *ctx, const char *const *cell,
                    const size_t *length, size_t cols) {
	echo_state *s = (echo_state *)ctx;
	size_t      j;

	for (j = 0; j < cols; j++) {
		const char *piece = (cell[j] != NULL) ? cell[j] : "<NULL>";
		size_t      n = (cell[j] != NULL) ? length[j] : 6;
		size_t      need = s->len + n + 2;

		if (need > s->cap) {
			size_t want = (s->cap != 0) ? s->cap : 256;
			char  *p;

			while (want < need) want *= 2;
			p = (char *)realloc(s->buf, want);
			if (p == NULL) {
				s->overflow = 1;
				return 1;
			}
			s->buf = p;
			s->cap = want;
		}

		if (j > 0) s->buf[s->len++] = '|';
		(void)memcpy(s->buf + s->len, piece, n);
		s->len += n;
	}

	s->buf[s->len++] = '\n';
	s->rows++;
	return 0;
}

/* 只收一行就停：用来验证"回调主动停止"这条路径，以及之后的丢弃逻辑。
 * 语义上 fn 返回非 0 表示调用方不再需要后面的行，函数仍返回 MC_OK。 */
static int echo_one(void *ctx, const char *const *cell,
                    const size_t *length, size_t cols) {
	(void)echo_row(ctx, cell, length, cols);
	return 1;
}

/* 把物化结果集按同一格式拼成字符串，供上面的对照使用 */
static char *rows_to_text(const mc_rows *set) {
	echo_state s;
	size_t     i;
	size_t     j;

	(void)memset(&s, 0, sizeof s);

	for (i = 0; i < set->rows; i++) {
		for (j = 0; j < set->cols; j++) {
			size_t      k = i * set->cols + j;
			const char *piece = (set->cell[k] != NULL) ? set->cell[k] : "<NULL>";
			size_t      n = (set->cell[k] != NULL) ? set->length[k] : 6;
			size_t      need = s.len + n + 2;

			if (need > s.cap) {
				size_t want = (s.cap != 0) ? s.cap : 256;
				char  *p;

				while (want < need) want *= 2;
				p = (char *)realloc(s.buf, want);
				if (p == NULL) return NULL;
				s.buf = p;
				s.cap = want;
			}

			if (j > 0) s.buf[s.len++] = '|';
			(void)memcpy(s.buf + s.len, piece, n);
			s.len += n;
		}
		s.buf[s.len++] = '\n';
	}

	if (s.buf == NULL) {
		s.buf = (char *)malloc(1);
		if (s.buf == NULL) return NULL;
		s.buf[0] = '\0';
	} else {
		s.buf[s.len] = '\0';
	}

	return s.buf;
}

/* ------------------------------------------------------------------
 * 准备一个干净的库与表
 * ------------------------------------------------------------------ */

static mc_status reset_table(mc_conn *conn) {

	(void)mc_exec(conn, "DROP TABLE IF EXISTS " TBL, NULL);
	(void)mc_exec(conn, "CREATE DATABASE IF NOT EXISTS " DB, NULL);

	return mc_exec(conn,
	               "CREATE TABLE " TBL " ("
	               "id INT AUTO_INCREMENT PRIMARY KEY,"
	               "i BIGINT NULL,"
	               "u BIGINT UNSIGNED NULL,"
	               "d DOUBLE NULL,"
	               "s LONGTEXT NULL,"
	               "b LONGBLOB NULL)", NULL);
}

/* ------------------------------------------------------------------
 * 性能模式：prepare 每次 vs prepare 一次
 * ------------------------------------------------------------------ */

static double now_ms(void) {
	struct timespec t;

	(void)clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* 基准用的空回调：只数行数，不做别的事 */
static int count_row(void *ctx, const char *const *cell,
                     const size_t *length, size_t cols) {
	size_t *n = (size_t *)ctx;

	(void)cell;
	(void)length;
	(void)cols;

	(*n)++;
	return 0;
}

static int run_bench(mc_conn *conn, unsigned long long n) {

	/*
	 * 量的是 prepare 的协议开销，因此用只读语句，两条路径都不写库。
	 *
	 * 为什么不用 INSERT：8.0 默认开着 binlog 且 sync_binlog = 1，
	 * 每条写语句提交时都要刷一次日志。在这台机器上实测单行 INSERT
	 * 约 6 ms（MEMORY 引擎也一样，因为刷的是 binlog 不是表），
	 * 而 prepare 省下的一次往返是几十微秒——无论换成什么表引擎，
	 * 刷盘都会把要测的信号淹掉，比值只会落在 1.0 上下一片。
	 * 只读语句没有提交、没有日志，两条路径的差别就只剩"prepare 几次"。
	 *
	 * 两组的取数路径完全相同（都是 stream_deliver），差别只在
	 * A 每条语句都重新 prepare，B 只 prepare 一次。
	 */
	static const char *sql_literal = "SELECT 1 AS a, 2 AS b";
	static const char *sql_params  = "SELECT ? AS a, ? AS b";
	mc_param      p[2];
	mc_status     st;
	unsigned long long i;
	double        a1, a2, b1, b2;
	mc_stmt      *stmt = NULL;
	int           rc = 0;

	p[0] = p_int(1);
	p[1] = p_int(2);

	/* A：一次性接口，每次调用都 prepare + execute + 取数 + close */
	a1 = now_ms();
	for (i = 0; i < n; i++) {
		size_t rows = 0;

		st = mc_query_each_ex(conn, sql_literal, NULL, count_row, &rows, NULL);
		if (st != MC_OK || rows != 1) {
			(void)printf("A 组第 %llu 次失败：%s\n", i, mc_error(conn));
			rc = 1;
			goto out;
		}
	}
	a1 = now_ms() - a1;

	/* B：prepare 一次，之后每条语句只 execute + 取数 */
	if (mc_stmt_prepare(conn, sql_params, &stmt) != MC_OK) {
		(void)printf("B 组 prepare 失败：%s\n", mc_error(conn));
		rc = 1;
		goto out;
	}

	b1 = now_ms();
	for (i = 0; i < n; i++) {
		size_t rows = 0;

		st = mc_stmt_execute(stmt, p, 2, NULL);
		if (st == MC_OK) st = mc_stmt_each(stmt, NULL, count_row, &rows, NULL);
		if (st != MC_OK || rows != 1) {
			(void)printf("B 组第 %llu 次失败：%s\n", i, mc_error(conn));
			rc = 1;
			goto out;
		}
	}
	b1 = now_ms() - b1;
	mc_stmt_close(stmt);
	stmt = NULL;

	/* 第二遍把顺序反过来，排除"先跑的那一组受预热影响" */
	if (mc_stmt_prepare(conn, sql_params, &stmt) != MC_OK) {
		(void)printf("B 组第二次 prepare 失败：%s\n", mc_error(conn));
		rc = 1;
		goto out;
	}

	b2 = now_ms();
	for (i = 0; i < n; i++) {
		size_t rows = 0;

		st = mc_stmt_execute(stmt, p, 2, NULL);
		if (st == MC_OK) st = mc_stmt_each(stmt, NULL, count_row, &rows, NULL);
		if (st != MC_OK || rows != 1) {
			(void)printf("B 组第二次第 %llu 次失败：%s\n", i, mc_error(conn));
			rc = 1;
			goto out;
		}
	}
	b2 = now_ms() - b2;
	mc_stmt_close(stmt);
	stmt = NULL;

	a2 = now_ms();
	for (i = 0; i < n; i++) {
		size_t rows = 0;

		st = mc_query_each_ex(conn, sql_literal, NULL, count_row, &rows, NULL);
		if (st != MC_OK || rows != 1) {
			(void)printf("A 组第二次第 %llu 次失败：%s\n", i, mc_error(conn));
			rc = 1;
			goto out;
		}
	}
	a2 = now_ms() - a2;

	(void)printf("\n%llu 次单行查询（SELECT 1, 2）\n", n);
	(void)printf("  prepare 每次（mc_query_each_ex）  %8.1f ms   %8.1f ms"
	             "   %6.1f µs/次\n",
	             a1, a2, a2 * 1e3 / (double)n);
	(void)printf("  prepare 一次（mc_stmt）           %8.1f ms   %8.1f ms"
	             "   %6.1f µs/次\n",
	             b1, b2, b2 * 1e3 / (double)n);
	(void)printf("  比值（第二轮）                  %8.2f 倍"
	             "   每次省 %.1f µs\n",
	             a2 / b2, (a2 - b2) * 1e3 / (double)n);

out:
	mc_stmt_close(stmt);
	return rc;
}

/* ------------------------------------------------------------------
 * 主流程
 * ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {

	mc_conn    *conn = NULL;
	const char *why = "";
	mc_status   st;
	mc_stmt    *stmt = NULL;
	mc_rows     a;
	mc_rows     b;

	(void)printf("mysqlclient 语句对象测试\n\n");

	st = open_from_env(&conn, &why);
	if (st != MC_OK) {
		(void)printf("连不上数据库（%s）：%s\n", mc_status_string(st), why);
		(void)printf("用法：MC_HOST=127.0.0.1 MC_USER=root MC_PASSWORD= ./test_stmt\n");
		return 1;
	}

	st = reset_table(conn);
	if (st != MC_OK) {
		(void)printf("建表失败：%s\n", mc_error(conn));
		mc_close(conn);
		return 1;
	}

	if (argc >= 3 && strcmp(argv[1], "--bench") == 0) {
		int rc = run_bench(conn, strtoull(argv[2], NULL, 10));

		(void)mc_exec(conn, "DROP TABLE IF EXISTS " TBL, NULL);
		(void)mc_exec(conn, "DROP DATABASE IF EXISTS " DB, NULL);
		mc_close(conn);
		return rc;
	}

	/* ------------------------------------------------------------------
	 * 1. prepare 与元数据
	 * ------------------------------------------------------------------ */
	{
		mc_stmt *bad = (mc_stmt *)&bad;   /* 故意留一个非 NULL 的哨兵 */

		st = mc_stmt_prepare(conn, "SELECT ?, ?", &stmt);
		check_long("prepare 一条带两个占位符的语句", (long)st, MC_OK);
		check_long("占位符个数", (long)mc_stmt_param_count(stmt), 2);
		check_long("结果集列数", (long)mc_stmt_columns(stmt), 2);

		check_long("参数非法（sql 为空）返回 PARAM",
		           (long)mc_stmt_prepare(conn, "", &bad), MC_ERR_PARAM);
		check_true("失败时 out 被置成 NULL", bad == NULL);

		check_long("参数非法（sql 为 NULL）返回 PARAM",
		           (long)mc_stmt_prepare(conn, NULL, &bad), MC_ERR_PARAM);

		st = mc_stmt_prepare(conn, "SELECT * FROM no_such_table_here", &bad);
		check_long("表不存在在 prepare 阶段暴露", (long)st, MC_ERR_STMT);
		check_true("错误信息以 prepare: 开头",
		           strncmp(mc_error(conn), "prepare:", 8) == 0);
		check_true("prepare 失败后 out 为 NULL", bad == NULL);

		/* 写语句的列数是 0，取数应当被拒绝——而不是交付一个空结果集 */
		{
			mc_stmt *w = NULL;

			st = mc_stmt_prepare(conn, "INSERT INTO " TBL " (i) VALUES (?)", &w);
			check_long("prepare 写语句", (long)st, MC_OK);
			check_long("写语句的列数为 0", (long)mc_stmt_columns(w), 0);

			(void)memset(&a, 0, sizeof a);
			st = mc_stmt_rows(w, &a);
			check_long("未执行的语句取数返回 STATE", (long)st, MC_ERR_STATE);
			check_long("失败时 rows 被清零", (long)a.rows, 0);

			{
				size_t delivered = 12345;

				st = mc_stmt_each(w, NULL, echo_row, NULL, &delivered);
				check_long("未执行的语句 each 返回 STATE", (long)st, MC_ERR_STATE);
				check_long("失败时 out_rows 为 0", (long)delivered, 0);
			}

			mc_stmt_close(w);
		}

		/* 带参数的 SELECT：execute 之后才允许取数 */
		{
			mc_param two[2];

			two[0] = p_int(-42);
			two[1] = p_text("hello");

			(void)memset(&a, 0, sizeof a);
			st = mc_stmt_rows(stmt, &a);
			check_long("未 execute 就取数返回 STATE（SELECT）",
			           (long)st, MC_ERR_STATE);

			st = mc_stmt_execute(stmt, two, 2, NULL);
			check_long("execute 两个参数", (long)st, MC_OK);

			st = mc_stmt_rows(stmt, &a);
			check_long("取数成功", (long)st, MC_OK);
			check_long("一行", (long)a.rows, 1);
			check_long("两列", (long)a.cols, 2);
			check_true("int 参数原样",
			           a.cell[0] != NULL && strcmp(a.cell[0], "-42") == 0);
			check_true("text 参数原样",
			           a.cell[1] != NULL && strcmp(a.cell[1], "hello") == 0);
			check_true("列名来自语句",
			           a.names[0] != NULL && strcmp(a.names[0], "?") == 0);

			mc_rows_free(&a);

			/* 结果集只能交付一次：第二次必须明确报状态错，
			 * 而不是给一个"0 行"让它和"确实没有行"混起来。 */
			(void)memset(&b, 0, sizeof b);
			st = mc_stmt_rows(stmt, &b);
			check_long("结果集取第二遍返回 STATE", (long)st, MC_ERR_STATE);
			check_long("第二遍不给行", (long)b.rows, 0);
			check_true("错误信息说明已消费",
			           strstr(mc_error(conn), "consumed") != NULL);

			/* 再 execute 一次就该能再取一遍：这也是"复用"的最短证据 */
			st = mc_stmt_execute(stmt, two, 2, NULL);
			check_long("同一对象再次 execute", (long)st, MC_OK);
			st = mc_stmt_rows(stmt, &b);
			check_long("再次取数成功", (long)st, MC_OK);
			check_true("仍然是同样的值",
			           b.cell[0] != NULL && strcmp(b.cell[0], "-42") == 0);
			mc_rows_free(&b);

			mc_stmt_close(stmt);
			stmt = NULL;
		}
	}

	/* ------------------------------------------------------------------
	 * 2. 参数：个数、kind、长字段
	 * ------------------------------------------------------------------ */
	{
		mc_param p[2];
		mc_rows  set;

		p[0] = p_int(1);
		p[1] = p_int(2);

		st = mc_stmt_prepare(conn, "SELECT ? + ?", &stmt);
		check_long("prepare 两参数算术", (long)st, MC_OK);

		st = mc_stmt_execute(stmt, p, 1, NULL);
		check_long("参数少给一个返回 PARAM", (long)st, MC_ERR_PARAM);
		check_true("错误信息带两边的个数",
		           strstr(mc_error(conn), "2") != NULL &&
		           strstr(mc_error(conn), "1") != NULL);

		st = mc_stmt_execute(stmt, p, 2, NULL);
		check_long("补上参数后执行成功（失败没破坏对象）", (long)st, MC_OK);
		st = mc_stmt_rows(stmt, &set);
		check_long("取数成功", (long)st, MC_OK);
		check_true("1 + 2 = 3", set.cell[0] != NULL && strcmp(set.cell[0], "3") == 0);
		mc_rows_free(&set);

		mc_stmt_close(stmt);
		stmt = NULL;

		/* 非法 kind 必须在动网络之前被拒：SQL 本身也是错的，
		 * 若先 prepare，报出来的会是 "SQL 有语法错误"，把人引向错误方向。 */
		{
			mc_param bad;

			(void)memset(&bad, 0, sizeof bad);
			bad.kind = (mc_param_kind)77;

			st = mc_stmt_prepare(conn, "SELECT 1", &stmt);
			check_long("prepare 一条无参数语句", (long)st, MC_OK);

			st = mc_stmt_execute(stmt, &bad, 1, NULL);
			check_long("未知 kind 返回 PARAM", (long)st, MC_ERR_PARAM);
			check_true("未知 kind 报在 params 环节",
			           strncmp(mc_error(conn), "params:", 7) == 0);

			mc_stmt_close(stmt);
			stmt = NULL;
		}

		/* 长字段：len 超过 MC_BLOB_CHUNK 走 send_long_data，
		 * 复用时这条路径每次都要重走一遍（参数缓冲区不留在对象里）。 */
		{
			const size_t len = 3 * MC_BLOB_CHUNK + 13;
			char        *text = (char *)malloc(len + 1);

			if (text == NULL) {
				fail("分配长字段缓冲区", "malloc 返回 NULL");
			} else {
				size_t k;

				for (k = 0; k < len; k++) text[k] = (char)('a' + (int)(k % 26));
				text[len] = '\0';

				p[0] = p_text(text);
				p[1] = p_int(0);

				st = mc_stmt_prepare(conn, "INSERT INTO " TBL " (s) VALUES (?)", &stmt);
				check_long("prepare 长文本写入", (long)st, MC_OK);

				st = mc_stmt_execute(stmt, p, 1, NULL);
				check_long("长文本第一次写入", (long)st, MC_OK);

				st = mc_stmt_execute(stmt, p, 1, NULL);
				check_long("长文本第二次写入（复用）", (long)st, MC_OK);

				mc_stmt_close(stmt);
				stmt = NULL;

				(void)memset(&set, 0, sizeof set);
				/* 三个聚合要一起 SELECT：sql_mode 默认为 only_full_group_by，
				 * 混一个非聚合列会被服务端直接拒绝。 */
				st = mc_query(conn,
				        "SELECT MIN(LENGTH(s)), MAX(LENGTH(s)), COUNT(*) FROM " TBL,
				        &set);
				check_long("回查长文本", (long)st, MC_OK);
				if (st != MC_OK) (void)printf("      (%s)\n", mc_error(conn));
				check_true("两行的长度都等于输入的字节数",
				           st == MC_OK && set.rows == 1 && set.cell[0] != NULL &&
				           set.cell[1] != NULL &&
				           (size_t)strtoull(set.cell[0], NULL, 10) == len &&
				           (size_t)strtoull(set.cell[1], NULL, 10) == len);
				check_true("写入了两行",
				           st == MC_OK && set.rows == 1 && set.cell[2] != NULL &&
				           strcmp(set.cell[2], "2") == 0);
				mc_rows_free(&set);

				free(text);
			}
		}
	}

	/* ------------------------------------------------------------------
	 * 3. 结果集：与 mc_query 逐格比对
	 *
	 * 同一份数据走两条协议：mc_query 是文本协议 + store_result，
	 * mc_stmt 是二进制协议 + 逐行读。它们给出的 cell / length / NULL
	 * 必须完全一致，否则"两条取数路径"就成了两个语义。
	 * ------------------------------------------------------------------ */
	{
		mc_stmt *s = NULL;

		(void)mc_exec(conn, "TRUNCATE TABLE " TBL, NULL);

		st = mc_stmt_prepare(conn,
		        "INSERT INTO " TBL " (i,u,d,s,b) VALUES (?,?,?,?,?)", &s);
		check_long("prepare 多列写入", (long)st, MC_OK);

		{
			static const unsigned char bin[] = { 0x00, 0x01, 0x00, 0xff };
			mc_param p[5];

			p[0] = p_int(-7);
			p[1] = p_uint(18446744073709551615ULL);
			p[2] = p_double(-0.25);
			p[3] = p_text("睡蓝蕉");
			p[4] = p_blob(bin, sizeof bin);
			check_long("第一行", (long)mc_stmt_execute(s, p, 5, NULL), MC_OK);

			p[0] = p_null();
			p[1] = p_null();
			p[2] = p_null();
			p[3] = p_text("");
			p[4] = p_blob(NULL, 0);
			check_long("第二行（大量 NULL 与空值）",
			           (long)mc_stmt_execute(s, p, 5, NULL), MC_OK);

			mc_stmt_close(s);
			s = NULL;
		}

		/* 带参数的 SELECT 与字面量 SELECT 的对照 */
		{
			mc_param p[2];
			unsigned long long affected = 999;

			p[0] = p_int(1);
			p[1] = p_int(5);

			st = mc_stmt_prepare(conn,
			        "SELECT id, i, u, d, s, b FROM " TBL
			        " WHERE id >= ? AND id < ? ORDER BY id", &stmt);
			check_long("prepare 带参数的查询", (long)st, MC_OK);

			st = mc_stmt_execute(stmt, p, 2, &affected);
			check_long("执行带参数的查询", (long)st, MC_OK);

			/* 返回结果集的语句不写 affected：那种语句"受影响行数"在服务端
			 * 是行数，与交付行数含义不同，混用会让调用方以为两者可以互换。 */
			check_long("返回结果集时不写 affected", (long)affected, 999);

			(void)memset(&a, 0, sizeof a);
			st = mc_stmt_rows(stmt, &a);
			check_long("物化取数", (long)st, MC_OK);
			check_long("两行", (long)a.rows, 2);
			check_long("六列", (long)a.cols, 6);

			(void)memset(&b, 0, sizeof b);
			st = mc_query(conn,
			        "SELECT id, i, u, d, s, b FROM " TBL
			        " WHERE id >= 1 AND id < 5 ORDER BY id", &b);
			check_long("字面量查询", (long)st, MC_OK);

			rows_equal("语句对象与 mc_query 逐格相同", &a, &b);

			check_true("第一行的 BLOB 含内嵌 0x00（长度为 4）",
			           a.length[5] == 4 && memcmp(a.cell[5], "\x00\x01\x00\xff", 4) == 0);
			check_true("第二行的 i 是 SQL NULL", a.cell[7] == NULL);
			check_true("第二行的 s 是空串而不是 NULL",
			           a.cell[10] != NULL && a.length[10] == 0);

			mc_rows_free(&a);
			mc_rows_free(&b);

			/* same statement, same result twice: 复用 SELECT 的关键一条 */
			st = mc_stmt_execute(stmt, p, 2, NULL);
			check_long("同一条 SELECT 再执行", (long)st, MC_OK);
			(void)memset(&a, 0, sizeof a);
			st = mc_stmt_rows(stmt, &a);
			check_long("再取数（不报 Commands out of sync）", (long)st, MC_OK);
			check_long("行数仍是 2", (long)a.rows, 2);
			mc_rows_free(&a);

			/* each 与 rows 必须交付同样的内容 */
			{
				echo_state echo;
				char      *text;

				(void)memset(&echo, 0, sizeof echo);

				st = mc_stmt_execute(stmt, p, 2, NULL);
				check_long("为 each 再执行一次", (long)st, MC_OK);

				{
					size_t delivered = 0;

					st = mc_stmt_each(stmt, NULL, echo_row, &echo, &delivered);
					check_long("mc_stmt_each 取数", (long)st, MC_OK);
					check_long("交付 2 行", (long)delivered, 2);
				}

				st = mc_stmt_execute(stmt, p, 2, NULL);
				(void)memset(&a, 0, sizeof a);
				st = mc_stmt_rows(stmt, &a);

				text = rows_to_text(&a);
				check_true("each 与 rows 交付的内容相同",
				           text != NULL && echo.buf != NULL &&
				           strcmp(text, echo.buf) == 0);

				free(text);
				free(echo.buf);
				mc_rows_free(&a);
			}

			/* 提前停止：回调读一行就返回非 0，剩下的行由下一次 execute 丢弃 */
			{
				echo_state echo;
				size_t     delivered = 0;

				(void)memset(&echo, 0, sizeof echo);

				st = mc_stmt_execute(stmt, p, 2, NULL);
				check_long("为提前停止再执行一次", (long)st, MC_OK);

				st = mc_stmt_each(stmt, NULL, echo_one, &echo, &delivered);
				check_long("回调主动停止仍返回 OK", (long)st, MC_OK);
				check_long("只交付 1 行", (long)delivered, 1);

				free(echo.buf);

				/*
				 * 上一次只读了 1 行（还剩 1 行在协议里），此时再 execute
				 * 必须能正常返回，并且下一次取数拿到的是完整的两行。
				 *
				 * 注入验证的结论要说清楚：把 stmt_discard_result 的调用删掉，
				 * 这组断言仍然全绿——8.0.46 的 mysql_stmt_execute 自己就会
				 * 丢弃上一次没读完的行。所以这里能证明的是"复用不被半读
				 * 破坏"，不是"显式 free_result 被执行"。实现里仍然显式调它，
				 * 依据是文档语义而不是实测差异：协议是半双工的，未读完的
				 * 结果集占着连接，依赖库自己清理属于依赖实现细节。
				 * 真正能区分的情形是服务端游标（MYSQL_STMT_ATTR_CURSOR_TYPE），
				 * 本模块没有用。
				 */
				st = mc_stmt_execute(stmt, p, 2, NULL);
				check_long("结果集没读完就再执行（丢弃路径）", (long)st, MC_OK);

				(void)memset(&a, 0, sizeof a);
				st = mc_stmt_rows(stmt, &a);
				check_long("丢弃之后仍能取到完整的两行", (long)st, MC_OK);
				check_long("行数 2", (long)a.rows, 2);
				mc_rows_free(&a);
			}

			mc_stmt_close(stmt);
			stmt = NULL;
		}

		/* 空结果集：有列名、0 行，且不算错误 */
		{
			st = mc_stmt_prepare(conn, "SELECT id, s FROM " TBL " WHERE 1 = 0", &stmt);
			check_long("prepare 空结果集查询", (long)st, MC_OK);
			st = mc_stmt_execute(stmt, NULL, 0, NULL);
			check_long("执行空结果集查询", (long)st, MC_OK);

			(void)memset(&a, 0, sizeof a);
			st = mc_stmt_rows(stmt, &a);
			check_long("空结果集不算错误", (long)st, MC_OK);
			check_long("0 行", (long)a.rows, 0);
			check_long("2 列", (long)a.cols, 2);
			check_true("空结果集也有列名",
			           a.names[0] != NULL && strcmp(a.names[0], "id") == 0);
			mc_rows_free(&a);

			mc_stmt_close(stmt);
			stmt = NULL;
		}
	}

	/* ------------------------------------------------------------------
	 * 4. 复用：批量写入的形状
	 *
	 * 这是语句对象存在的理由。同一条 INSERT 执行 N 次，
	 * 每次的受影响行数都必须是 1，总行数必须是 N。
	 * ------------------------------------------------------------------ */
	{
		const int n = 50;
		int       k;
		int       bad_affected = 0;
		mc_param  p;

		(void)mc_exec(conn, "TRUNCATE TABLE " TBL, NULL);

		st = mc_stmt_prepare(conn, "INSERT INTO " TBL " (i) VALUES (?)", &stmt);
		check_long("prepare 批量写入", (long)st, MC_OK);

		for (k = 0; k < n; k++) {
			unsigned long long affected = 0;

			p = p_int(k);
			st = mc_stmt_execute(stmt, &p, 1, &affected);

			if (st != MC_OK) break;
			if (affected != 1) bad_affected++;
		}

		check_true("50 次执行全部成功且每次都影响 1 行",
		           st == MC_OK && bad_affected == 0);
		mc_stmt_close(stmt);
		stmt = NULL;

		(void)memset(&a, 0, sizeof a);
		st = mc_query(conn, "SELECT COUNT(*), MIN(i), MAX(i) FROM " TBL, &a);
		check_long("回查", (long)st, MC_OK);
		check_true("表里正好 50 行", a.cell[0] != NULL && strcmp(a.cell[0], "50") == 0);
		check_true("最小值 0", a.cell[1] != NULL && strcmp(a.cell[1], "0") == 0);
		check_true("最大值 49", a.cell[2] != NULL && strcmp(a.cell[2], "49") == 0);
		mc_rows_free(&a);

		/* 失败之后对象仍可用：主键冲突不该让整批作废 */
		(void)mc_exec(conn, "TRUNCATE TABLE " TBL, NULL);
		(void)mc_exec(conn, "INSERT INTO " TBL " (id, i) VALUES (1, 100)", NULL);

		st = mc_stmt_prepare(conn, "INSERT INTO " TBL " (id, i) VALUES (?,?)", &stmt);
		check_long("prepare 带主键的写入", (long)st, MC_OK);

		{
			mc_param two[2];
			unsigned long long affected = 0;

			two[0] = p_int(1);          /* 主键已存在 */
			two[1] = p_int(1);
			st = mc_stmt_execute(stmt, two, 2, &affected);
			check_long("主键冲突返回 QUERY", (long)st, MC_ERR_QUERY);
			check_true("错误信息报在 execute 环节",
			           strncmp(mc_error(conn), "execute:", 8) == 0);

			two[0] = p_int(2);          /* 换一个主键，同一个对象 */
			two[1] = p_int(2);
			affected = 0;
			st = mc_stmt_execute(stmt, two, 2, &affected);
			check_long("失败之后同一对象仍可执行", (long)st, MC_OK);
			check_long("受影响 1 行", (long)affected, 1);
		}

		mc_stmt_close(stmt);
		stmt = NULL;

		(void)memset(&a, 0, sizeof a);
		st = mc_query(conn, "SELECT COUNT(*) FROM " TBL, &a);
		check_true("表里 2 行（1 行原有 + 1 行补写）",
		           st == MC_OK && a.cell[0] != NULL && strcmp(a.cell[0], "2") == 0);
		mc_rows_free(&a);
	}

	/* ------------------------------------------------------------------
	 * 5. 空操作与边界
	 * ------------------------------------------------------------------ */
	{
		mc_stmt_close(NULL);
		ok("mc_stmt_close(NULL) 是空操作");

		check_long("mc_stmt_param_count(NULL)", (long)mc_stmt_param_count(NULL), 0);
		check_long("mc_stmt_columns(NULL)", (long)mc_stmt_columns(NULL), 0);
		check_long("mc_stmt_rows(NULL)", (long)mc_stmt_rows(NULL, &a), MC_ERR_PARAM);
		check_long("mc_stmt_execute(NULL)",
		           (long)mc_stmt_execute(NULL, NULL, 0, NULL), MC_ERR_PARAM);

		/* 无参数语句：n 为 0、params 为 NULL 是合法的 */
		st = mc_stmt_prepare(conn, "SELECT 7", &stmt);
		check_long("prepare 无参数语句", (long)st, MC_OK);
		check_long("n 为 0 执行", (long)mc_stmt_execute(stmt, NULL, 0, NULL), MC_OK);
		(void)memset(&a, 0, sizeof a);
		check_long("取数", (long)mc_stmt_rows(stmt, &a), MC_OK);
		check_true("值为 7", cell_text(&a, "SELECT 7 的值") != NULL &&
		                     strcmp(a.cell[0], "7") == 0);
		mc_rows_free(&a);
		mc_stmt_close(stmt);
		stmt = NULL;

		/* params 为 NULL 而 n 非 0 */
		st = mc_stmt_prepare(conn, "SELECT ?", &stmt);
		check_long("prepare 单参数语句", (long)st, MC_OK);
		check_long("params 为 NULL 且 n 非 0",
		           (long)mc_stmt_execute(stmt, NULL, 1, NULL), MC_ERR_PARAM);
		mc_stmt_close(stmt);
		stmt = NULL;
	}

	/* ---- 清理 ---- */
	(void)mc_exec(conn, "DROP TABLE IF EXISTS " TBL, NULL);
	(void)mc_exec(conn, "DROP DATABASE IF EXISTS " DB, NULL);

	mc_close(conn);

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_fails);

	return g_fails;
}
