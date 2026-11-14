/*
 * main.c —— 命令行驱动
 *
 * 这一层只做四件事：解析参数、建立连接、把结果排版到终端、决定退出码。
 * 所有数据库操作都在模块里（libmysqlclient.a），驱动只链接静态库，
 * 不重复编译模块源码——"这个模块能被独立链接"因此在每次构建时都被验证一次。
 *
 * 与最初那份单文件程序相比，最大的结构变化是**取消了固定流程**：
 * 原 main 写死"插入 → 查 → 删 → 查 → 传图 → 取图"六步，跑一次之后
 * 库里就多一行、多一张图，再跑一次又多一份，想单独验证某一步是不可能的。
 * 现在每个动作是一个子命令，执行哪一步由使用者决定，因此可以脚本化。
 *
 * 连接参数的来源有三层，优先级从低到高：
 *   1. 编译期默认值：config.h（存在时）或内置的本机默认值
 *   2. 环境变量 MC_HOST / MC_PORT / MC_USER / MC_PASSWORD / MC_DB
 *   3. 命令行选项 -H / -P / -u / -p / -d
 * 分三层不是过度设计：config.h 适合本机长期配置，环境变量适合一次性覆盖
 * （例如连测试库），命令行适合脚本里显式传参。凭据始终可以不进版本库。
 */

#include "mysqlclient.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/*
 * config.h 由使用者从 config.h.example 复制生成，含真实凭据，
 * 被 .gitignore 忽略。它不存在时用下面这组只连本机、不含密码的默认值，
 * 保证从干净工作树执行 make 也能编过。
 */
#ifdef MC_HAVE_CONFIG_H
#include "config.h"
#else
#define KING_DB_SERVER_IP   "127.0.0.1"
#define KING_DB_SERVER_PORT  3306
#define KING_DB_USERNAME    "root"
#define KING_DB_PASSWORD    ""
#define KING_DB_DEFAULTDB   ""
#endif

/* blob-get 的默认缓冲区上限：16 MiB。够放一张照片或一段短视频，
 * 越界时返回的是明确的 TOOBIG 而不是截断，可以按提示用 -s 调大。 */
#define DEFAULT_BLOB_CAP  (16u * 1024u * 1024u)

/* 输入不是普通文件（管道、终端、/dev/stdin）时无法预知长度，
 * 给一个 64 KiB 的默认上限，要用更大的就显式 -s。 */
#define STREAM_BLOB_CAP   (64u * 1024u)

#define PROG "mysqlclient"

/* 退出码：0 成功，1 运行期失败，2 用法错误。
 * 2 与 1 分开，脚本才能区分"参数写错了"与"操作失败了"。 */

static void usage(FILE *out, const char *prog) {
	(void)fprintf(out,
	    "Usage: %s [OPTIONS] COMMAND [COMMAND-OPTIONS]\n"
	    "\n"
	    "Commands:\n"
	    "  ping                      与服务器握一次手（COM_PING）\n"
	    "  sql   -q SQL              执行查询，把结果按制表符分隔打印到 stdout\n"
	    "  each  -q SQL [-n N]       逐行取数并打印，内存占用与行数无关\n"
	    "  exec  -q SQL              执行不返回结果集的语句，打印受影响行数\n"
	    "  exec-params -q SQL -a P   同上，但值走参数绑定而不是拼进 SQL\n"
	    "  stmt  -q SQL [-a P] [-r N]\n"
	    "                            预编译一次、执行 N 次（默认 1），\n"
	    "                            返回结果集的语句按 sql 的格式打印\n"
	    "  blob-put -q SQL -f FILE   把 FILE 的字节写入 SQL 的唯一一个 ? 参数\n"
	    "  blob-get -q SQL -o FILE   把单行单列的查询结果写成 FILE\n"
	    "\n"
	    "Options:\n"
	    "  -H HOST    主机（默认来自 config.h 或 $MC_HOST）\n"
	    "  -P PORT    端口\n"
	    "  -u USER    用户名\n"
	    "  -p PASS    密码（留空表示无密码）\n"
	    "  -d DB      默认库名\n"
	    "  -s BYTES   blob 缓冲区上限，0 表示按文件大小自动\n"
	    "  -h         显示本说明\n"
	    "\n"
	    "exec-params 与 stmt 的 -a TYPE:VALUE 按出现顺序对应 SQL 里的 ?：\n"
	    "  -a int:-42        有符号 64 位整数\n"
	    "  -a uint:18446...  无符号 64 位整数\n"
	    "  -a double:2.5     双精度浮点\n"
	    "  -a text:hello     文本（UTF-8，值里的引号不需要转义）\n"
	    "  -a blob:@photo.jpg  二进制，@ 之后是文件路径\n"
	    "  -a null           SQL NULL\n"
	    "\n"
	    "stdout 只输出数据，行数等摘要在 stderr，因此可以安全地接管道。\n"
	    "sql / each / stmt 的输出格式相同（表头 + 制表符分隔的行），\n"
	    "同一查询用这三个子命令打印出来的字节完全一致。\n"
	    "\n"
	    "Example:\n"
	    "  %s -H 127.0.0.1 -u root sql -q 'SELECT 1'\n"
	    "  %s -u root blob-put -q 'INSERT T(U_IMG) VALUES(?)' -f photo.jpg\n",
	    prog, prog, prog);
}

