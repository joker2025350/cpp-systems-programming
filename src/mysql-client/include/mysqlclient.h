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
    MC_ERR_NODATA     = 11   /* 期望有一行数据，实际没有（或该列为 SQL NULL） */
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
 * 注意：结果集全量驻留内存。几十万行以上的查询要自己用游标接口，
 * 本模块不提供流式取数（见 README"已知局限"）。 */
mc_status mc_query(mc_conn *conn, const char *sql, mc_rows *out);

/* 释放 mc_rows 内部的全部内存，并把结构体清零。传 NULL 安全。 */
void mc_rows_free(mc_rows *rows);

/* ------------------------------------------------------------------
 * 大字段（BLOB）
 * ------------------------------------------------------------------ */

/* 把 data[0..len) 写入 sql 的第一个 ? 参数。
 * 数据分 MC_BLOB_CHUNK 大小的块依次发送，服务端负责拼接，
 * 因此不受单次报文长度限制——这是 send_long_data 的用途。
 * len 为 0 是合法的（写入零长度 BLOB）。 */
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
 *     对它们 rename 只会失败。 */
mc_status mc_file_write(const char *path, const void *buf, size_t len);

/* 状态码转可读描述。未知值返回 "unknown status"，不返回 NULL。 */
const char *mc_status_string(mc_status status);

#endif /* MYSQLCLIENT_H */
