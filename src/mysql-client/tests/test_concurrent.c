/*
 * test_concurrent.c —— 并发：多连接同时写、死锁、锁等待超时、错误号
 *
 * 需要一台运行中的 MySQL，并且是 InnoDB（行锁、死锁检测、锁等待超时
 * 都是 InnoDB 的行为，MyISAM 只有表锁，测不出这些）。连接参数走环境变量：
 *   MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root MC_PASSWORD= ./test_concurrent
 * 自建自删一个库（mc_conc_db），不碰其它数据。
 *
 * 覆盖五类东西：
 *
 *   1. 独立连接并发写同一张表：8 条连接 × 250 行，核对总行数、
 *      自增编号连续、每个 worker 的行数各自对上。这一组回答的是
 *      "多条连接同时往一张表里插，行会不会串、编号会不会撞"。
 *   2. 死锁：两条连接以相反顺序更新同两行，必然形成等待环。
 *      核对"恰好一方拿到 1213、另一方成功""拿到错误的那笔事务被
 *      服务器整笔回滚（它的第一次更新不留痕迹）""连接在错误之后仍然可用"。
 *   3. 锁等待超时：会话把 innodb_lock_wait_timeout 设成 1 s，
 *      另一条连接占着行锁不放。核对错误号 1205、SQLSTATE、耗时落在
 *      1 s 附近，以及"1205 只回滚语句、事务还在"——这一点与 1213 相反。
 *   4. 语句对象在多连接上并发使用：4 个线程各自 prepare 一次、执行 100 次。
 *      语句对象借用连接，所以它能不能并发，等价于"各自的连接能不能并发"。
 *   5. 错误号的契约：本地错误清零、成功不清零、越界参数不碰网络、
 *      未失败的连接是 0 / "00000"、建连失败走线程局部的那一份。
 *
 * 前三组都要多线程：同一线程里先做 A 的阻塞语句、再发 B 的语句时，
 * B 根本还没发出，服务端看到的只是"A 在等一把没人要的锁"，
 * 等到 50 s 超时也不会出现死锁——那不是死锁检测没生效，是死锁没形成。
 * 这一点是本文件必须用 pthread 的唯一原因。
 */

#include "mysqlclient.h"

#include <pthread.h>
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

static void check_str(const char *name, const char *actual, const char *expected) {
	char detail[128];

	if (actual != NULL && strcmp(actual, expected) == 0) {
		ok(name);
		return;
	}

	(void)snprintf(detail, sizeof detail, "实际 \"%s\"，期望 \"%s\"",
	               (actual != NULL) ? actual : "(null)", expected);
	fail(name, detail);
}

/* ------------------------------------------------------------------
 * 建连与查询辅助
 * ------------------------------------------------------------------ */

#define DBNAME "mc_conc_db"

static mc_status open_env(const char *db, mc_conn **out) {
	mc_options     opt;
	const char    *v;
	unsigned int   port = 3306;

	mc_options_init(&opt);
	(void)snprintf(opt.host, sizeof opt.host, "127.0.0.1");
	(void)snprintf(opt.user, sizeof opt.user, "root");

	if ((v = getenv("MC_HOST")) != NULL && v[0] != '\0') {
		(void)snprintf(opt.host, sizeof opt.host, "%s", v);
	}
	if ((v = getenv("MC_PORT")) != NULL && v[0] != '\0') {
		unsigned long parsed = strtoul(v, NULL, 10);
		if (parsed > 0 && parsed <= 65535ul) port = (unsigned int)parsed;
	}
	if ((v = getenv("MC_USER")) != NULL && v[0] != '\0') {
		(void)snprintf(opt.user, sizeof opt.user, "%s", v);
	}
	if ((v = getenv("MC_PASSWORD")) != NULL) {
		(void)snprintf(opt.password, sizeof opt.password, "%s", v);
	}
	if (db != NULL) (void)snprintf(opt.database, sizeof opt.database, "%s", db);

	opt.port = port;
	return mc_open(&opt, out);
}