/*
 * 把 src 拷进 dst 时同时检查长度。
 *
 * 用 snprintf 而不是 strcpy：后者的目标缓冲区容量不在参数里，
 * 一旦源字符串超长，越界写会直接落到相邻字段上（这里是密码字段，
 * 实际后果是"主机名太长把密码覆盖掉"，然后报一条难以理解的认证失败）。
 */
static int copy_field(char *dst, size_t cap, const char *src, const char *what) {
	int n = snprintf(dst, cap, "%s", src);

	if (n < 0 || (size_t)n >= cap) {
		(void)fprintf(stderr, PROG ": %s 过长（上限 %lu 字节）\n",
		              what, (unsigned long)(cap - 1));
		return -1;
	}
	return 0;
}

/* 端口：接受 1..65535，其余一律拒绝 */
static int parse_port(const char *text, unsigned int *out) {
	char         *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(text, &end, 10);

	if (errno != 0 || end == text || *end != '\0' || v == 0 || v > 65535ul) {
		(void)fprintf(stderr, PROG ": 端口必须是 1..65535 之间的整数：%s\n", text);
		return -1;
	}

	*out = (unsigned int)v;
	return 0;
}

static int parse_size(const char *text, size_t *out) {
	char      *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(text, &end, 10);

	if (errno != 0 || end == text || *end != '\0') {
		(void)fprintf(stderr, PROG ": -s 需要一个非负整数：%s\n", text);
		return -1;
	}

	*out = (size_t)v;
	return 0;
}

/* each -n：行数上限。0 表示不限（也是默认值），因此不接受负号与空串 */
static int parse_limit(const char *text, unsigned long long *out) {
	char          *end = NULL;
	unsigned long long v;

	if (text[0] == '-' || text[0] == '\0') {
		(void)fprintf(stderr, PROG ": -n 需要一个非负整数：%s\n", text);
		return -1;
	}

	errno = 0;
	v = strtoull(text, &end, 10);

	if (errno != 0 || end == text || *end != '\0') {
		(void)fprintf(stderr, PROG ": -n 需要一个非负整数：%s\n", text);
		return -1;
	}

	*out = v;
	return 0;
}

/* 环境变量为空或未定义时保持 opt 里已有的值 */static void apply_env(mc_options *opt) {
	const char *v;

	if ((v = getenv("MC_HOST")) != NULL && v[0] != '\0') {
		(void)copy_field(opt->host, MC_HOST_MAX, v, "MC_HOST");
	}
	if ((v = getenv("MC_PORT")) != NULL && v[0] != '\0') {
		(void)parse_port(v, &opt->port);
	}
	if ((v = getenv("MC_USER")) != NULL && v[0] != '\0') {
		(void)copy_field(opt->user, MC_USER_MAX, v, "MC_USER");
	}
	if ((v = getenv("MC_PASSWORD")) != NULL && v[0] != '\0') {
		(void)copy_field(opt->password, MC_PASSWORD_MAX, v, "MC_PASSWORD");
	}
	if ((v = getenv("MC_DB")) != NULL && v[0] != '\0') {
		(void)copy_field(opt->database, MC_DBNAME_MAX, v, "MC_DB");
	}
}

/* ------------------------------------------------------------------
 * exec-params 的参数表
 *
 * -a 可以重复出现，个数由 SQL 里 ? 的个数决定（由库核对）。
 * 容量直接取 argc：每个 -a 至少占两个 argv 元素，参数个数不可能超过它，
 * 于是不需要扩容逻辑，也就没有"翻倍时溢出"这类问题要防。
 *
 * blob 参数需要把文件读进内存，那几块缓冲区记在 owned 里统一释放：
 * mc_param 只保存指针，模块不拷贝数据，所以它们必须活到 exec 返回之后。
 * ------------------------------------------------------------------ */

