/*
 * test_params.c —— mc_exec_params 的类型往返与失败路径
 *
 * 需要一台运行中的 MySQL：参数绑定只能对真实服务器验证。
 * "绑好之后会变成什么值"由服务端的类型转换决定，客户端库只负责把
 * 字节与类型码发出去，本地没有可查的对照物。
 * 连接参数走环境变量，与 test_stream.c 同一套：
 *   MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root MC_PASSWORD= ./test_params
 *
 * 自建自删一个库（mc_params_db）与一张表，不碰其它数据。
 *
 * 覆盖四类东西：
 *   1. 五种 kind 各自的往返值，含 64 位整数的两个端点；
 *   2. 长字段（len > MC_BLOB_CHUNK）走 send_long_data 的那条路径，
 *      含"恰好一块"与"一块加一字节"的边界；
 *   3. 多个参数混在一起时的对位关系——尤其两个长字段在同一语句里，
 *      send_long_data 用的是参数下标，串一位就会把内容写进别的列；
 *   4. 失败路径：占位符个数不符、kind 非法、data 为 NULL、
 *      把 SELECT 交给只处理写语句的接口。
 */

#include "mysqlclient.h"

#include <stdio.h>
#include <stdlib.h>
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

#define DB      "mc_params_db"
#define TBL     DB ".p"
#define INSERT  "INSERT INTO " TBL " (id, i, u, d, s, b) VALUES (?,?,?,?,?,?)"

/* ------------------------------------------------------------------
 * 夹具
 *
 * id 由测试显式给出，不用 AUTO_INCREMENT：本模块没有暴露
 * mysql_insert_id，靠 LAST_INSERT_ID 回查要多一层间接。
 * ------------------------------------------------------------------ */

static mc_param param_null(void) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_NULL;
	return p;
}

static mc_param param_int(long long v) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_INT;
	p.v.i  = v;
	return p;
}

static mc_param param_uint(unsigned long long v) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_UINT;
	p.v.u  = v;
	return p;
}

static mc_param param_double(double v) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind = MC_PARAM_DOUBLE;
	p.v.d  = v;
	return p;
}

static mc_param param_text(const char *s) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind          = MC_PARAM_TEXT;
	p.v.bytes.data  = s;
	p.v.bytes.len   = (s != NULL) ? strlen(s) : 0;
	return p;
}

static mc_param param_blob(const void *data, size_t len) {
	mc_param p;

	(void)memset(&p, 0, sizeof p);
	p.kind          = MC_PARAM_BLOB;
	p.v.bytes.data  = data;
	p.v.bytes.len   = len;
	return p;
}

/* 插一行：five 依次对应 i / u / d / s / b，不用的填 MC_PARAM_NULL */
static mc_status insert_row(mc_conn *conn, long long id, const mc_param five[5],
                            unsigned long long *affected) {
	mc_param p[6];
	size_t   k;

	p[0].kind = MC_PARAM_INT;
	p[0].v.i  = id;

	for (k = 0; k < 5; k++) p[1 + k] = five[k];

	return mc_exec_params(conn, INSERT, p, 6, affected);
}

/*
 * 按 id 取一列，把内容拷进 buf。
 * 返回 1 表示取到值，0 表示该列是 SQL NULL，-1 表示查询失败。
 */
static int cell_of(mc_conn *conn, long long id, const char *col,
                   char *buf, size_t cap) {
	char        sql[160];
	mc_rows     set;
	mc_status   st;
	const char *cell;
	int         rc;

	(void)snprintf(sql, sizeof sql, "SELECT %s FROM " TBL " WHERE id=%lld",
	               col, id);

	st = mc_query(conn, sql, &set);
	if (st != MC_OK) {
		(void)printf("      （查询失败：%s）\n", mc_error(conn));
		return -1;
	}

	if (set.rows != 1 || set.cols != 1) {
		mc_rows_free(&set);
		return -1;
	}

	cell = MC_CELL(&set, 0, 0);
	if (cell == NULL) {
		rc = 0;
	} else {
		(void)snprintf(buf, cap, "%s", cell);
		rc = 1;
	}

	mc_rows_free(&set);
	return rc;
}

