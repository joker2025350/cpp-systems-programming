/*
 * mysqlclient.c —— 数据库访问层
 *
 * 这一层是模块里唯一 Include <mysql.h> 的编译单元。接口层（mysqlclient.h）
 * 不出现任何 MySQL 类型，因此库的使用者不被绑定到具体的客户端库版本：
 * 8.0.1 起 MYSQL_BIND::is_null 的类型从 my_bool* 改成了 bool*，
 * 这类变化只影响本文件。
 *
 * 错误信息的存放位置有讲究：
 *   - 已经拿到句柄之后，错误写进 conn->errbuf；
 *   - 建连之前还没有句柄，写进模块级的 thread-local 缓冲，
 *     用 mc_error(NULL) 取。
 * 不把错误文本挂在语句句柄上，是因为 mysql_stmt_error 返回的指针
 * 在 mysql_stmt_close 之后失效——本模块的每个错误路径都以 close 收尾，
 * 若等到调用方来读，读到的是一块已释放的内存。
 */

#include "mysqlclient.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mysql.h>

#define MC_ERRBUF 512

struct mc_conn {
	MYSQL *mysql;
	char   errbuf[MC_ERRBUF];
};

/*
 * _Thread_local 是 C11 6.7.1 定义的存储类说明符：每个线程各有一份副本。
 * 理由与 errno 相同——多线程共用一个静态缓冲时，A 线程的建连失败
 * 会被 B 线程的失败覆盖，报出来的原因与实际问题无关。
 */
static _Thread_local char g_last_error[MC_ERRBUF];

static void set_error(mc_conn *conn, const char *stage, const char *detail) {
	char *dst = (conn != NULL) ? conn->errbuf : g_last_error;

	if (detail == NULL || detail[0] == '\0') detail = "unknown error";

	(void)snprintf(dst, MC_ERRBUF, "%s: %s", stage, detail);
}

/* 语句级错误：文本必须在 close 之前取走 */
static mc_status stmt_error(mc_conn *conn, MYSQL_STMT *stmt,
                            const char *stage, mc_status code) {
	set_error(conn, stage, mysql_stmt_error(stmt));
	return code;
}

const char *mc_error(const mc_conn *conn) {
	if (conn == NULL) return g_last_error;
	return conn->errbuf;
}

/* ------------------------------------------------------------------
 * 建连与断开
 * ------------------------------------------------------------------ */

