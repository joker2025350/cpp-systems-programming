/*
 * test_stream.c —— mc_query_each 的两种用法
 *
 *   test_stream                      正确性：与 mc_query 逐格比对
 *   test_stream mem query '<SQL>'    测量：物化取数的行数 / 校验和 / 峰值内存
 *   test_stream mem each  '<SQL>'    测量：流式取数的同一组指标
 *
 * 为什么测量要分成两个进程：峰值 RSS 是进程级、单调不减的指标
 * （getrusage 的 ru_maxrss），同一个进程里先后跑两种取数方式，
 * 测到的永远是先跑的那个的峰值。
 *
 * 需要一台运行中的 MySQL，连接参数走环境变量，默认连本机 3306 的 root：
 *   MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root MC_PASSWORD= \
 *     ./test_stream
 *
 * 正确性部分自建、自删一张表（表名 mc_stream_t），不碰其它数据。
 * 这种"需要服务器才能验"的内容不并进 make test：没有数据库的机器上
 * 它必然全红，那样的测试等于没有（见 test_db.c 顶部的说明）。
 */

#include "mysqlclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/resource.h>

static int g_checks = 0;
static int g_fails  = 0;

static void ok(const char *name) {
	g_checks++;
	(void)printf("ok    %s\n", name);
}

static void fail(const char *name, const char *detail) {
	g_checks++;
	g_fails++;
	(void)printf("FAIL  %-52s %s\n", name, detail);
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

static void check_str(const char *name, const char *actual, const char *expected) {
	char detail[192];

	if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) {
		ok(name);
		return;
	}

	(void)snprintf(detail, sizeof detail, "实际 \"%s\"，期望 \"%s\"",
	               (actual != NULL) ? actual : "(null)",
	               (expected != NULL) ? expected : "(null)");
	fail(name, detail);
}

/* ------------------------------------------------------------------
 * 建连：与驱动层同样的三层来源里只用前两层（环境变量 + 内置默认值），
 * 命令行这一层对测试没有意义。
 * ------------------------------------------------------------------ */