typedef struct {
	mc_param *items;
	void    **owned;
	size_t    n;
	size_t    owned_n;
} param_list;

static void param_list_free(param_list *list) {
	size_t i;

	for (i = 0; i < list->owned_n; i++) free(list->owned[i]);
	free(list->owned);
	free(list->items);
}

/*
 * 分配一块按上限计的缓冲区。
 *
 * cap 为 0 时不能用 malloc(0)：C11 7.22.3.1 规定它要么返回空指针，
 * 要么返回一个不能解引用的唯一指针，两种结果都不适合后续的读取。
 */
static void *alloc_payload(size_t cap, const char *what) {
	void *p = malloc(cap);

	if (p == NULL) {
		(void)fprintf(stderr, PROG ": %s: 无法分配 %lu 字节\n",
		              what, (unsigned long)cap);
	}
	return p;
}

/* 把整个文件读进一段新分配的缓冲区。cap 为 0 表示按文件大小自动。
 * 成功返回 0 且 *out_buf 由调用方负责 free；失败已打印原因。 */
static int load_file(const char *path, size_t cap, void **out_buf, size_t *out_len) {
	mc_status st;
	void     *buf;

	if (cap == 0) {
		struct stat info;

		/* stat 只用来给缓冲区选一个合理的初值。失败不算错误：
		 * 路径可能是管道，此时用默认上限。 */
		cap = STREAM_BLOB_CAP;
		if (stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0) {
			cap = (size_t)info.st_size;
		}
	}

	buf = alloc_payload(cap, path);
	if (buf == NULL) return -1;

	st = mc_file_read(path, buf, cap, out_len);
	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s: %s: %s\n", path,
		              mc_status_string(st), strerror(errno));
		free(buf);
		return -1;
	}

	*out_buf = buf;
	return 0;
}

/* 比较 TYPE:VALUE 里冒号前的那一段 */
static int key_is(const char *key, size_t keylen, const char *name) {
	return strlen(name) == keylen && strncmp(key, name, keylen) == 0;
}

/*
 * 解析一个 -a TYPE:VALUE。
 *
 * 类型名与 mc_param_kind 一一对应，但只覆盖"命令行能表达"的那部分：
 * 二进制必须从文件来（argv 里放不下 '\0'），所以 blob 的写法是 @路径。
 * 这些都不是库的设计，是驱动的选择——库里 mc_param 只有 kind 与联合体，
 * 值的来源由调用方决定。
 */
static int parse_param(param_list *list, const char *text,
                       int have_cap, size_t cap) {
	mc_param   *p = &list->items[list->n];
	const char *colon = strchr(text, ':');
	const char *value;
	size_t      keylen;

	if (colon == NULL) {
		/* 只有 null 可以省略冒号：其余类型没有"默认值"可言 */
		keylen = strlen(text);
		value  = "";
	} else {
		keylen = (size_t)(colon - text);
		value  = colon + 1;
	}

	if (key_is(text, keylen, "null")) {
		p->kind = MC_PARAM_NULL;
	} else if (key_is(text, keylen, "int")) {
		char      *end = NULL;
		long long  v;

		errno = 0;
		v = strtoll(value, &end, 10);
		if (value[0] == '\0' || errno != 0 || end == value || *end != '\0') {
			(void)fprintf(stderr, PROG ": -a int 需要一个十进制整数：%s\n", text);
			return -1;
		}
		p->kind = MC_PARAM_INT;
		p->v.i  = v;
	} else if (key_is(text, keylen, "uint")) {
		char               *end = NULL;
		unsigned long long  v;

		/* strtoull 接受负号并回绕，这一条必须自己挡：
		 * -a uint:-1 会变成 18446744073709551615，赋给无符号列时
		 * 看起来"成功"了，值却不可能是使用者想要的。 */
		if (value[0] == '-') {
			(void)fprintf(stderr, PROG ": -a uint 不接受负数：%s\n", text);
			return -1;
		}
		errno = 0;
		v = strtoull(value, &end, 10);
		if (value[0] == '\0' || errno != 0 || end == value || *end != '\0') {
			(void)fprintf(stderr, PROG ": -a uint 需要一个非负整数：%s\n", text);
			return -1;
		}
		p->kind = MC_PARAM_UINT;
		p->v.u  = v;
	} else if (key_is(text, keylen, "double")) {
		char   *end = NULL;
		double  v;

		errno = 0;
		v = strtod(value, &end);
		if (value[0] == '\0' || errno != 0 || end == value || *end != '\0') {
			(void)fprintf(stderr, PROG ": -a double 需要一个浮点数：%s\n", text);
			return -1;
		}
		p->kind = MC_PARAM_DOUBLE;
		p->v.d  = v;
	} else if (key_is(text, keylen, "text")) {
		/* 指向 argv，进程活着就有效；含 '\0' 的文本写不出来，
		 * 那是 argv 的限制（C11 5.1.2.2.1：argv 的字符串以 '\0' 结尾），
		 * 二进制内容要走 blob。 */
		p->kind         = MC_PARAM_TEXT;
		p->v.bytes.data = value;
		p->v.bytes.len  = strlen(value);
	} else if (key_is(text, keylen, "blob")) {
		void  *buf = NULL;
		size_t len = 0;

		if (value[0] != '@' || value[1] == '\0') {
			(void)fprintf(stderr, PROG ": -a blob 的写法是 @路径：%s\n", text);
			return -1;
		}
		if (load_file(value + 1, have_cap ? cap : 0, &buf, &len) != 0) return -1;

		list->owned[list->owned_n++] = buf;

		p->kind         = MC_PARAM_BLOB;
		p->v.bytes.data = buf;
		p->v.bytes.len  = len;
	} else {
		(void)fprintf(stderr, PROG ": 未知的参数类型 %.*s"
		              "（可用 int / uint / double / text / blob / null）\n",
		              (int)keylen, text);
		return -1;
	}

	list->n++;
	return 0;
}

