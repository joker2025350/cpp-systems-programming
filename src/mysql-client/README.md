# 03 MySQL C API（mysqlclient 模块）

用 MySQL 官方 C 客户端库（`libmysqlclient`）访问数据库的一个可复用模块，
外加一个建立在它之上的命令行驱动。覆盖建连、查询、参数化语句、
流式取数，以及二进制大对象（BLOB）的写入与读回。

模块的目标不只是"能跑通一段示例"，而是把下面五件事做成可依赖的性质：

1. **调用方不需要 `#include <mysql.h>`。** 接口层只出现 `mc_conn`（不透明句柄）、
   `mc_status`、`mc_rows` 这些自有类型，客户端库的版本差异被关在一个编译单元里。
2. **每个接口都有明确的失败语义。** 十一个状态码，文件类与数据库类分开；
   驱动层据此决定退出码与提示文案，而不是一律 `printf` 一句错误就返回 0。
3. **二进制数据按字节搬运。** 不经过 `strlen`、不经过字符集转换，
   写入与读回的内容逐字节相同。
4. **容量不足时明确失败，绝不截断后返回成功。** 这是 BLOB 处理里最常见、
   也最难发现的错误。
5. **结果集可以选择不驻留内存。** `mc_query` 全量取回，`mc_query_each`
   逐行交付，峰值内存只与最宽的一行有关。100 万行实测 10.7 MiB 对 161.9 MiB。

```bash
$ ./bin/mysqlclient -H 127.0.0.1 -u root sql -q 'SELECT id, name FROM T'
id	name
1	king
2	睡蓝蕉
2 行
```

`stdout` 只有数据，行数摘要在 `stderr`，因此可以直接接 `cut` / `awk`。

---

## 1 目录结构

```
src/mysql-client/
├── include/
│   └── mysqlclient.h     对外接口：状态码、连接、结果集、参数绑定、BLOB、文件辅助
├── src/
│   ├── mysqlclient.c     数据库访问层（唯一 Include <mysql.h> 的编译单元）
│   ├── options.c         连接参数：初始化与校验
│   ├── file.c            文件读写（不访问数据库）
│   ├── status.c          状态码 → 可读描述
│   └── main.c            命令行驱动（子命令、参数解析、输出排版、退出码）
├── tests/
│   ├── test_pure.c       纯逻辑单元测试，99 项，不链接 libmysqlclient
│   ├── test_db.c         接口层测试，23 项，含真实建连失败路径
│   ├── check_cli.sh      命令行端到端检查，41 项，不需要数据库
│   ├── check_live.sh     真实数据库往返验证，96 项，需要数据库
│   ├── test_params.c     参数绑定，65 项，需要数据库
│   ├── test_stream.c     流式取数，正确性 56 项 / 内存测量，需要数据库
│   ├── test_stmt.c       语句对象，100 项，需要数据库
│   ├── check_scale.sh    百万行下的峰值内存对照，需要数据库
│   └── bench_file.c      文件读写吞吐测量
├── config.h.example      连接参数模板（config.h 不入库）
├── Makefile
└── README.md
```

编译产物统一落在 `bin/`，已被根目录的 `.gitignore` 忽略。

**为什么 `src/` 分成四个实现文件**，而不是一个 `mysqlclient.c`：
`file.c`、`options.c`、`status.c` 完全不引用 `mysql_*` 符号。
把它们分开，链接器按需抽取静态库成员时就不会把 `mysqlclient.o` 拉进来，
于是 `test_pure` 可以在**没有装 MySQL 客户端库的机器上**编译并运行。
这不是洁癖，它决定了"能不能在这个仓库的任意一台机器上跑一半的测试"。

驱动层（`main.c`）只链接 `libmysqlclient.a`，不重复编译模块源码。
"这个模块能被独立链接使用"因此在每次构建时都被动验证一遍。

---

## 2 对外接口

### 2.1 状态码

```c
typedef enum {
    MC_OK             = 0,   /* 成功 */
    MC_ERR_PARAM      = 1,   /* 参数非法：NULL、空串、端口越界 */
    MC_ERR_NOMEM      = 2,
    MC_ERR_FILE_OPEN  = 3,   /* 保留 errno */
    MC_ERR_FILE_READ  = 4,
    MC_ERR_FILE_WRITE = 5,
    MC_ERR_CONNECT    = 6,   /* 建连或握手失败 */
    MC_ERR_QUERY      = 7,   /* SQL 执行失败 */
    MC_ERR_RESULT     = 8,   /* 结果集的行列数与接口约定不符 */
    MC_ERR_STMT       = 9,   /* 语句句柄的创建、绑定、取数失败 */
    MC_ERR_TOOBIG     = 10,  /* 数据超出调用方给出的缓冲区容量 */
    MC_ERR_NODATA     = 11   /* 期望有一行，实际没有（或该列为 SQL NULL） */
} mc_status;
```

`MC_OK` 固定为 0，因此可以直接写 `if (st)`。

文件类错误（3–5）与数据库类错误（6–9）分开编号，是因为驱动层要给出不同的提示：
`cannot open file` 与 `cannot connect` 的处置方式完全不同，
合并成一个笼统的失败码会让使用者无从下手。

`MC_ERR_TOOBIG` 单独存在是这套接口最重要的约定：容量不足时**报错**，
不截断。一个"成功返回了前 64 KB"的函数看起来完全正常，
错误要等到把图片打开、发现下半截是花屏时才暴露。

### 2.2 连接

```c
void      mc_options_init(mc_options *opt);      /* 置空，端口 3306 */
mc_status mc_options_check(const mc_options *opt);
mc_status mc_open(const mc_options *opt, mc_conn **out);
void      mc_close(mc_conn *conn);               /* 传 NULL 安全 */
const char *mc_error(const mc_conn *conn);       /* 不返回 NULL */
mc_status mc_ping(mc_conn *conn);
```

`mc_conn` 只有声明没有定义：

```c
typedef struct mc_conn mc_conn;   /* 不透明 */
```

调用方只能在栈上放一个指针，`sizeof(mc_conn)` 在模块之外不可知。

`mc_options` 按值拷贝全部参数（含密码），模块不持有调用方的指针，
因此 `opt` 可以放在栈上、随作用域结束。`connect_timeout` 为 0 表示用库的默认值。

### 2.3 结果集

```c
typedef struct {
    size_t  rows;
    size_t  cols;
    char  **names;    /* cols 个 */
    char  **cell;     /* rows * cols 个，行优先 */
    size_t *length;   /* rows * cols 个，字节长度 */
    char   *arena;    /* 内部使用 */
    char   *name_arena;
} mc_rows;

mc_status mc_query(mc_conn *conn, const char *sql, mc_rows *out);
void      mc_rows_free(mc_rows *rows);
mc_status mc_exec(mc_conn *conn, const char *sql, unsigned long long *affected);
```

单元格访问是 `cell[row * cols + col]`（有 `MC_CELL(rset, i, j)` 这个宏）。

两条性质必须记住：

- `cell[k] == NULL` 表示 **SQL NULL**；`cell[k]` 指向 `""` 表示空串。
  两者不是一回事，接口也不把它们合成一个。
- `length[k]` 是真实字节数。`cell[k]` 虽然以 `'\0'` 结尾（便于直接 `printf`），
  但 BLOB 列的字节里可以含 `'\0'`，此时 `strlen(cell[k]) < length[k]`。

`mc_query` 把整个结果集取回内存，成功后必须 `mc_rows_free`。
`mc_exec` 用于不返回结果集的语句，`affected` 可为 NULL。

### 2.4 流式取数（游标）

```c
typedef int (*mc_row_fn)(void *ctx, const char *const *cell,
                         const size_t *length, size_t cols);
typedef int (*mc_cols_fn)(void *ctx, const char *const *names, size_t cols);

mc_status mc_query_each(mc_conn *conn, const char *sql,
                        mc_row_fn fn, void *ctx, size_t *out_rows);

mc_status mc_query_each_ex(mc_conn *conn, const char *sql,
                           mc_cols_fn on_cols, mc_row_fn fn, void *ctx,
                           size_t *out_rows);
```

回调的 `cell` / `length` 与 `mc_rows` 里的同名成员语义完全一致
（`cell[j] == NULL` 是 SQL NULL，`length[j]` 是真实字节数，`cell[j][length[j]]` 是 `'\0'`），
区别只在**生命周期**：这片内存在回调返回后就被下一行覆盖，不能保存指针。

回调返回 0 继续，返回非 0 停止。停止有两种来源，返回值必须分开：

| 情况 | 函数返回 | `*out_rows` |
|---|---|---|
| 取完 | `MC_OK` | 结果集行数 |
| 回调返回非 0（主动停止） | `MC_OK` | 已交付的行数（含最后一行） |
| 取数中途失败 | `MC_ERR_STMT` / `MC_ERR_QUERY` / `MC_ERR_NOMEM` | 失败前已交付的行数 |
| 语句不返回结果集 | `MC_ERR_RESULT` | 0 |

"主动停止"不能被表示成错误：`each -n 1` 只要一行是正常用法。
`*out_rows` 在失败的路径上也写入，因为已经交付的行回调确实处理过了，
续算的起点要靠它。

列名走另一个回调。`mc_query_each_ex` 的 `on_cols` 在**交付第一行之前**被调用一次，
`names[j]` 与 `cell[j]` 同序、非 NULL、以 `'\0'` 结尾；`on_cols` 返回非 0 表示
调用方不需要这些行，函数随即返回 `MC_OK`、`*out_rows` 为 0。三条契约：

- **只要语句执行成功就调用，与结果集有几行无关**。0 行的结果集也有列名——
  它是服务器元数据的一部分，不是从行里推出来的。
- **`names` 与其中的字符串在整个调用期间有效**，行回调里可以继续读。
  实现从元数据里拷了一份出来，元数据对象随即释放，因此这份有效性不依赖
  `libmysqlclient` 的任何对象还活着。
- 语句执行失败时一次都不调用。语法错、表不存在、权限不足都发生在 `on_cols`
  之前，调用方看到一个回调都没发生的失败，就不会误以为表头已经打出去了。

`on_cols` 传 NULL 时行为与 `mc_query_each` 完全相同（连这次拷贝都不做）。

两条约束：

- 取数期间这条连接被该语句占用。回调里对同一个 `conn` 再发查询，
  `libmysqlclient` 会报 `Commands out of sync`——协议是半双工的，
  结果集没读完就发下一条命令属于违例。要并发就用另一条连接。
- 不能回退。读过的行不留在任何地方，要两遍遍历只能用 `mc_query`。

### 2.5 参数绑定

```c
typedef enum {
    MC_PARAM_NULL = 0, MC_PARAM_INT = 1, MC_PARAM_UINT = 2,
    MC_PARAM_DOUBLE = 3, MC_PARAM_TEXT = 4, MC_PARAM_BLOB = 5
} mc_param_kind;

typedef struct {
    mc_param_kind kind;
    union {
        long long           i;
        unsigned long long  u;
        double              d;
        struct { const void *data; size_t len; } bytes;
    } v;
} mc_param;

mc_status mc_exec_params(mc_conn *conn, const char *sql,
                         const mc_param *params, size_t n,
                         unsigned long long *affected);
```

`params[i]` 依次对应 `sql` 里第 `i` 个 `?`。个数必须与占位符个数相等，
否则返回 `MC_ERR_PARAM` 并在 `mc_error` 里给出两边的数字。

调用方只填 `kind` 与对应的联合体成员：

```c
mc_param p[2] = {
    { .kind = MC_PARAM_INT,  .v.i = 42 },
    { .kind = MC_PARAM_TEXT, .v.bytes = { "张三", 6 } },
};
if (mc_exec_params(conn, "INSERT INTO t(a,b) VALUES(?,?)", p, 2, NULL) != MC_OK)
        puts(mc_error(conn));
```

`data` 不拷贝，指针必须活到函数返回。`len` 超过 `MC_BLOB_CHUNK` 的
`TEXT` / `BLOB` 参数改走 `send_long_data` 分块发送（3.5、3.16）。

`mc_blob_write` 现在是 `n == 1` 的 `MC_PARAM_BLOB` 包装，接口与行为保持兼容。

### 2.6 语句对象