static mc_status open_from_env(mc_conn **out, const char **why) {
	mc_options opt;
	const char *v;
	mc_status   st;

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

/* ------------------------------------------------------------------
 * 正确性：流式取回的行必须与物化取回的行逐格相同
 * ------------------------------------------------------------------ */

#define ROWS 7

/*
 * 逐格比对：在回调里立刻和物化的结果集比较。
 * 不在回调外保存指针，也就不存在"回调返回后 cell 失效"的陷阱。
 */
typedef struct {
	const mc_rows *expect;
	size_t         cursor;    /* 当前该比对第几行 */
	int            bad;
	char           detail[256];
} compare_state;

static void set_detail(compare_state *st, const char *what, size_t row, size_t j) {
	(void)snprintf(st->detail, sizeof st->detail, "%s（第 %lu 行第 %lu 列）",
	               what, (unsigned long)row, (unsigned long)j);
}

static int compare_row(void *ctx, const char *const *cell,
                       const size_t *length, size_t cols) {
	compare_state *st = (compare_state *)ctx;
	size_t         i = st->cursor;
	size_t         j;

	st->cursor++;

	if (i >= st->expect->rows) {
		(void)snprintf(st->detail, sizeof st->detail,
		               "流式多出一行（第 %lu 行）", (unsigned long)i);
		st->bad = 1;
		return 1;
	}

	if (cols != st->expect->cols) {
		(void)snprintf(st->detail, sizeof st->detail,
		               "列数 %lu，期望 %lu",
		               (unsigned long)cols, (unsigned long)st->expect->cols);
		st->bad = 1;
		return 1;
	}

	for (j = 0; j < cols; j++) {
		const char *want = MC_CELL(st->expect, i, j);

		if (want == NULL) {
			if (cell[j] != NULL) {
				set_detail(st, "期望 SQL NULL，实际有值", i, j);
				st->bad = 1;
				return 1;
			}
			continue;
		}

		if (cell[j] == NULL) {
			set_detail(st, "期望有值，实际是 SQL NULL", i, j);
			st->bad = 1;
			return 1;
		}

		if (length[j] != st->expect->length[i * cols + j]) {
			(void)snprintf(st->detail, sizeof st->detail,
			               "长度 %lu，期望 %lu（第 %lu 行第 %lu 列）",
			               (unsigned long)length[j],
			               (unsigned long)st->expect->length[i * cols + j],
			               (unsigned long)i, (unsigned long)j);
			st->bad = 1;
			return 1;
		}

		/*
		 * 用 memcmp 比 length 字节，不用 strcmp：
		 * BLOB 列的内容里可以含 '\0'，strcmp 在这里会提前判等。
		 */
		if (length[j] > 0 && memcmp(cell[j], want, length[j]) != 0) {
			set_detail(st, "内容不同", i, j);
			st->bad = 1;
			return 1;
		}
	}

	return 0;
}

/* 只数行、到 limit 就停 */
typedef struct {
	size_t rows;
	size_t stop_at;   /* 0 表示不停 */
} count_state;

static int count_row(void *ctx, const char *const *cell,
                     const size_t *length, size_t cols) {
	count_state *st = (count_state *)ctx;

	(void)cell;
	(void)length;
	(void)cols;

	st->rows++;
	return (st->stop_at != 0 && st->rows >= st->stop_at) ? 1 : 0;
}

static const char *TBL = "mc_stream_t";

/*
 * ---- 列名回调的夹具 ----
 *
 * saved[] 里存的是**列名回调收到的指针**，不是拷贝：头文件的契约说
 * names 与其中的字符串在整个 mc_query_each_ex 调用期间有效，那么行回调
 * 里继续解引用它们就是合法的，而不该出现的释放会被 ASan 当场抓住。
 * cols_fnv 是列名回调里算的指纹，行回调里用同一批指针重算一遍再比——
 * 指针"还在但内容被改过"（缓冲区被复用）也能被发现。
 */
typedef struct {
	size_t      calls;          /* 列名回调被调用几次 */
	size_t      cols;           /* 列名回调收到的列数 */
	const char *saved[8];       /* 存下的列名指针（夹具上限 8 列） */
	size_t      rows;           /* 行回调被调用次数 */
	int         stop;           /* 非 0：列名回调直接放弃取数 */
	int         cols_bad;       /* 行回调看到的列数与列名回调不一致 */
	int         names_bad;      /* 行回调里重算的指纹与列名回调不同 */
	unsigned long long fnv;     /* 列名回调里算的列名指纹 */
	char        joined[256];    /* 用 '|' 连起来的列名，便于一条断言比全部 */
} names_state;

static unsigned long long fnv_str(unsigned long long h, const char *s) {
	if (s == NULL) {
		h ^= 0xffu;
		return h * 1099511628211ull;
	}
	for (; *s != '\0'; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ull;
	}
	return h;
}

static unsigned long long names_fnv(const char *const *names, size_t cols) {
	unsigned long long h = 14695981039346656037ull;
	size_t             j;

	for (j = 0; j < cols; j++) h = fnv_str(h, names[j]);
	return h;
}

/* 把列名用 '|' 连起来。列数超过夹具上限时只连前 8 个（cols_bad 会报）。 */
static void join_names(names_state *st) {
	size_t used = 0;
	size_t j;

	st->joined[0] = '\0';

	for (j = 0; j < st->cols && j < 8; j++) {
		int n = snprintf(st->joined + used, sizeof st->joined - used, "%s%s",
		                 (j == 0) ? "" : "|",
		                 (st->saved[j] != NULL) ? st->saved[j] : "(null)");

		if (n < 0 || (size_t)n >= sizeof st->joined - used) break;
		used += (size_t)n;
	}
}

static int capture_cols(void *ctx, const char *const *names, size_t cols) {
	names_state *st = (names_state *)ctx;
	size_t       j;

	st->calls++;
	st->cols = cols;
	if (cols > 8) st->cols_bad = 1;

	for (j = 0; j < cols && j < 8; j++) st->saved[j] = names[j];
	st->fnv = names_fnv(st->saved, cols < 8 ? cols : 8);

	join_names(st);

	return st->stop;
}

/*
 * 行回调里把 saved 里的指针再读一遍、重算指纹。
 * 指针若已失效（代码把名字放在语句对象上、又在行循环里 close 了语句），
 * ASan 会当场报 heap-use-after-free；缓冲区若被复用，指纹对不上。
 */
static int names_row(void *ctx, const char *const *cell,
                     const size_t *length, size_t cols) {
	names_state *st = (names_state *)ctx;
	size_t       n = (cols < 8) ? cols : 8;

	(void)cell;
	(void)length;

	if (st->cols != cols) st->cols_bad = 1;
	if (names_fnv(st->saved, n) != st->fnv) st->names_bad = 1;

	st->rows++;
	return 0;
}

static int run_correctness(void) {
	mc_conn     *conn = NULL;
	const char  *why = "";
	mc_status    st;
	mc_rows      expect;
	size_t       got = 0;
	size_t       i;
	char         sql[256];
	compare_state cmp;
	count_state   cnt;
	unsigned char big[100 * 1024];

	(void)printf("mysqlclient 流式取数（游标）测试\n\n");

	st = open_from_env(&conn, &why);
	if (st != MC_OK) {
		(void)printf("连接失败：%s (%s)\n", why, mc_status_string(st));
		(void)printf("先启动数据库，或用 MC_HOST / MC_PORT / MC_USER 指定。\n");
		return 1;
	}

	/*
	 * 测试自己建库再 USE，而不是要求使用者先建好：
	 * 连上去时不一定有默认库，"No database selected" 会让所有用例
	 * 在第一步就红掉，而那与要验证的东西毫无关系。
	 * 库名可以用 MC_TEST_DB 换，便于在同一台服务器上并行跑多份。
	 */
	{
		const char *db = getenv("MC_TEST_DB");

		if (db == NULL || db[0] == '\0') db = "mc_stream_db";

		(void)snprintf(sql, sizeof sql,
		               "CREATE DATABASE IF NOT EXISTS `%s`", db);
		st = mc_exec(conn, sql, NULL);
		if (st != MC_OK) {
			(void)printf("建库失败：%s\n", mc_error(conn));
			mc_close(conn);
			return 1;
		}

		(void)snprintf(sql, sizeof sql, "USE `%s`", db);
		st = mc_exec(conn, sql, NULL);
		if (st != MC_OK) {
			(void)printf("切换库失败：%s\n", mc_error(conn));
			mc_close(conn);
			return 1;
		}
	}

	(void)mc_exec(conn, "DROP TABLE IF EXISTS mc_stream_t", NULL);

	st = mc_exec(conn,
	             "CREATE TABLE mc_stream_t ("
	             " id INT PRIMARY KEY AUTO_INCREMENT,"
	             " s  VARCHAR(64),"
	             " n  INT,"
	             " b  LONGBLOB)", NULL);
	if (st != MC_OK) {
		(void)printf("建表失败：%s\n", mc_error(conn));
		mc_close(conn);
		return 1;
	}

	/*
	 * 七行覆盖这些情况：
	 *   普通文本 / 空串 / 全 NULL / 内容含 '\0' 的二进制 /
	 *   超过初始缓冲区 256 字节的 100 KiB 值 / UTF-8 多字节文本 / 负整数。
	 * 少任何一种，流式路径上的某条分支就不会被走到。
	 */
	(void)mc_exec(conn, "INSERT INTO mc_stream_t(s,n) VALUES('plain', 7)", NULL);
	(void)mc_exec(conn, "INSERT INTO mc_stream_t(s,n) VALUES('', 0)", NULL);
	(void)mc_exec(conn, "INSERT INTO mc_stream_t(s,n) VALUES(NULL, NULL)", NULL);
	{
		const char nul[3] = { 'a', '\0', 'b' };

		st = mc_blob_write(conn, "INSERT INTO mc_stream_t(s,b) VALUES('nul', ?)",
		                   nul, sizeof nul);
		check_long("写入含 '\\0' 的二进制值", st, MC_OK);
	}
	for (i = 0; i < sizeof big; i++) {
		big[i] = (unsigned char)(i & 0xff);
	}
	st = mc_blob_write(conn, "INSERT INTO mc_stream_t(s,b) VALUES('big', ?)",
	                   big, sizeof big);
	check_long("写入 100 KiB 值", st, MC_OK);
	(void)mc_exec(conn, "INSERT INTO mc_stream_t(s,n) VALUES('睡蓝蕉', -42)", NULL);
	(void)mc_exec(conn, "INSERT INTO mc_stream_t(s,n) VALUES('tail', 2147483647)", NULL);

	(void)snprintf(sql, sizeof sql, "SELECT id, s, n, b FROM %s ORDER BY id", TBL);

	/* ---- 1. 与物化取数逐格比对 ---- */
	st = mc_query(conn, sql, &expect);
	check_long("mc_query 取回结果集", st, MC_OK);
	check_long("结果集行数", (long)expect.rows, ROWS);

	memset(&cmp, 0, sizeof cmp);
	cmp.expect = &expect;

	st = mc_query_each(conn, sql, compare_row, &cmp, &got);
	check_long("mc_query_each 返回 MC_OK", st, MC_OK);
	check_long("交付行数与 mc_query 一致", (long)got, (long)expect.rows);
	check_long("逐格比对的行数", (long)cmp.cursor, (long)expect.rows);
	check_long("逐格比对无差异", cmp.bad, 0);
	if (cmp.bad) fail("比对细节", cmp.detail);
	else         ok("流式与物化的单元格逐字节相同");

	/* 100 KiB 那一行单独确认长度：它必然走过 fetch_column 那条路径。
	 * 注意第 2 列可以是 SQL NULL（s 为 NULL 的那一行就是），
	 * 直接 strcmp 会解引用空指针。 */
	for (i = 0; i < expect.rows; i++) {
		const char *s = MC_CELL(&expect, i, 1);

		if (s != NULL && strcmp(s, "big") == 0) {
			check_long("100 KiB 值长度", (long)expect.length[i * 4 + 3],
			           (long)sizeof big);
			break;
		}
	}
	mc_rows_free(&expect);

	/* ---- 2. 主动停止 ---- */
	memset(&cnt, 0, sizeof cnt);
	cnt.stop_at = 3;

	got = 0;
	st = mc_query_each(conn, sql, count_row, &cnt, &got);
	check_long("回调返回非 0 时返回 MC_OK", st, MC_OK);
	check_long("提前停止时交付 3 行", (long)got, 3);
	check_long("提前停止时回调被调用 3 次", (long)cnt.rows, 3);

	/*
	 * ---- 3. 提前停止之后连接必须还能用 ----
	 *
	 * 这一条是整个实现里最容易出错的地方：结果集没读完就发下一条命令，
	 * 协议上是半双工违例，库会报 "Commands out of sync"。
	 * 实现里靠 mysql_stmt_free_result 把剩下的行读掉，这里验证它确实有效。
	 */
	{
		mc_rows one;

		st = mc_query(conn, "SELECT 1", &one);
		check_long("提前停止后同一连接仍可用", st, MC_OK);
		if (st == MC_OK) {
			check_true("提前停止后查询结果正确",
			           one.rows == 1 && strcmp(MC_CELL(&one, 0, 0), "1") == 0);
			mc_rows_free(&one);
		}
	}

	/* ---- 4. 空结果集：0 行是成功，不是错误 ---- */
	(void)snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE id < 0", TBL);
	got = 12345;   /* 故意给个脏值，验证失败/成功路径都会写它 */
	memset(&cnt, 0, sizeof cnt);
	st = mc_query_each(conn, sql, count_row, &cnt, &got);
	check_long("零行的查询返回 MC_OK", st, MC_OK);
	check_long("零行的查询交付 0 行", (long)got, 0);

	/* ---- 5. 错误路径 ---- */
	st = mc_query_each(conn, "UPDATE mc_stream_t SET n = n WHERE id = 1",
	                   count_row, &cnt, &got);
	check_long("不返回结果集的语句被拒绝", st, MC_ERR_RESULT);
	check_true("提示改用 mc_exec", strstr(mc_error(conn), "mc_exec") != NULL);

	got = 999;
	st = mc_query_each(conn, "SELECT FROM", count_row, &cnt, &got);
	check_long("语法错误在 prepare 阶段被拒", st, MC_ERR_STMT);
	check_long("语法错误时不报告任何已交付行", (long)got, 0);
	check_true("语法错误信息带 prepare 前缀",
	           strncmp(mc_error(conn), "prepare:", 8) == 0);

	memset(&cnt, 0, sizeof cnt);
	st = mc_query_each(conn, sql, NULL, &cnt, &got);
	check_long("回调为 NULL 被拒绝", st, MC_ERR_PARAM);

	st = mc_query_each(conn, "", count_row, &cnt, &got);
	check_long("空 SQL 被拒绝", st, MC_ERR_PARAM);

	/* ---- 6. 列名（mc_query_each_ex） ---- */
	{
		names_state ns;
		char        longname[65];

		/* 6a. 主查询：列名与 mc_query 的 names 逐个相同，行数不变 */
		memset(&ns, 0, sizeof ns);
		(void)snprintf(sql, sizeof sql,
		               "SELECT id, s, n, b FROM %s ORDER BY id", TBL);
		got = 0;
		st = mc_query_each_ex(conn, sql, capture_cols, names_row, &ns, &got);
		check_long("each_ex 返回 MC_OK", st, MC_OK);
		check_long("列名回调恰好被调用 1 次", (long)ns.calls, 1);
		check_long("列名回调收到 4 列", (long)ns.cols, 4);
		check_str("列名内容与顺序", ns.joined, "id|s|n|b");
		check_long("行回调看到的列数与列名回调一致", ns.cols_bad, 0);
		check_long("行回调里列名指针仍然有效", ns.names_bad, 0);
		check_long("交付行数不受列名回调影响", (long)got, ROWS);
		check_long("行回调次数", (long)ns.rows, ROWS);

		/* 6b. 0 行的结果集同样有列名：元数据与行数无关 */
		memset(&ns, 0, sizeof ns);
		(void)snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE id < 0", TBL);
		got = 0;
		st = mc_query_each_ex(conn, sql, capture_cols, names_row, &ns, &got);
		check_long("空结果集返回 MC_OK", st, MC_OK);
		check_long("空结果集也交付列名", (long)ns.calls, 1);
		check_str("空结果集的列名", ns.joined, "id");
		check_long("空结果集不交付行", (long)got, 0);
		check_long("空结果集行回调未被调用", (long)ns.rows, 0);

		/* 6c. 别名是服务器给的名字，不是 SQL 里的表达式 */
		memset(&ns, 0, sizeof ns);
		st = mc_query_each_ex(conn, "SELECT 1 AS a, 2 + 2 AS b, 'x' AS c",
		                      capture_cols, names_row, &ns, &got);
		check_long("别名查询返回 MC_OK", st, MC_OK);
		check_str("列名取服务器的别名", ns.joined, "a|b|c");

		/* 6d. 没有别名时服务器给的就是表达式原文 */
		memset(&ns, 0, sizeof ns);
		st = mc_query_each_ex(conn, "SELECT 1 + 1", capture_cols, names_row,
		                      &ns, &got);
		check_long("表达式查询返回 MC_OK", st, MC_OK);
		check_str("无别名时列名是表达式原文", ns.joined, "1 + 1");

		/* 6e. UTF-8 列名按字节原样交付（9 字节） */
		memset(&ns, 0, sizeof ns);
		st = mc_query_each_ex(conn, "SELECT 1 AS `睡蓝蕉`", capture_cols,
		                      names_row, &ns, &got);
		check_long("UTF-8 列名查询返回 MC_OK", st, MC_OK);
		check_str("UTF-8 列名不被转义", ns.joined, "睡蓝蕉");
		check_long("UTF-8 列名字节数", (long)strlen(ns.joined), 9);

		/* 6f. 64 字节的列名不被截断——这条挡的是"按固定长度拷贝" */
		memset(longname, 'a', 64);
		longname[64] = '\0';
		memset(&ns, 0, sizeof ns);
		(void)snprintf(sql, sizeof sql, "SELECT 1 AS `%s`", longname);
		st = mc_query_each_ex(conn, sql, capture_cols, names_row, &ns, &got);
		check_long("64 字节列名查询返回 MC_OK", st, MC_OK);
		check_long("64 字节列名完整交付", (long)strlen(ns.joined), 64);
		check_str("64 字节列名内容", ns.joined, longname);

		/* 6g. 列名里的空格是内容，不是分隔符 */
		memset(&ns, 0, sizeof ns);
		st = mc_query_each_ex(conn, "SELECT 1 AS `a b`", capture_cols,
		                      names_row, &ns, &got);
		check_long("含空格的列名查询返回 MC_OK", st, MC_OK);
		check_str("列名里的空格原样保留", ns.joined, "a b");

		/* 6h. 列名回调返回非 0：放弃取数，不是错误 */
		memset(&ns, 0, sizeof ns);
		ns.stop = 1;
		got = 777;
		(void)snprintf(sql, sizeof sql,
		               "SELECT id, s, n, b FROM %s ORDER BY id", TBL);
		st = mc_query_each_ex(conn, sql, capture_cols, names_row, &ns, &got);
		check_long("列名回调放弃时返回 MC_OK", st, MC_OK);
		check_long("列名回调放弃时交付 0 行", (long)got, 0);
		check_long("列名回调放弃时行回调未被调用", (long)ns.rows, 0);

		/* 6i. 执行失败时一行列名都不交付：调用方不会误以为表头已打出 */
		memset(&ns, 0, sizeof ns);
		st = mc_query_each_ex(conn, "SELECT id FROM mc_no_such_table",
		                      capture_cols, names_row, &ns, &got);
		check_true("表不存在时返回失败", st != MC_OK);
		check_long("失败时不调用列名回调", (long)ns.calls, 0);
		check_long("失败时不交付行", (long)got, 0);

		/* 6j. 提前停止后连接仍可用（列名路径也要排空结果集） */
		memset(&cnt, 0, sizeof cnt);
		cnt.stop_at = 2;
		(void)snprintf(sql, sizeof sql, "SELECT id FROM %s ORDER BY id", TBL);
		st = mc_query_each(conn, sql, count_row, &cnt, &got);
		check_long("列名路径之后连接仍可用", st, MC_OK);
	}

	/* ---- 7. 收尾 ---- */
	(void)mc_exec(conn, "DROP TABLE IF EXISTS mc_stream_t", NULL);
	mc_close(conn);

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_fails);
	return g_fails;
}

