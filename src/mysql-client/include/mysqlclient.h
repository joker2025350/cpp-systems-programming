/*
 * mysqlclient.h —— MySQL 客户端模块的公开接口
 *
 * 设计目标：调用方使用本模块时**不需要 Include <mysql.h>**，
 * 也不需要链接位置正确的 libmysqlclient。
 *
 * 做法是把 MYSQL 句柄藏进不透明类型 mc_conn 里（见下），
 * 头文件因此不出现任何 MySQL 的类型、宏与结构体。
 * 这一条不是洁癖：libmysqlclient 的头文件会把 my_bool、MYSQL、
 * MYSQL_BIND 等名字塞进全局命名空间，而它们的定义随 8.0 前后的版本
 * 变化（my_bool 已在 8.0.1 起被移除）。调用方一旦 Include 它，
 * 就绑定了具体的库版本。接口层不暴露它，升级库时调用方不用重编。
 *
 * 名词约定：
 *   连接（connection）  一次到服务器的会话，由 mc_conn 表示。
 *   语句（statement）   服务器端预编译的 SQL，带 ? 参数占位符。
 *   结果集（result set）一条查询返回的行与列，由 mc_rows 承载。
 */

#ifndef MYSQLCLIENT_H
#define MYSQLCLIENT_H

#include <stddef.h>

/* ------------------------------------------------------------------
 * 容量常量
 *
 * 公开成宏而不是写死在实现里，是为了让调用方能直接用它们声明缓冲区。
 * 硬编码一个数字的后果是模块扩容之后调用方的缓冲区悄悄变小，
 * 而编译器不会给任何提示。
 * ------------------------------------------------------------------ */
#define MC_HOST_MAX      128   /* 含 '\0' */
#define MC_USER_MAX       64   /* 含 '\0' */
#define MC_PASSWORD_MAX  128   /* 含 '\0' */
#define MC_DBNAME_MAX     64   /* 含 '\0' */

/* send_long_data 的分块大小。16 KiB 接近一个以太网 MTU 下的
 * 合理报文尺寸，且远小于任何默认 max_allowed_packet（8.0 为 64 MiB）。 */
#define MC_BLOB_CHUNK  16384

/* ------------------------------------------------------------------
 * 状态码
 *
 * 沿用 C 的惯例：MC_OK 为 0，非 0 表示失败，因此可以直接写 if (st)。
 *
 * 文件类错误与数据库类错误分开编号，是因为驱动层要给出不同的提示：
 * "文件打不开" 与 "连不上服务器" 的处置方式完全不同，
 * 合并成一个笼统的失败码会让使用者无从下手。
 * ------------------------------------------------------------------ */
typedef enum {
    MC_OK             = 0,   /* 成功 */
    MC_ERR_PARAM      = 1,   /* 参数非法：NULL、空串、端口越界等 */
    MC_ERR_NOMEM      = 2,   /* 内存分配失败 */
    MC_ERR_FILE_OPEN  = 3,   /* 文件无法打开（保留 errno） */
    MC_ERR_FILE_READ  = 4,   /* 读文件过程中出错（保留 errno） */
    MC_ERR_FILE_WRITE = 5,   /* 写文件过程中出错（保留 errno） */
    MC_ERR_CONNECT    = 6,   /* 建连或握手失败 */
    MC_ERR_QUERY      = 7,   /* SQL 执行失败（语法错误、权限、表不存在……） */
    MC_ERR_RESULT     = 8,   /* 结果集的行列数与接口约定不符 */
    MC_ERR_STMT       = 9,   /* 语句句柄的创建、绑定、取数环节失败 */
    MC_ERR_TOOBIG     = 10,  /* 数据超出调用方给出的缓冲区容量 */
    MC_ERR_NODATA     = 11,  /* 期望有一行数据，实际没有（或该列为 SQL NULL） */
    MC_ERR_STATE      = 12   /* 对象状态不允许这次操作：未执行就取数、
                                结果集已经消费过 */
} mc_status;