```c
typedef struct mc_stmt mc_stmt;

mc_status mc_stmt_prepare(mc_conn *conn, const char *sql, mc_stmt **out);
size_t    mc_stmt_param_count(const mc_stmt *st);
size_t    mc_stmt_columns(const mc_stmt *st);
mc_status mc_stmt_execute(mc_stmt *st, const mc_param *params, size_t n,
                          unsigned long long *affected);
mc_status mc_stmt_rows(mc_stmt *st, mc_rows *out);
mc_status mc_stmt_each(mc_stmt *st, mc_cols_fn on_cols, mc_row_fn fn,
                       void *ctx, size_t *out_rows);
void      mc_stmt_close(mc_stmt *st);
```

术语：**预编译语句**（prepared statement）是服务器端保存的一条已编译语句。
准备（prepare）是一次往返——客户端发 `COM_STMT_PREPARE`，服务器解析语句、
生成执行计划，回一个语句编号与"这条语句返回什么形状"的元数据；
此后每次执行只发语句编号与参数值，SQL 文本不再出现在报文里，服务器也不再解析。

2.5 的那些接口都是"prepare → execute → close"一步到位。一问一答时这没有浪费，
但同一条语句要执行很多次时（批量写入、按主键逐条查询），每次多付一次往返
与一次解析。`mc_stmt` 把三步拆开：

```c
mc_stmt *st;
mc_param p[2] = { { .kind = MC_PARAM_INT,  .v.i = 1 },
                  { .kind = MC_PARAM_TEXT, .v.bytes = { "x", 1 } } };

if (mc_stmt_prepare(conn, "SELECT a FROM t WHERE id=? AND s=?", &st) != MC_OK) {
        puts(mc_error(conn));
} else {
        mc_rows set;

        if (mc_stmt_execute(st, p, 2, NULL) == MC_OK &&
            mc_stmt_rows(st, &set) == MC_OK) {
                /* 形状与 mc_query 的 mc_rows 完全一致 */
                mc_rows_free(&set);
        }
        mc_stmt_close(st);
}
```

七条契约（完整版在头文件）：

- **对象借用连接。** `conn` 必须活得比 `st` 长，顺序是先 `mc_stmt_close`
  再 `mc_close`——`mysql_close` 之后语句句柄失效。
- **参数在每次 execute 时给出**，对象不保存参数缓冲区，execute 返回后即可释放。
  个数必须等于 `mc_stmt_param_count()`，否则在动网络之前返回 `MC_ERR_PARAM`。
- **允许返回结果集**，这与 `mc_exec_params` 相反：后者把"语句返回行"当错误，
  因为它的职责只是执行写语句；语句对象在 prepare 时就知道形状
  （`mc_stmt_columns`，0 表示不返回行）。
- **结果集只能取一次。** 未 execute 就取、取过之后再取，都返回 `MC_ERR_STATE`
  而不是给一个"0 行"——后者会与"查询确实没有行"混淆。要给同一个结果两次，
  就再 execute 一次：语句已经编译好，代价只有一次往返。
- **上一次没读完就再 execute 是允许的**，实现先把剩余行读掉丢弃（3.17）。
- **执行失败不毁掉对象。** 主键冲突之后再 execute 一次正常参数仍然成功——
  批量写入要的正是这个性质：单行失败不该让整批作废。
- **`affected` 只在语句不返回结果集时被写入。** 返回结果集时不改写它：
  那种语句的"受影响行数"在服务端是行数，与"交付了几行"含义不同，共用一个出参
  会让人以为两者可以互换。

### 2.7 大字段

```c
mc_status mc_blob_write(mc_conn *conn, const char *sql,
                        const void *data, size_t len);
mc_status mc_blob_read(mc_conn *conn, const char *sql,
                       void *buf, size_t cap, size_t *out_len);
```

`mc_blob_write` 把 `data[0..len)` 绑到 `sql` 的**唯一**一个 `?` 上，分块发送。
`sql` 里出现多个占位符时返回 `MC_ERR_PARAM`：包装成 `mc_exec_params` 之后
"占位符个数与参数个数必须相等"这条对它同样生效（3.16 说明了这条检查的由来）。

`mc_blob_read` 的契约是三条，缺一不可，违反时返回 `MC_ERR_RESULT`：

- 结果集恰好**一列**；
- 结果集恰好**一行**（多行返回 `MC_ERR_RESULT`，零行返回 `MC_ERR_NODATA`）；
- 该列不是 SQL NULL（NULL 也返回 `MC_ERR_NODATA`）。

比"取到几行算几行"严格，是因为被覆盖的那几行不会留下任何痕迹：
循环体每轮都从头写同一块缓冲区，最后只剩下最后一行的内容，而函数看起来是成功的。

### 2.8 文件辅助

```c
mc_status mc_file_read(const char *path, void *buf, size_t cap, size_t *out_len);
mc_status mc_file_write(const char *path, const void *buf, size_t len);
```

它们不访问数据库，与 BLOB 的两端一一对应，因此放在同一个模块里。
两个都是"整块读写 + 容量检查"，并且都能被不链接数据库的测试覆盖。

`mc_file_write` 走"先写 `path.tmp`、再 `rename` 覆盖"，
因此要求**目录可写**而不只是目标文件可写，且 `path` 必须是普通文件路径
（管道、`/dev/stdout` 不行，对它们 `rename` 只会失败）。

覆盖已有文件时，**目标文件原有的 `0777` 权限位被复制到新文件上**；
新建文件时权限仍是 `0666 & ~umask`（与直接创建文件一致）。
`setuid` / `setgid` 位不复制，理由见 3.14。属主、属组、ACL、
扩展属性都不保留，见 8.9。

```bash
$ ls -l export.bin          # 0600
$ ./bin/mysqlclient blob-get -q 'SELECT u_img FROM T WHERE id=1' -o export.bin
$ ls -l export.bin          # 仍然是 0600
```

### 2.9 驱动层

```
Usage: mysqlclient [OPTIONS] COMMAND [COMMAND-OPTIONS]

Commands:
  ping                      与服务器握一次手（COM_PING）
  sql   -q SQL              执行查询，结果按制表符分隔打印到 stdout
  each  -q SQL [-n N]       逐行取数并打印，内存占用与行数无关
  exec  -q SQL              执行不返回结果集的语句，打印受影响行数
  exec-params -q SQL -a P   同上，值走参数绑定（-a 可重复）
  stmt  -q SQL [-a P] [-r N] 预编译一次、执行 N 次（默认 1）
  blob-put -q SQL -f FILE   把 FILE 的字节写入 SQL 的唯一一个 ? 参数
  blob-get -q SQL -o FILE   把单行单列的查询结果写成 FILE

Options:
  -H HOST  -P PORT  -u USER  -p PASS  -d DB  -s BYTES  -h

exec-params 与 stmt 的 -a TYPE:VALUE 按出现顺序对应 SQL 里的 ?：
  -a int:-42  -a uint:18446744073709551615  -a double:2.5
  -a text:hello  -a blob:@photo.jpg  -a null
```

退出码三档：`0` 成功，`1` 运行期失败，`2` 用法错误。
分成两档（而不是"非 0 即失败"）是为了让脚本能区分"参数写错了"与"操作失败了"。

`exec-params` 与 `exec` 的区别只在值的传递方式：前者把值当参数交给服务器，
后者把值拼进 SQL 文本。前者的值里出现单引号不需要写成两个，
`check_live.sh` 用同一份内容分别走这两条路，验证写入结果逐字相同。
`-a` 只对 `exec-params` 与 `stmt` 有效，用在别的子命令上直接报用法错误——
静默忽略会让使用者以为值绑上去了，而语句里其实还留着未绑定的 `?`。

`stmt` 与 `exec-params` 的区别只在 prepare 的次数：同一个二进制、同一份 SQL、
同样的参数，两者写进库里的值必须完全一样。`-r N` 让"复用"这件事可被脚本观察：
每次执行的结果都会打印，因此 `stmt -q 'SELECT ?' -a int:9 -r 3` 的输出是
三份"表头 + 一行"；stderr 给出执行次数、行数、受影响行数与总耗时。

`sql` / `each` / `stmt` 的输出格式相同：都先打一行表头（列名，制表符分隔），
再打数据行。因此同一查询经三个子命令打印出来的**字节完全一致**，`check_live.sh`
直接用 `cmp` 比对整份输出——`stmt` 走的是二进制协议、逐行读，
一旦它在 NULL、空串、表头或列名上与文本协议有一点不同，`cmp` 就会红。
列名由 `mc_query_each_ex` 的列名回调在交付第一行之前给出，0 行的结果集同样打表头
——表头来自元数据，与有没有行无关。
`-n` 只对 `each` 有效、`-r` 只对 `stmt` 有效，别的子命令用了它们直接报用法错误——
静默忽略会让 `sql -n 10` 看起来限制了行数，实际一行也没少。

---

## 3 设计要点

### 3.1 不透明句柄：把版本差异关在一个文件里

`mysql.h` 会把 `MYSQL`、`MYSQL_BIND`、`my_bool` 这些名字塞进全局命名空间，
而它们的定义随版本变化。一个具体的例子：`MYSQL_BIND::is_null` 的类型
在 8.0.1 从 `my_bool *` 改成了 `bool *`。

调用方一旦 `#include <mysql.h>`，就绑定了具体的库版本：库升级后调用方必须重编，
而重编时发现的错误与它自己的代码毫无关系。

本模块的处理是把 `MYSQL *` 藏进 `struct mc_conn`，头文件里不出现任何 MySQL 类型。
`my_bool` / `bool` 的差异只影响 `src/mysqlclient.c` 一个文件。

代价是本模块声明的最低版本是 **MySQL 8.0**（`bool *`）。要同时兼容 5.7
就得在实现里写版本条件编译；5.7 已于 2023 年 10 月结束支持，不值得。

### 3.2 错误信息存在哪里

三处，各有理由：

| 场景 | 存放位置 | 理由 |
|---|---|---|
| 已拿到句柄之后的失败 | `conn->errbuf` | 与连接同生命周期，多连接互不干扰 |
| 建连之前/建连失败 | 模块级 `_Thread_local` 缓冲 | 那时还没有句柄可以承载错误 |
| 语句级错误 | 先拷进 `conn->errbuf`，再 close | `mysql_stmt_error` 的指针在 `mysql_stmt_close` 之后失效 |

第三条值得展开。本模块每个错误路径都以 `mysql_stmt_close(stmt)` 收尾，
而 `mysql_stmt_error(stmt)` 返回的是语句句柄内部的内存。若把那个指针留给调用方，
调用方读到的是已释放的内存——在 ASan 下必然报 `heap-use-after-free`，
在没有 ASan 的日子里则可能打印出一段看似正常的旧文本。

`_Thread_local` 是 C11 6.7.1 定义的存储类说明符，每个线程各有一份副本。
理由与 `errno` 相同：多线程共用一份静态缓冲时，A 线程的建连失败会被
B 线程的失败覆盖，报出来的原因与实际问题无关。

```c
static _Thread_local char g_last_error[MC_ERRBUF];
```

`mc_error(NULL)` 取的就是这份缓冲。建连失败后 `*out` 已经是 NULL，
所以调用方只能这样取原因——这一点写在头文件注释里，免得有人去 `mc_error(*out)`。

### 3.3 结果集：两遍遍历，一次分配

`mc_query` 的内存布局是"指针数组 + 两段连续内存"：

```
names ──▶ [p0][p1][p2]        ──▶ "id\0name\0u_img\0"
cell  ──▶ [p0][p1]...         ──▶ "1\0king\0<二进制>\0"
length──▶ [1][4][65536]...
```

遍历两遍：第一遍只为算出单元格字符串需要的总字节数，第二遍才拷贝。
代价是要 `mysql_data_seek(res, 0)` 回到开头——这对 `mysql_store_result`
取回的结果集成立（数据已经在客户端），对 `mysql_use_result` 的流式结果集不成立，
所以这里只用 store。

为什么不逐格 `malloc`：十万行十列的查询就是一百万次分配，每一次都可能失败，
清理逻辑会膨胀到没人愿意读，而 `free` 的调用次数同样是一百万。
一次分配之后，`mc_rows_free` 只需要 free 五个指针。

为什么 `cell` 是一维数组而不是 `char ***`：二维指针数组要么多一次分配，
要么释放时要先逐行 `free` 再 `free` 外层，出错路径多一倍。
行优先展开之后，`i * cols + j` 的乘法换成一次加法，代价可以忽略。

`rows * cols` 做了溢出检查：

```c
if (cols != 0 && rows > (SIZE_MAX / cols)) { ... }
```