/* ------------------------------------------------------------------
 * 子命令
 * ------------------------------------------------------------------ */

static int cmd_ping(mc_conn *conn) {
	mc_status st = mc_ping(conn);

	if (st != MC_OK) {
		/* mc_error 里已经带了出错环节（ping: / query: / prepare: …），
		 * 这里不再重复子命令名，否则会打出 "exec: exec: …"。 */
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(st));
		return 1;
	}

	(void)printf("ok\n");
	return 0;
}

/*
 * 把一个物化结果集打印成"表头 + 制表符分隔的行"。
 *
 * 抽出来是为了让 sql 与 stmt 两个子命令共用同一份排版：
 * 同一查询走文本协议（sql）与走语句对象（stmt）应当逐字节相同，
 * 两份各自拼格式的代码迟早会出现一处改了、另一处没改。
 */
static void print_rows(const mc_rows *set) {
	size_t i;
	size_t j;

	/* 表头。列数为 0 时（空结果集）只报告行数，不打印空行。 */
	for (j = 0; j < set->cols; j++) {
		(void)printf("%s%s", (j == 0) ? "" : "\t", set->names[j]);
	}
	if (set->cols > 0) (void)printf("\n");

	for (i = 0; i < set->rows; i++) {
		for (j = 0; j < set->cols; j++) {
			const char *c = MC_CELL(set, i, j);

			/*
			 * SQL NULL 与空串必须打印成两种东西。都输出空的话，
			 * 一行 "a\t\tc" 无法判断第二列是 NULL 还是 ''。
			 */
			(void)printf("%s%s", (j == 0) ? "" : "\t",
			             (c != NULL) ? c : "NULL");
		}
		(void)printf("\n");
	}
}

static int cmd_sql(mc_conn *conn, const char *sql) {
	mc_rows   set;
	mc_status st = mc_query(conn, sql, &set);

	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(st));
		return 1;
	}

	print_rows(&set);

	/* 摘要走 stderr：stdout 只留数据，才能直接接 cut / awk */
	(void)fprintf(stderr, "%lu 行\n", (unsigned long)set.rows);

	mc_rows_free(&set);
	return 0;
}

/*
 * each 子命令的上下文。params 里放行数上限，rows 由回调累加：
 * 回调的返回值要表示"停不停"，就没法同时把计数带出来，
 * 计数只能放在调用方给的 ctx 里。
 */
typedef struct {
	unsigned long long limit;   /* 0 表示不限 */
	size_t             rows;
} each_state;

/*
 * 表头。列名来自结果集元数据，在交付第一行之前就由库交到这里，
 * 因此表头一定出现在数据之前——这也是列名要单独一个回调的理由：
 * 若等第一行才知道列名，就得先把这一行缓存起来。
 *
 * 格式与 cmd_sql 的表头逐字节相同，两个子命令的输出才能直接 cmp。
 */
static int print_cols(void *ctx, const char *const *names, size_t cols) {
	size_t j;

	(void)ctx;

	for (j = 0; j < cols; j++) {
		/* 契约上 names[j] 不会是 NULL，这里仍然判一次：
		 * printf("%s", NULL) 是未定义行为，代价只有一条比较。 */
		(void)printf("%s%s", (j == 0) ? "" : "\t",
		             (names[j] != NULL) ? names[j] : "");
	}
	if (cols > 0) (void)printf("\n");

	return 0;
}