/* ------------------------------------------------------------------
 * 连接参数
 *
 * 全部按值拷贝进结构体，模块不持有指针，调用方可以放心让 opt 出作用域。
 * 密码同样是拷贝：这是本模块唯一存储明文密码的地方，mc_close 之后
 * 随 mc_conn 一起被释放（不清零，见 README"已知局限"）。
 * ------------------------------------------------------------------ */
typedef struct {
    char         host[MC_HOST_MAX];
    char         user[MC_USER_MAX];
    char         password[MC_PASSWORD_MAX];
    char         database[MC_DBNAME_MAX];
    unsigned int port;               /* 0 表示由调用方决定，本模块不接受 0 */
    unsigned int connect_timeout;    /* 秒；0 表示用库的默认值 */
} mc_options;

/* ------------------------------------------------------------------
 * 连接句柄（不透明）
 *
 * 只声明不定义：sizeof(mc_conn) 在模块之外不可知，调用方只能在栈上放
 * 一个指针。这样内部结构（MYSQL* 加一段错误信息缓冲）随时可以改，
 * 不需要调用方重编。
 * ------------------------------------------------------------------ */
typedef struct mc_conn mc_conn;

/* ------------------------------------------------------------------
 * 结果集
 *
 * 行列都按行优先展开成一维数组，cell[row * cols + col]。
 * 用一维而不做 char ***，是因为二维指针数组要额外一次分配，
 * 释放时要先逐行 free 再 free 外层，出错路径多一倍。
 *
 * cell 里的字符串总是以 '\0' 结尾，便于直接 printf；
 * 但它**可能是二进制数据**（BLOB 列可以内含 '\0'），
 * 因此真实长度必须看 length，不能用 strlen。
 * SQL NULL 在 cell 里表现为 NULL 指针，length 为 0。
 * ------------------------------------------------------------------ */
typedef struct {
    size_t  rows;     /* 行数 */
    size_t  cols;     /* 列数 */
    char  **names;    /* cols 个，列名 */
    char  **cell;     /* rows * cols 个 */
    size_t *length;   /* rows * cols 个，字节长度 */

    /*
     * 以下两个字段供 mc_rows_free 使用，调用方不要读写。
     *
     * 放公开结构体而不是再包一层不透明类型，是一个权衡：单元格字符串
     * 全部连着放在一段内存里（一次 malloc 而不是每格一次），
     * 释放时需要这段内存的起始地址，而 cell[] 里的元素是这个块内部的偏移，
     * 无法反推出起点——第 0 行第 0 列为 SQL NULL 时 cell[0] 就是 NULL。
     * 为一个纯内部用途引入完整的不透明类型，会让调用方连
     * cell[row * cols + col] 这句都要改成函数调用，不划算。
     */
    char   *arena;      /* 单元格字符串的连续内存 */
    char   *name_arena; /* 列名的连续内存 */
} mc_rows;

/* 取单元格的便捷写法。不做越界检查，调用方自己用 rows / cols 把关。 */
#define MC_CELL(rset, i, j)  ((rset)->cell[(i) * (rset)->cols + (j)])

/* ------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------ */

/* 把 opt 置成"全空、端口 3306、超时 0"。栈上分配之后必须先调用。 */
void mc_options_init(mc_options *opt);

/* 检查参数是否可用于建连：host / user 非空，port 在 1..65535。
 * database 允许为空——不指定库也是合法的连接。 */
mc_status mc_options_check(const mc_options *opt);

/* 建连。成功时 *out 指向新建的句柄，失败时 *out 被置为 NULL。
 * 调用方负责用 mc_close 释放。 */
mc_status mc_open(const mc_options *opt, mc_conn **out);

/* 关闭并释放句柄。传 NULL 是安全的空操作，可以无条件调用。 */
void mc_close(mc_conn *conn);

/* 最近一次失败的描述，形如 "select: Table 'X' doesn't exist"。
 * 保证不返回 NULL：没有错误时返回空串，因此可以直接 printf("%s", ...)。
 *
 * conn 为 NULL 时返回模块级的 thread-local 缓冲，用于建连失败——
 * 那时还没有句柄可以承载错误信息。因此 mc_open 失败后应当用
 * mc_error(NULL) 取原因，而不是 mc_error(*out)（*out 已经是 NULL）。 */