两个 `size_t` 相乘会回绕，回绕之后分配出来的数组比实际要用的短，
越界写就发生在下一次循环。

### 3.4 NULL 与空串必须分开

```c
if (row[j] == NULL) { cell[k] = NULL; length[k] = 0; continue; }
```

`NULL` 是"没有值"，`""` 是"值就是空的"。合成一种表示之后，
一行 `a\t\tc` 无法判断中间那列到底是哪种情况，
而这两种情况在业务上往往意味着完全不同的处理。

驱动层因此把 NULL 打印成字面量 `NULL`：

```
id	name	u_img
1	king	NULL
```

### 3.5 写入大字段：`send_long_data` 的分块协议

`MYSQL_TYPE_LONG_BLOB` 字段可能远大于单次网络包能承载的长度。
两步协议：

1. 绑定时只声明"这个参数是长字段"，不给数据：

   ```c
   param.buffer_type   = MYSQL_TYPE_LONG_BLOB;
   param.buffer        = NULL;
   param.buffer_length = 0;     /* 必须为 0 */
   ```

2. 数据用 `mysql_stmt_send_long_data` 分块追加，服务端按顺序拼接。

分块大小取 16 KiB（`MC_BLOB_CHUNK`），接近一个以太网 MTU 下的合理报文尺寸。
64 KiB 的图片是 4 次调用，5 MiB 是 320 次。

一次传完整块当然也能工作，但换成 100 MB 的文件时，单次调用就要求驱动
在内存里再准备一份完整副本（或直接失败）。分块是唯一不需要为大字段
额外分配内存的写法。

`buffer_length` 必须为 0，`is_null` 与 `length` 可以留空——
前提是数据分块送。绑定缓冲区与 `send_long_data` 是二选一的两条路，
混用会得到一个"参数长度为 0"的空值。

### 3.6 `MYSQL_DATA_TRUNCATED` 不是错误

读侧先用长度为 0 的绑定取一次，只为了拿到字段的真实长度：

```c
result.buffer        = NULL;
result.buffer_length = 0;
result.length        = &col_len;   /* fetch 之后是真实字节数 */
```

对任何非空字段，`mysql_stmt_fetch` 都会返回 `MYSQL_DATA_TRUNCATED`。
这是"我告诉你长度了，你自己来取"的正常协议。

原实现是这么写的：

```c
ret = mysql_stmt_fetch(stmt);
if (ret != 0 && ret != MYSQL_DATA_TRUNCATED) break;   /* ← 问题在这里 */
```

`MYSQL_NO_DATA`（100）、`MYSQL_DATA_TRUNCATED`（101）与真正的错误码
（非 0 且非这两个值）被同一个 `break` 吞掉。后果是：网络中断时函数同样
返回一个非负的 `read_length`，调用方拿到一段长度 0 的"成功"数据。
现在的写法是分别判断：

```c
if (ret == MYSQL_NO_DATA)              → MC_ERR_NODATA
if (ret != 0 && ret != TRUNCATED)      → MC_ERR_STMT（真错误）
if (is_null)                           → MC_ERR_NODATA（列为 NULL）
```

### 3.7 取数：一次请求整段剩余长度

原实现把 `buffer_length` 固定为 1：

```c
result.buffer_length = 1;
mysql_stmt_fetch_column(stmt, &result, 0, start);
start += result.buffer_length;
```

每调用一次只取回**一个字节**。读满 64 KiB 需要 65536 次函数调用，
每一次都涉及一次到客户端缓冲区的拷贝。现在按区间请求：

```c
unsigned long remaining = (unsigned long)(wanted - offset);
result.buffer        = (char *)buf + offset;
result.buffer_length = remaining;
mysql_stmt_fetch_column(stmt, &result, 0, (unsigned long)offset);
```

请求的正是"剩余的全部长度"，因此不会发生截断——`fetch_column` 只在
请求长度小于字段长度时才截断。同样的 64 KiB，从 65536 次调用变成 1 次。

### 3.8 读文件不依赖 `fseek` / `ftell`

原实现先 `fseek(fp, 0, SEEK_END)` 再 `ftell` 拿长度，隐含了
"文件可 seek 且长度已知"这个前提。这个前提对普通文件成立，对下面三类不成立：

| 来源 | 行为 |
|---|---|
| FIFO / 管道 | POSIX 规定不可 seek，`fseek` 失败并设置 `ESPIPE`，`ftell` 返回 -1 |
| 终端、`/dev` 下的字符设备 | 同上 |
| `/proc`、`/sys` 里的伪文件 | 可 seek，但报告的长度是 0，按"长度 0"去读一个字节都读不到 |

现在的实现是"一直 `fread` 到 EOF，缓冲区填满后再多读一个字节"：
三类来源都能正常读出内容，越界仍然能被发现。
`test_pure.c` 里有一条针对 `/proc/version` 的回归用例——
旧实现在这里返回长度 0，新实现读出 `Linux version ...` 全文。

不检查 `feof` 也是同一条原则的延续：C11 7.21.10.2 规定文件结束指示器由
**失败的**读取设置，是事后标志，用它当循环条件必然多跑一轮。

### 3.9 `fwrite` 成功不等于数据落盘，写入要能回滚

ISO C 7.21.3 规定数据先进入流的缓冲区，真正的 `write(2)` 由 `fflush`
或 `fclose` 触发。所以 `fwrite` 返回值正确只说明"数据交给了缓冲区"；
磁盘满（`ENOSPC`）、配额用尽（`EDQUOT`）这些错误要到刷新时才报出来。
不检查 `fflush` / `fclose` 的返回值，等于把一次失败的写入报告成成功。

再往前一步：`fopen(path, "wb")` 在打开的一瞬间就把目标文件截断为 0 字节
（C11 7.21.5.3，`"w"` 模式的语义就是 truncate）。此后的写入一旦失败，
目标文件只剩一份残缺内容，原来的内容已经没了。

所以写入走临时文件：

```c
snprintf(tmp, n, "%s.tmp", path);
fp = fopen(tmp, "wb");        /* 写失败就 remove(tmp) */
...
rename(tmp, path);            /* POSIX 上同一文件系统内是原子替换 */
```

`rename` 原子替换的后果是：任何时刻从并发读者的角度，看到的要么是完整的旧内容，
要么是完整的新内容，不存在"半截"。临时文件必须放在目标文件所在目录，
不能放 `/tmp`——跨文件系统时 `rename` 返回 `EXDEV`，内核不会帮你搬数据，
原子性就没了。
这一点也是 `contact-list` 模块的选择，两个模块在同一个问题上给出了同一个答案。

### 3.10 退出码与输出流的分工

| 流 | 内容 |
|---|---|
| `stdout` | 只有数据（结果集的列名与行、受影响行数） |
| `stderr` | 摘要（`3 行`、`写入 5242880 字节`）与错误 |

这样 `mysqlclient sql -q '...' | cut -f2` 才能直接可用。
如果摘要混进 stdout，每一行数据都要被 `grep -v 行` 过滤掉。

退出码三档：`0` 成功、`1` 运行期失败、`2` 用法错误。
原实现的 `main` 在每条失败路径上都 `goto Exit` 然后 `return 0`——
一个把"删表失败""连不上库""图片读不出来"全部报告成成功状态的程序，
在脚本里是危险的：`&&` 后面的命令照常执行。

### 3.11 凭据的来源：三层，优先级从低到高

1. 编译期默认值：`config.h`（存在时）或一组"只连本机、无密码"的默认值
2. 环境变量 `MC_HOST` / `MC_PORT` / `MC_USER` / `MC_PASSWORD` / `MC_DB`
3. 命令行 `-H` / `-P` / `-u` / `-p` / `-d`

`config.h` 由 `config.h.example` 复制生成，被 `.gitignore` 忽略。
**Git 的历史不可撤销**：凭据一旦提交，即使后续删除，它依然永久留在旧提交里，
因此唯一的办法是从来不提交。

三层不是过度设计：`config.h` 适合本机长期配置，环境变量适合一次性覆盖
（连测试库），命令行适合脚本里显式传参。`check_live.sh` 用的就是第二层。

`config.h` 不存在时驱动退回内置默认值，因此**从干净检出执行 `make` 不会
因为找不到 `config.h` 而失败**。这一条是模块化时才补上的：
原实现无条件 `#include "config.h"`，新克隆的仓库编译必然报错。

### 3.12 参数校验发生在动网络之前

`mc_options_check` 在 `mysql_real_connect` 之前把明显不合法的参数挡掉：

- `host` / `user` 非空，且**在容量内以 `'\0'` 结尾**；
- 端口在 1..65535。

第二条不是多余的：调用方可能用 `memcpy` 填满整个数组而不写结尾符，
这时 `strlen` 会一路读出去。把它交给 `mysql_real_connect`，
就是把一次越界读交给了库。

端口 0 也被拒绝。`connect(2)` 里端口 0 是合法值，含义是"由内核挑一个临时端口"，
而客户端库对它的处理是回退到默认端口——两种解释都不是调用方想要的。
参数错误应当以参数错误的形式报出来，不要混进"连接被拒绝"。

### 3.13 流式取数：关键在于**不**调用 `store_result`

`mc_query` 走文本协议：`mysql_store_result` 把整个应答读进客户端，
之后 `mysql_data_seek(res, 0)` 才能支持"第一遍量尺寸、第二遍拷内容"。
`mc_query_each` 走预处理语句的二进制协议，并且**不调用** `mysql_stmt_store_result`：

```c
mysql_stmt_prepare → mysql_stmt_bind_result → mysql_stmt_execute
loop: mysql_stmt_fetch   /* 每次从套接字上读一行 */
```

`mysql_stmt_store_result` 的作用就是"把剩下的行全读进内存"。
一旦调用它，内存占用立刻变得与行数成正比，流式的意义就消失了——
它的存在只是为了支持 `mysql_stmt_data_seek` 与 `mysql_stmt_num_rows`，
而游标接口按定义不需要回退。这是一个**反直觉的取舍**：
库提供的最"完整"的那条调用路径，恰恰是要避免的那条。

三处细节决定这个实现是否真的正确：

1. **每行开始前把 `is_null` / `error` / `length` 清零。**
   库只在这些字段需要改变时写它们。不清零时，"上一行是 NULL"会让
   这一行也报成 NULL，而返回值仍然是 0。
2. **`buffer_length` 永远比分配长度少 1**，最后那个字节留给 `'\0'`。
   若让 `buffer_length == cap`，当值正好填满缓冲区时 `ret` 为 0（不算截断），
   接着写 `buf[len] = '\0'` 就越界一字节——ASan 能抓到，未插桩时不会崩。
3. **`mysql_stmt_free_result` 必须在关闭语句之前调用。**
   回调提前停止时结果集里还有没读的行，此时连接处于"结果未读完"状态，
   下一条命令会得到 `Commands out of sync`。这个调用把剩余行读掉丢弃。
   `test_stream.c` 里有一条用例专门验它：回调在第 3 行停下，
   然后用同一个连接执行 `SELECT 1`。

另外，`ret` 的三种取值要分开处理，理由与 3.6 节相同但方向相反：
那里 `MYSQL_DATA_TRUNCATED` 被当成错误吞掉了真错误，这里若把真错误
当成"取完了"，调用方会以为结果集只有这么长——**静默丢行**比报错难查得多。

### 3.14 原子替换换掉的是 inode，权限位必须自己搬

`rename(2)` 换的是目录项，不是目录项指向的那个 inode。
新 inode 是 `fopen(tmp, "wb")` 按 `0666 & ~umask` 创建的，
与目标文件原来的权限、属主、属组都没有关系。于是"覆盖同名文件"这个
看起来只改动内容操作，会顺带把权限换掉：

```
$ umask            # 002
$ ls -l out.bin    # -rw-------   0600
$ （mc_file_write 覆盖一次）
$ ls -l out.bin    # -rw-rw-r--   0664
```

一个原本只有所有者能读的导出文件，写第二次之后同组用户就能读了，
而函数返回 `MC_OK`、字节数也完全正确。这类偏差在功能测试里看不出来，
只能靠 `stat` 发现。

修法是：写入之前先 `stat(path)` 记下目标文件已有的权限位，
在 `fchmod(fileno(fp), mode)` 之后再开始写数据，最后 `rename` 覆盖。

三处顺序上的选择，每一处都能说出理由：

1. **`stat` 在 `rename` 之前**。`rename` 之后原来的 inode 已经被摘掉，
   权限和内容一起消失了，再想取也没有来源。