mc_status mc_open(const mc_options *opt, mc_conn **out) {

	mc_conn     *conn;
	mc_status    st;
	unsigned int timeout;

	if (out == NULL) return MC_ERR_PARAM;
	*out = NULL;

	/* 参数不合法时不接触网络：见 options.c 顶部关于"回退"的说明 */
	st = mc_options_check(opt);
	if (st != MC_OK) {
		set_error(NULL, "options", mc_status_string(st));
		return st;
	}

	/*
	 * 句柄在堆上，调用方只拿到指针。MYSQL 结构体的体积属于实现细节
	 * （8.0 的 struct MYSQL 有数百字节），把它放进公开头文件就等于
	 * 把它的大小写进 ABI：库一升级，调用方的栈帧布局就得跟着重编。
	 */
	conn = (mc_conn *)calloc(1, sizeof *conn);
	if (conn == NULL) {
		set_error(NULL, "calloc", "out of memory");
		return MC_ERR_NOMEM;
	}

	/* mysql_init(NULL) 内部自己分配 MYSQL 结构体，失败返回 NULL */
	conn->mysql = mysql_init(NULL);
	if (conn->mysql == NULL) {
		set_error(NULL, "mysql_init", "out of memory");
		free(conn);
		return MC_ERR_NOMEM;
	}

	/*
	 * 字符集必须在建连之前设定：它决定握手时使用的 collation。
	 * 8.0 的服务端默认是 utf8mb4_0900_ai_ci，但客户端库的默认值取决于
	 * 编译选项，两边不一致时，非 ASCII 文本会在传输层被按 latin1 解释
	 * 而出现乱码——这种错误在 BLOB 上看不出来（BLOB 不走字符集转换），
	 * 只在文本列上暴露，容易被误判成"数据存进去时就坏了"。
	 */
	if (mysql_options(conn->mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
		set_error(NULL, "mysql_options(charset)", mysql_error(conn->mysql));
		mysql_close(conn->mysql);
		free(conn);
		return MC_ERR_CONNECT;
	}

	if (opt->connect_timeout > 0) {
		timeout = opt->connect_timeout;

		/*
		 * MYSQL_OPT_CONNECT_TIMEOUT 要求参数指向一个 unsigned int，
		 * 生命周期只需覆盖这次调用——库在 mysql_options 里就把值拷走了。
		 */
		if (mysql_options(conn->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0) {
			set_error(NULL, "mysql_options(timeout)", mysql_error(conn->mysql));
			mysql_close(conn->mysql);
			free(conn);
			return MC_ERR_CONNECT;
		}
	}

	if (mysql_real_connect(conn->mysql, opt->host, opt->user,
	                       (opt->password[0] != '\0') ? opt->password : NULL,
	                       (opt->database[0] != '\0') ? opt->database : NULL,
	                       opt->port, NULL, 0) == NULL) {
		set_error(NULL, "connect", mysql_error(conn->mysql));
		mysql_close(conn->mysql);
		free(conn);
		return MC_ERR_CONNECT;
	}

	*out = conn;
	return MC_OK;
}

void mc_close(mc_conn *conn) {
	if (conn == NULL) return;

	mysql_close(conn->mysql);   /* 顺带发送 COM_QUIT */
	free(conn);
}

mc_status mc_ping(mc_conn *conn) {
	if (conn == NULL) return MC_ERR_PARAM;

	/*
	 * mysql_ping 在连接已断开时会尝试自动重连（如果 MYSQL_OPT_RECONNECT
	 * 打开）。8.0 起该选项默认关闭，因此这里的行为是"探测"而不是"悄悄重建"：
	 * 反手重连会让调用方以为连接一直有效，而事务、临时表、
	 * 会话变量其实都丢了。
	 */
	if (mysql_ping(conn->mysql) != 0) {
		set_error(conn, "ping", mysql_error(conn->mysql));
		return MC_ERR_CONNECT;
	}

	return MC_OK;
}

/* ------------------------------------------------------------------
 * SQL 执行
 * ------------------------------------------------------------------ */

mc_status mc_exec(mc_conn *conn, const char *sql, unsigned long long *affected) {

	my_ulonglong n;

	if (conn == NULL || sql == NULL || sql[0] == '\0') return MC_ERR_PARAM;

	/*
	 * mysql_real_query 要求显式传长度，因此 SQL 里可以含 '\0'
	 * （例如把一个二进制串直接拼进语句）。本模块只用 strlen，
	 * 含 '\0' 的 SQL 会被截断——需要写二进制请走 mc_blob_write 的参数绑定。
	 */
	if (mysql_real_query(conn->mysql, sql, (unsigned long)strlen(sql)) != 0) {
		set_error(conn, "exec", mysql_error(conn->mysql));
		return MC_ERR_QUERY;
	}

	if (affected != NULL) {
		n = mysql_affected_rows(conn->mysql);

		/*
		 * 失败时 mysql_affected_rows 返回 (my_ulonglong)-1，
		 * 也就是无符号最大值。它不是"影响了很多行"，而是一个哨兵值；
		 * 直接转成 unsigned long long 输出会变成一个天文数字。
		 */
		if (n == (my_ulonglong)-1) {
			set_error(conn, "affected_rows", mysql_error(conn->mysql));
			return MC_ERR_QUERY;
		}

		*affected = (unsigned long long)n;
	}

	return MC_OK;
}

/*
 * 取回整个结果集。
 *
 * 内存布局：columns 的名字与全部单元格各自放在一段连续内存里，
 * 指针数组只保存偏移。分两遍遍历是为了第一遍就能算出这段内存的总长度：
 * 逐格 malloc 的话，十万行十列的查询就是一百万次分配，
 * 而每一次都可能失败，清理逻辑会膨胀到没人愿意读。
 *
 * 代价是要对结果集做一次 mysql_data_seek(res, 0) 回到开头。
 * 这对 mysql_store_result 取回的结果集成立（数据已经在客户端），
 * 对 mysql_use_result 的流式结果集不成立——所以这里只用 store。
 */
mc_status mc_query(mc_conn *conn, const char *sql, mc_rows *out) {

	MYSQL        *mysql;
	MYSQL_RES    *res = NULL;
	MYSQL_FIELD  *fields;
	MYSQL_ROW     row;
	unsigned long *lens;
	size_t        rows;
	size_t        cols;
	size_t        cells;
	size_t        total_cells = 0;
	size_t        total_names = 0;
	size_t        pos;
	size_t        i;
	size_t        j;
	char        **names = NULL;
	char        **cell = NULL;
	size_t       *length = NULL;
	char         *arena = NULL;
	char         *name_arena = NULL;
	mc_status     fail_status = MC_ERR_NOMEM;

	if (out == NULL) return MC_ERR_PARAM;
	(void)memset(out, 0, sizeof *out);

	if (conn == NULL || sql == NULL || sql[0] == '\0') return MC_ERR_PARAM;

	mysql = conn->mysql;

	if (mysql_real_query(mysql, sql, (unsigned long)strlen(sql)) != 0) {
		set_error(conn, "query", mysql_error(mysql));
		return MC_ERR_QUERY;
	}

	res = mysql_store_result(mysql);
	if (res == NULL) {
		/*
		 * 两种情况下 store_result 返回 NULL：语句本来就不返回结果集
		 * （INSERT / UPDATE / DDL），或者取回过程中出错。前者由
		 * mysql_field_count 为 0 区分，把它报成错误能防止调用方
		 * 对着一个空结构体做遍历，却不知道语句其实是 UPDATE。
		 */
		if (mysql_field_count(mysql) == 0) {
			set_error(conn, "store_result",
			          "statement returns no result set, use mc_exec");
			return MC_ERR_RESULT;
		}

		set_error(conn, "store_result", mysql_error(mysql));
		return MC_ERR_QUERY;
	}

	rows = (size_t)mysql_num_rows(res);
	cols = (size_t)mysql_num_fields(res);

	/* rows * cols 溢出检查：两个 size_t 相乘可能回绕，
	 * 回绕之后分配出来的数组比实际要用的短，越界写就发生在下一次循环。 */
	if (cols != 0 && rows > (SIZE_MAX / cols)) {
		set_error(conn, "alloc", "result set too large");
		mysql_free_result(res);
		return MC_ERR_NOMEM;
	}
	cells = rows * cols;

	/* ---- 第一遍：算出单元格字符串需要的总字节数 ---- */
	while ((row = mysql_fetch_row(res)) != NULL) {
		lens = mysql_fetch_lengths(res);
		if (lens == NULL) {
			set_error(conn, "fetch_lengths", mysql_error(mysql));
			mysql_free_result(res);
			return MC_ERR_QUERY;
		}

		for (j = 0; j < cols; j++) {
			if (row[j] == NULL) continue;   /* SQL NULL 不占空间 */

			if (total_cells > SIZE_MAX - ((size_t)lens[j] + 1)) {
				set_error(conn, "alloc", "result set too large");
				mysql_free_result(res);
				return MC_ERR_NOMEM;
			}
			total_cells += (size_t)lens[j] + 1;
		}
	}

	mysql_data_seek(res, 0);

	fields = mysql_fetch_fields(res);
	for (j = 0; j < cols; j++) {
		total_names += strlen(fields[j].name) + 1;
	}

	if (cells > 0) {
		cell   = (char **)calloc(cells, sizeof *cell);
		length = (size_t *)calloc(cells, sizeof *length);

		if (cell == NULL || length == NULL) {
			set_error(conn, "alloc", "out of memory");
			goto fail;
		}
	}

	if (cols > 0) {
		names = (char **)calloc(cols, sizeof *names);
		if (names == NULL) {
			set_error(conn, "alloc", "out of memory");
			goto fail;
		}
	}

	if (total_cells > 0) {
		arena = (char *)malloc(total_cells);
		if (arena == NULL) {
			set_error(conn, "alloc", "out of memory");
			goto fail;
		}
	}

	if (total_names > 0) {
		name_arena = (char *)malloc(total_names);
		if (name_arena == NULL) {
			set_error(conn, "alloc", "out of memory");
			goto fail;
		}
	}

	/* 列名 */
	pos = 0;
	for (j = 0; j < cols; j++) {
		size_t n = strlen(fields[j].name);

		(void)memcpy(name_arena + pos, fields[j].name, n);
		name_arena[pos + n] = '\0';
		names[j] = name_arena + pos;
		pos += n + 1;
	}

	/* ---- 第二遍：拷贝单元格 ---- */
	pos = 0;
	for (i = 0; i < rows; i++) {
		row = mysql_fetch_row(res);
		if (row == NULL) {
			/* num_rows 说还有，fetch 却给不出——结果集在取回过程中被破坏 */
			set_error(conn, "fetch_row", "result set truncated");
			fail_status = MC_ERR_RESULT;
			goto fail;
		}

		lens = mysql_fetch_lengths(res);
		if (lens == NULL) {
			set_error(conn, "fetch_lengths", mysql_error(mysql));
			fail_status = MC_ERR_QUERY;
			goto fail;
		}

		for (j = 0; j < cols; j++) {
			size_t k = i * cols + j;
			size_t n;

			if (row[j] == NULL) {
				/*
				 * SQL NULL 与空串必须区分开：前者是"没有值"，
				 * 后者是"值就是空的"。用 NULL 指针表示前者，
				 * 调用方用 cell[k] == NULL 判断即可。
				 */
				cell[k]   = NULL;
				length[k] = 0;
				continue;
			}

			/*
			 * 长度取自 mysql_fetch_lengths，不是 strlen：
			 * BLOB 列的字节里可以含 '\0'，strlen 会在这里提前结束。
			 */
			n = (size_t)lens[j];

			(void)memcpy(arena + pos, row[j], n);
			arena[pos + n] = '\0';    /* 便于直接当字符串用，不计入 length */
			cell[k]   = arena + pos;
			length[k] = n;
			pos += n + 1;
		}
	}

	mysql_free_result(res);

	out->rows       = rows;
	out->cols       = cols;
	out->names      = names;
	out->cell       = cell;
	out->length     = length;
	out->arena      = arena;
	out->name_arena = name_arena;

	return MC_OK;

fail:
	free(names);
	free(cell);
	free(length);
	free(arena);
	free(name_arena);
	if (res != NULL) mysql_free_result(res);

	return fail_status;
}

void mc_rows_free(mc_rows *rows) {
	if (rows == NULL) return;

	/*
	 * cell[] 与 names[] 里的元素都指向 arena / name_arena 内部，
	 * 逐个 free 会在第一次调用时就崩（不是 malloc 返回的地址）。
	 * 真正要释放的只有这五块。
	 */
	free(rows->names);
	free(rows->cell);
	free(rows->length);
	free(rows->arena);
	free(rows->name_arena);

	(void)memset(rows, 0, sizeof *rows);

	/*
	 * 清零不是讲究：调用方对同一个结构体重复调用 free 时必须安全，
	 * 而"释放过"这件事没有任何外部标志可依赖，只能靠指针本身。
	 * free(NULL) 是空操作（C11 7.22.3.3），于是第二次调用什么都不做。
	 */
}

/* ------------------------------------------------------------------
 * 流式取数（游标）
 *
 * 与 mc_query 的差别是"什么时候分配"：mc_query 先知道总行数，
 * 算出一块连续内存一次分配完；这里事先不知道有多少行，
 * 因此每一列的缓冲区按"当前这一行有多宽"增长，行与行之间复用。
 * 峰值内存 = 各列历史最宽值之和，与行数无关。
 *
 * 取数走的是预处理语句的二进制协议，且**不调用 mysql_stmt_store_result**。
 * 这一条是整个实现的关键：store_result 会让库把应答里的全部行读进
 * 客户端内存（那正是 mc_query 的行为），不调用时 mysql_stmt_fetch
 * 每次从套接字上读一行。
 * ------------------------------------------------------------------ */

/* 一列在取数过程中的可变状态 */
typedef struct {
	char         *buf;    /* cap 字节；最后 1 字节留给 '\0'，不计入内容 */
	size_t        cap;
	unsigned long len;    /* 本轮该列的字节数，由库写入 */
	bool          null;   /* 本轮该列是否 SQL NULL，由库写入 */
	bool          trunc;  /* 本轮该列是否被截断，由库写入 */
} stream_col;

/* 首行之前的初始宽度。选 256 是因为它覆盖了绝大多数整数、
 * 时间戳与短字符串，常见场景下第一行就不必再调 fetch_column。
 * 不选更大的值是因为它按列数成倍占用：宽表 100 列时 4 MiB 就是浪费。 */
#define MC_STREAM_INIT 256

/* 让 col 至少能放下 need 字节内容，外加 1 字节 '\0'。
 * 增长按倍数进行，避免"每次只多一字节"时逐行 realloc。
 * 返回 0 成功，-1 表示内存不足（原缓冲区保持有效）。 */
static int col_reserve(stream_col *col, size_t need) {
	size_t want;
	char  *p;

	if (need == SIZE_MAX) return -1;   /* need + 1 会回绕 */

	want = need + 1;
	if (col->cap >= want) return 0;
	if (want < col->cap * 2) want = col->cap * 2;

	p = (char *)realloc(col->buf, want);
	if (p == NULL) return -1;

	col->buf = p;
	col->cap = want;
	return 0;
}

mc_status mc_query_each(mc_conn *conn, const char *sql,
                        mc_row_fn fn, void *ctx, size_t *out_rows) {

	MYSQL_STMT   *stmt = NULL;
	MYSQL_BIND   *bind = NULL;
	stream_col   *col = NULL;
	const char  **cell = NULL;
	size_t       *length = NULL;
	size_t        cols = 0;
	size_t        j;
	size_t        delivered = 0;
	bool          executed = false;
	mc_status     st = MC_OK;
	int           ret;

	/* 先写 0：后面每条失败路径都不必再操心这个出参 */
	if (out_rows != NULL) *out_rows = 0;

	if (conn == NULL || sql == NULL || sql[0] == '\0' || fn == NULL) {
		set_error(conn, "query_each", "invalid argument");
		return MC_ERR_PARAM;
	}

	stmt = mysql_stmt_init(conn->mysql);
	if (stmt == NULL) {
		set_error(conn, "stmt_init", mysql_error(conn->mysql));
		return MC_ERR_STMT;
	}

	if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
		st = stmt_error(conn, stmt, "prepare", MC_ERR_STMT);
		goto out;
	}

	/*
	 * 列数来自服务器发回的元数据，prepare 之后就有。
	 * 为 0 说明这条语句不产生结果集，与 mc_query 里的判据一致：
	 * 把它报成错误，比让调用方对着一个空回调序列以为"没有数据"要好。
	 */
	cols = (size_t)mysql_stmt_field_count(stmt);
	if (cols == 0) {
		set_error(conn, "query_each",
		          "statement returns no result set, use mc_exec");
		st = MC_ERR_RESULT;
		goto out;
	}

	bind   = (MYSQL_BIND *)calloc(cols, sizeof *bind);
	col    = (stream_col *)calloc(cols, sizeof *col);
	cell   = (const char **)calloc(cols, sizeof *cell);
	length = (size_t *)calloc(cols, sizeof *length);

	if (bind == NULL || col == NULL || cell == NULL || length == NULL) {
		set_error(conn, "alloc", "out of memory");
		st = MC_ERR_NOMEM;
		goto out;
	}

	for (j = 0; j < cols; j++) {
		col[j].buf = (char *)malloc(MC_STREAM_INIT);
		if (col[j].buf == NULL) {
			set_error(conn, "alloc", "out of memory");
			st = MC_ERR_NOMEM;
			goto out;
		}
		col[j].cap = MC_STREAM_INIT;

		/*
		 * buffer_type 一律声明成 MYSQL_TYPE_STRING：库会把整数、浮点、
		 * 日期都转成文本形式写进缓冲区，BLOB 则原样拷字节
		 * （长度看 length，不受内容里的 '\0' 影响）。
		 * 逐列声明真实类型能省掉这次转换，代价是要为每种类型写一遍分支，
		 * 而本接口交付给回调的本来就是"字节 + 长度"，没有必要。
		 */
		bind[j].buffer_type   = MYSQL_TYPE_STRING;
		bind[j].buffer        = col[j].buf;
		bind[j].buffer_length = (unsigned long)(col[j].cap - 1);
		bind[j].length        = &col[j].len;
		bind[j].is_null       = &col[j].null;
		bind[j].error         = &col[j].trunc;
	}

	if (mysql_stmt_bind_result(stmt, bind) != 0) {
		st = stmt_error(conn, stmt, "bind_result", MC_ERR_STMT);
		goto out;
	}

	if (mysql_stmt_execute(stmt) != 0) {
		st = stmt_error(conn, stmt, "execute", MC_ERR_QUERY);
		goto out;
	}
	executed = true;

	for (;;) {
		/* 库只在"值被截断"或"出错"时写这三个字段，不清零就会把
		 * 上一行的状态当成这一行的：上一行的 NULL 会让这一行也报 NULL，
		 * 上一行的 trunc 会让这一行多走一遍 fetch_column。 */
		for (j = 0; j < cols; j++) {
			col[j].len   = 0;
			col[j].null  = false;
			col[j].trunc = false;
			bind[j].buffer        = col[j].buf;
			bind[j].buffer_length = (unsigned long)(col[j].cap - 1);
		}

		ret = mysql_stmt_fetch(stmt);

		if (ret == MYSQL_NO_DATA) break;

		/*
		 * 三个返回值的含义：
		 *   0                     本行取回成功；
		 *   MYSQL_DATA_TRUNCATED  本行取回，但有列超出缓冲区长度——
		 *                         库已经把真实长度写进 length，值要另行取；
		 *   其余                  真错误（连接断开、语句被并发占用等）。
		 * 原实现的同类代码把后两者合在一个 break 里，于是断线时
		 * 调用方看不到任何错误，只看到"数据取完了"。
		 */
		if (ret != 0 && ret != MYSQL_DATA_TRUNCATED) {
			st = stmt_error(conn, stmt, "fetch", MC_ERR_STMT);
			goto out;
		}

		if (ret == MYSQL_DATA_TRUNCATED) {
			/*
			 * 判据不只看库给的 trunc 标志，也看"长度已经达到缓冲区上限"：
			 * 只信标志的话，一旦某个版本没有按列置位，接下来写
			 * buf[len] = '\0' 就越界了。两个条件取并集是廉价的，
			 * 每行每列多一次比较而已。
			 */
			for (j = 0; j < cols; j++) {
				if (col[j].null) continue;
				if (!col[j].trunc &&
				    (size_t)col[j].len < col[j].cap) continue;

				if (col_reserve(&col[j], (size_t)col[j].len) != 0) {
					set_error(conn, "alloc", "out of memory");
					st = MC_ERR_NOMEM;
					goto out;
				}

				/*
				 * 请求长度正好等于字段长度，因此不会再被截断。
				 * 此时不能沿用 buffer_length = cap - 1：那会多拷一个
				 * 字节到缓冲区之外（cap-1 已经是"内容上限"）。
				 */
				bind[j].buffer        = col[j].buf;
				bind[j].buffer_length = col[j].len;

				if (mysql_stmt_fetch_column(stmt, &bind[j],
				                            (unsigned int)j, 0) != 0) {
					st = stmt_error(conn, stmt, "fetch_column",
					                MC_ERR_STMT);
					goto out;
				}
			}
		}

		/* 交给回调的视图。NULL 与空串是两件事：空串的 buf 非 NULL
		 * （buf[0] == '\0'），SQL NULL 的 cell 才是 NULL。
		 * 每一列的 buf 在进入循环之前都已分配，因此这里不需要判空。 */
		for (j = 0; j < cols; j++) {
			if (col[j].null) {
				cell[j]   = NULL;
				length[j] = 0;
				continue;
			}

			col[j].buf[col[j].len] = '\0';
			cell[j]   = col[j].buf;
			length[j] = (size_t)col[j].len;
		}

		delivered++;

		/* 非 0 表示调用方不再需要后续的行，这是正常结束 */
		if (fn(ctx, cell, length, cols) != 0) break;
	}

	st = MC_OK;

out:
	/*
	 * free_result 不能省。提前停止时结果集里还有没读的行，
	 * 此时直接发下一条命令会得到 "Commands out of sync"；
	 * 这个调用把这些行读掉丢弃，连接才回到可用状态。
	 * mysql_stmt_close 内部也会做同样的事，但依赖这一点属于
	 * 依赖实现细节，写出来更稳。
	 */
	if (stmt != NULL) {
		/* 没执行过就没有结果集可释放，对未执行的语句调用它没有意义 */
		if (executed) (void)mysql_stmt_free_result(stmt);

		if (mysql_stmt_close(stmt) != 0 && st == MC_OK) {
			set_error(conn, "stmt_close", mysql_error(conn->mysql));
			st = MC_ERR_STMT;
		}
	}

	if (col != NULL) {
		for (j = 0; j < cols; j++) free(col[j].buf);
	}
	free(col);
	free(bind);
	free(cell);
	free(length);

	if (out_rows != NULL) *out_rows = delivered;

	return st;
}

/* ------------------------------------------------------------------
 * 大字段
 * ------------------------------------------------------------------ */

mc_status mc_blob_write(mc_conn *conn, const char *sql,
                        const void *data, size_t len) {

	MYSQL_STMT *stmt = NULL;
	MYSQL_BIND  param;
	mc_status   st = MC_OK;
	const char *bytes = (const char *)data;
	size_t      off = 0;

	if (conn == NULL || sql == NULL || sql[0] == '\0') return MC_ERR_PARAM;
	if (data == NULL && len > 0) return MC_ERR_PARAM;

	stmt = mysql_stmt_init(conn->mysql);
	if (stmt == NULL) {
		/* 与语句无关的错误（内存不足），文本挂在连接上 */
		set_error(conn, "stmt_init", mysql_error(conn->mysql));
		return MC_ERR_STMT;
	}

	if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
		st = stmt_error(conn, stmt, "prepare", MC_ERR_STMT);
		goto out;
	}

	(void)memset(&param, 0, sizeof param);
	param.buffer_type   = MYSQL_TYPE_LONG_BLOB;
	param.buffer        = NULL;
	param.buffer_length = 0;

	/*
	 * 这里声明的是"这个参数是个长字段"，而不是它的内容。
	 * buffer_length 必须为 0：真正的字节由下面的 send_long_data 增量送出，
	 * 库在 execute 时自己累计总长度。is_null 与 length 留空是允许的，
	 * 前提是数据分块送（一次性 send 与绑定缓冲区二选一）。
	 */
	if (mysql_stmt_bind_param(stmt, &param) != 0) {
		st = stmt_error(conn, stmt, "bind_param", MC_ERR_STMT);
		goto out;
	}

	/*
	 * 分块发送。send_long_data 的语义是"追加一段"，多次调用按顺序拼接，
	 * 因此单次报文长度只受 max_allowed_packet 约束，而不受一次传入的长度约束。
	 * 一次传整个 64 KiB 图片在当前网络下当然也能work，但一旦换成 100 MB
	 * 的文件，单次调用就要求驱动在内存里再准备一份完整副本（或直接失败）；
	 * 分块是唯一不需要为大字段额外分配内存的写法。
	 */
	while (off < len) {
		size_t chunk = len - off;

		if (chunk > MC_BLOB_CHUNK) chunk = MC_BLOB_CHUNK;

		if (mysql_stmt_send_long_data(stmt, 0, bytes + off,
		                              (unsigned long)chunk) != 0) {
			st = stmt_error(conn, stmt, "send_long_data", MC_ERR_STMT);
			goto out;
		}

		off += chunk;
	}

	if (mysql_stmt_execute(stmt) != 0) {
		st = stmt_error(conn, stmt, "execute", MC_ERR_QUERY);
		goto out;
	}

out:
	/*
	 * 无论成功失败都要 close；close 本身也可能失败（例如服务端在
	 * 语句执行期间断开），失败信息保留，但不覆盖前面更具体的那一条。
	 */
	if (mysql_stmt_close(stmt) != 0 && st == MC_OK) {
		set_error(conn, "stmt_close", mysql_error(conn->mysql));
		st = MC_ERR_STMT;
	}

	return st;
}