const char *mc_error(const mc_conn *conn);

/* 与服务器握一次手（COM_PING），用来判断连接是否还活着。
 * 连接空闲超过 wait_timeout 后服务器会单方面断开，
 * 直接发查询只会得到"server has gone away"，ping 能更早发现。 */
mc_status mc_ping(mc_conn *conn);

/* ------------------------------------------------------------------
 * SQL 执行
 * ------------------------------------------------------------------ */

/* 执行一条不返回结果集的语句（INSERT / UPDATE / DELETE / DDL）。
 * affected 可以为 NULL；非 NULL 时写入受影响行数。 */
mc_status mc_exec(mc_conn *conn, const char *sql, unsigned long long *affected);

/* 执行查询并把整个结果集取回内存。成功时调用方必须 mc_rows_free(out)。
 * 失败时 out 已被清零，free 一次也是安全的。
 *
 * 结果集全量驻留内存：内存占用与行数成正比。只需逐行处理、不回头遍历的
 * 场景（导出、计数、批量校验）应当用 mc_query_each，那里占用与行数无关。 */
mc_status mc_query(mc_conn *conn, const char *sql, mc_rows *out);

/* 释放 mc_rows 内部的全部内存，并把结构体清零。传 NULL 安全。 */
void mc_rows_free(mc_rows *rows);

/* ------------------------------------------------------------------
 * 流式取数（游标）
 *
 * 术语说明：结果集（result set）是服务器对一条查询的应答，由若干行组成；
 * 游标（cursor）指"每次只向前取一行、读完即丢弃"的取数方式。
 * 与之相对的是 mc_query 的物化（materialize）方式——先把全部行取到
 * 客户端内存里，再交给调用方遍历。
 *
 * 两者的差别只在内存：物化的峰值与行数成正比，游标的峰值只与
 * 最宽的那一行成正比。行数上百万时前者会占几百 MB，后者仍是几 MB。
 *
 * 回调签名里的 cell / length 与 mc_rows 里的同名成员语义完全一致：
 *   cell[j] == NULL      该列是 SQL NULL
 *   length[j]            该列的真实字节数（可能为 0，可能是 BLOB）
 *   cell[j][length[j]]   '\0'，因此非二进制列可以直接 printf
 * 区别是**生命周期**：这片内存在回调返回之后就被下一行覆盖，
 * 调用方不能保存指针，需要保留时必须自己拷贝。
 *
 * length 数组本身也属于"回调期间有效"，同样不能保存。
 *
 * 列名不走这个回调：它是结果集的属性而不是行的属性，因此在
 * mc_query_each_ex 里由单独的 mc_cols_fn 在交付第一行之前调用一次。
 * ------------------------------------------------------------------ */

/* 返回 0 继续取下一行，返回非 0 停止。 */
typedef int (*mc_row_fn)(void *ctx, const char *const *cell,
                         const size_t *length, size_t cols);

/* 列名回调：一次拿到整个结果集的列名，返回 0 继续取数，返回非 0 放弃取数。
 *
 * 单独成一个回调，而不是把 names 加进 mc_row_fn，理由是列名属于结果集、
 * 不属于行：它是服务器元数据的一部分，在取到第一行之前就已经完整。
 * 每行都传一遍既浪费，也表达不出"第一行之前就知道"这个事实——
 * 而"打印表头"恰好必须发生在第一行之前，用行回调实现就得先把第一行
 * 存在调用方那边等表头打完，为一个本来不存在的顺序依赖增加状态。
 *
 * 三条契约：
 *   - 语句执行成功就调用，**与结果集有几行无关**。0 行的结果集同样有列名，
 *     因为元数据来自服务器，不是从行里推断的。
 *   - names[j] 非 NULL 且以 '\0' 结尾，顺序与行回调里 cell[j] 一致；
 *     names 数组与其中的字符串在整个 mc_query_each_ex 调用期间都有效，
 *     所以行回调里可以继续读它们，不需要自己拷贝。
 *   - 返回非 0 表示调用方不需要这些行：函数随即返回 MC_OK，*out_rows 为 0，
 *     一行都不交付。这与行回调的"提前停止"是同一个语义——都是调用方
 *     主动放弃，不是错误，因此不能用错误码表示。
 */