/* 单行单列的整数查询。失败返回 -1 并把原因打到 stderr。 */
static long query_long(mc_conn *c, const char *sql) {
	mc_rows   rs;
	mc_status st;
	long      v = -1;

	st = mc_query(c, sql, &rs);
	if (st != MC_OK) {
		(void)fprintf(stderr, "查询失败：%s（%s）\n", sql, mc_error(c));
		return -1;
	}

	if (rs.rows == 1 && rs.cols == 1 && MC_CELL(&rs, 0, 0) != NULL) {
		v = strtol(MC_CELL(&rs, 0, 0), NULL, 10);
	}

	mc_rows_free(&rs);
	return v;
}

static long exec_ok(mc_conn *c, const char *sql) {
	mc_status st = mc_exec(c, sql, NULL);

	if (st != MC_OK) {
		(void)fprintf(stderr, "执行失败：%s（%s）\n", sql, mc_error(c));
		return -1;
	}
	return 0;
}

/* 数行数的回调：只累加，不看内容 */
static int count_cb(void *ctx, const char *const *cell, const size_t *length,
                    size_t cols) {
	(void)cell;
	(void)length;
	(void)cols;
	(*(size_t *)ctx)++;
	return 0;
}

/* ------------------------------------------------------------------
 * 1. 独立连接并发写同一张表
 * ------------------------------------------------------------------ */

#define INS_THREADS 8
#define INS_ROWS    250

typedef struct {
	int          worker;
	int          errors;
	mc_status    first_bad;
	unsigned int first_errnum;
	char         first_msg[160];
} ins_arg;

static void *ins_worker(void *p) {
	ins_arg *a = p;
	mc_conn *c = NULL;
	int      i;

	if (open_env(DBNAME, &c) != MC_OK) {
		a->errors++;
		a->first_bad = MC_ERR_CONNECT;
		a->first_errnum = mc_errno(NULL);
		(void)snprintf(a->first_msg, sizeof a->first_msg, "%s", mc_error(NULL));
		return NULL;
	}

	for (i = 0; i < INS_ROWS; i++) {
		mc_param par[2] = {
			{ .kind = MC_PARAM_INT, .v.i = a->worker },
			{ .kind = MC_PARAM_INT, .v.i = i }
		};
		mc_status st = mc_exec_params(c,
		                              "INSERT INTO conc_ins(worker, seq) VALUES(?, ?)",
		                              par, 2, NULL);

		if (st != MC_OK) {
			if (a->errors == 0) {
				a->first_bad = st;
				a->first_errnum = mc_errno(c);
				(void)snprintf(a->first_msg, sizeof a->first_msg, "%s",
				               mc_error(c));
			}
			a->errors++;
		}
	}

	mc_close(c);
	return NULL;
}

static void case_concurrent_insert(mc_conn *c) {
	pthread_t th[INS_THREADS];
	ins_arg   args[INS_THREADS];
	int       total_errors = 0;
	int       i;
	char      sql[160];

	(void)printf("\n--- 1. %d 条连接各插 %d 行 ---\n", INS_THREADS, INS_ROWS);

	(void)exec_ok(c, "DROP TABLE IF EXISTS conc_ins");
	(void)exec_ok(c, "CREATE TABLE conc_ins("
	                 "id INT AUTO_INCREMENT PRIMARY KEY, "
	                 "worker INT NOT NULL, "
	                 "seq INT NOT NULL, "
	                 "UNIQUE KEY uk_worker_seq(worker, seq)) ENGINE=InnoDB");

	for (i = 0; i < INS_THREADS; i++) {
		(void)memset(&args[i], 0, sizeof args[i]);
		args[i].worker = i;
		if (pthread_create(&th[i], NULL, ins_worker, &args[i]) != 0) {
			(void)fprintf(stderr, "pthread_create 失败\n");
			return;
		}
	}
	for (i = 0; i < INS_THREADS; i++) {
		(void)pthread_join(th[i], NULL);
		total_errors += args[i].errors;
	}

	if (total_errors != 0) {
		/* 第一个失败者的原文比"共有 N 个失败"有用得多 */
		for (i = 0; i < INS_THREADS; i++) {
			if (args[i].errors > 0) {
				(void)fprintf(stderr, "worker %d：%d 个失败，首个 %s / %u / %s\n",
				              i, args[i].errors,
				              mc_status_string(args[i].first_bad),
				              args[i].first_errnum, args[i].first_msg);
				break;
			}
		}
	}

	check_long("并发插入的错误数", total_errors, 0);
	check_long("总行数", query_long(c, "SELECT COUNT(*) FROM conc_ins"),
	           (long)(INS_THREADS * INS_ROWS));
	check_long("自增编号互不相同",
	           query_long(c, "SELECT COUNT(DISTINCT id) FROM conc_ins"),
	           (long)(INS_THREADS * INS_ROWS));
	check_long("自增编号连续（无空洞）",
	           query_long(c, "SELECT MAX(id) - MIN(id) + 1 FROM conc_ins"),
	           (long)(INS_THREADS * INS_ROWS));
	check_long("(worker, seq) 组合无重复",
	           query_long(c, "SELECT COUNT(*) FROM "
	                         "(SELECT worker, seq FROM conc_ins "
	                         " GROUP BY worker, seq HAVING COUNT(*) > 1) t"),
	           0);

	(void)snprintf(sql, sizeof sql,
	               "SELECT COUNT(*) FROM conc_ins WHERE worker = %d", INS_THREADS - 1);
	check_long("最后一个 worker 的行数", query_long(c, sql), INS_ROWS);

	(void)snprintf(sql, sizeof sql,
	               "SELECT COUNT(*) FROM (SELECT worker FROM conc_ins "
	               "GROUP BY worker HAVING COUNT(*) <> %d) t", INS_ROWS);
	check_long("每个 worker 都写了完整的一份", query_long(c, sql), 0);
}