/* 断言某行某列的文本值等于期望串 */
static void expect_cell(mc_conn *conn, long long id, const char *col,
                        const char *expected, const char *name) {
	char detail[512];
	char buf[256];
	int  rc = cell_of(conn, id, col, buf, sizeof buf);

	if (rc == 1 && strcmp(buf, expected) == 0) {
		ok(name);
		return;
	}

	if (rc < 0) {
		fail(name, "查询失败");
		return;
	}

	(void)snprintf(detail, sizeof detail, "实际 \"%s\"，期望 \"%s\"",
	               (rc == 0) ? "(SQL NULL)" : buf, expected);
	fail(name, detail);
}

/* 断言某行某列是 SQL NULL */
static void expect_null(mc_conn *conn, long long id, const char *col,
                        const char *name) {
	char buf[8];

	check_true(name, cell_of(conn, id, col, buf, sizeof buf) == 0);
}

/* 取一个 BLOB 列并与期望字节比对 */
static void check_blob(mc_conn *conn, long long id, const char *name,
                       const void *want, size_t want_len) {
	char          sql[160];
	unsigned char got[128 * 1024];
	size_t        got_len = 0;
	mc_status     st;

	(void)snprintf(sql, sizeof sql, "SELECT b FROM " TBL " WHERE id=%lld", id);

	st = mc_blob_read(conn, sql, got, sizeof got, &got_len);
	if (st != MC_OK) {
		char detail[160];

		(void)snprintf(detail, sizeof detail, "mc_blob_read 返回 %s：%s",
		               mc_status_string(st), mc_error(conn));
		fail(name, detail);
		return;
	}

	if (got_len != want_len) {
		char detail[128];

		(void)snprintf(detail, sizeof detail, "长度 %lu，期望 %lu",
		               (unsigned long)got_len, (unsigned long)want_len);
		fail(name, detail);
		return;
	}

	if (memcmp(got, want, want_len) == 0) {
		ok(name);
		return;
	}

	/* 报出第一处不同的偏移，比"字节不一致"有用得多 */
	{
		size_t k = 0;
		char   detail[128];

		while (k < want_len && got[k] == ((const unsigned char *)want)[k]) k++;

		(void)snprintf(detail, sizeof detail,
		               "第 %lu 字节起不同：%02x vs %02x", (unsigned long)k,
		               got[k], ((const unsigned char *)want)[k]);
		fail(name, detail);
	}
}

/* 造一段可判序的字节：每个位置的值与下标相关，块序错乱会立刻暴露 */
static void fill_pattern(unsigned char *buf, size_t len, unsigned int seed) {
	size_t k;

	for (k = 0; k < len; k++) {
		buf[k] = (unsigned char)((k * 31u + seed) & 0xffu);
	}
}