mc_status mc_blob_read(mc_conn *conn, const char *sql,
                       void *buf, size_t cap, size_t *out_len) {

	MYSQL_STMT   *stmt = NULL;
	MYSQL_BIND    result;
	mc_status     st = MC_OK;
	unsigned long col_len = 0;
	bool          is_null = false;
	size_t        wanted;
	size_t        offset;
	int           ret;

	if (conn == NULL || sql == NULL || sql[0] == '\0') return MC_ERR_PARAM;
	if (buf == NULL || out_len == NULL) return MC_ERR_PARAM;

	/* cap 为 0 无法容纳任何数据；把它与"字段长度为 0"区分开 */
	if (cap == 0) return MC_ERR_PARAM;

	stmt = mysql_stmt_init(conn->mysql);
	if (stmt == NULL) {
		set_error(conn, "stmt_init", mysql_error(conn->mysql));
		return MC_ERR_STMT;
	}

	if (mysql_stmt_prepare(stmt, sql, (unsigned long)strlen(sql)) != 0) {
		st = stmt_error(conn, stmt, "prepare", MC_ERR_STMT);
		goto out;
	}

	(void)memset(&result, 0, sizeof result);
	result.buffer_type   = MYSQL_TYPE_LONG_BLOB;
	result.buffer        = NULL;      /* 先不给缓冲区，只问长度 */
	result.buffer_length = 0;
	result.length        = &col_len;  /* 取回后是字段的真实字节数 */
	result.is_null       = &is_null;

	/*
	 * is_null 的类型在 8.0.1 从 my_bool* 改成了 bool*。用 bool 是本模块
	 * 声明的最低版本要求（8.0 及以上）的直接结果；要同时兼容 5.7 就得写
	 * 版本条件编译，这不值得——5.7 已在 2023 年结束支持。
	 */

	if (mysql_stmt_bind_result(stmt, &result) != 0) {
		st = stmt_error(conn, stmt, "bind_result", MC_ERR_STMT);
		goto out;
	}

	if (mysql_stmt_execute(stmt) != 0) {
		st = stmt_error(conn, stmt, "execute", MC_ERR_QUERY);
		goto out;
	}

	if (mysql_stmt_store_result(stmt) != 0) {
		st = stmt_error(conn, stmt, "store_result", MC_ERR_QUERY);
		goto out;
	}

	/* 列数必须恰好一列：多列时 fetch_column 的第 2 个参数取 0 只取第一列，
	 * 结果是"函数成功但取错了列"，比报错难查。 */
	if (mysql_stmt_field_count(stmt) != 1) {
		set_error(conn, "field_count", "statement must return exactly one column");
		st = MC_ERR_RESULT;
		goto out;
	}

	ret = mysql_stmt_fetch(stmt);

	if (ret == MYSQL_NO_DATA) {
		set_error(conn, "fetch", "query returned no row");
		st = MC_ERR_NODATA;
		goto out;
	}

	/*
	 * 绑定缓冲区长度为 0 时，MySQL 对任何非空字段都返回 MYSQL_DATA_TRUNCATED
	 * ——这是"我告诉你长度了，你自己来取"的正常协议，不是错误。
	 * 原实现把 MYSQL_DATA_TRUNCATED 与其他非零返回值一起 break 掉，
	 * 顺带也吞掉了真正的错误：网络中断时函数同样返回 0，
	 * 调用方拿到的是一段长度 0 的"成功"数据。
	 */
	if (ret != 0 && ret != MYSQL_DATA_TRUNCATED) {
		st = stmt_error(conn, stmt, "fetch", MC_ERR_STMT);
		goto out;
	}

	if (is_null) {
		/* 该行存在，但这一列是 SQL NULL，与"查不到行"是两件事 */
		set_error(conn, "fetch", "column is SQL NULL");
		st = MC_ERR_NODATA;
		goto out;
	}

	wanted = (size_t)col_len;

	if (wanted > cap) {
		(void)snprintf(conn->errbuf, MC_ERRBUF,
		               "capacity: column is %lu bytes, buffer is %lu bytes",
		               col_len, (unsigned long)cap);
		st = MC_ERR_TOOBIG;
		goto out;
	}

	/*
	 * 按区间取回。每次请求的正是"剩余的全部长度"，因此不会发生截断：
	 * fetch_column 只会在请求长度小于字段长度时才截断。
	 * 原实现把 buffer_length 固定为 1，读满 64 KiB 要 65536 次调用。
	 */
	offset = 0;
	while (offset < wanted) {
		unsigned long remaining = (unsigned long)(wanted - offset);

		result.buffer        = (char *)buf + offset;
		result.buffer_length = remaining;

		if (mysql_stmt_fetch_column(stmt, &result, 0,
		                            (unsigned long)offset) != 0) {
			st = stmt_error(conn, stmt, "fetch_column", MC_ERR_STMT);
			goto out;
		}

		offset += remaining;
	}

	/*
	 * 再 fetch 一次，确认结果集只有一行。
	 * "取到第几行算第几行"的写法会把多行结果悄悄变成最后一行的内容：
	 * 循环体每轮都从头写同一块缓冲区，函数返回值看起来完全正常。
	 */
	ret = mysql_stmt_fetch(stmt);
	if (ret != MYSQL_NO_DATA) {
		if (ret == 0 || ret == MYSQL_DATA_TRUNCATED) {
			set_error(conn, "fetch", "query returned more than one row");
			st = MC_ERR_RESULT;
		} else {
			st = stmt_error(conn, stmt, "fetch", MC_ERR_STMT);
		}
		goto out;
	}

	*out_len = wanted;

out:
	if (mysql_stmt_close(stmt) != 0 && st == MC_OK) {
		set_error(conn, "stmt_close", mysql_error(conn->mysql));
		st = MC_ERR_STMT;
	}

	return st;
}