/* ------------------------------------------------------------------
 * 2. 死锁：两条连接以相反顺序更新同两行
 *
 * 时序必须靠栅栏卡住：两方各自更新第一行之后才同时去要第二行，
 * 这样等待环才形成。谁拿到 1213 由服务端决定（它按事务"代价"
 * 挑一个牺牲者），所以断言只能写"恰好一个"，不能写死是哪一方。
 * ------------------------------------------------------------------ */

typedef struct {
	pthread_barrier_t *gate;
	int                first_row;    /* 先锁的行 */
	int                second_row;   /* 后锁的行，这里形成环 */
	mc_status          st_first;
	mc_status          st_second;
	unsigned int       errnum;
	char               sqlstate[6];
	char               message[200];
	int                usable;       /* 出错之后连接还能不能查询 */
} dl_arg;

static void *dl_worker(void *p) {
	dl_arg  *a = p;
	mc_conn *c = NULL;
	char     sql[128];
	size_t   n = 0;

	if (open_env(DBNAME, &c) != MC_OK) {
		a->st_first = MC_ERR_CONNECT;
		return NULL;
	}

	/* 把等待上界压到 5 s：万一死锁没有被检测出来（例如检测被关掉），
	 * 用例会在 5 s 内以 1205 结束并让断言报错，而不是挂 50 s。 */
	(void)exec_ok(c, "SET SESSION innodb_lock_wait_timeout = 5");
	(void)exec_ok(c, "BEGIN");

	(void)snprintf(sql, sizeof sql,
	               "UPDATE conc_row SET v = v + 1 WHERE id = %d", a->first_row);
	a->st_first = mc_exec(c, sql, NULL);

	(void)pthread_barrier_wait(a->gate);

	(void)snprintf(sql, sizeof sql,
	               "UPDATE conc_row SET v = v + 1 WHERE id = %d", a->second_row);
	a->st_second = mc_exec(c, sql, NULL);

	if (a->st_second != MC_OK) {
		a->errnum = mc_errno(c);
		(void)snprintf(a->sqlstate, sizeof a->sqlstate, "%s", mc_sqlstate(c));
		(void)snprintf(a->message, sizeof a->message, "%s", mc_error(c));
	}

	/*
	 * 拿到死锁错误的那一方已经被服务器整笔回滚，这里再 COMMIT 也无害；
	 * 成功的一方必须 COMMIT，否则它的两次更新会在 mc_close 时随连接
	 * 一起被隐式回滚，后面的"落库结果"就看不到它的效果。
	 */
	(void)exec_ok(c, (a->st_second == MC_OK) ? "COMMIT" : "ROLLBACK");

	/* 错误之后连接是否还能用：协议上死锁只回滚事务，不断连接 */
	if (mc_query_each(c, "SELECT 1", count_cb, &n, NULL) == MC_OK && n == 1) {
		a->usable = 1;
	}

	mc_close(c);
	return NULL;
}

