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
	    "  blob-put -q SQL -f FILE   把 FILE 的字节写入 SQL 的第一个 ? 参数\n"
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
	    "stdout 只输出数据，行数等摘要在 stderr，因此可以安全地接管道。\n"
	    "each 不打印表头——游标接口回调的是行内容，不含列名；\n"
	    "需要表头时用 sql（或对结果自行拼一行）。\n"
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

static int cmd_sql(mc_conn *conn, const char *sql) {
	mc_rows   set;
	mc_status st = mc_query(conn, sql, &set);
	size_t    i;
	size_t    j;

	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(st));
		return 1;
	}

	/* 表头。列数为 0 时（空结果集）只报告行数，不打印空行。 */
	for (j = 0; j < set.cols; j++) {
		(void)printf("%s%s", (j == 0) ? "" : "\t", set.names[j]);
	}
	if (set.cols > 0) (void)printf("\n");

	for (i = 0; i < set.rows; i++) {
		for (j = 0; j < set.cols; j++) {
			const char *c = MC_CELL(&set, i, j);

			/*
			 * SQL NULL 与空串必须打印成两种东西。都输出空的话，
			 * 一行 "a\t\tc" 无法判断第二列是 NULL 还是 ''。
			 */
			(void)printf("%s%s", (j == 0) ? "" : "\t",
			             (c != NULL) ? c : "NULL");
		}
		(void)printf("\n");
	}

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

	st = mc_query_each(conn, sql, print_row, &state, &delivered);

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

static int cmd_exec(mc_conn *conn, const char *sql) {	unsigned long long affected = 0;
	mc_status          st = mc_exec(conn, sql, &affected);

	if (st != MC_OK) {
		(void)fprintf(stderr, PROG ": %s (%s)\n",
		              mc_error(conn), mc_status_string(st));
		return 1;
	}

	(void)printf("%llu\n", affected);
	return 0;
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
	const char *cmd;
	const char *sql = NULL;
	const char *infile = NULL;
	const char *outfile = NULL;

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

	/* 命令自己的选项：-q / -f / -o / -n，同样手工解析 */
	for (; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "-q") == 0 || strcmp(a, "-f") == 0 ||
		    strcmp(a, "-o") == 0 || strcmp(a, "-n") == 0) {
			if (i + 1 >= argc) {
				(void)fprintf(stderr, PROG ": 选项 %s 缺少参数\n", a);
				return 2;
			}

			if (a[1] == 'q')      sql     = argv[++i];
			else if (a[1] == 'f') infile  = argv[++i];
			else if (a[1] == 'o') outfile = argv[++i];
			else {
				if (parse_limit(argv[++i], &limit) != 0) return 2;
				have_limit = 1;
			}

			continue;
		}

		(void)fprintf(stderr, PROG ": 无法识别的参数 %s\n", a);
		return 2;
	}

	/* ---- 命令与参数组合的静态检查，一律在建连之前完成 ---- */
	if (strcmp(cmd, "ping") == 0) {
		if (sql != NULL || infile != NULL || outfile != NULL) {
			(void)fprintf(stderr, PROG ": ping 不接受 -q / -f / -o\n");
			return 2;
		}
	} else if (strcmp(cmd, "sql") == 0 || strcmp(cmd, "exec") == 0) {
		if (sql == NULL || sql[0] == '\0') {
			(void)fprintf(stderr, PROG ": %s 需要 -q SQL\n", cmd);
			return 2;
		}
	} else if (strcmp(cmd, "each") == 0) {
		if (sql == NULL || sql[0] == '\0') {
			(void)fprintf(stderr, PROG ": each 需要 -q SQL\n");
			return 2;
		}
		if (infile != NULL || outfile != NULL) {
			(void)fprintf(stderr, PROG ": each 不接受 -f / -o\n");
			return 2;
		}
	} else if (strcmp(cmd, "blob-put") == 0) {
		if (sql == NULL || infile == NULL) {
			(void)fprintf(stderr, PROG ": blob-put 需要 -q SQL 与 -f FILE\n");
			return 2;
		}
	} else if (strcmp(cmd, "blob-get") == 0) {
		if (sql == NULL || outfile == NULL) {
			(void)fprintf(stderr, PROG ": blob-get 需要 -q SQL 与 -o FILE\n");
			return 2;
		}
	} else {
		(void)fprintf(stderr, PROG ": 未知命令 %s\n", cmd);
		usage(stderr, argv[0]);
		return 2;
	}

	/*
	 * -n 只对 each 有意义。若允许别的子命令静默忽略它，
	 * "sql -n 10" 这种写法看起来限制了行数，实际一行也没少。
	 */
	if (have_limit && strcmp(cmd, "each") != 0) {
		(void)fprintf(stderr, PROG ": -n 只用于 each\n");
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
			size_t cap = STREAM_BLOB_CAP;

			if (have_size && size_hint > 0) {
				cap = size_hint;
			} else {
				struct stat info;

				/* stat 只用来给缓冲区选一个合理的初值。
				 * 失败不算错误：路径可能是管道，此时用默认上限。 */
				if (stat(infile, &info) == 0 && S_ISREG(info.st_mode) &&
				    info.st_size > 0) {
					cap = (size_t)info.st_size;
				}
			}

			payload = alloc_payload(cap, "blob-put");
			if (payload == NULL) return 1;

			st = mc_file_read(infile, payload, cap, &payload_len);
			if (st != MC_OK) {
				(void)fprintf(stderr, PROG ": %s: %s: %s\n", infile,
				              mc_status_string(st), strerror(errno));
				free(payload);
				return 1;
			}
		}

		st = mc_open(&opt, &conn);
		if (st != MC_OK) {
			/* 建连失败时还没有句柄，错误在模块级的 thread-local 缓冲里 */
			(void)fprintf(stderr, PROG ": %s (%s)\n",
			              mc_error(NULL), mc_status_string(st));
			free(payload);
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
	return rc;
}