2. **`fchmod` 在 `fwrite` 之前**。刚 `fopen` 出来的临时文件是 `0664`（umask 002），
   此刻它是空的，没有内容可泄露；若把 `fchmod` 放到写完数据之后，
   就会出现一段"内容已经落盘、权限还是 `0664`"的窗口。两种写法只差几毫秒，
   但前者不需要为那几毫秒写一段辩护。
3. **只复制 `0777` 三段，不带 `setuid` / `setgid`**。这两个位的语义是
   "执行这个文件时以文件所有者（或所属组）的身份运行"（`execve(2)`）。
   换进来的是一份**新的、未经任何审查的内容**，把旧文件的 setuid 位搬过去，
   等于给"能触发这次写入的人"一个以文件所有者身份运行的二进制。
   内核在 `write(2)` 写已有文件时会清掉 setuid 位，但这里写的是新 inode，
   内核不会替调用方清——只能由实现主动不带。

`stat` 跟随符号链接，所以 `path` 是链接时取到的是被指向文件的权限；
但 `rename` 替换的是链接本身而不是它指向的文件。这两件事不一致，
属于 8.9 记录的行为差异，没有在本轮处理。

还有一个副产品：`rename` 只看目录写权限，不看目标文件的写权限。
因此目标文件是 `0444` 时覆盖**仍然成功**（用 `fopen(path, "wb")` 会 `EACCES`）。
既然行为是"覆盖成功"，权限位就更必须保持，否则"成功"只对了一半。

### 3.15 列名单独一个回调，而不是加进行回调

列名是**结果集的属性**，不是行的属性：它来自服务器在 `prepare` 之后返回的元数据，
在取到第一行之前就已经完整。三条路都试过，只有第三条不用为这个事实付代价：

1. **把 `names` 加进 `mc_row_fn`**：签名变成五个参数，每行都传一遍同一个指针。
   更麻烦的是空结果集——0 行时行回调一次也不调用，表头就打印不出来，
   而 `SELECT … WHERE 1=0` 恰恰是"只想要表头"的常用写法。
   若为了这个再补一次"没行也要调一次"，那就等于在行回调里埋一个只走一次的
   分支，比单独一个回调更难讲清楚。
2. **另开 `mc_query_names(conn, sql, char ***)` 预先取元数据**：要拿列名就得
   把语句执行一次。同一份 SQL 执行两遍，中间隔着别人的写操作时两次的形状
   可能不同；若不执行两遍，就得把语句句柄跨调用存下来，接口变成"先 open
   再 fetch"的两段式，调用方要自己管一个由列名查询创建的对象。
3. **`mc_query_each_ex` 多一个 `on_cols` 回调**（选了这个）：顺序天然是
   `prepare → execute → on_cols → 逐行`，表头在第一行之前落地，
   空结果集也走同一条路径，接口仍是"一次调用完成一次取数"。

`on_cols` 的"返回非 0"与行回调的"返回非 0"是同一语义：调用方主动放弃，
函数返回 `MC_OK`。两处语义一致，调用方不用记两套约定。

实现在 `mysql_stmt_result_metadata()` 返回的 `MYSQL_RES` 上取列名
（`mysql_fetch_field_direct()`），取完立刻 `mysql_free_result`。
这一步不能省：那个 `MYSQL_RES` 是独立分配的，不属于语句句柄。
名字也不能直接递出去——它属于这个 `MYSQL_RES`，释放就没了，因此整体拷进
一段连续内存（与 `mc_rows` 的 `name_arena` 同一手法，一次 `malloc` 而不是每列一次）。

`on_cols` 的调用点放在 `mysql_stmt_execute` 之后而不是 `prepare` 之后：
`prepare` 只验证语法，表不存在、列名写错、权限不足这些在执行阶段才暴露。
放在执行之后，"失败时一次回调都不发生"就是自然结果，而不是靠额外判断维持。

### 3.16 参数绑定：一个带标签的联合体，和四个实测出来的坑

**为什么不用变参函数。** `mc_exec_params(conn, sql, ...)` 这种签名省掉结构体，
代价是把类型错误从编译期推到运行期：`va_arg` 取的类型与实参不符是未定义行为
（C11 7.16.1.1p2 只要求"与提升后的类型兼容"），而编译器对 `printf` 之外的
变参函数不做格式检查。值一律按 8 字节读出来之后，`int` 与 `double` 的错位
不会报错，只会写入一个谁也解释不了的值。所以参数是数组 + 显式 `kind`。

**为什么不是六个构造函数。** `mc_param_int(42)` 这类写法能把"kind 与联合体
成员匹配"收进库里，代价是每个类型多一个函数、多一份文档，而调用方仍可以
在栈上直接初始化结构体绕过去——C 的类型系统不给带标签的联合体任何保护。
既然漏洞堵不死，就选接口更小的一侧：结构体 + 指定初始化器，
`.kind = MC_PARAM_INT, .v.i = 42` 读起来与构造函数一样清楚。

**坑一：占位符个数必须自己核对。** `mysql_stmt_bind_param` 只拿到一个数组指针，
它按**占位符个数**遍历，不知道数组有多长。参数给少了就是读越界——
读到的可能是同一个栈帧里别的变量，值不确定，而 `execute` 往往成功。
所以 `mysql_stmt_param_count()` 与 `n` 不等时直接返回 `MC_ERR_PARAM`，
错误信息里带上两边的数字。

**坑二：`is_unsigned` 在参数方向上"只看有没有给，不看给的是什么"。**
文档说的是"指向 `bool`，表示是否无符号"，实测（手工绑 `MYSQL_BIND` 复现，
三种组合各跑一次）却是：`&false` 失败、`&true` 失败、`NULL` 成功。
`-42` 在头两种下变成 `18446744073709551614`，服务端报
`Out of range value for column ...`。也就是说有符号整数的正确写法是
**让 `is_unsigned` 保持 `NULL`**。结果集绑定那条路径相反（会回填
`*is_unsigned`），这个不一致只出现在参数方向，写在这里免得下次再踩。

**坑三：零字节参数的 `buffer` 不能是 `NULL`。** `len == 0` 时若把 data 里的
空指针原样递下去，客户端在 `execute` 期间会解引用它。零字节的内容无处可读，
但指针必须可读，于是指到槽位自己的内存上（那 8 字节一个也不会被读）。
这条与"`len == 0` 且 `data == NULL` 合法"是配套的：前者是调用方的便利，
后者是实现要接住的结果。

**坑四：`const void *` 转 `void *` 会触发 `-Wcast-qual`。** `MYSQL_BIND::buffer`
的类型是 `void *`，而模块承诺不修改调用方的数据。限定符只在赋值与解引用时
参与匹配（C11 6.3.2.3p2），转换本身良定义，问题只在 GCC 的告警。
先转 `uintptr_t` 再转指针：整型不携带限定符，告警消失，运行时值完全相同。

**长字段的门槛取 `MC_BLOB_CHUNK`。** `len` 超过它就改走 `send_long_data`
（`buffer_type = MYSQL_TYPE_LONG_BLOB`、`buffer_length = 0`、`length` 与
`is_null` 都留空），内容分块追加。这个写法与 `3.5` 里 `mc_blob_write` 的
老实现一致；低于门槛时用普通的绑定缓冲区，省掉"声明长字段 + 分块追加"这两步。
两条路径的分界点必须在**同一处**判断，否则 `mc_blob_write` 与新接口
会对同一个长度做出不同处理。

**`mc_blob_write` 改为 `n == 1` 的包装。** 收益是长字段分块只有一份实现，
代价是错误码有一处变化：`sql` 里出现多个 `?` 时现在返回 `MC_ERR_PARAM`。
这个变化其实是修掉了一个既有隐患——包装之前，绑定数组只有一个元素，
而 `mysql_stmt_bind_param` 会按占位符个数去读，第二个参数落在栈上相邻的
内存里；值不确定，`execute` 却可能成功。

**安全性质。** 值作为独立报文发送，服务器不会把它当 SQL 文本解析，
因此值里的引号、反斜杠、注释符既不改变语句结构也不需要转义
（`check_live.sh` 用 `it's ok` 与 `a b<TAB>c` 两例钉住；单元测试里还用了
`it's; DROP TABLE p; -- 注释`）。这是参数化的全部意义，与
"`mysql_real_query` 本身是否安全"无关——拼接发生在客户端。

### 3.17 语句对象：复用的是一次往返与一次解析

**省下来的到底是什么。** prepare 与 execute 各占一次往返，prepare 那次还多一次
服务端解析与优化。同一条语句执行 N 次，`mc_exec_params` 要付 2N 次往返、
N 次解析；语句对象付 N+1 次往返、1 次解析。表观收益是"接近 2 倍"，
但这句话只在**往返本身比语句执行便宜**时成立——所以 6.4 的量法必须把
语句的其它成本（刷盘、锁）拿掉，否则测的是磁盘而不是协议。

**为什么不用 INSERT 量。** 最初的基准是 20000 次单行 INSERT，两次运行都由
"prepare 每次"取胜不了：每次 6.0~6.6 ms，比值 0.94~1.01。换成 MEMORY 引擎
之后仍然是 6 ms——因为刷的是 **binlog**，不是表：8.0 默认开着 binlog 且
`sync_binlog = 1`，每次写语句提交都要等一次 `fsync`。一次 `fsync` 是毫秒量级，
而 prepare 省下的往返是几十微秒，信号被淹掉两个数量级。
最终改成只读语句（`SELECT 1, 2`）才测出干净的数（6.4）。

这条教训与 11-09 记的"`yes` 参数超 `MAX_ARG_STRLEN` 造出 0 字节输入"是同一类：
**基准要先确认它量的是想量的那个东西**。测出一个 1.0 倍的比值时，
先怀疑被测量，再怀疑实现。

**取数路径只有一份。** `mc_query_each_ex` 的循环被抽成 `stream_deliver`：
它假定语句已经 execute 成功、结果集已就绪，只做 `bind_result`、交付列名、
逐行 fetch 与截断处理。`mc_query_each_ex` 与 `mc_stmt_each` 都调它，
因此"数据被截断时怎么办""列名什么时候交付""SQL NULL 与空串怎么区分"
只有一处实现。两处各写一遍的话，这些细节会各自演进，最后同一份数据在两条
路径上给出不同的 `cell` / `length`。

**物化结果集不调 `store_result`。** `mc_stmt_rows` 走的是与流式相同的逐行
fetch，一边读一边把单元格追加到一段增长的缓冲区里；`mc_query` 则是
`mysql_store_result` + 两遍遍历（3.3）。两者最终的内存布局一致
（名字与单元格各一段连续内存、指针数组只存偏移），但前者不需要事先知道行数。
收集器里记的是**偏移**而不是指针：arena 增长时 `realloc` 会让指针失效，偏移不会。

**状态只有两个布尔量。**

| 动作 | 前置 | 之后 |
|---|---|---|
| prepare 成功 | — | `executed = 0`、`consumed = 0` |
| execute 成功 | 参数个数与 kind 合法 | `executed = 1`、`consumed = 0` |
| 取数（rows / each）返回 | `executed && !consumed` | `consumed = 1` |
| 再次 execute | 任意 | 先丢弃旧结果集，再置成上表第二行 |

取数前判 `executed && !consumed`，任一不成立就返回 `MC_ERR_STATE`。
不用"0 行 + MC_OK"表示这两种情况，是因为它与"查询确实没有行"不可区分——
一个静默的空结果集比一个明确的错误码难查得多。

**未读完就再执行。** 实现里在 execute 之前显式 `mysql_stmt_free_result`，
依据是协议是半双工的：未读完的结果集占着连接，直接发下一条命令会得到
"Commands out of sync"。但注入验证给出了一个必须写下来的反例：把这行删掉，
`test_stmt` 的 100 项**仍然全绿**——8.0.46 的 `mysql_stmt_execute` 自己就会丢弃
上一次没读完的行。所以那组断言能证明的是"复用不被半读破坏"，不是"显式释放被执行"。
显式调用保留，理由是文档语义而不是实测差异；真正能区分两者的情形是服务端游标
（`MYSQL_STMT_ATTR_CURSOR_TYPE`），本模块没有用。

**`affected` 只在没有结果集时写。** 返回结果集的语句调 `mysql_stmt_affected_rows`
拿到的是服务端报告的行数，与"交付了几行"含义不同（提前停止时两者就不等）。
把它写进同一个出参，调用方迟早会把两者当成一回事，于是干脆不写：
`affected` 为 NULL 或保持调用方原值，行数走 `*out_rows`。