typedef int (*mc_cols_fn)(void *ctx, const char *const *names, size_t cols);

/* 逐行取数：每一行都调用一次 fn，max 行内存占用与总行数无关。
 *
 * out_rows 可以为 NULL；非 NULL 时写入**已交付给回调的行数**，
 * 包括触发提前停止的那一行，以及中途失败之前已经交付的那些行——
 * 这些行确实已经被回调处理过，把它们算进去，调用方才能据以续算。
 * 函数在开始取数之前就写 0，因此失败时它也不会是未定义的值。
 *
 * 停止的两种来源要分清：
 *   - fn 返回非 0：这是调用方主动停止，函数返回 MC_OK；
 *   - 取数过程出错：返回 MC_ERR_STMT / MC_ERR_QUERY 等，此时
 *     *out_rows 小于结果集的真实行数。
 * 前者是正常路径（"只要前 N 条"），不能也用非 0 返回值表示。
 *
 * 语句不返回结果集（INSERT / UPDATE / DDL）时返回 MC_ERR_RESULT。
 *
 * 三条使用约束：
 *   1. 取数期间这条连接被该语句占用。回调里若对同一个 conn 再发查询，
 *      libmysqlclient 会报 "Commands out of sync"——协议上它是半双工的，
 *      结果集没读完就发下一条命令属于协议违例。需要并发就用另一条连接。
 *   2. 结果集是从连接上边读边处理的，没有 max_allowed_packet 之外的
 *      缓冲上限，因此慢回调会一直占着服务端的这条连接。
 *   3. 不支持回退：读过的行不会留在任何地方，需要两遍遍历就只能
 *      用 mc_query。
 */
mc_status mc_query_each(mc_conn *conn, const char *sql,
                        mc_row_fn fn, void *ctx, size_t *out_rows);

/* 同 mc_query_each，另外在交付第一行之前调用一次 on_cols 交付列名。
 *
 * on_cols 为 NULL 时行为与 mc_query_each 逐字节相同（连内存分配也不做）。
 * 列名是拷贝出来的：实现从服务器元数据里取出后放进自己的缓冲区，
 * 元数据对象随即释放，因此调用方看到的 names 不依赖 libmysqlclient
 * 的任何对象生命周期。
 *
 * 顺序固定为：prepare → execute → on_cols（若有）→ 逐行 fn。执行失败
 * （语法错、表不存在、权限不足）发生在 on_cols 之前，此时一行列名也
 * 不会交付——调用方看到一个回调都没被调用的失败，不会误以为表头已经
 * 打出去了。 */
mc_status mc_query_each_ex(mc_conn *conn, const char *sql,
                           mc_cols_fn on_cols, mc_row_fn fn, void *ctx,
                           size_t *out_rows);

/* ------------------------------------------------------------------
 * 参数绑定
 *
 * 术语：参数化语句（parameterized statement）指 SQL 文本里用 ?
 * 占位、值在执行之前单独绑定的写法。服务器先把语句编译成执行计划
 * （这一步叫 prepare），值在协议上是另一种报文，永远不会被当作 SQL
 * 文本解析。因此值里出现引号、反斜杠、注释符都不会改变语句结构——
 * 把值拼进 SQL 字符串则是另一回事：拼接发生在客户端，服务器看到的
 * 已经是"一条带值的语句"，值里带的引号就会闭合字符串常量，
 * 语句结构随之改变，这就是 SQL 注入（SQL injection）的成因。
 *
 * 值不拼字符串还带来两个性质：二进制数据（含 '\0'）不需要转义，
 * 同一个值绑定到不同类型的列上也不需要再考虑字面量的写法。
 * ------------------------------------------------------------------ */