/* ------------------------------------------------------------------
 * 测量：同一进程内只做一种取数，峰值 RSS 才有意义
 * ------------------------------------------------------------------ */

typedef struct {
	unsigned long long rows;
	unsigned long long fnv;
	unsigned long long bytes;
} digest_state;

/* FNV-1a 64 位：逐字节混入，BLOB 里的 '\0' 同样参与，
 * 因此"长度对但内容错"也能被发现。 */
static int digest_row(void *ctx, const char *const *cell,
                      const size_t *length, size_t cols) {
	digest_state *st = (digest_state *)ctx;
	size_t        j;

	st->rows++;

	for (j = 0; j < cols; j++) {
		size_t k;

		st->fnv ^= (cell[j] == NULL) ? 0xffu : 0x01u;
		st->fnv *= 1099511628211ull;

		if (cell[j] == NULL) continue;

		st->bytes += length[j];
		for (k = 0; k < length[j]; k++) {
			st->fnv ^= (unsigned char)cell[j][k];
			st->fnv *= 1099511628211ull;
		}
	}

	return 0;
}

static long maxrss_kb(void) {
	struct rusage ru;

	if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;

	/* Linux 上 ru_maxrss 的单位是 KiB（POSIX 未规定单位，
	 * 4.4BSD 与 macOS 上是字节——这是不可移植的地方，本测试只跑 Linux） */
	return (long)ru.ru_maxrss;
}