---

## 4 构建

```bash
make              # 静态库 bin/libmysqlclient.a 与命令行工具 bin/mysqlclient
make lib          # 只构建静态库
make cli          # 只构建命令行工具
make test         # 纯逻辑 99 项 + 接口层 23 项
make check        # 命令行端到端 41 项（不需要数据库）
make check-live   # 真实数据库往返 96 项 + 参数绑定 65 项 + 流式比对 56 项 + 语句对象 100 项（需要数据库，见下）
make check-scale  # 百万行下的峰值内存对照（需要数据库，约 30 秒）
make sanitize     # ASan + UBSan 重新构建并跑测试，产物在 bin/sanitize/
make sanitize-live # 同上，另跑需要数据库的四套用例（需要数据库）
make bench        # 文件读写吞吐
make clean        # 删除 bin/（含全部变体）
```

默认编译参数：

```make
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g
CPPFLAGS += -Iinclude -D_POSIX_C_SOURCE=200809L
```

`-D_POSIX_C_SOURCE=200809L` 是必须的：`-std=c11` 定义 `__STRICT_ANSI__`，
关掉 glibc 的默认扩展，驱动层用到的 `stat` / `S_ISREG` 声明就看不到了。

### 4.1 外部依赖：libmysqlclient

位置按下面的顺序确定，三处都可以用命令行变量覆盖：

1. `mysql_config --cflags / --libs`（装了 `libmysqlclient-dev` 就有）
2. `pkg-config mysqlclient`
3. `make MYSQL_CFLAGS=... MYSQL_LIBS=...`

**没有免密 sudo 的机器上，第 3 条是可用的。** 把开发包与运行库解包到临时目录，
不需要安装、不需要 root：

```bash
mkdir -p /tmp/mcpkg && cd /tmp/mcpkg
apt-get download libmysqlclient-dev libmysqlclient21
mkdir -p root && for f in *.deb; do dpkg-deb -x "$f" root; done

cd /path/to/src/mysql-client
make MYSQL_CFLAGS="-I/tmp/mcpkg/root/usr/include/mysql -I/tmp/mcpkg/root/usr/include" \
     MYSQL_LIBS="-L/tmp/mcpkg/root/usr/lib/x86_64-linux-gnu -lmysqlclient \
                 -Wl,-rpath,/tmp/mcpkg/root/usr/lib/x86_64-linux-gnu"
```

`-I` 要写**两级**。`mysql.h` 位于 `<prefix>/include/mysql/`，而它内部写的是
`#include "mysql/udf_registration_types.h"`；只给 `-I<prefix>/include/mysql`
时这个包含会失败。装上开发包的机器不会遇到，因为 `<prefix>/include` 是默认搜索路径。

`-Wl,-rpath` 也是必须的：`-L` 只影响链接期，运行期动态链接器仍会去
`/etc/ld.so.cache` 里找，找不到就报 `error while loading shared libraries`。

找不到头文件时，Makefile 会先打一行提示，然后由编译器报
`fatal error: mysql.h: No such file or directory`——不静默跳过：

```
Makefile:66: 未找到 mysql.h 的位置：装 libmysqlclient-dev，或用 MYSQL_CFLAGS / MYSQL_LIBS 指定（见本文件顶部注释）。
src/mysqlclient.c:26:10: fatal error: mysql.h: No such file or directory
```

### 4.2 根 Makefile 的 `SKIP`

本模块是仓库里唯一有外部依赖的模块，因此在缺库的机器上会挡住整仓构建。
根 `Makefile` 提供 `SKIP`：

```bash
make SKIP=mysql-client          # 跳过它，其余四个模块照常构建
make SKIP="mysql-client word-count"
make list                       # 列出识别到的模块，被跳过的标 (SKIP)
```

选"显式跳过"而不是"自动探测失败就不构建"：静默少构建一个模块，
最后看到的是"全部成功"，而少掉的东西没有任何提示。

### 4.3 变体构建分目录

`sanitize` 递归调用自身并把 `OUT` 覆盖成 `bin/sanitize`。
原因是 make 判断"要不要重建"只比较时间戳、不比较编译选项：
共用 `bin/` 时先跑 `make sanitize` 再跑 `make`，普通构建会认为目标已是最新，
留下一个插桩过的可执行文件，运行时报 sanitizer 的符号错误。

`MYSQL_CFLAGS` / `MYSQL_LIBS` 在递归调用里显式透传，否则子 make 会重新探测一遍，
在"用命令行指定解包目录"的场景下探测不到任何东西。

---

## 5 测试与验证

验证环境：**Linux 6.8.0-138 / gcc 11.4.0 / x86_64**，
MySQL **8.0.46**（客户端库与服务端同版本，从 Ubuntu 22.04 的 deb 解包到
`/tmp` 下运行，无需 root 安装）。

### 5.1 编译警告

| 编译参数 | 结果 |
|---|---|
| `-std=c11 -Wall -Wextra -Wconversion -O2 -g` | **0 条** |
| `make sanitize`（追加 `-fsanitize=address,undefined -O1`） | **0 条** |
| 追加 `-Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef` | **0 条** |

第三种是刻意加严的：`-Wstrict-prototypes` 能抓出 `int f()` 这种不写参数的旧式声明，
`-Wcast-qual` 能抓出为了省事去掉 `const` 的强制转换。两者在本模块都干净。

### 5.2 单元测试（不需要数据库）

`make test`，122 项：

```
mysqlclient 纯逻辑单元测试            ← 99 项
--- 状态码 ---
ok    十二个状态码都有描述
ok    未知状态码返回 unknown status
--- 连接参数 ---
ok    init 之后端口为 3306
ok    空 host 被拒绝
ok    host 未以 '\0' 结尾被拒绝
ok    端口 0 被拒绝
ok    端口 65535 被接受
--- 文件读写 ---
ok    内容逐字节相同（含内嵌 '\0'）
ok    容量恰好等于长度时成功
ok    容量少一字节时报 TOOBIG
ok    失败时不写 out_len
ok    读目录不会静默成功
ok    /proc/version 读到了内容（长度大于 0）
ok    写入成功后不残留 .tmp 文件
ok    写入失败后不残留 .tmp 文件
--- 覆盖写的权限位 ---
ok    新建文件的权限是 0666 & ~umask
ok    0600 覆盖后仍是 0600
ok    0444 覆盖后仍是 0444
ok    setuid 位不被继承（04755 -> 0755）
ok    目录不可写时返回 FILE_OPEN
ok    失败后目标权限不变
ok    失败后目标内容不变（仍是 old）

mysqlclient 接口层测试                ← 23 项
ok    mc_ping(NULL)
ok    mc_error(NULL) 非 NULL
ok    失败时 out 被置为 NULL
ok    连接 127.0.0.1:1 返回 CONNECT
ok    错误信息带 connect 前缀
ok    mc_query_each 回调为 NULL 被拒绝
ok    失败时 out_rows 被写成 0
```

四组用例值得单独说明：

- **`/proc/version`**：伪文件报告长度为 0，是"不依赖 `ftell`"这条改动的回归测试。
  旧实现在这里返回 0 字节。
- **容量少一字节**：容量恰好等于长度必须成功，少一字节必须报 `TOOBIG`。
  差一个字节的边界是这类代码最容易写错的地方，而它的错误表现是"偶尔花屏"。
- **未以 `'\0'` 结尾的字段**：`memset` 填满整个数组再校验，验证不会越界读。
- **覆盖写的权限位**：`0600` / `0640` / `0444` / `0755` 四种起点各覆盖一次，
  逐个用 `stat` 核对；`04755` 那一组同时验 `setuid` 位不被搬过去。
  把旧实现换上跑同一份测试，这 5 条会同时报 `实际 0664`
  ——先确认测试能抓到缺陷，再确认修复后它变绿。

`test_pure` 不链接 `libmysqlclient`：

```
$ ldd bin/test_pure | grep mysql
（无输出）
```

这是分层设计的实证——`file.o` / `options.o` / `status.o` 里没有任何 `mysql_*` 引用。

`test_db` 连接的端点是 `127.0.0.1:1`（保留的 tcpmux 端口）。内核直接回 RST，
`connect(2)` 立刻返回 `ECONNREFUSED`，毫秒级完成，不依赖任何外部环境。
用 `192.0.2.0/24` 这类"必然不可达"的地址会退化成超时，让测试卡在 TCP 重传上。

### 5.3 命令行端到端（不需要数据库）

`make check`，41 项。全部用例都不需要运行中的服务器：

```
ok    帮助退出码 0
ok    帮助列出 blob-put
ok    无参数退出码 2
ok    端口非数字退出码 2
ok    端口越界退出码 2
ok    sql 缺 -q 退出码 2
ok    ping 拒绝 -q 退出码 2
ok    each 缺 -q 退出码 2
ok    each 拒绝 -f 退出码 2
ok    each -n 非数字退出码 2
ok    sql 用 -n 退出码 2
ok    blob-put 文件不存在退出码 1
ok    文件不存在时报 cannot open file
ok    文件错误不被误报成连接错误
ok    连不上时退出码 1
ok    连不上时报 connect
ok    空密码不触发用法错误
ok    有效文件读到之后才报连接错误
ok    环境变量 MC_PORT 生效
```

"文件错误不被误报成连接错误"这一条验的是**顺序**：`blob-put` 先读文件、
后建连。反过来做的话，一个写错的路径要等一次完整的 TCP 握手之后才报错，
而它跟服务器一点关系都没有。用例用 `-P 1`（必连不上）配合：
若顺序反了，输出里会出现 `connect` 而不是 `cannot open file`。

### 5.4 真实数据库往返

`make check-live`，96 项（2b 组另见 5.5，2c 组另见 5.10）。需要一台连得上的
MySQL/MariaDB：

```bash
MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root MC_PASSWORD= make check-live
```

脚本自建、自删一个测试库，覆盖：

| 组 | 内容 |
|---|---|
| 文本 | UTF-8 原样取回、空串与 NULL 打印成不同的东西、列名来自服务器 |
| 流式 | `each` 与 `sql` 的整份输出 `cmp` 逐字节相同（含表头）、0 行时仍打表头、`-n 1` 出一行 |
| BLOB 往返 | 64 KiB / 5 MiB / 0 字节，三种长度都 `cmp` 逐字节相同 |
| 权限位 | `blob-get` 覆盖一个已存在的 `0600` 文件，事后 `stat` 仍是 `600` |
| 容量 | `-s 1000` 读 5 MiB → 退出码 1、报出 `5242880 bytes`、**不产生输出文件** |
| 多行 | `blob-get` 收到 3 行 → `more than one row` |
| NULL 列 | `blob-get` 命中 NULL → `SQL NULL` |
| 无行 | `blob-get` 查不到 → `query returned no row` |
| 语句错误 | 表不存在 → `query`；用 `sql` / `each` 跑 UPDATE → 提示改用 `mc_exec` |
| 绑定 | 含单引号的文本经 SQL 字面量写入后原样取回 |
| 参数绑定 | `exec-params` 的六种类型、长字段往返、五条用法错误（28 项，明细见 5.5） |
| 语句对象 | `stmt` 与 `sql` 的输出 `cmp` 逐字节相同、`-r 5` 多 5 行、`-r 3` 打 6 行、`-r` 只用于 `stmt`（13 项，明细见 5.10） |

5 MiB 那一项同时验证了分块协议：`MC_BLOB_CHUNK` 是 16 KiB，5 MiB 要 320 次
`send_long_data`，读回则是一次 `fetch_column`。

`check_live` 不并进 `make check`：没有数据库的机器上它必然全红，
那种"永远失败的目标"等于没有目标。

### 5.5 参数绑定

两部分：`test_params`（65 项，库层）与 `check_live.sh` 的 2b 组（28 项，驱动层），
都由 `make check-live` 一并运行。