/* 参数类型。取值的语义见下面 mc_param 的注释。 */
typedef enum {
    MC_PARAM_NULL   = 0,  /* SQL NULL，忽略 v */
    MC_PARAM_INT    = 1,  /* v.i，有符号 64 位 */
    MC_PARAM_UINT   = 2,  /* v.u，无符号 64 位 */
    MC_PARAM_DOUBLE = 3,  /* v.d，双精度浮点 */
    MC_PARAM_TEXT   = 4,  /* v.bytes，按文本发送（MYSQL_TYPE_STRING） */
    MC_PARAM_BLOB   = 5   /* v.bytes，按二进制发送（MYSQL_TYPE_BLOB） */
} mc_param_kind;

/*
 * 一个参数。
 *
 * kind 决定读联合体里的哪个成员——这是本结构唯一的隐性约定，
 * 也是设计上最弱的一处：C 没有"带标签的联合"的类型系统支持，
 * 编译器不会检查"kind 与 v 的成员是否匹配"。换成六个构造函数
 * （mc_param_int、mc_param_text……）可以把这条约定收进库里，
 * 代价是每个类型多一个函数、多一份文档，而且调用方仍然可能在
 * 栈上直接初始化结构体绕过它。这里选择直接暴露结构体，
 * 用指定初始化器写出来可读性足够：
 *
 *     mc_param p[2] = {
 *         { .kind = MC_PARAM_INT,  .v.i = 42 },
 *         { .kind = MC_PARAM_TEXT, .v.bytes = { "x", 1 } },
 *     };
 *
 * data 的**生命周期**：模块不拷贝数据，只把它绑上去。指针必须在
 * mc_exec_params 返回之前一直有效（包括其中的长字段分块发送阶段）。
 * 这一条与 mc_rows / 行回调的语义相反，用的时候容易记混，故写明。
 *
 * len 大于 MC_BLOB_CHUNK 的 TEXT / BLOB 参数改走 send_long_data：
 * 内容分块送出，服务端负责拼接。此时 len 仍按字节计，
 * 一次报文的上限由 max_allowed_packet 决定，与单次传入的长度无关。
 */
typedef struct {
    mc_param_kind kind;
    union {
        long long          i;
        unsigned long long u;
        double             d;
        struct {
            const void *data;   /* len 为 0 时可以是 NULL */
            size_t      len;    /* 字节数，不是字符数 */
        } bytes;
    } v;
} mc_param;

/* 执行一条带参数的、不返回结果集的语句（INSERT / UPDATE / DELETE / DDL）。
 *
 * params 的长度 n 必须与 SQL 里 ? 的个数相等，否则返回 MC_ERR_PARAM
 * 并在 mc_error 里给出两边的数字。不检查的后果不是报错而是绑定错位：
 * mysql_stmt_bind_param 按数组顺序填参，多给的被忽略、少给的读越界，
 * 两种都可能让 execute 成功，只是值落到了别的列上。
 *
 * n 为 0 且 params 为 NULL 是合法的（等价于 mc_exec，只是多走一次
 * prepare）。params 为 NULL 而 n 非 0 返回 MC_ERR_PARAM。
 *
 * kind 不在 mc_param_kind 之内、或 TEXT / BLOB 的 data 为 NULL 而 len
 * 非 0 时返回 MC_ERR_PARAM。这两个检查在 prepare 之前完成：
 * 它们与服务器无关，先检查就不必为一个本地错误付一次网络往返。
 *
 * affected 可以为 NULL；非 NULL 时只在成功路径被写入，语义与 mc_exec 相同。
 *
 * 与 mc_exec 的差异（都是刻意的）：
 *   - 语句由服务器编译后执行。同一条语句反复执行时服务端可以复用执行
 *     计划，但本接口每次调用都重新 prepare，不缓存语句句柄。
 *   - 语句返回结果集时返回 MC_ERR_RESULT（提示改用 mc_query / mc_stmt），
 *     不会把行留在连接上。mc_exec 走 mysql_real_query，同样会报这个错，
 *     但它是从 mysql_field_count 发现的，这里是 mysql_stmt_field_count。
 *   - SQL 的长度仍按 strlen 计，因此 SQL 文本本身不能含 '\0'；
 *     需要传二进制请放参数，不要拼进语句。
 */