/*
 * 打印一行并决定是否继续。
 *
 * length 参数在这里刻意不用：打印走 printf("%s")，对文本列足够，
 * 对含 '\0' 的二进制列会截断——与 sql 子命令的行为一致，
 * 也正因为一致，两者的输出可以逐字节比对（见 check_live.sh）。
 * 取二进制请用 blob-get，那里按字节长度写文件。
 */
static int print_row(void *ctx, const char *const *cell,
                     const size_t *length, size_t cols) {
	each_state *state = (each_state *)ctx;
	size_t      j;

	(void)length;

	for (j = 0; j < cols; j++) {
		(void)printf("%s%s", (j == 0) ? "" : "\t",
		             (cell[j] != NULL) ? cell[j] : "NULL");
	}
	(void)printf("\n");

	state->rows++;

	return (state->limit != 0 && (unsigned long long)state->rows >= state->limit)
	       ? 1 : 0;
}

static int cmd_each(mc_conn *conn, const char *sql, unsigned long long limit) {
	each_state state;
	mc_status  st;
	size_t     delivered = 0;

	state.limit = limit;
	state.rows  = 0;

	st = mc_query_each_ex(conn, sql, print_cols, print_row, &state, &delivered);

	if (st != MC_OK) {
		/* 中途失败时 delivered 是"已经打印出去的那些行"，
		 * 报出来才能判断输出文件是完整结果还是半截。 */
		(void)fprintf(stderr, PROG ": %s (%s)，已输出 %lu 行\n",
		              mc_error(conn), mc_status_string(st),
		              (unsigned long)delivered);
		return 1;
	}

	/* 库里交付的行数才是权威值：回调收到的次数与它相等，
	 * 但用库的返回值可以在两者不一致时（理论上不会）暴露接线错误。 */
	(void)fprintf(stderr, "%lu 行\n", (unsigned long)delivered);
	return 0;
}

static int cmd_exec(mc_conn *conn, const char *sql) {
	unsigned long long affected = 0;
	mc_status          st = mc_exec(conn, sql, &affected);

	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(st));
		return 1;
	}

	(void)printf("%llu\n", affected);
	return 0;
}

static int cmd_blob_put(mc_conn *conn, const char *sql,
                        const void *payload, size_t len) {
	mc_status st = mc_blob_write(conn, sql, payload, len);

	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(st));
		return 1;
	}

	(void)fprintf(stderr, "写入 %lu 字节\n", (unsigned long)len);
	return 0;
}

/* exec-params：值走参数绑定，不再拼进 SQL 文本。 */
static int cmd_exec_params(mc_conn *conn, const char *sql,
                           const mc_param *params, size_t n) {
	unsigned long long affected = 0;
	mc_status          st = mc_exec_params(conn, sql, params, n, &affected);

	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(st));
		return 1;
	}

	(void)printf("%llu\n", affected);
	return 0;
}

/*
 * stmt：预编译一次、执行 repeat 次。
 *
 * -r 不是为了省事，而是让"复用"这件事能被脚本与文档观察到：每次执行
 * 的结果都会打印，因此 3 次执行应当出现 3 份表头与 3 份数据；耗时与
 * 行数摘要在 stderr，stdout 仍然只有数据。
 *
 * 与 exec-params 的差别只在"prepare 了几次"：同样的 SQL、同样的参数，
 * 两者写进库里的值必须完全一样，因此它也是校验语句对象没有改变语义的
 * 对照物（check_live.sh 里有一条整份 cmp）。
 */
static int cmd_stmt(mc_conn *conn, const char *sql, const mc_param *params,
                    size_t n, unsigned long long repeat) {

	mc_stmt           *st = NULL;
	mc_status          s;
	unsigned long long i;
	unsigned long long total_affected = 0;
	size_t             total_rows = 0;
	struct timespec    t0;
	struct timespec    t1;
	double             ms;

	if (repeat == 0) repeat = 1;

	s = mc_stmt_prepare(conn, sql, &st);
	if (s != MC_OK) {
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(s));
		return 1;
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &t0);

	for (i = 0; i < repeat; i++) {
		unsigned long long affected = 0;

		s = mc_stmt_execute(st, params, n, &affected);
		if (s != MC_OK) {
			(void)fprintf(stderr, PROG ": 第 %llu 次执行：%s (%s)\n",
			              i + 1, mc_error(conn), mc_status_string(s));
			mc_stmt_close(st);
			return 1;
		}

		total_affected += affected;

		if (mc_stmt_columns(st) > 0) {
			mc_rows set;

			s = mc_stmt_rows(st, &set);
			if (s != MC_OK) {
				(void)fprintf(stderr, PROG ": 第 %llu 次取数：%s (%s)\n",
				              i + 1, mc_error(conn), mc_status_string(s));
				mc_stmt_close(st);
				return 1;
			}

			print_rows(&set);
			total_rows += set.rows;
			mc_rows_free(&set);
		}
	}

	(void)clock_gettime(CLOCK_MONOTONIC, &t1);

	/* 两个时钟域相减要按"秒差 + 纳秒差"合成：tv_nsec 的差可以是负数，
	 * 单独取绝对值会算错跨越整秒的那一次。 */
	ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
	     (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;

	(void)fprintf(stderr,
	              "%llu 次执行，%lu 行，影响 %llu 行，耗时 %.3f ms\n",
	              repeat, (unsigned long)total_rows, total_affected, ms);

	mc_stmt_close(st);
	return 0;
}