| 组 | 项数 | 内容 |
|---|---|---|
| 有符号整数 | 5 | `-42`、`LLONG_MIN`、`LLONG_MAX` 三条往返 |
| 无符号整数 | 2 | `ULLONG_MAX` 与 `0` |
| 双精度 | 2 | `3.141592653589793`、`-0.5`（比对服务端格式化后的文本） |
| 文本 | 6 | 引号与注释符原样、UTF-8、空串、空串 `IS NULL` 为 0、SQL NULL |
| 二进制 | 4 | 含 `0x00` 的 6 字节、零字节、零字节不是 NULL |
| 长字段边界 | 7 | 恰好一块 / 一块加一字节 / 跨五块，各一次往返；另加跨三块的长文本 |
| 两个长字段 | 5 | 同一语句里两个不同长度的长字段，内容不互相追加；长字段在第一个位置 |
| 失败路径 | 13 | 占位符多少不符、未知 kind、`data` 为 NULL 而 `len` 非 0、SELECT、语法错、主键重复、失败后连接仍可用 |
| 受影响行数 | 6 | 单行 / 多行 / 未命中 / `affected` 为 NULL |
| 驱动层 `-a` | 28 | 六种类型的解析、引号与制表符原样、NULL、uint 上界、64 KiB 长字段往返、零字节、五条用法错误 |

四条注入（每条都让两层同时变红，失败集合互不重叠）：

| 注入 | `test_params` | `check_live` |
|---|---|---|
| 不把槽位的 `bind` 拷进绑定数组（`bind[i] = s->bind` 注释掉） | 败 42 项 | 败 21 项 |
| 有符号整数也把 `is_unsigned` 指针给上 | 败 8 项 | 败 1 项 |
| `send_long_data` 的下标写死成 0 | 败 7 项 | 败 2 项 |
| 去掉占位符个数检查（`if (want != n)` 改成 `if (0)`） | 败 3 项 | 败 2 项 |

第一条是开发过程中真踩到的：`calloc` 出来的 `MYSQL_BIND` 数组全零，
而 `buffer_type == 0` 就是 `MYSQL_TYPE_DECIMAL`，于是服务器收到
"每个参数都是数字 0"——语句成功、受影响行数正确，只有值不对。
它说明"能执行"与"值对"是两件事，判据必须落在值上。

第四条最不容易察觉：参数给少了不会报错，`bind` 数组只有 1 个元素而库按
3 个占位符遍历，读越界的值恰好是 0（后面的栈槽是干净的），
于是写入成功、值全错——只有"两个数字是否相等"这条断言能抓到。

### 5.6 流式取数（游标）

`test_stream` 56 项，同样由 `make check-live` 一并运行。七行数据覆盖：
普通文本 / 空串 / 全 NULL / 内含 `'\0'` 的二进制 / 100 KiB 的值 /
UTF-8 多字节文本 / 负数与 `INT` 上界。

| 用例组 | 内容 |
|---|---|
| 逐格比对 | 同一结果集分别经 `mc_query` 与 `mc_query_each`，比对 NULL 性质、长度、内容，全等 |
| 长值 | 100 KiB 那一行的长度逐字节核对（它必然走 `fetch_column` 分支） |
| 提前停止 | 回调在第 3 行返回非 0 → `MC_OK`，`out_rows == 3` |
| 连接复用 | 提前停止后同一连接执行 `SELECT 1` 仍然成功（验证 `free_result` 确实排空了结果集） |
| 列名 | `mc_query_each_ex` 的 `on_cols` 恰好调用一次、列名与顺序正确、行回调里这些指针仍可解引用 |
| 列名的边界 | 0 行的结果集也交付列名、别名优先于表达式原文、UTF-8 / 64 字节 / 含空格的列名不被截断 |
| 列名回调放弃 | `on_cols` 返回非 0 → `MC_OK`、`out_rows == 0`、行回调一次都不调用 |
| 失败顺序 | 表不存在时 `on_cols` 一次都不调用（表头不会打出一半） |
| 空结果集 | 0 行是成功，`out_rows == 0` |
| 无结果集 | `UPDATE` → `MC_ERR_RESULT`，提示改用 `mc_exec` |
| 语法错误 | 在 `prepare` 阶段被拒 → `MC_ERR_STMT`，`out_rows` 为 0 |
| 参数 | 回调为 NULL / SQL 为空 → `MC_ERR_PARAM` |

"连接复用"这一条是本节的重点。它验的不是输出对不对，而是**协议状态有没有收拾干净**：
少一次 `mysql_stmt_free_result`（或把语句句柄直接 `close` 掉而依赖内部实现），
这里就会得到 `Commands out of sync`。

列名那几条契约各做过一次注入，确认它们不是"功能没生效也照样绿"：

| 注入 | 结果 |
|---|---|
| 跳过 `on_cols`（把判据改成 `if (0 && …)`） | `check_live.sh` 败 6 项、`test_stream` 败 16 项；`each` 的第一行变成数据行 |
| 列名拷进 arena 之后立刻 `free`（模拟"名字跟着元数据一起释放"） | ASan 在 `names_fnv` 报 `heap-use-after-free`，`freed by mc_query_each_ex`——正好是"行回调里还能读"这条契约 |
| 列名按 8 字节截断 | 64 字节与 9 字节 UTF-8 两个用例失败，脚本层的失败信息本身也变成非 UTF-8 |

第三条说明的不只是"名字短了"：截断切在多字节字符中间会产出**非法 UTF-8**，
终端、`grep`、下游解析器都会受影响（`grep` 直接把整段输出当二进制）。
所以列名不能用固定长度的缓冲区接，长度必须来自 `name_length`。

### 5.7 百万行下的峰值内存

`make check-scale`，5 项检查。1048576 行、每行 3 列（`INT` / `VARCHAR(32)` /
`VARBINARY(16)` 含一个内嵌 `'\0'`），内容共 20 846 464 字节：

```
模式      行数   内容字节 峰值RSS(KiB) 耗时(ms)
-------------------------------------------------------
query      1048576       20846464        165760     292.5
each       1048576       20846464         11008     211.8

ok    query 行数等于服务器 COUNT(*)
ok    each 行数等于服务器 COUNT(*)
ok    两种模式的校验和相同
峰值内存比（query / each）= 15.1
ok    流式峰值内存低于物化的三分之一
ok    流式峰值内存低于 32 MiB（与 100 万行无关）
```

三点说明：

- **峰值内存必须在两个进程里测。** `ru_maxrss` 是进程级、单调不减的指标，
  同一进程里先跑物化再跑流式，测到的仍然是物化的峰值。
- **正确性基准来自服务器，不是另一份实现。** 行数用服务器自己算的
  `COUNT(*)` 核对，内容用 FNV-1a 校验和核对；只让两个实现互相比对时，
  共同的错误会互相掩护（比如两边都按 `strlen` 算长度）。
- **`each` 反而更快。** 物化路径多了一遍"拷进 arena"的 `memcpy` 与
  几十 MB 的分配，这些开销在 100 万行上是可测的。流式省掉的是内存搬运，
  不是网络 I/O——两者的字节数完全相同。

### 5.8 消毒剂

`make sanitize` 跑 99 + 23 项；把需要数据库的三套用例也放到插桩版上跑，用
`make sanitize-live`（等价于 `sanitize` 再跑 `check-live`、`test_params`、
`test_stream` 与 `test_stmt`）：

```
--- 纯逻辑测试（ASan + UBSan）---   99 项检查，0 项失败
--- 接口测试（ASan + UBSan）---     23 项检查，0 项失败
--- check_live（ASan + LSan）---    检查项 96，失败 0
--- test_params（ASan + LSan）---   65 项检查，0 项失败
--- test_stream（ASan + LSan）---   56 项检查，0 项失败
--- test_stmt（ASan + LSan）---    100 项检查，0 项失败
```

`sanitize-live` 单独一个目标而不是并进 `sanitize`，因为它要求一台连得上的
服务器；把"需要环境"的用例塞进默认目标，没有数据库的机器上一次都跑不通。

没有 ASan / UBSan 报告，也没有 LeakSanitizer 报告。后者是有意义的：
`mc_rows` 的 arena、每条语句的 `MYSQL_STMT`、连接句柄在三条失败路径上
都要求释放，漏一处就会在 5 MiB 那一轮被放大成几 MB 的泄漏。
流式路径上的泄漏点更密：每列一块缓冲区、四块指针数组，
外加"回调提前停止"这条额外的退出路径——它在 5.6 节的用例里被走到。

语句对象又添了两处：`mc_stmt` 自己那个结构体，以及它持有的 `MYSQL_STMT`。
`test_stmt` 里有 17 处 `mc_stmt_prepare` 与同样数量的 `mc_stmt_close` 成对出现，
prepare 失败的那几处不会产生句柄、因此不需要 close——"哪些路径要 close"
少判一处，LeakSanitizer 就会在进程退出时把漏掉的 `MYSQL_STMT` 连分配栈一起报出来。

### 5.9 全仓

根 `make test` 按模块表顺序逐个跑，每个模块打印自己的汇总行。
`mysql-client` 需要 `libmysqlclient`，在没有它的机器上用 `SKIP=mysql-client` 跳过：

```
$ make SKIP=mysql-client test
==> concurrent-lock     72 项检查，0 项失败
==> contact-list       242 + 466 + 20 + 43 项检查，0 项失败
==> thread-pool        215 项检查，0 项失败
==> word-count          40 + 226 项检查，0 项失败
```

合计 **1324** 项。本模块自己的 `make test` 另算 122 项（99 + 23），
要连库的 317 项（96 + 65 + 56 + 100）由 `make check-live` 负责。

### 5.10 语句对象

两部分：`test_stmt`（100 项，库层）与 `check_live.sh` 的 2c 组（13 项，驱动层），
都由 `make check-live` 一并运行。

`test_stmt` 自建自删一个 `mc_stmt_db.s` 库，五组：

| 组 | 项数 | 内容 |
|---|---|---|
| prepare 与元数据 | 29 | 占位符个数、结果集列数（`SELECT ?, ?` 是 2 列）、`sql` 为 NULL / 空串 → `PARAM`、表不存在在 prepare 阶段暴露成 `STMT` 且错误信息以 `prepare:` 开头、写语句的列数为 0、未 execute 就取数 → `STATE`、结果集取第二遍 → `STATE`、同一对象再次 execute 能再取一遍 |
| 参数 | 15 | 少给一个参数 → `PARAM` 且错误信息带两边的个数、补上参数后同一对象仍能执行、未知 kind 先于任何网络往返被拒、跨三块的长文本连续写两次 |
| 结果集 | 33 | 与 `mc_query` 的同一份数据逐格比对（NULL 性质、长度、内容、列名）、NULL 与空串不混、内嵌 `'\0'` 的 BLOB、空结果集也有列名、`mc_stmt_each` 与 `mc_stmt_rows` 的文本输出相同、`on_cols` 与行回调的调用次数、提前停止后丢弃、丢弃之后仍能取到完整的两行、返回结果集时不写 `affected` |
| 复用 | 12 | 同一条 `INSERT` 执行 50 次、每次 `affected` 都是 1、表里正好 50 行且 `MIN` / `MAX` 是 0 / 49、主键冲突 → `QUERY` 且报在 `execute:` 环节、失败之后同一对象仍可执行并存进 1 行 |
| 空操作与边界 | 11 | 七个函数在句柄为 NULL 时的返回值、`params` 为 NULL 而 `n` 非 0 → `PARAM`、无参数语句用 `n == 0` + `params == NULL` 执行 |

驱动层 2c 组 13 项：

| 用例 | 判据 |
|---|---|
| 带参数取数 | `stmt` 打印 `SELECT ? AS a, ? AS b, NULL AS c`，与 `sql` 跑等价字面量的输出 `cmp` **逐字节**相同 |
| 无参数语句 | `SELECT 1+1` 出 `2` |
| 空结果集 | 只打表头 |
| `-r 5` 写入 | 退出码 0、stderr 含"5 次执行"、表里多 5 行 |
| `-r 3` 查结果集 | 打出 3 份表头 + 3 行数据共 6 行——上一次的结果集若没取尽，第二次 execute 会撞上 `Commands out of sync` |
| 占位符个数不符 | 退出码 1、信息含 `placeholder` |
| `-r` 用于 `sql` | 退出码 2、提示里出现 `stmt` |

"逐字节相同"这条是本节最重要的判据：语句对象走的是**二进制协议**，
`sql` 走文本协议，两条路径在 NULL、空串、数字格式化、表头时机上任何一点
不一致都会被 `cmp` 抓住。行数、行内容各自相等不代表字节相同。

三条注入（前两条让 `test_stmt` 变红，第三条没有）：

| 注入 | `test_stmt` | `check_live` |
|---|---|---|
| 去掉 `!st->consumed` 判据（结果集可以取第二遍） | 败 2 项 | 0 |
| `affected` 的写入不再判 `st->cols == 0` | 败 1 项 | 0 |
| 删掉 `stmt_discard_result(st)` 调用 | **0** | 0 |