mc_status mc_exec_params(mc_conn *conn, const char *sql,
                         const mc_param *params, size_t n,
                         unsigned long long *affected);

/* ------------------------------------------------------------------
 * 语句对象：预编译一次，执行多次
 *
 * 术语：预编译语句（prepared statement）是服务器端保存的一条已编译语句。
 * 准备（prepare）是一次往返——客户端发 COM_STMT_PREPARE，服务器解析语句、
 * 生成执行计划，回一个语句编号与"这条语句返回什么形状"的元数据；
 * 此后每次执行（execute）只发语句编号与参数值，SQL 文本不再出现在报文里，
 * 服务器也不再解析。
 *
 * 本模块此前的接口都是"prepare 一次 → execute 一次 → 关掉"的形状：
 * mc_exec_params、mc_query_each_ex 每次调用都重新 prepare。一问一答时这样
 * 没有浪费，但同一条语句要执行很多次时（批量写入、按主键逐条查询、分页取数），
 * 每次多付一次往返与一次解析。mc_stmt 把这两步拆开：prepare 得到对象，
 * execute 调用 N 次。
 *
 * 两类接口的分工：
 *   mc_exec_params / mc_query_each_ex  一次性的语句，调用方不管生命周期；
 *   mc_stmt                            可复用的语句，调用方负责关闭。
 * 前者等价于"prepare、执行一次、关闭"，因此两者在**结果**上没有差异，
 * 差别只有往返次数（性能一节有实测）与对象的所有权。
 * ------------------------------------------------------------------ */

/* 语句对象（不透明）。与 mc_conn 同理：内部持有 MYSQL_STMT，
 * 公开出去就等于把 libmysqlclient 的 ABI 写进本模块的头文件。 */
typedef struct mc_stmt mc_stmt;

/* 预编译一条语句。成功时 *out 指向新对象，失败时 *out 为 NULL。
 *
 * SQL 的长度按 strlen 计，因此语句文本本身不能含 '\0'；二进制值请走参数。
 * 语法错误、表或列不存在、权限不足都在这里暴露，返回 MC_ERR_STMT，
 * mc_error 以 "prepare: " 开头——这些错误与"执行时才知道"的错误
 * （主键冲突、除零）不属于同一层，分开放才能让调用方决定要不要重试。
 *
 * 调用方必须用 mc_stmt_close 释放。对象**借用**连接：conn 必须活得比 st 长，
 * 顺序是先 mc_stmt_close 再 mc_close——mysql_close 只会释放连接，
 * 之后对语句句柄的任何调用都是未定义行为。
 */
mc_status mc_stmt_prepare(mc_conn *conn, const char *sql, mc_stmt **out);

/* 语句里 ? 占位符的个数。prepare 之后即可读，用于在执行前核对参数个数。 */
size_t mc_stmt_param_count(const mc_stmt *st);

/* 结果集的列数；0 表示这条语句不返回结果集（INSERT / UPDATE / DDL）。
 *
 * 与 mc_exec_params 的差异在这里：那个接口把"返回结果集"当错误，
 * 因为它的职责只是执行写语句；语句对象事先就知道形状，于是由调用方
 * 按这个值决定走 mc_stmt_rows / mc_stmt_each 还是只读受影响行数。
 */
size_t mc_stmt_columns(const mc_stmt *st);