static int cmd_blob_get(mc_conn *conn, const char *sql,
                        const char *path, size_t cap) {
	void     *buf = alloc_payload(cap, "blob-get");
	size_t    len = 0;
	mc_status st;

	if (buf == NULL) return 1;

	st = mc_blob_read(conn, sql, buf, cap, &len);
	if (st != MC_OK) {
		/*
		 * 容量不足时给出具体建议。只说"缓冲区太小"等于让使用者
		 * 自己去猜该给多大，而库刚刚已经知道确切的字节数了。
		 */
		if (st == MC_ERR_TOOBIG) {
			(void)fprintf(stderr,
			    PROG ": %s（用 -s 指定更大的上限，当前 %lu 字节）\n",
			    mc_error(conn), (unsigned long)cap);
		} else {
			(void)fprintf(stderr, PROG ": %s (%s)\n",
			              mc_error(conn), mc_status_string(st));
		}
		free(buf);
		return 1;
	}

	st = mc_file_write(path, buf, len);
	free(buf);

	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s: %s: %s\n",
		              path, mc_status_string(st), strerror(errno));
		return 1;
	}

	(void)fprintf(stderr, "写出 %lu 字节\n", (unsigned long)len);
	return 0;
}

/* ------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {

	mc_options opt;
	mc_conn   *conn = NULL;
	mc_status  st;
	int        i = 1;
	int        rc = 1;
	int        have_size = 0;
	size_t     size_hint = 0;
	int        have_limit = 0;
	unsigned long long limit = 0;
	int        have_repeat = 0;
	unsigned long long repeat = 0;
	param_list plist;
	const char *cmd;
	const char *sql = NULL;
	const char *infile = NULL;
	const char *outfile = NULL;

	/*
	 * 参数表按需分配：不用 exec-params 的调用不付这份内存，
	 * 也不会在 -h 之类的路径上留下待释放的块。
	 */
	plist.n       = 0;
	plist.owned_n = 0;
	plist.items   = NULL;
	plist.owned   = NULL;

	mc_options_init(&opt);
	(void)copy_field(opt.host, MC_HOST_MAX, KING_DB_SERVER_IP, "host");
	(void)copy_field(opt.user, MC_USER_MAX, KING_DB_USERNAME, "user");
	(void)copy_field(opt.password, MC_PASSWORD_MAX, KING_DB_PASSWORD, "password");
	(void)copy_field(opt.database, MC_DBNAME_MAX, KING_DB_DEFAULTDB, "database");
	opt.port = (unsigned int)KING_DB_SERVER_PORT;

	apply_env(&opt);

	/*
	 * 全局选项只出现在命令之前，手工解析而不借助 getopt：
	 * GNU getopt 会重排 argv（把非选项参数挪到后面），
	 * 于是 "sql -q X" 里的 -q 会被当成全局选项吃掉，
	 * 命令自己的参数就再也分不清是给谁的。
	 */
	for (; i < argc; i++) {
		const char *a = argv[i];
		const char *v;

		if (a[0] != '-' || a[1] == '\0') break;          /* 命令名 */
		if (strcmp(a, "--") == 0) { i++; break; }

		if (strcmp(a, "-h") == 0) {
			usage(stdout, argv[0]);
			return 0;
		}

		/* 需要取值的选项：先确认后面还有参数 */
		if (strcmp(a, "-H") != 0 && strcmp(a, "-P") != 0 &&
		    strcmp(a, "-u") != 0 && strcmp(a, "-p") != 0 &&
		    strcmp(a, "-d") != 0 && strcmp(a, "-s") != 0) {
			(void)fprintf(stderr, PROG ": 未知选项 %s\n", a);
			usage(stderr, argv[0]);
			return 2;
		}

		if (i + 1 >= argc) {
			(void)fprintf(stderr, PROG ": 选项 %s 缺少参数\n", a);
			return 2;
		}

		v = argv[++i];

		if (strcmp(a, "-H") == 0) {
			if (copy_field(opt.host, MC_HOST_MAX, v, "主机名") != 0) return 2;
		} else if (strcmp(a, "-P") == 0) {
			if (parse_port(v, &opt.port) != 0) return 2;
		} else if (strcmp(a, "-u") == 0) {
			if (copy_field(opt.user, MC_USER_MAX, v, "用户名") != 0) return 2;
		} else if (strcmp(a, "-p") == 0) {
			if (copy_field(opt.password, MC_PASSWORD_MAX, v, "密码") != 0) return 2;
		} else if (strcmp(a, "-d") == 0) {
			if (copy_field(opt.database, MC_DBNAME_MAX, v, "库名") != 0) return 2;
		} else { /* -s */
			if (parse_size(v, &size_hint) != 0) return 2;
			have_size = 1;
		}
	}

	if (i >= argc) {
		(void)fprintf(stderr, PROG ": 缺少命令\n");
		usage(stderr, argv[0]);
		return 2;
	}

	cmd = argv[i++];

	/* 命令自己的选项：-q / -f / -o / -n / -a，同样手工解析 */
	for (; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "-q") == 0 || strcmp(a, "-f") == 0 ||
		    strcmp(a, "-o") == 0 || strcmp(a, "-n") == 0 ||
		    strcmp(a, "-r") == 0 || strcmp(a, "-a") == 0) {
			if (i + 1 >= argc) {
				(void)fprintf(stderr, PROG ": 选项 %s 缺少参数\n", a);
				param_list_free(&plist);
				return 2;
			}

			if (a[1] == 'q')      sql     = argv[++i];
			else if (a[1] == 'f') infile  = argv[++i];
			else if (a[1] == 'o') outfile = argv[++i];
			else if (a[1] == 'a') {
				/*
				 * 容量取 argc：每个 -a 至少占两个 argv 元素，
				 * 参数个数不可能超过它，因此不需要扩容逻辑，
				 * 也就没有"翻倍时溢出"这类问题要防。
				 */
				if (plist.items == NULL) {
					plist.items = (mc_param *)calloc((size_t)argc,
					                                 sizeof *plist.items);
					plist.owned = (void **)calloc((size_t)argc,
					                              sizeof *plist.owned);
					if (plist.items == NULL || plist.owned == NULL) {
						(void)fprintf(stderr, PROG ": 无法分配参数表\n");
						param_list_free(&plist);
						return 1;
					}
				}
				if (parse_param(&plist, argv[++i], have_size, size_hint) != 0) {
					param_list_free(&plist);
					return 2;
				}
			} else {
				unsigned long long v;

				if (parse_limit(argv[++i], &v) != 0) {
					param_list_free(&plist);
					return 2;
				}

				if (a[1] == 'n') {
					limit      = v;
					have_limit = 1;
				} else {            /* -r：执行次数 */
					repeat      = v;
					have_repeat = 1;
				}
			}

			continue;
		}

		(void)fprintf(stderr, PROG ": 无法识别的参数 %s\n", a);
		param_list_free(&plist);
		return 2;
	}

	/* ---- 命令与参数组合的静态检查，一律在建连之前完成 ---- */
	if (strcmp(cmd, "ping") == 0) {
		if (sql != NULL || infile != NULL || outfile != NULL) {
			(void)fprintf(stderr, PROG ": ping 不接受 -q / -f / -o\n");
			param_list_free(&plist);
			return 2;
		}
	} else if (strcmp(cmd, "sql") == 0 || strcmp(cmd, "exec") == 0) {
		if (sql == NULL || sql[0] == '\0') {
			(void)fprintf(stderr, PROG ": %s 需要 -q SQL\n", cmd);
			param_list_free(&plist);
			return 2;
		}
	} else if (strcmp(cmd, "each") == 0) {
		if (sql == NULL || sql[0] == '\0') {
			(void)fprintf(stderr, PROG ": each 需要 -q SQL\n");
			param_list_free(&plist);
			return 2;
		}
		if (infile != NULL || outfile != NULL) {
			(void)fprintf(stderr, PROG ": each 不接受 -f / -o\n");
			param_list_free(&plist);
			return 2;
		}
	} else if (strcmp(cmd, "exec-params") == 0) {
		if (sql == NULL || sql[0] == '\0') {
			(void)fprintf(stderr, PROG ": exec-params 需要 -q SQL\n");
			param_list_free(&plist);
			return 2;
		}
		if (infile != NULL || outfile != NULL) {
			(void)fprintf(stderr, PROG ": exec-params 不接受 -f / -o\n");
			param_list_free(&plist);
			return 2;
		}
	} else if (strcmp(cmd, "stmt") == 0) {
		if (sql == NULL || sql[0] == '\0') {
			(void)fprintf(stderr, PROG ": stmt 需要 -q SQL\n");
			param_list_free(&plist);
			return 2;
		}
		if (infile != NULL || outfile != NULL) {
			(void)fprintf(stderr, PROG ": stmt 不接受 -f / -o\n");
			param_list_free(&plist);
			return 2;
		}
	} else if (strcmp(cmd, "blob-put") == 0) {
		if (sql == NULL || infile == NULL) {
			(void)fprintf(stderr, PROG ": blob-put 需要 -q SQL 与 -f FILE\n");
			param_list_free(&plist);
			return 2;
		}
	} else if (strcmp(cmd, "blob-get") == 0) {
		if (sql == NULL || outfile == NULL) {
			(void)fprintf(stderr, PROG ": blob-get 需要 -q SQL 与 -o FILE\n");
			param_list_free(&plist);
			return 2;
		}
	} else {
		(void)fprintf(stderr, PROG ": 未知命令 %s\n", cmd);
		usage(stderr, argv[0]);
		param_list_free(&plist);
		return 2;
	}

	/*
	 * -n 只对 each 有意义。若允许别的子命令静默忽略它，
	 * "sql -n 10" 这种写法看起来限制了行数，实际一行也没少。
	 * -a 同理：它不是通用选项，静默忽略等于让使用者以为值绑上了。
	 * -r 只对 stmt 有意义，理由相同。
	 */
	if (have_limit && strcmp(cmd, "each") != 0) {
		(void)fprintf(stderr, PROG ": -n 只用于 each\n");
		param_list_free(&plist);
		return 2;
	}

	if (have_repeat && strcmp(cmd, "stmt") != 0) {
		(void)fprintf(stderr, PROG ": -r 只用于 stmt\n");
		param_list_free(&plist);
		return 2;
	}

	if (plist.n > 0 && strcmp(cmd, "exec-params") != 0 &&
	    strcmp(cmd, "stmt") != 0) {
		(void)fprintf(stderr, PROG ": -a 只用于 exec-params 与 stmt\n");
		param_list_free(&plist);
		return 2;
	}

	/*
	 * blob-put 先读文件、后建连。
	 * 反过来做的话，路径写错这种错误要等一次完整的 TCP 握手之后才出现，
	 * 而它跟服务器一点关系都没有。
	 */
	{
		void  *payload = NULL;
		size_t payload_len = 0;

		if (strcmp(cmd, "blob-put") == 0) {
			if (load_file(infile, have_size ? size_hint : 0,
			              &payload, &payload_len) != 0) {
				param_list_free(&plist);
				return 1;
			}
		}

		st = mc_open(&opt, &conn);
		if (st != MC_OK) {
			/* 建连失败时还没有句柄，错误在模块级的 thread-local 缓冲里 */
			(void)fprintf(stderr, PROG ": %s (%s)\n",
			              mc_error(NULL), mc_status_string(st));
			free(payload);
			param_list_free(&plist);
			return 1;
		}

		if (strcmp(cmd, "ping") == 0) {
			rc = cmd_ping(conn);
		} else if (strcmp(cmd, "sql") == 0) {
			rc = cmd_sql(conn, sql);
		} else if (strcmp(cmd, "each") == 0) {
			rc = cmd_each(conn, sql, have_limit ? limit : 0);
		} else if (strcmp(cmd, "exec") == 0) {
			rc = cmd_exec(conn, sql);
		} else if (strcmp(cmd, "exec-params") == 0) {
			rc = cmd_exec_params(conn, sql, plist.items, plist.n);
		} else if (strcmp(cmd, "stmt") == 0) {
			rc = cmd_stmt(conn, sql, plist.items, plist.n,
			              have_repeat ? repeat : 1);
		} else if (strcmp(cmd, "blob-put") == 0) {
			rc = cmd_blob_put(conn, sql, payload, payload_len);
		} else {
			size_t cap = have_size ? size_hint : (size_t)DEFAULT_BLOB_CAP;

			if (cap == 0) cap = (size_t)DEFAULT_BLOB_CAP;
			rc = cmd_blob_get(conn, sql, outfile, cap);
		}

		free(payload);
	}

	mc_close(conn);
	param_list_free(&plist);
	return rc;
}
