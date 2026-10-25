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
│   └── mysqlclient.h     对外接口：状态码、连接、结果集、BLOB、文件辅助
├── src/
│   ├── mysqlclient.c     数据库访问层（唯一 Include <mysql.h> 的编译单元）
│   ├── options.c         连接参数：初始化与校验
│   ├── file.c            文件读写（不访问数据库）
│   ├── status.c          状态码 → 可读描述
│   └── main.c            命令行驱动（子命令、参数解析、输出排版、退出码）
├── tests/
│   ├── test_pure.c       纯逻辑单元测试，62 项，不链接 libmysqlclient
│   ├── test_db.c         接口层测试，20 项，含真实建连失败路径
│   ├── check_cli.sh      命令行端到端检查，41 项，不需要数据库
│   ├── check_live.sh     真实数据库往返验证，48 项，需要数据库
│   ├── test_stream.c     流式取数，正确性 24 项 / 内存测量，需要数据库
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

mc_status mc_query_each(mc_conn *conn, const char *sql,
                        mc_row_fn fn, void *ctx, size_t *out_rows);
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

三条约束：

- 取数期间这条连接被该语句占用。回调里对同一个 `conn` 再发查询，
  `libmysqlclient` 会报 `Commands out of sync`——协议是半双工的，
  结果集没读完就发下一条命令属于违例。要并发就用另一条连接。
- 不能回退。读过的行不留在任何地方，要两遍遍历只能用 `mc_query`。
- 回调拿不到列名。需要表头时用 `mc_query`，或者自己拼一行。

### 2.5 大字段

```c
mc_status mc_blob_write(mc_conn *conn, const char *sql,
                        const void *data, size_t len);
mc_status mc_blob_read(mc_conn *conn, const char *sql,
                       void *buf, size_t cap, size_t *out_len);
```

`mc_blob_write` 把 `data[0..len)` 绑到 `sql` 的第一个 `?` 上，分块发送。

`mc_blob_read` 的契约是三条，缺一不可，违反时返回 `MC_ERR_RESULT`：

- 结果集恰好**一列**；
- 结果集恰好**一行**（多行返回 `MC_ERR_RESULT`，零行返回 `MC_ERR_NODATA`）；
- 该列不是 SQL NULL（NULL 也返回 `MC_ERR_NODATA`）。

比"取到几行算几行"严格，是因为被覆盖的那几行不会留下任何痕迹：
循环体每轮都从头写同一块缓冲区，最后只剩下最后一行的内容，而函数看起来是成功的。

### 2.6 文件辅助

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

### 2.7 驱动层

```
Usage: mysqlclient [OPTIONS] COMMAND [COMMAND-OPTIONS]

Commands:
  ping                      与服务器握一次手（COM_PING）
  sql   -q SQL              执行查询，结果按制表符分隔打印到 stdout
  each  -q SQL [-n N]       逐行取数并打印，内存占用与行数无关
  exec  -q SQL              执行不返回结果集的语句，打印受影响行数
  blob-put -q SQL -f FILE   把 FILE 的字节写入 SQL 的第一个 ? 参数
  blob-get -q SQL -o FILE   把单行单列的查询结果写成 FILE

Options:
  -H HOST  -P PORT  -u USER  -p PASS  -d DB  -s BYTES  -h
```

退出码三档：`0` 成功，`1` 运行期失败，`2` 用法错误。
分成两档（而不是"非 0 即失败"）是为了让脚本能区分"参数写错了"与"操作失败了"。

`sql` 与 `each` 的输出只在表头上不同：`each` 不打印列名（回调拿不到），
**行的内容逐字节相同**，`check_live.sh` 用 `cmp` 比对这一点。
`-n` 只对 `each` 有效，别的子命令用了它直接报用法错误——
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

---

## 4 构建