/* 执行一次。params / n 的约定与 mc_exec_params 完全相同：
 * n 必须等于 mc_stmt_param_count()，参数个数不符返回 MC_ERR_PARAM；
 * kind 非法、TEXT / BLOB 的 data 为 NULL 而 len 非 0 在动网络之前就被拒。
 *
 * 参数只在本次调用期间被读取（含长字段的 send_long_data 分块阶段），
 * execute 返回之后调用方可以立刻释放它指向的内存。语句对象**不**保存
 * 参数缓冲区——这与 mc_query_each 的回调"只在回调期间有效"是同一类约定，
 * 但与 mc_param 的注释并不冲突：那里说的"活到 mc_exec_params 返回之前"
 * 在这里换成"活到 mc_stmt_execute 返回之前"。
 *
 * 允许语句返回结果集。返回结果集时，行由 mc_stmt_rows / mc_stmt_each 取，
 * 二者取走的就是**这次**执行的结果；在取完（或丢弃）之前再 execute 是允许的，
 * 实现会先把上一次的剩余行读掉丢弃——协议是半双工的，未读完的结果集占着
 * 连接，此时直接发下一条命令会得到 "Commands out of sync"，
 * 而"只取前 N 行就复用语句"是合法用法，报错不合适。
 *
 * affected 可以为 NULL；非 NULL 时**只在语句不返回结果集时**被写入
 * （INSERT / UPDATE / DELETE 的受影响行数，语义与 mc_exec 相同）。
 * 返回结果集时不改写它：那种语句的"受影响行数"在服务端是行数，
 * 而"取了几行"由 *out_rows 给出，两个数含义不同，混用会误导。
 *
 * 执行失败（主键冲突、超出范围、断线）返回 MC_ERR_QUERY 或 MC_ERR_STMT，
 * 对象本身仍然可用：上一次的绑定数组已经在内部释放，再 execute 不受影响。
 * 这一点对批量写入很重要——单行失败不该让整批作废。
 */
mc_status mc_stmt_execute(mc_stmt *st, const mc_param *params, size_t n,
                          unsigned long long *affected);

/* 把本次执行的结果集一次取回内存，形状与 mc_query 的 mc_rows 完全一致
 * （行优先、cell 以 '\0' 结尾、SQL NULL 为 NULL 指针、真实长度看 length）。
 * 成功时调用方必须 mc_rows_free(out)；失败时 out 已清零，free 一次也安全。
 *
 * 与 mc_query 的差别在取数协议：mc_query 走文本协议并把结果集一次读进
 * libmysqlclient 的内部缓冲，这里走二进制协议、逐行读并追加到自己的缓冲区，
 * 两种实现的内存布局相同（名字与单元格各一段连续内存，指针数组只存偏移）。
 * 前一列的宽度不影响后一列，单元格按"本行本列的字节数"追加，
 * 因此不为最宽的那一列预留空间，峰值内存约等于结果集本身的字节数。
 *
 * 三条状态约束，违反时返回 MC_ERR_STATE 而不是给一个空结果集：
 *   - 必须先 execute 成功；
 *   - 语句必须返回结果集（否则返回 MC_ERR_RESULT）；
 *   - 同一个结果集只能取一次。取过之后再调，行已经在协议层被读走，
 *     实现无法给出第二份——返回 0 行会与"查询确实没有行"混淆。
 * 想再读一遍就再 execute 一次：语句已经编译好，代价只有一次往返。
 */
mc_status mc_stmt_rows(mc_stmt *st, mc_rows *out);

/* 逐行取数，语义与 mc_query_each_ex 完全相同（含 on_cols 的时机、
 * 行回调返回非 0 表示调用方主动停止、*out_rows 计数口径、
 * 回调里不能对同一个 conn 再发命令），只是语句由调用方预先准备好。
 *
 * 状态约束与 mc_stmt_rows 相同：先 execute、必须返回结果集、结果集只交付一次。
 * 提前停止（fn 或 on_cols 返回非 0）算作已交付：剩下的行以后不再可用，
 * 但下一次 execute 会把它们读掉丢弃，因此复用不受影响。
 */
mc_status mc_stmt_each(mc_stmt *st, mc_cols_fn on_cols, mc_row_fn fn, void *ctx,
                       size_t *out_rows);

/* 关闭并释放语句对象。传 NULL 是安全的空操作。
 *
 * 调用它之前不需要把结果集读完：mysql_stmt_close 会连同未读的行一起丢掉。
 * 但**必须先于 mc_close(conn)**——见 mc_stmt_prepare 的说明。
 */
void mc_stmt_close(mc_stmt *st);

/* ------------------------------------------------------------------
 * 大字段（BLOB）
 * ------------------------------------------------------------------ */