第一条失败的是"结果集取第二遍返回 STATE"与"错误信息说明已消费"。
去掉判据之后第二遍取数走的是 `free_result` 之后的空结果集，
返回 `MC_OK` 与 0 行——这正是"0 行"和"已经取过"必须分开报告的理由。
第二条失败的是"执行带参数的查询"，`affected` 实际被写成 7、期望 0。

第三条**没有**变红：8.0.46 的 `mysql_stmt_execute` 自己会丢弃未读完的结果集，
显式 `free_result` 与依赖库的实现在这里没有行为差异。那组断言
（"没读完就再执行，之后仍能取到完整两行"）证明的是**复用不被半读破坏**，
不是"显式 `free_result` 被执行"。两件事只能靠读实现代码区分，
所以记在 3.17，而不是声称 100 项覆盖了它。

---

## 6 性能

`make bench`，每格取 5 轮最小值（排除调度抖动），`/tmp` 下的普通文件：

```
      size  write(ms)    W(MB/s)   read(ms)        R(MB/s)
--------------------------------------------------------------
     1024K       0.59     1702.4       0.29         3498.1
     8192K      13.80      579.6       2.26         3547.3
    65536K     157.69      405.9      18.96         3375.9
```

读比写快一个数量级，因为数据在页缓存里：写要经过"用户缓冲区 → 页缓存"的拷贝，
而且 64 MiB 那次已经超过脏页回写的速率，被内核限流。这些数字说明的是
**本地搬运不是瓶颈**，不是磁盘的极限带宽。

### 6.1 与网络侧的量级对比

一个 64 KiB 的 BLOB 走本模块的完整路径：

| 环节 | 次数 |
|---|---|
| `mc_file_read`（本地） | 1 次 `fread` 序列，约 0.02 ms |
| `mysql_stmt_send_long_data`（分块 16 KiB） | 4 次 |
| `mysql_stmt_fetch_column`（读回，整段一次） | **1 次**（原实现 65536 次） |
| `mc_file_write`（本地，临时文件 + rename） | 1 次写入 + 1 次 `rename` |

原实现的 65536 次 `fetch_column` 是本模块最值得修的性能问题：
每次调用都是一次函数调用加一次内存拷贝，虽然总数据量只有 64 KiB，
但调用开销把它变成了毫秒级操作。改完之后，同一次往返的耗时主要由网络往返
（`RTT`）决定，本地部分可以忽略。

### 6.2 结果集的内存放大

`mc_query` 把结果集全部取回，每行的每个非 NULL 单元格都额外占 1 字节（`'\0'`）。
一个有 10 万行、每行 10 个短字段的查询，数据本身约 5 MB，进程内存会涨到
6~7 MB 加上指针数组（10 万 × 10 × 8 B ≈ 8 MB）。

这个放大率对"取回来就处理完丢掉"的场景无所谓，对"一次导出百万行"就不合适了。
那种规模用 `mc_query_each`。

### 6.3 百万行：内存与吞吐

同一张 1048576 行的表（内容 20 846 464 字节），同一台机器（Ryzen 7 5700X，
服务端与客户端都在本机，走 loopback）：

| 方式 | 行数 | 峰值 RSS | 耗时 | 吞吐（按内容字节） |
|---|---|---|---|---|
| `mc_query`（物化） | 1048576 | 161.9 MiB | 292.5 ms | 71 MB/s |
| `mc_query_each`（游标） | 1048576 | 10.7 MiB | 211.8 ms | 99 MB/s |

**内存差 15.1 倍，且流式那一侧与行数无关**——10.7 MiB 里绝大部分是
`libmysqlclient` 自身的连接缓冲与代码段，把行数从 100 万加到 1000 万，
这个数字不会变。物化那一侧则是线性的：行数翻倍，RSS 翻倍。

流式反而快 28%，原因不是"流式协议更高效"，两条路径在网络上传输的字节完全相同。
省掉的是本地那一段：物化要为整个结果集分配 24 MB 指针数组 + 24 MB 长度数组 +
20 MB arena，再用 `memcpy` 把每个单元格拷进去；游标只维护每列一块缓冲区，
行与行之间复用，拷贝发生在库把值写进缓冲区的那一次。

所以这个接口的收益首先是内存，其次是顺带的吞吐——不要把它当成"并行取数"
之类的加速手段。

> 数字来自 `make check-scale`。它造数据用自连接倍增（20 条语句到 100 万行），
> 造完即删，不依赖外部 SQL 文件。

### 6.4 prepare 复用的实测

`test_stmt --bench N` 把两条路径交替跑 N 次，取各自总耗时：

| 路径 | 每次做什么 |
|---|---|
| A（`mc_query_each_ex`） | `stmt_init` → `prepare` → `execute` → 取尽 → `free_result` → `close` |
| B（`mc_stmt`） | prepare 一次；循环里 `execute` → 取尽 |

语句是只读的 `SELECT 1 AS a, 2 AS b`（A）与 `SELECT ? AS a, ? AS b`（B），
两边都返回一行两列。20000 次，四轮：

```
                 总耗时(ms)            折算每次
A（每次 prepare）  1409 ~ 1466          72.2 ~ 72.7 µs
B（prepare 一次）   589 ~  630          29.8 ~ 31.5 µs

四轮比值：2.31 / 2.43 / 2.33 / 2.40
```

折算到每一次，B 比 A 省 **41 ~ 43 µs**，比值稳定在 2.3 ~ 2.4。
省下的是每个语句一次 `COM_STMT_PREPARE` 往返加上服务端的一次解析——
B 只付一次。

这个数字**不是**"查询快了一倍"：两条路径执行的是同一类语句，服务端做的活一样，
差的是每次多出来的那趟往返。真正的查询耗时（索引、扫描、聚合）两边相同，
所以语句数越多、单语句越简单，比值越接近 2；单语句越重，比值越接近 1。

两版失败的基准值得记下来，因为它们各自说明了一个坑：

| 版本 | 测到的比值 | 原因 |
|---|---|---|
| InnoDB 单行 `INSERT` | 1.01 | 每次提交等一次 `fsync`（约 6 ms），比值里全是磁盘 |
| MEMORY 表单行 `INSERT` | 0.94 | 换引擎没用：等的是 **binlog** 的 `sync_binlog = 1`，同样每次一次 `fsync` |

第二版尤其容易误判——换掉存储引擎看起来已经排除了刷盘因素，实际上
`--log-bin` 开着的时候，任何写语句的提交都要等 binlog 落盘。
基准要测"prepare 省了几次往返"，就必须把写路径整个排除掉，只留只读语句。

> 数字来自 `make check-live` 跑完之后手工执行
> `MC_HOST=127.0.0.1 MC_USER=root ./test_stmt --bench 20000`。

---

## 7 实现记录：从单文件到模块

这个主题从一份 355 行的单文件示例 `mysql.c` 开始。原版能跑——连上库、
插入一行、删掉、把一张 JPEG 塞进 `LONGBLOB` 再读回来——但它的结构使得
任何一点改动都无法验证：`main` 写死六步流程，跑一次库里多一行、多一张图，
再跑一次又多一份；除了"看图能不能打开"，没有任何办法判断某一步是否成功。

### 7.1 结构上的改变

| 原版 `mysql.c` | 本模块 |
|---|---|
| 一个文件包含 SQL 宏、读写图片、BLOB 存取、`main` 流程 | `include/` + 4 个实现文件 + 驱动层 |
| 只能按写死的流程跑一遍 | 5 个子命令，可单独执行、可脚本化 |
| `main` 在每条失败路径上 `goto Exit` 后 `return 0` | 退出码 0 / 1 / 2 分层 |
| 调用方必须 `#include <mysql.h>` | `mc_conn` 不透明，头文件不出现 MySQL 类型 |
| 错误一律 `printf`，语句错误用连接级 `mysql_error` | `conn->errbuf` + 语句级 `mysql_stmt_error`，11 个状态码 |
| `#include "config.h"` 无条件 | `config.h` 存在才编进去，否则用内置默认值 |
| 手写 `gcc mysql.c -o bin/mysql` | Makefile 八个目标，零警告构建 |
| 无自动化测试 | `make test` 79 项 + `check` 33 项 + `check-live` 38 项 |

### 7.2 修掉的缺陷

| # | 缺陷 | 依据与后果 | 处理 |
|---|---|---|---|
| 1 | 连接凭据硬编码在源码里 | Git 历史不可撤销，事后删除无效 | 抽成 `config.h` + `.gitignore`，另加环境变量与命令行两层覆盖 |
| 2 | `read_image(char *filename, char *buffer)` 没有容量参数 | 函数不知道缓冲区多大，文件更长时越界写 | 增加 `cap`，读之前判断，超限返回 `TOOBIG` |
| 3 | 64 KiB 数组放在栈上 | 栈空间有限（Linux 默认 8 MB），调用层次一深就危险 | 改堆分配，驱动层统一 `free` |
| 4 | `mysql_read` 逐字节取数 | 64 KiB 需要 65536 次 `fetch_column` | 一次请求剩余长度，1 次调用 |
| 5 | `mysql_stmt_init` 返回值没检查 | 内存不足时返回 NULL，直接传给 `prepare` 会崩 | 两处都加 NULL 检查 |
| 6 | **`MYSQL_DATA_TRUNCATED` 与真错误混在一个 `break` 里** | 网络中断时函数返回 0 字节并"成功" | 三种返回值分别判断，真错误返回 `MC_ERR_STMT` |
| 7 | `main` 的所有失败路径都 `return 0` | 脚本无法感知失败，`&&` 后面的命令照常执行 | 退出码 0 / 1 / 2 |
| 8 | 多行结果的读回静默覆盖 | 循环每轮从头写同一块缓冲区，只剩最后一行，返回值却正常 | 单行单列契约，违反返回 `MC_ERR_RESULT` |
| 9 | 语句级错误用 `mysql_error(handle)` | 连接级错误信息对语句错误常返回空串，掩盖真实原因 | 统一 `mysql_stmt_error(stmt)`，并在 close 之前拷走 |
| 10 | `write_image` 用 `"wb+"` | 多出的 `+` 增加了"目标必须可读"的限制，代码里没有任何读操作 | 改 `"wb"` |
| 11 | `write_image` 不查 `fclose` | `ENOSPC` 在刷新时才报，写入失败被报告成成功 | 查 `fflush` / `fclose`，失败返回 `FILE_WRITE` |
| 12 | **写入直接截断目标文件** | `fopen("wb")` 先 truncate，此后失败就丢原内容 | 临时文件 + `rename` 原子替换 |
| 13 | 读文件依赖 `fseek` / `ftell` | FIFO 上 `ESPIPE`，`/proc` 伪文件报告长度 0 | 读到 EOF，填满后多读一字节判断越界 |
| 14 | 无条件 `#include "config.h"` | 新克隆的仓库编译必然失败 | `Makefile` 检测文件存在才定义 `MC_HAVE_CONFIG_H` |

第 6 条是这一组里最值得留意的：它**不触发任何 sanitizer**。所有缓冲区都在范围内，
没有越界、没有泄漏，`read_length` 也在合理范围内——只是语义错了。
这类缺陷只能靠"注入一次失败"或者"读一遍协议的返回值表"发现，
而这恰恰是 BLOB 代码最容易长期带着跑的一类问题。

第 8 条同源：多行结果的覆盖不会崩溃，只会返回一行看起来完全正常的数据。

第 12 条属于"失败时才暴露"的那一类：磁盘不满的时候一切正常。

### 7.3 接口取舍：为什么结果集不用 `char ***`

`cell` 用一维数组而不是二维指针数组，除了分配次数，还有一个理由：
`mc_rows` 需要能被 `memset` 清零后安全地 `free` 一次（失败路径上就是这样用的）。
二维指针数组释放时必须先逐行 `free` 内层，中途失败的行数不确定，
清理代码要么多一个计数器，要么在 `free` 时判断每个指针。

### 7.4 第二轮：给流式接口补列名

第一轮（7.1 ~ 7.3）把单文件改成了模块，`mc_query_each` 的签名是
`(ctx, cell, length, cols)`——列名不在其中，于是驱动层 `each` 打不出表头，
而 `sql` 能打。两个子命令的输出因此不能整体比对，脚本里要先去掉表头那一行。

这一轮的改动只有一条线：列名怎么交。结论是单独一个 `mc_cols_fn`，
在第一行之前调一次；详细取舍见 3.15。顺带把三件相关的事一起做了：