```bash
make              # 静态库 bin/libmysqlclient.a 与命令行工具 bin/mysqlclient
make lib          # 只构建静态库
make cli          # 只构建命令行工具
make test         # 纯逻辑 99 项 + 接口层 20 项
make check        # 命令行端到端 41 项（不需要数据库）
make check-live   # 真实数据库往返 50 项 + 流式比对 24 项（需要数据库，见下）
make check-scale  # 百万行下的峰值内存对照（需要数据库，约 30 秒）
make sanitize     # ASan + UBSan 重新构建并跑测试，产物在 bin/sanitize/
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

`make test`，119 项：

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

mysqlclient 接口层测试                ← 20 项
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

`make check-live`，50 项。需要一台连得上的 MySQL/MariaDB：

```bash
MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root MC_PASSWORD= make check-live
```

脚本自建、自删一个测试库，覆盖：

| 组 | 内容 |
|---|---|
| 文本 | UTF-8 原样取回、空串与 NULL 打印成不同的东西、列名来自服务器 |
| 流式 | `each` 与 `sql` 的行内容 `cmp` 逐字节相同（表头除外）、`-n 1` 只出一行 |
| BLOB 往返 | 64 KiB / 5 MiB / 0 字节，三种长度都 `cmp` 逐字节相同 |
| 权限位 | `blob-get` 覆盖一个已存在的 `0600` 文件，事后 `stat` 仍是 `600` |
| 容量 | `-s 1000` 读 5 MiB → 退出码 1、报出 `5242880 bytes`、**不产生输出文件** |
| 多行 | `blob-get` 收到 3 行 → `more than one row` |
| NULL 列 | `blob-get` 命中 NULL → `SQL NULL` |
| 无行 | `blob-get` 查不到 → `query returned no row` |
| 语句错误 | 表不存在 → `query`；用 `sql` / `each` 跑 UPDATE → 提示改用 `mc_exec` |
| 绑定 | 含单引号的文本经 SQL 字面量写入后原样取回 |

5 MiB 那一项同时验证了分块协议：`MC_BLOB_CHUNK` 是 16 KiB，5 MiB 要 320 次
`send_long_data`，读回则是一次 `fetch_column`。

`check_live` 不并进 `make check`：没有数据库的机器上它必然全红，
那种"永远失败的目标"等于没有目标。

### 5.5 流式取数（游标）

`test_stream` 24 项，同样由 `make check-live` 一并运行。七行数据覆盖：
普通文本 / 空串 / 全 NULL / 内含 `'\0'` 的二进制 / 100 KiB 的值 /
UTF-8 多字节文本 / 负数与 `INT` 上界。

| 用例组 | 内容 |
|---|---|
| 逐格比对 | 同一结果集分别经 `mc_query` 与 `mc_query_each`，比对 NULL 性质、长度、内容，全等 |
| 长值 | 100 KiB 那一行的长度逐字节核对（它必然走 `fetch_column` 分支） |
| 提前停止 | 回调在第 3 行返回非 0 → `MC_OK`，`out_rows == 3` |
| 连接复用 | 提前停止后同一连接执行 `SELECT 1` 仍然成功（验证 `free_result` 确实排空了结果集） |
| 空结果集 | 0 行是成功，`out_rows == 0` |
| 无结果集 | `UPDATE` → `MC_ERR_RESULT`，提示改用 `mc_exec` |
| 语法错误 | 在 `prepare` 阶段被拒 → `MC_ERR_STMT`，`out_rows` 为 0 |
| 参数 | 回调为 NULL / SQL 为空 → `MC_ERR_PARAM` |

"连接复用"这一条是本节的重点。它验的不是输出对不对，而是**协议状态有没有收拾干净**：
少一次 `mysql_stmt_free_result`（或把语句句柄直接 `close` 掉而依赖内部实现），
这里就会得到 `Commands out of sync`。

### 5.6 百万行下的峰值内存

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

### 5.7 消毒剂

`make sanitize` 跑 99 + 20 项，再用 ASan 版本跑一遍 50 + 24 项实测：

```
--- 纯逻辑测试（ASan + UBSan）---   99 项检查，0 项失败
--- 接口测试（ASan + UBSan）---     20 项检查，0 项失败
--- check_live（ASan + LSan）---    检查项 50，失败 0
--- test_stream（ASan + LSan）---   24 项检查，0 项失败
```

没有 ASan / UBSan 报告，也没有 LeakSanitizer 报告。后者是有意义的：
`mc_rows` 的 arena、每条语句的 `MYSQL_STMT`、连接句柄在三条失败路径上
都要求释放，漏一处就会在 5 MiB 那一轮被放大成几 MB 的泄漏。
流式路径上的泄漏点更密：每列一块缓冲区、四块指针数组，
外加"回调提前停止"这条额外的退出路径——它在 5.5 节的用例里被走到。

### 5.8 全仓

```
$ make test
==> concurrent-lock   68 项检查，0 项失败
==> contact-list     136 项检查，0 项失败
==> mysql-client     119 项检查（99 + 20），0 项失败
==> thread-pool       54 项检查，0 项失败
==> word-count        40 项检查，0 项失败
合计 417 项，0 项失败
```

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

---

## 8 已知局限

### 8.1 `mc_query_each` 不能回退，也拿不到列名

游标接口按定义只能向前：读过的行不留在任何地方，`mysql_data_seek` 这类回退
在二进制协议上也不可用。需要两遍遍历（先算总数再分配、或者先统计后输出）
就只能用 `mc_query`，代价是内存与行数成正比。

回调只拿到单元格与长度，没有列名。要做"带表头的 CSV 导出"，
目前得先用 `mc_query` 拿 `names`，或者自己拼一行表头。
把列名加进回调签名是可行的（元数据在 `prepare` 之后就有），
但会让签名变成五个参数，暂时没有做。

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

### 8.4 `mc_exec` 的 SQL 是非参数化的

`mc_exec` 直接 `mysql_real_query` 一段字符串。若调用方用 `snprintf` 把用户输入
拼进去，就有 SQL 注入——与 `mysql_real_query` 本身是否安全无关，
问题在拼接。需要传值的场景应当走参数化路径（`mc_blob_write` 是其中的特例）。
本模块不提供通用的"多参数绑定"接口，因为每个参数的类型都要单独声明，
把它做成变参函数只会把类型错误从编译期推到运行期。

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

- [ ] 回调里给出列名：`mc_query_each` 的签名加一个 `const char *const *names`，
      或者单独一个 `mc_query_names(conn, sql, char ***)` 预先取元数据
- [ ] 通用的参数绑定：`mc_exec_params(conn, sql, mc_param[], n)`，
      支持 int / string / blob / NULL 四类
- [x] `mc_file_write` 保留权限位：`rename` 覆盖会把目标文件的权限改成临时文件的
      （`0666 & ~umask`），导出到已有文件时这一点会改变使用者预期（3.14 已完成；
      属主、ACL 等仍然不保留，见 8.9）
- [ ] 连接字符串解析：`mysql://user:pass@host:port/db`，省掉五项参数
- [ ] `check_live.sh` 加一组并发用例：两个连接同时写同一张表，
      验证模块在"多连接"这个维度上不出错
- [ ] 密码清零：`explicit_bzero` 版本，用 `#if defined(__GLIBC__)` 包起来
- [ ] 把 `MC_BLOB_CHUNK` 做成可配置：现在的 16 KiB 是按"小于 MTU 的整数倍"选的，
      没有实测过不同块大小的吞吐差异
- [ ] `mc_query_each` 的流式结果集不支持参数绑定：
      目前 `sql` 只能是字面量，要传值仍得自己拼字符串（见 8.4）