static void case_deadlock(mc_conn *c) {
	pthread_barrier_t gate;
	pthread_t         th[2];
	dl_arg            args[2];
	int               losers = 0;
	int               i;

	(void)printf("\n--- 2. 相反顺序更新同两行 ---\n");

	(void)exec_ok(c, "DROP TABLE IF EXISTS conc_row");
	(void)exec_ok(c, "CREATE TABLE conc_row(id INT PRIMARY KEY, v INT NOT NULL) "
	                 "ENGINE=InnoDB");
	(void)exec_ok(c, "INSERT INTO conc_row VALUES(1, 0), (2, 0)");

	if (pthread_barrier_init(&gate, NULL, 2) != 0) {
		fail("初始化栅栏", "pthread_barrier_init 失败");
		return;
	}

	(void)memset(args, 0, sizeof args);
	args[0].gate = &gate;
	args[0].first_row = 1;
	args[0].second_row = 2;
	args[1].gate = &gate;
	args[1].first_row = 2;
	args[1].second_row = 1;

	for (i = 0; i < 2; i++) {
		if (pthread_create(&th[i], NULL, dl_worker, &args[i]) != 0) {
			(void)fprintf(stderr, "pthread_create 失败\n");
			(void)pthread_barrier_destroy(&gate);
			return;
		}
	}
	for (i = 0; i < 2; i++) (void)pthread_join(th[i], NULL);
	(void)pthread_barrier_destroy(&gate);

	for (i = 0; i < 2; i++) {
		if (args[i].st_second != MC_OK) {
			losers++;
			(void)fprintf(stderr, "第 %d 方拿到错误：%s / %u / %s\n",
			              i, mc_status_string(args[i].st_second),
			              args[i].errnum, args[i].message);
		}
	}

	check_long("恰好一方拿到死锁错误", losers, 1);
	check_true("两方都完成了第一次更新", args[0].st_first == MC_OK &&
	                                      args[1].st_first == MC_OK);

	for (i = 0; i < 2; i++) {
		if (args[i].st_second != MC_OK) {
			check_long("死锁的错误号是 1213", (long)args[i].errnum, 1213);
			check_str("死锁的 SQLSTATE 是 40001", args[i].sqlstate, "40001");
			check_true("死锁以查询失败的形式返回",
			           args[i].st_second == MC_ERR_QUERY);
		}
	}

	check_true("两方连接都仍可用", args[0].usable == 1 && args[1].usable == 1);

	/*
	 * 落库结果只有一方的事务生效：两行各 +1 一次。
	 * 若被回滚的那一方留下了第一次更新，这里会是 1 和 2（或 2 和 1）。
	 */
	check_long("第 1 行只加分一次",
	           query_long(c, "SELECT v FROM conc_row WHERE id = 1"), 1);
	check_long("第 2 行只加分一次",
	           query_long(c, "SELECT v FROM conc_row WHERE id = 2"), 1);
}

/* ------------------------------------------------------------------
 * 3. 锁等待超时：1205 只回滚语句，不回滚事务
 *
 * 这一组不需要线程：占锁的那条连接先做完自己的更新（锁已经拿到），
 * 被阻塞的一方在自己的语句里等，主线程此时不需要再对第一条连接发命令。
 * ------------------------------------------------------------------ */