int main(void) {

	mc_conn      *conn = NULL;
	const char   *why = "";
	mc_status     st;
	mc_param      five[5];
	unsigned long long affected = 0;
	int           i;

	(void)printf("mysqlclient 参数绑定测试\n\n");

	st = open_from_env(&conn, &why);
	if (st != MC_OK) {
		(void)printf("连不上数据库（%s）：%s\n", mc_status_string(st), why);
		(void)printf("用法：MC_HOST=127.0.0.1 MC_USER=root MC_PASSWORD= ./test_params\n");
		return 1;
	}

	/* ---- 准备 ----
	 * s 用 LONGTEXT：后面要让它与 BLOB 一起走长字段分块，
	 * VARCHAR(200) 装不下三块（48 KiB）的内容。 */
	(void)mc_exec(conn, "DROP TABLE IF EXISTS " TBL, NULL);
	(void)mc_exec(conn, "CREATE DATABASE IF NOT EXISTS " DB, NULL);
	st = mc_exec(conn,
	             "CREATE TABLE " TBL " ("
	             "id INT PRIMARY KEY,"
	             "i BIGINT NULL,"
	             "u BIGINT UNSIGNED NULL,"
	             "d DOUBLE NULL,"
	             "s LONGTEXT NULL,"
	             "b LONGBLOB NULL)", NULL);

	check_long("建表", (long)st, MC_OK);

	/* ------------------------------------------------------------------
	 * 1. 有符号整数：两个端点与一个普通负数
	 *
	 * LLONG_MIN 这条不是凑数：libmysqlclient 的参数绑定路径只判断
	 * is_unsigned 指针是否为空、不看它指向的真假，把指针给成 &false
	 * 也会按无符号发送，-42 于是变成 18446744073709551614，
	 * 服务端报 "Out of range value"。有符号必须让指针保持 NULL。
	 * ------------------------------------------------------------------ */
	for (i = 0; i < 5; i++) five[i] = param_null();

	five[0] = param_int(-42);
	check_long("插入 int 负数", (long)insert_row(conn, 1, five, &affected), MC_OK);
	check_long("受影响行数 1", (long)affected, 1);
	expect_cell(conn, 1, "i", "-42", "取回 int -42");

	five[0] = param_int(-9223372036854775807LL - 1);
	check_long("插入 int 下界", (long)insert_row(conn, 2, five, &affected), MC_OK);
	expect_cell(conn, 2, "i", "-9223372036854775808", "取回下界原样");

	five[0] = param_int(9223372036854775807LL);
	check_long("插入 int 上界", (long)insert_row(conn, 3, five, &affected), MC_OK);
	expect_cell(conn, 3, "i", "9223372036854775807", "取回上界原样");

	/* ------------------------------------------------------------------
	 * 2. 无符号整数
	 * ------------------------------------------------------------------ */
	five[0] = param_null();
	five[1] = param_uint(18446744073709551615ULL);
	check_long("插入 uint 上界", (long)insert_row(conn, 4, five, &affected), MC_OK);
	expect_cell(conn, 4, "u", "18446744073709551615", "取回 uint 上界");

	five[1] = param_uint(0);
	check_long("插入 uint 0", (long)insert_row(conn, 5, five, &affected), MC_OK);
	expect_cell(conn, 5, "u", "0", "取回 uint 0");

	/* ------------------------------------------------------------------
	 * 3. 双精度
	 *
	 * 取回的是服务端格式化成文本的结果，因此比对串而不是位模式：
	 * 这条测的是"值经服务端解释之后仍是同一个数"，
	 * 不是"内存里那 8 字节没变"。
	 * ------------------------------------------------------------------ */
	five[1] = param_null();
	five[2] = param_double(3.141592653589793);
	check_long("插入 double", (long)insert_row(conn, 6, five, &affected), MC_OK);
	expect_cell(conn, 6, "d", "3.141592653589793", "取回 double 原样");

	five[2] = param_double(-0.5);
	check_long("插入 double 负数", (long)insert_row(conn, 7, five, &affected), MC_OK);
	expect_cell(conn, 7, "d", "-0.5", "取回 double -0.5");

	/* ------------------------------------------------------------------
	 * 4. 文本：引号、注释符、UTF-8、空串、NULL
	 *
	 * 第一条是参数化与拼字符串的分水岭：值里的单引号不需要转义，
	 * 也不可能闭合 SQL 里的字符串常量。
	 * ------------------------------------------------------------------ */
	five[2] = param_null();
	five[3] = param_text("it's; DROP TABLE p; -- 注释");
	check_long("插入含引号与注释符的文本",
	           (long)insert_row(conn, 8, five, &affected), MC_OK);
	expect_cell(conn, 8, "s", "it's; DROP TABLE p; -- 注释",
	            "取回文本逐字相同（值没有被当成 SQL 语法）");

	five[3] = param_text("睡蓝蕉");
	check_long("插入 UTF-8 文本", (long)insert_row(conn, 9, five, &affected), MC_OK);
	expect_cell(conn, 9, "s", "睡蓝蕉", "取回 UTF-8 文本原样");

	five[3] = param_text("");
	check_long("插入空串", (long)insert_row(conn, 10, five, &affected), MC_OK);
	expect_cell(conn, 10, "s", "", "空串取回为空串");
	expect_cell(conn, 10, "s IS NULL", "0", "空串不是 SQL NULL");

	five[3] = param_null();
	check_long("插入 NULL 文本", (long)insert_row(conn, 11, five, &affected), MC_OK);
	expect_null(conn, 11, "s", "NULL 文本取回为 NULL");

	/* ------------------------------------------------------------------
	 * 5. 二进制：含 '\0' 的内容必须原样往返
	 *
	 * 这条是"文本"与"二进制"分开的理由：同一句 INSERT，若按文本发送，
	 * '\0' 之后的内容会被截掉；按 MYSQL_TYPE_BLOB 发则不会。
	 * ------------------------------------------------------------------ */
	{
		static const unsigned char bin[] = { 0x00, 0x01, 0x02, 0x00, 0xff, 0x7f };

		five[3] = param_null();
		five[4] = param_blob(bin, sizeof bin);
		check_long("插入含 0x00 的 BLOB",
		           (long)insert_row(conn, 12, five, &affected), MC_OK);
		check_blob(conn, 12, "含 0x00 的 BLOB 往返逐字节相同", bin, sizeof bin);
	}

	{	/* 零字节：data 为 NULL 也合法，且不能被当成 SQL NULL */
		five[4] = param_blob(NULL, 0);
		check_long("插入零字节 BLOB",
		           (long)insert_row(conn, 13, five, &affected), MC_OK);
		expect_cell(conn, 13, "LENGTH(b)", "0", "零字节 BLOB 的长度是 0");
		expect_cell(conn, 13, "b IS NULL", "0", "零字节 BLOB 不是 NULL");
	}

	/* ------------------------------------------------------------------
	 * 6. 长字段边界：恰好一块 / 一块加一字节 / 跨多块
	 *
	 * MC_BLOB_CHUNK 是"绑定缓冲区"与"send_long_data 分块"的分界线。
	 * 边界两侧各一条，再加一条跨五块的：三条合起来才能区分
	 * "分块没发生"与"分块丢了尾巴"。
	 * ------------------------------------------------------------------ */
	{
		unsigned char *big = (unsigned char *)malloc(5 * MC_BLOB_CHUNK + 7);

		if (big == NULL) {
			fail("分配长字段测试缓冲区", "malloc 返回 NULL");
		} else {
			size_t len;

			len = MC_BLOB_CHUNK;
			fill_pattern(big, len, 1);
			five[4] = param_blob(big, len);
			check_long("插入恰好一块的 BLOB",
			           (long)insert_row(conn, 14, five, &affected), MC_OK);
			check_blob(conn, 14, "恰好一块的 BLOB 往返相同", big, len);

			len = MC_BLOB_CHUNK + 1;
			fill_pattern(big, len, 2);
			five[4] = param_blob(big, len);
			check_long("插入一块加一字节的 BLOB",
			           (long)insert_row(conn, 15, five, &affected), MC_OK);
			check_blob(conn, 15, "一块加一字节的 BLOB 往返相同", big, len);

			len = 5 * MC_BLOB_CHUNK + 7;
			fill_pattern(big, len, 3);
			five[4] = param_blob(big, len);
			check_long("插入跨五块的 BLOB",
			           (long)insert_row(conn, 16, five, &affected), MC_OK);
			check_blob(conn, 16, "跨五块的 BLOB 往返相同", big, len);

			free(big);
		}
	}

	/* ------------------------------------------------------------------
	 * 7. 一次两个长字段，且类型不同
	 *
	 * send_long_data 的第二个参数是"第几个占位符"。下标写死成 0
	 * 或与循环变量错位时，两个长字段会互相追加到同一个参数上——
	 * 单长字段的用例完全察觉不到。这里让 TEXT 走两块、BLOB 走三块，
	 * 长度与内容都不同，串位必然表现为长度不符。
	 * ------------------------------------------------------------------ */
	{
		const size_t len_t = MC_BLOB_CHUNK * 2 + 11;
		const size_t len_b = MC_BLOB_CHUNK * 3 + 5;
		char          *text = (char *)malloc(len_t + 1);
		unsigned char *blob = (unsigned char *)malloc(len_b);

		if (text == NULL || blob == NULL) {
			fail("分配双长字段测试缓冲区", "malloc 返回 NULL");
		} else {
			mc_param p[3];
			size_t   k;

			for (k = 0; k < len_t; k++) text[k] = (char)('a' + (int)(k % 26));
			text[len_t] = '\0';
			fill_pattern(blob, len_b, 97);

			p[0] = param_int(21);
			p[1] = param_text(text);
			p[2] = param_blob(blob, len_b);

			st = mc_exec_params(conn,
			                    "INSERT INTO " TBL " (id, s, b) VALUES (?,?,?)",
			                    p, 3, &affected);
			check_long("同一语句里的两个长字段", (long)st, MC_OK);

			{
				char lenbuf[32];

				(void)snprintf(lenbuf, sizeof lenbuf, "%lu",
				               (unsigned long)len_t);
				expect_cell(conn, 21, "LENGTH(s)", lenbuf, "长文本长度未被 BLOB 影响");
			}

			check_blob(conn, 21, "长 BLOB 内容未被文本影响", blob, len_b);

			/* 再把长字段放到语句的第一个位置，确认下标不是"从 1 开始对齐" */
			p[0] = param_blob(blob, len_b);
			p[1] = param_text("tail");
			p[2] = param_int(22);

			/* 先占住主键。five[] 在第 6 组里指向已经释放的缓冲区，
			 * 必须重建成全 NULL 再用——否则这里是一次 use-after-free。 */
			for (k = 0; k < 5; k++) five[k] = param_null();
			(void)insert_row(conn, 22, five, &affected);
			st = mc_exec_params(conn,
			                    "UPDATE " TBL " SET b=?, s=? WHERE id=?",
			                    p, 3, &affected);
			check_long("长字段在第一个位置的 UPDATE", (long)st, MC_OK);
			check_blob(conn, 22, "UPDATE 的长 BLOB 内容正确", blob, len_b);
			expect_cell(conn, 22, "s", "tail", "UPDATE 的短文本正确");

			free(text);
			free(blob);
		}
	}

	/* ------------------------------------------------------------------
	 * 8. 失败路径
	 * ------------------------------------------------------------------ */

	/* 占位符 3 个、参数 1 个 */
	{
		mc_param one = param_int(1);

		affected = 12345;
		st = mc_exec_params(conn, "INSERT INTO " TBL " (i,u,d) VALUES (?,?,?)",
		                    &one, 1, &affected);
		check_long("占位符多于参数时返回 PARAM", (long)st, MC_ERR_PARAM);
		check_long("失败时 affected 不被改写", (long)affected, 12345);
		check_true("错误信息给出两个个数",
		           strstr(mc_error(conn), "3") != NULL &&
		           strstr(mc_error(conn), "1") != NULL);
	}

	/* 参数 2 个、占位符 1 个 */
	{
		mc_param two[2];

		two[0] = param_int(1);
		two[1] = param_int(2);
		st = mc_exec_params(conn, "INSERT INTO " TBL " (i) VALUES (?)",
		                    two, 2, NULL);
		check_long("参数多于占位符时返回 PARAM", (long)st, MC_ERR_PARAM);
	}

	/*
	 * kind 不在枚举内，且 SQL 本身还有语法错误。
	 * 期望错误落在 params 而不是 prepare：本地就能判定的错误不该
	 * 先发一轮网络请求，也不该报成"SQL 有问题"把使用者引错方向。
	 */
	{
		mc_param bad;

		(void)memset(&bad, 0, sizeof bad);
		bad.kind = (mc_param_kind)99;
		st = mc_exec_params(conn, "INSERT INTO", &bad, 1, NULL);
		check_long("未知 kind 返回 PARAM", (long)st, MC_ERR_PARAM);
		check_true("未知 kind 先于 prepare 被拒",
		           strncmp(mc_error(conn), "params:", 7) == 0);
	}

	/* data 为 NULL 而 len 非 0：同样是本地检查 */
	{
		mc_param bad;

		(void)memset(&bad, 0, sizeof bad);
		bad.kind         = MC_PARAM_TEXT;
		bad.v.bytes.data = NULL;
		bad.v.bytes.len  = 5;
		st = mc_exec_params(conn, "INSERT INTO", &bad, 1, NULL);
		check_long("data 为 NULL 而 len 非 0 返回 PARAM", (long)st, MC_ERR_PARAM);
		check_true("空 data 检查也先于 prepare",
		           strncmp(mc_error(conn), "params:", 7) == 0);
	}

	/* params 为 NULL 而 n 非 0 */
	st = mc_exec_params(conn, "INSERT INTO " TBL " (i) VALUES (?)", NULL, 1, NULL);
	check_long("params 为 NULL 且 n 非 0 返回 PARAM", (long)st, MC_ERR_PARAM);

	/* SELECT 交给只处理写语句的接口 */
	{
		mc_param one = param_int(1);

		st = mc_exec_params(conn, "SELECT ?", &one, 1, NULL);
		check_long("SELECT 返回 RESULT", (long)st, MC_ERR_RESULT);
		check_true("提示改用 mc_query",
		           strstr(mc_error(conn), "mc_query") != NULL);
	}

	/* 语法错误在 prepare 阶段被发现 */
	st = mc_exec_params(conn, "INSERT INTO", NULL, 0, NULL);
	check_long("语法错误返回 STMT", (long)st, MC_ERR_STMT);
	check_true("语法错误报 prepare",
	           strncmp(mc_error(conn), "prepare:", 8) == 0);

	/* 主键重复：prepare 能过，execute 才失败 */
	{
		mc_param dup[2];

		dup[0] = param_int(1);          /* id=1 已在第 1 组插入 */
		dup[1] = param_text("dup");
		st = mc_exec_params(conn, "INSERT INTO " TBL " (id,s) VALUES (?,?)",
		                    dup, 2, NULL);
		check_long("主键重复返回 QUERY", (long)st, MC_ERR_QUERY);
		check_true("主键重复报 execute",
		           strncmp(mc_error(conn), "execute:", 8) == 0);
	}

	/* 上面那些失败之后，连接必须仍然可用（语句与结果集都已释放） */
	{
		mc_param ok2[2];

		ok2[0] = param_int(99);
		ok2[1] = param_int(5);
		st = mc_exec_params(conn, "INSERT INTO " TBL " (id,i) VALUES (?,?)",
		                    ok2, 2, &affected);
		check_long("失败之后连接仍可用", (long)st, MC_OK);
	}

	/* n 为 0 且 params 为 NULL：等价于 mc_exec */
	{
		unsigned long long n = 0;

		st = mc_exec_params(conn, "DELETE FROM " TBL " WHERE id=99", NULL, 0, &n);
		check_long("n 为 0 时按无参语句执行", (long)st, MC_OK);
		check_long("受影响行数 1", (long)n, 1);
		check_long("affected 为 NULL 也不崩",
		           (long)mc_exec_params(conn,
		                                "DELETE FROM " TBL " WHERE id=123456",
		                                NULL, 0, NULL), MC_OK);
	}

	/* ------------------------------------------------------------------
	 * 9. 受影响行数：0 行与多行
	 * ------------------------------------------------------------------ */
	{
		mc_param p[2];
		unsigned long long n = 0;

		p[0] = param_text("bulk");
		p[1] = param_int(20);           /* id < 20 命中前四组的一批行 */

		st = mc_exec_params(conn, "UPDATE " TBL " SET s=? WHERE id<?", p, 2, &n);
		check_long("多行更新成功", (long)st, MC_OK);
		check_true("受影响行数与命中行数一致", n > 1);

		p[0] = param_text("none");
		p[1] = param_int(-1);
		st = mc_exec_params(conn, "UPDATE " TBL " SET s=? WHERE id<?", p, 2, &n);
		check_long("未命中任何行仍是成功", (long)st, MC_OK);
		check_long("未命中时受影响 0 行", (long)n, 0);
	}

	/* ---- 清理 ---- */
	(void)mc_exec(conn, "DROP TABLE IF EXISTS " TBL, NULL);
	(void)mc_exec(conn, "DROP DATABASE IF EXISTS " DB, NULL);

	mc_close(conn);

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_fails);

	return g_fails;
}