static int run_mem(const char *mode, const char *sql) {
	mc_conn      *conn = NULL;
	const char   *why = "";
	mc_status     st;
	digest_state  dg;
	struct timespec t0;
	struct timespec t1;
	double        ms;

	st = open_from_env(&conn, &why);
	if (st != MC_OK) {
		(void)fprintf(stderr, "连接失败：%s\n", why);
		return 1;
	}

	memset(&dg, 0, sizeof dg);
	dg.fnv = 14695981039346656037ull;

	(void)clock_gettime(CLOCK_MONOTONIC, &t0);

	if (strcmp(mode, "query") == 0) {
		mc_rows set;
		size_t  i;
		size_t  j;

		st = mc_query(conn, sql, &set);
		if (st != MC_OK) {
			(void)fprintf(stderr, "mc_query：%s\n", mc_error(conn));
			mc_close(conn);
			return 1;
		}

		/* 逐格喂给同一个摘要函数，保证两种模式的校验和可以直接比 */
		for (i = 0; i < set.rows; i++) {
			const char *cell[64];
			size_t      len[64];
			size_t      cols = (set.cols < 64) ? set.cols : 64;

			for (j = 0; j < cols; j++) {
				cell[j] = MC_CELL(&set, i, j);
				len[j]  = set.length[i * set.cols + j];
			}
			(void)digest_row(&dg, cell, len, cols);
		}
		mc_rows_free(&set);
	} else {
		st = mc_query_each(conn, sql, digest_row, &dg, NULL);
		if (st != MC_OK) {
			(void)fprintf(stderr, "mc_query_each：%s\n", mc_error(conn));
			mc_close(conn);
			return 1;
		}
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &t1);
	ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
	     (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

	mc_close(conn);

	/* 一行一个结果，脚本按键值取用，不解析自由文本 */
	(void)printf("mode=%s rows=%llu bytes=%llu fnv=%016llx maxrss_kb=%ld ms=%.1f\n",
	             mode, dg.rows, dg.bytes, dg.fnv, maxrss_kb(), ms);
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc >= 4 && strcmp(argv[1], "mem") == 0) {
		return run_mem(argv[2], argv[3]);
	}

	if (argc != 1) {
		(void)fprintf(stderr,
		              "用法：%s                     正确性测试\n"
		              "      %s mem query|each '<SQL>'  测内存与耗时\n",
		              argv[0], argv[0]);
		return 2;
	}

	return run_correctness();
}