static void case_lock_timeout(mc_conn *c) {
	mc_conn      *a = NULL;
	mc_conn      *b = NULL;
	mc_status     st;
	struct timespec t0, t1;
	double        elapsed;
	size_t        n = 0;

	(void)printf("\n--- 3. 锁等待超时 ---\n");

	if (open_env(DBNAME, &a) != MC_OK || open_env(DBNAME, &b) != MC_OK) {
		fail("建连", mc_error(NULL));
		mc_close(a);
		mc_close(b);
		return;
	}

	check_long("新连接的错误号为 0", (long)mc_errno(a), 0);
	check_str("新连接的 SQLSTATE 为 00000", mc_sqlstate(a), "00000");

	(void)exec_ok(c, "INSERT INTO conc_row VALUES(3, 0) ON DUPLICATE KEY UPDATE v = 0");

	(void)exec_ok(a, "SET SESSION innodb_lock_wait_timeout = 1");
	(void)exec_ok(b, "SET SESSION innodb_lock_wait_timeout = 1");
	(void)exec_ok(a, "BEGIN");
	(void)exec_ok(b, "BEGIN");
	(void)exec_ok(a, "UPDATE conc_row SET v = v + 10 WHERE id = 3");

	(void)clock_gettime(CLOCK_MONOTONIC, &t0);
	st = mc_exec(b, "UPDATE conc_row SET v = v + 1 WHERE id = 3", NULL);
	(void)clock_gettime(CLOCK_MONOTONIC, &t1);

	elapsed = (double)(t1.tv_sec - t0.tv_sec) +
	          (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

	check_true("等待锁的语句失败", st != MC_OK);
	check_long("锁等待超时的错误号是 1205", (long)mc_errno(b), 1205);
	check_str("锁等待超时的 SQLSTATE", mc_sqlstate(b), "HY000");
	check_true("耗时在 0.9 ~ 4.0 s 之间（会话设的是 1 s）",
	           elapsed >= 0.9 && elapsed <= 4.0);

	/* 超时之后连接仍能收发命令 */
	check_true("超时之后连接仍可用",
	           mc_query_each(b, "SELECT 1", count_cb, &n, NULL) == MC_OK && n == 1);

	/*
	 * 1205 与 1213 的差别就在这里：死锁回滚整笔事务，超时只回滚当前语句。
	 * 判断"事务还在不在"不能看返回值（两种都返回失败），得看后续动作：
	 * 在 b 的事务里改一行别的数据，再回滚，然后由第三方连接确认那处改动
	 * 消失了——只有事务还开着，这个改动才可能随回滚一起消失。
	 */
	check_true("超时之后同一事务仍能写入",
	           exec_ok(b, "UPDATE conc_row SET v = 99 WHERE id = 1") == 0);
	(void)exec_ok(b, "ROLLBACK");
	check_long("回滚带走了那次写入（事务没有被 1205 结束）",
	           query_long(c, "SELECT v FROM conc_row WHERE id = 1"), 1);

	(void)exec_ok(a, "ROLLBACK");
	check_long("a 的未提交更新随回滚消失",
	           query_long(c, "SELECT v FROM conc_row WHERE id = 3"), 0);

	/* 超时之后 b 还能正常执行写语句（会话没有进入不可用状态） */
	check_true("超时之后仍能写入",
	           exec_ok(b, "UPDATE conc_row SET v = 7 WHERE id = 3") == 0);
	check_long("写入生效", query_long(c, "SELECT v FROM conc_row WHERE id = 3"), 7);

	mc_close(a);
	mc_close(b);
}

/* ------------------------------------------------------------------
 * 4. 语句对象在多连接上并发使用
 * ------------------------------------------------------------------ */

#define STMT_THREADS 4
#define STMT_ROWS    100

typedef struct {
	int       worker;
	int       errors;
	int       prepared;
	mc_status first_bad;
	char      first_msg[160];
} stmt_arg;

static void *stmt_worker(void *p) {
	stmt_arg *a = p;
	mc_conn  *c = NULL;
	mc_stmt  *st = NULL;
	mc_status s;
	int       i;

	if (open_env(DBNAME, &c) != MC_OK) {
		a->errors++;
		a->first_bad = MC_ERR_CONNECT;
		return NULL;
	}

	s = mc_stmt_prepare(c, "INSERT INTO conc_stmt(worker, seq) VALUES(?, ?)", &st);
	if (s != MC_OK) {
		a->errors++;
		a->first_bad = s;
		(void)snprintf(a->first_msg, sizeof a->first_msg, "%s", mc_error(c));
		mc_close(c);
		return NULL;
	}
	a->prepared = 1;

	for (i = 0; i < STMT_ROWS; i++) {
		mc_param par[2] = {
			{ .kind = MC_PARAM_INT, .v.i = a->worker },
			{ .kind = MC_PARAM_INT, .v.i = i }
		};

		s = mc_stmt_execute(st, par, 2, NULL);
		if (s != MC_OK) {
			if (a->errors == 0) {
				a->first_bad = s;
				(void)snprintf(a->first_msg, sizeof a->first_msg, "%s",
				               mc_error(c));
			}
			a->errors++;
		}
	}

	mc_stmt_close(st);
	mc_close(c);
	return NULL;
}

static void case_concurrent_stmt(mc_conn *c) {
	pthread_t th[STMT_THREADS];
	stmt_arg  args[STMT_THREADS];
	int       total_errors = 0;
	int       prepared = 0;
	int       i;

	(void)printf("\n--- 4. %d 条连接各自 prepare 一次、执行 %d 次 ---\n",
	             STMT_THREADS, STMT_ROWS);

	(void)exec_ok(c, "DROP TABLE IF EXISTS conc_stmt");
	(void)exec_ok(c, "CREATE TABLE conc_stmt(id INT AUTO_INCREMENT PRIMARY KEY, "
	                 "worker INT NOT NULL, seq INT NOT NULL, "
	                 "UNIQUE KEY uk_worker_seq(worker, seq)) ENGINE=InnoDB");

	for (i = 0; i < STMT_THREADS; i++) {
		(void)memset(&args[i], 0, sizeof args[i]);
		args[i].worker = i;
		if (pthread_create(&th[i], NULL, stmt_worker, &args[i]) != 0) {
			(void)fprintf(stderr, "pthread_create 失败\n");
			return;
		}
	}
	for (i = 0; i < STMT_THREADS; i++) {
		(void)pthread_join(th[i], NULL);
		total_errors += args[i].errors;
		prepared += args[i].prepared;
	}

	for (i = 0; i < STMT_THREADS; i++) {
		if (args[i].errors > 0) {
			(void)fprintf(stderr, "worker %d：%d 个失败，首个 %s / %s\n",
			              i, args[i].errors,
			              mc_status_string(args[i].first_bad), args[i].first_msg);
		}
	}

	check_long("prepare 成功的线程数", prepared, STMT_THREADS);
	check_long("并发复用语句对象的错误数", total_errors, 0);
	check_long("总行数", query_long(c, "SELECT COUNT(*) FROM conc_stmt"),
	           (long)(STMT_THREADS * STMT_ROWS));
	check_long("(worker, seq) 组合无重复",
	           query_long(c, "SELECT COUNT(*) FROM "
	                         "(SELECT worker, seq FROM conc_stmt "
	                         " GROUP BY worker, seq HAVING COUNT(*) > 1) t"),
	           0);
}

/* ------------------------------------------------------------------
 * 5. 错误号的契约
 * ------------------------------------------------------------------ */

static void case_errno_contract(mc_conn *c) {
	mc_conn    *tmp = NULL;
	mc_options  opt;
	mc_status   st;

	(void)printf("\n--- 5. 错误号的契约 ---\n");

	/* 5.1 服务端错误：唯一键冲突 */
	(void)exec_ok(c, "DROP TABLE IF EXISTS conc_uniq");
	(void)exec_ok(c, "CREATE TABLE conc_uniq(k INT PRIMARY KEY) ENGINE=InnoDB");
	(void)exec_ok(c, "INSERT INTO conc_uniq VALUES(1)");

	st = mc_exec(c, "INSERT INTO conc_uniq VALUES(1)", NULL);
	check_true("重复键插入失败", st != MC_OK);
	check_long("重复键的错误号是 1062", (long)mc_errno(c), 1062);
	check_str("重复键的 SQLSTATE 是 23000", mc_sqlstate(c), "23000");

	/* 5.2 成功不刷新：契约是"只在失败路径更新"，成功之后读到的是旧值 */
	(void)exec_ok(c, "INSERT INTO conc_uniq VALUES(2)");
	check_long("成功之后错误号保持上一次失败的值", (long)mc_errno(c), 1062);

	/* 5.3a 指针为 NULL 的早退不记原因，因此也不动错误号——这是既有行为，
	 * 与 mc_error 保持一致（两者要一起改才有意义，见 README 已知局限）。
	 * 断言它的目的不是"这样对"，而是把它钉住：将来统一这段行为时，
	 * 这条用例会红，提醒改的人同时更新文档。 */
	st = mc_exec(c, NULL, NULL);
	check_true("SQL 为 NULL 时返回参数错误", st == MC_ERR_PARAM);
	check_long("早退不记录新原因，错误号保持上一次的 1062",
	           (long)mc_errno(c), 1062);

	/* 5.3b 本地错误（参数类型非法）清零。类型检查在 prepare 之前完成，
	 * 失败时用 set_failure 记一条描述，因此错误号与 SQLSTATE 一起被清成
	 * "没有服务端错误"。
	 *
	 * 这里刻意不用"占位符个数不符"当本地错误：那种语句在 prepare 阶段
	 * 就被服务端以 1136（列数不符）拒掉，测到的是服务端的检查，
	 * 与本模块的本地校验不是一回事——11-14 已经在 check_live.sh 踩过一次。 */
	{
		mc_param bad = { .kind = (mc_param_kind)99, .v.i = 0 };

		st = mc_exec_params(c, "INSERT INTO conc_uniq VALUES(?)", &bad, 1, NULL);
		check_true("参数类型非法时失败", st == MC_ERR_PARAM);
		check_true("参数类型非法时给出了原因",
		           mc_error(c) != NULL && mc_error(c)[0] != '\0');
		check_long("本地错误之后错误号为 0", (long)mc_errno(c), 0);
		check_str("本地错误之后 SQLSTATE 为 00000", mc_sqlstate(c), "00000");
	}

	/* 5.4 语法错误 */
	st = mc_exec(c, "SELEC 1", NULL);
	check_true("语法错误时失败", st != MC_OK);
	check_long("语法错误的错误号是 1064", (long)mc_errno(c), 1064);
	check_str("语法错误的 SQLSTATE 是 42000", mc_sqlstate(c), "42000");

	/* 5.5 语句对象的错误同样带号：prepare 阶段的语法错 */
	{
		mc_stmt *bad = NULL;
		st = mc_stmt_prepare(c, "SELEC 2", &bad);
		check_true("语句准备失败", st != MC_OK);
		check_long("prepare 阶段的错误号是 1064", (long)mc_errno(c), 1064);
		mc_stmt_close(bad);
	}

	/* 5.6 建连失败：句柄不存在，走线程局部的那一份 */
	mc_options_init(&opt);
	(void)snprintf(opt.host, sizeof opt.host, "127.0.0.1");
	(void)snprintf(opt.user, sizeof opt.user, "root");
	opt.port = 1;                       /* 本机 1 号端口不会有 MySQL */
	opt.connect_timeout = 2;
	st = mc_open(&opt, &tmp);
	check_true("连不上的端口建连失败", st != MC_OK);
	check_true("建连失败后句柄为 NULL", tmp == NULL);
	check_true("建连失败时错误号非 0（客户端库给的 2002/2003 一类）",
	           mc_errno(NULL) != 0);
	check_str("建连失败的 SQLSTATE 是 HY000", mc_sqlstate(NULL), "HY000");

	/* 5.7 参数检查先于网络：非法端口不会碰网络，号必须是 0 */
	mc_options_init(&opt);
	(void)snprintf(opt.host, sizeof opt.host, "127.0.0.1");
	(void)snprintf(opt.user, sizeof opt.user, "root");
	opt.port = 0;
	st = mc_open(&opt, &tmp);
	check_true("端口为 0 被拒", st == MC_ERR_PARAM);
	check_long("本地参数检查之后错误号为 0", (long)mc_errno(NULL), 0);
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */

int main(void) {
	mc_conn  *setup = NULL;
	mc_conn  *c = NULL;

	if (open_env(NULL, &setup) != MC_OK) {
		(void)fprintf(stderr, "连不上数据库：%s\n", mc_error(NULL));
		return 1;
	}

	if (mc_exec(setup, "DROP DATABASE IF EXISTS " DBNAME, NULL) != MC_OK) {
		(void)fprintf(stderr, "删库失败：%s\n", mc_error(setup));
		mc_close(setup);
		return 1;
	}
	if (mc_exec(setup, "CREATE DATABASE " DBNAME, NULL) != MC_OK) {
		(void)fprintf(stderr, "建库失败：%s\n", mc_error(setup));
		mc_close(setup);
		return 1;
	}
	mc_close(setup);

	if (open_env(DBNAME, &c) != MC_OK) {
		(void)fprintf(stderr, "连不上测试库：%s\n", mc_error(NULL));
		return 1;
	}

	case_concurrent_insert(c);
	case_deadlock(c);
	case_lock_timeout(c);
	case_concurrent_stmt(c);
	case_errno_contract(c);

	/* 清理。库留着无害，但下次运行要从干净状态开始，索性删掉。 */
	mc_close(c);

	if (open_env(NULL, &setup) == MC_OK) {
		(void)mc_exec(setup, "DROP DATABASE IF EXISTS " DBNAME, NULL);
		mc_close(setup);
	}

	(void)printf("\n%d 项检查，%d 项失败\n", g_checks, g_fails);
	return (g_fails == 0) ? 0 : 1;
}