- `mc_query_each` 保留原签名，实现改成 `mc_query_each_ex` 的一行包装。
  旧调用方（含 `tests/test_stream.c` 里的逐格比对、`check_scale.sh` 的内存测量）
  一行都不用改，`on_cols == NULL` 时连列名拷贝都不发生。
- 驱动层 `each` 打表头，`check_live.sh` 的判据从"去掉表头再 `cmp`"升级成
  整份 `cmp`——判据变强了：表头的字节、列序、0 行时是否打表头都在断言范围内。
- 新增 `make sanitize-live`。以前"My 插桩版也跑过真实数据库"这件事只写在
  README 里，靠人记着手工执行；现在是能重复的目标，且 `OUT` 覆盖成
  `bin/sanitize`，不会与普通产物混在一起。

代价是多了一个 6 参数的函数和一个回调类型。这个代价换回来的是：
流式取数第一次能做"带表头的 CSV 导出"，而不必退回物化取数——
后者在百万行上是几百 MB 的峰值内存（6.3 节的实测）。

### 7.5 第三轮：通用参数绑定

前两轮之后，`mc_exec` 与 `mc_blob_write` 是两条互不相干的写路径：
前者完全没有参数，后者只认一个 `?` 且固定按长字段发。要写
"INSERT 一行四个不同类型的值"就只能拼字符串，而那意味着 SQL 注入面
（8.4 当时是这么记的）。

这一轮的改动是补上 `mc_exec_params`（3.16 记录了四个实现上的坑），
并把 `mc_blob_write` 收编成 `n == 1` 的包装。收益与代价不对称：

- 收益：长字段分块只有一份实现；`mc_blob_write` 顺带获得占位符个数检查，
  修掉了一个"`sql` 里有两个 `?` 时读越界"的既有隐患。
- 代价：`mc_blob_write` 的错误码有一处变化（多占位符从 `MC_ERR_STMT`
  变成 `MC_ERR_PARAM`），语义上更准确，但与旧行为不同。

三处实现细节值得记下来，因为它们都是"看着对、跑起来不对"：
`bind[i] = s->bind` 漏掉时所有值变成 0（`buffer_type == 0` 是
`MYSQL_TYPE_DECIMAL`）；`is_unsigned` 在参数方向上看指针是否存在而不看值；
零字节参数的 `buffer` 不能是 `NULL`。三条都是实测发现的，
没有一条能靠读文档避免。

驱动层同步加了 `exec-params` 子命令：`-a TYPE:VALUE` 可重复，六种类型，
blob 写作 `@路径`。`blob-put` 的文件读取逻辑一并抽成 `load_file()`，
两处共用。`-a` 与 `-n` 一样只对特定子命令有效，用在别处直接报用法错误。

### 7.6 第四轮：语句对象

第三轮之后 8.4 记的是"参数绑定只覆盖不返回结果集的语句"。
这一轮把查询方向也补上了，形状是 `mc_stmt` 而不是
`mc_query_params`——理由在 3.17，这里记实现过程。

先做的是取数的复用。`mc_query_each_ex` 原来把
`bind_result` → 交付列名 → 循环 fetch 这三十行写在函数体里，
语句对象要用完全一样的一段。抽成 `stream_deliver(conn, stmt, on_cols, fn, ctx, out_rows)`
之后两条路径共用一份实现：截断处理、列名时机、NULL 与空串的区分
不会在两条路径上各自演进——这正是"两条路径、一个语义"最容易分叉的地方。

状态用两个布尔量表达（`executed` / `consumed`），而不是靠
`mysql_stmt_field_count()` 之类的库内状态去推断。理由是库不知道
"调用方是否还指望那份结果集"：`free_result` 之后 `field_count` 仍然是 2，
照它判断就会把"已经取过"误判成"可以再取"，交付一个 0 行的空结果集。
现在这条路径返回 `MC_ERR_STATE`，错误信息里带 `consumed` 一词。

三处"注入才看得出"的地方：

- 删掉 `mc_stmt_execute` 里的 `stmt_discard_result`，100 项仍全绿。
  8.0.46 的 `mysql_stmt_execute` 自己会丢弃未读完的结果集，
  所以这里没有可观测差异。保留这次调用是因为它把"丢弃"写成了显式意图，
  而不是依赖库的实现细节——但断言不能声称覆盖了它（3.17）。
- 去掉 `affected` 只在 `st->cols == 0` 时写入这个判据，
  一条断言立刻变红：返回结果集的查询会把上一次写语句的 `affected`
  留在调用方的变量里，看起来"查询影响了 7 行"。
- 去掉 `!st->consumed` 判据，两条断言变红（第二遍取数、错误信息）。

驱动层的 `stmt` 子命令是这一轮的验收工具：`-r N` 是唯一能测"prepare 一次、
执行 N 次"的参数，而它的判据落在**输出字节**上——`stmt` 与 `sql` 跑同一份
数据，用 `cmp` 比整份输出（5.10）。`-r` 只对 `stmt` 有效，用在 `sql` 上
退出码 2，与 `-a` / `-n` 的处理一致。

代价是多了一个不透明类型和七个函数，以及 8.4 划的那条边界向后挪了位置：
现在"查询方向要传值"有解了，但解法的形状是语句对象，不是给
`mc_query` 加参数。

---

## 8 已知局限

### 8.1 `mc_query_each` 不能回退

游标接口按定义只能向前：读过的行不留在任何地方，`mysql_data_seek` 这类回退
在二进制协议上也不可用。需要两遍遍历（先算总数再分配、或者先统计后输出）
就只能用 `mc_query`，代价是内存与行数成正比。

列名不在这个限制里：`mc_query_each_ex` 的 `on_cols` 在第一行之前就把整份
列名交给调用方，取数过程中不需要为此多读一遍（见 2.4、3.15）。

### 8.2 一个连接一个线程

`mc_conn` 内部不加锁，同一个句柄不能被多个线程同时使用（这与
`libmysqlclient` 的要求一致：`mysql_real_query` 等函数对同一连接不是线程安全的）。
不同连接之间可以并行使用，但多线程程序在首次使用前应当调用
`mysql_library_init` / `mysql_thread_init`，本模块没有替调用方做这件事——
它属于进程级的初始化，不该藏在"打开一个连接"里面。

### 8.3 明文密码留在内存里，不清零

`mc_options` 与 `mc_conn` 都按值保存密码，`mc_close` 之后随 `free` 释放，
但内存里的字节不会主动清零。`explicit_bzero`（glibc 2.25+）或
`memset_s` 可以做到，代价是与平台绑定；本模块暂时不做，
因此"密码在进程内存里可被 core dump 带出"这个风险依然存在。

### 8.4 参数绑定分在两条路径上

`mc_exec_params` 只用于不返回结果集的语句（返回结果集时 `MC_ERR_RESULT`）；
查询方向要传值得用 `mc_stmt`（2.6）——`mc_query` 与 `mc_query_each_ex`
的 `sql` 仍然只能是字面量。

这不是遗漏，是接口形状决定的：`mc_query` 的签名是"给我 SQL，还你结果集"，
把参数数组塞进去之后还要决定"参数在返回之后还有没有效"、"绑定失败算哪一步"，
而这些问题在有语句对象时都有唯一答案（参数只在 `mc_stmt_execute` 期间被读）。
所以现在的关系是：

| 场景 | 接口 |
|---|---|
| 无参数的查询 | `mc_query` / `mc_query_each_ex`，一次调用完成 |
| 有参数的写语句 | `mc_exec_params` |
| 有参数的查询、或同一语句要执行多次 | `mc_stmt_prepare` + `mc_stmt_execute` |

仍然局限的是 `mc_stmt` 不做连接池、不做语句缓存，也没有 `mc_stmt_reset`
之类的"清掉绑定状态"接口；每次 `mc_stmt_execute` 都要重新给全 `params`
（不允许"这次只改第 2 个参数"），因为部分绑定会让"哪些槽位是上一轮的值"
变成隐式状态。

### 8.5 只支持 MySQL 8.0 及以上

`MYSQL_BIND::is_null` 的类型是 `bool *`（8.0.1 起）。5.7 是 `my_bool *`，
本模块不做版本条件编译。

### 8.6 `mc_file_write` 不支持管道与设备

原子替换依赖 `rename`，对 `/dev/stdout` 这类路径只会失败。
需要写管道时自行 `fwrite`，或者先写临时文件再 `cat`。

### 8.7 没有事务控制接口

`mc_exec` 可以执行 `BEGIN` / `COMMIT`，但没有 `mc_begin` / `mc_commit` 这类
封装，也没有"出错自动回滚"的语义。BLOB 写入是一条语句，本身是原子的；
多条语句的组合需要调用方自己管。

### 8.8 连接池、重连、超时重试都没有

`mc_ping` 只做探测，不自动重连（8.0 起 `MYSQL_OPT_RECONNECT` 默认关闭，
反手重连会让调用方以为连接一直有效，而事务、临时表、会话变量其实都丢了）。
断线之后要重新 `mc_open`。

### 8.9 `mc_file_write` 只保留权限位，其余元数据都会丢

3.14 节只搬了 `0777` 三段权限位。`rename` 换的是 inode，
下面这些元数据随旧 inode 一起消失，新 inode 用的是默认值：

| 元数据 | 覆盖后的结果 |
|---|---|
| 属主 | 变成执行写入的进程的有效 uid（只有 root 能改回原值） |
| 属组 | 目录的 setgid 位起作用时随目录，否则跟随写入进程 |
| `setuid` / `setgid` | 刻意不保留（3.14） |
| ACL、扩展属性 | 不保留 |
| 硬链接 | 其他链接仍指向旧 inode，旧内容还在，两条路径从此分叉 |

第二条在共享目录（`chmod g+s`）下是符合预期的；第一条只在"替别人写文件"时
才会被注意到，而那种场景通常伴随权限问题，不属于本模块的定位。

另外两处行为差异一并记在这里：

- `path` 是符号链接时，`stat` 跟随链接取权限，但 `rename` 替换的是**链接本身**：
  写入之后 `path` 变成一个普通文件，链接指向的原文件保持不动。
  这与 `fopen(path, "wb")` 写穿透的行为不同。
- 上述所有"保留"都是 best-effort：`stat` 失败（例如只有目标文件可读、
  目录不可搜索）时按新建文件处理，权限回落到 `0666 & ~umask`。

---

## 9 待办

- [x] 回调里给出列名：做成了 `mc_query_each_ex` 的 `on_cols` 回调（3.15），
      `mc_query_each` 保留为 `on_cols == NULL` 的包装，旧调用方不用改
- [x] 通用的参数绑定：做成了 `mc_exec_params(conn, sql, mc_param[], n)`，
      六种 kind（null / int / uint / double / text / blob），
      超长字段走 `send_long_data`（3.16），`mc_blob_write` 改为它的特例
- [x] `mc_file_write` 保留权限位：`rename` 覆盖会把目标文件的权限改成临时文件的
      （`0666 & ~umask`），导出到已有文件时这一点会改变使用者预期（3.14 已完成；
      属主、ACL 等仍然不保留，见 8.9）
- [ ] 连接字符串解析：`mysql://user:pass@host:port/db`，省掉五项参数
- [ ] `check_live.sh` 加一组并发用例：两个连接同时写同一张表，
      验证模块在"多连接"这个维度上不出错
- [ ] 密码清零：`explicit_bzero` 版本，用 `#if defined(__GLIBC__)` 包起来
- [ ] 把 `MC_BLOB_CHUNK` 做成可配置：现在的 16 KiB 是按"小于 MTU 的整数倍"选的，
      没有实测过不同块大小的吞吐差异
- [x] 查询方向的参数绑定：做成了 `mc_stmt` 语句对象（2.6 / 3.17），
      查询与写语句共用一条取数路径（`stream_deliver`），
      prepare 一次可执行多次（6.4 的实测：每次省 41 ~ 43 µs）
- [ ] 参数数组的个数校验提前到编译期：目前 `n` 与占位符个数在运行期比对，
      而 `sql` 是字面量时编译器本可以数出 `?` 的个数（需要在字符串里跳过
      引号与注释，只能做启发式——暂不做）
- [ ] `mc_stmt` 的部分绑定：现在每次 execute 都要给全 `params`。
      做"只改第 2 个参数"需要清掉哪些槽位是上一轮值的隐式状态，
      先想清楚语义（见 8.4）