/* 把 data[0..len) 写入 sql 的**唯一**一个 ? 参数。
 * 数据分 MC_BLOB_CHUNK 大小的块依次发送，服务端负责拼接，
 * 因此不受单次报文长度限制——这是 send_long_data 的用途。
 * len 为 0 是合法的（写入零长度 BLOB），data 可以同时为 NULL。
 *
 * 本函数是 mc_exec_params 的包装：一个 MC_PARAM_BLOB 参数、n = 1。
 * SQL 里出现多个 ? 时返回 MC_ERR_PARAM（占位符个数与参数个数不等），
 * 而不是只绑第一个。这条在包装之前是个隐患：那时绑定数组只有 1 个元素，
 * mysql_stmt_bind_param 却会按占位符个数去读，第二个参数落在栈上
 * 相邻的内存里——值不确定，却可能 execute 成功。
 * 需要多参数时直接用 mc_exec_params。 */
mc_status mc_blob_write(mc_conn *conn, const char *sql,
                        const void *data, size_t len);

/* 把"单行单列"查询的结果写进 buf[0..cap)。
 *
 * 契约（三条，缺一不可，违反时返回 MC_ERR_RESULT）：
 *   - 结果集恰好一列；
 *   - 结果集恰好一行；
 *   - 该列不是 SQL NULL（NULL 返回 MC_ERR_NODATA）。
 * 这比"取到几行算几行"严格，是因为被覆盖的那几行不会有任何痕迹：
 * 循环里每读一行就 memset 一遍缓冲区，最后只剩下最后一行的内容，
 * 而函数看起来是成功的。
 *
 * 容量不足时返回 MC_ERR_TOOBIG，buf 内容不保证；
 * out_len 只在 MC_OK 时被写入。 */
mc_status mc_blob_read(mc_conn *conn, const char *sql,
                       void *buf, size_t cap, size_t *out_len);

/* ------------------------------------------------------------------
 * 文件辅助
 *
 * 这两个函数不访问数据库，单独成层的原因是可被单元测试完全覆盖：
 * 只要不链接 libmysqlclient，就能在没有服务器的机器上验证它们。
 * 它们的输入输出恰好是 BLOB 的输入输出，因此放在同一个模块里。
 * ------------------------------------------------------------------ */

/* 读取整个文件到 buf[0..cap)，实际字节数写入 *out_len。
 *
 * 用"边读边判断是否越界"的方式实现，不依赖 seek：
 * 普通文件可以用 fseek/ftell 得到长度，但管道、终端、/dev 下的字符设备
 * 与 /proc 里的伪文件都不可 seek（POSIX 规定 fseek 在 FIFO 上失败并设置
 * ESPIPE），/proc 文件更会报告长度为 0。以 ftell 的结果为准时，
 * 这些来源会被读成空。
 *
 * 文件大于 cap 时返回 MC_ERR_TOOBIG，此时 buf 里是前 cap 字节的残片，
 * 调用方必须丢弃。 */
mc_status mc_file_read(const char *path, void *buf, size_t cap, size_t *out_len);

/* 把 buf[0..len) 写到 path，覆盖同名文件。
 *
 * 实现是"先写 path.tmp，再 rename 覆盖"。POSIX 上同一文件系统内的
 * rename 是原子替换：写入中途失败时目标文件保持原样，不会出现半截文件。
 * 两个推论写在这里，免得调用方踩：
 *   - 要求目录可写，而不只是目标文件可写；
 *   - path 必须是普通文件路径。管道、字符设备（/dev/stdout）不行，
 *     对它们 rename 只会失败。
 *
 * 覆盖已有文件时保留它原有的 0777 权限位（rename 换的是 inode，
 * 不显式复制就会变成 0666 & ~umask）；新建文件时权限仍由 umask 决定。
 * setuid / setgid 位刻意不复制——新写入的内容没有经过审查，
 * 不该继承"以文件所有者身份执行"的语义。
 * 属主、属组、ACL 不保留，path 是符号链接时替换的是链接本身。 */
mc_status mc_file_write(const char *path, const void *buf, size_t len);

/* 状态码转可读描述。未知值返回 "unknown status"，不返回 NULL。 */
const char *mc_status_string(mc_status status);

#endif /* MYSQLCLIENT_H */
