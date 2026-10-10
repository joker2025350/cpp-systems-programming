# 03 MySQL C API

用 MySQL 官方 C 客户端库（`libmysqlclient`）操作数据库的实战练习，
覆盖建连、增删查、以及二进制大对象（BLOB）的写入与读回。

## 功能流程

`main` 按顺序执行五个动作：

1. `mysql_init` + `mysql_real_connect` 建立连接
2. 执行 `INSERT` 插入一行用户记录，再 `SELECT` 打印整张表
3. 执行 `DELETE`（通过存储过程 `PROC_DELETE_USER`），再次 `SELECT` 确认
4. 读本地图片文件 → 写入 `TBL_USER.U_IMG` 字段（LONGBLOB 类型）
5. 从该字段读回二进制 → 写成另一个本地图片文件，完成一次数据往返

第 4、5 步是这份练习的重点：**图片在数据库里只是一个字节序列**，
存进去和读出来靠的都是二进制安全的接口。

## 涉及的 API

| API | 作用 |
|---|---|
| `mysql_init` | 初始化连接句柄 |
| `mysql_real_connect` | 用地址/账号/密码/库名建立真实连接 |
| `mysql_real_query` | 发送一条 SQL（非参数化） |
| `mysql_store_result` | 把整个结果集取回客户端内存 |
| `mysql_num_rows` / `mysql_num_fields` | 结果集的行数、列数 |
| `mysql_fetch_row` | 逐行取出，返回字符串数组 |
| `mysql_free_result` | 释放结果集 |
| `mysql_stmt_init` / `mysql_stmt_prepare` | 创建并预编译一条参数化语句 |
| `mysql_stmt_bind_param` | 绑定输入参数 |
| `mysql_stmt_send_long_data` | 分块发送大字段数据 |
| `mysql_stmt_bind_result` | 绑定输出列 |
| `mysql_stmt_fetch_column` | 取出某列的指定区间 |
| `mysql_close` | 关闭连接 |

## 两个核心概念

**预处理语句（prepared statement）**：SQL 语句先在服务器端完成解析并生成执行计划，
之后可以带着不同参数反复执行。客户端用 `?` 作为参数占位符，通过
`MYSQL_BIND` 结构体绑定实际值。它带来两个好处——参数值不会被当作 SQL 语法解析，
因此天然免疫 SQL 注入；重复执行时省去重复解析的开销。

**分块传输大字段**：`MYSQL_TYPE_LONG_BLOB` 字段可能远大于单次网络包能承载的长度。
`mysql_stmt_send_long_data` 允许把数据切成多段依次发送，服务端拼接；
读取侧对应地用 `mysql_stmt_fetch_column` 按区间取出。这样避免了在内存中
为整个字段准备一块连续空间。

## 文件说明

| 文件 | 内容 |
|---|---|
| `mysql.c` | 完整源码 |
| `mysql.png` | 流程笔记图 |

## 环境准备

代码依赖一张表和一支存储过程，根据源码中的 SQL 语句推断出的 DDL 如下：

```sql
CREATE DATABASE IF NOT EXISTS KING_DB;
USE KING_DB;

CREATE TABLE TBL_USER (
    U_NAME   VARCHAR(64),
    U_GENDER VARCHAR(16),
    U_IMG    LONGBLOB
);

DELIMITER //
CREATE PROCEDURE PROC_DELETE_USER(IN name VARCHAR(64))
BEGIN
    DELETE FROM TBL_USER WHERE U_NAME = name;
END //
DELIMITER ;
```

连接参数目前以宏的形式写在源码顶部（`KING_DB_SERVER_IP`、`KING_DB_USERNAME`、
`KING_DB_PASSWORD`、`KING_DB_DEFAULTDB`），运行前需要改成你自己的环境。

## 编译与运行

需要先安装 MySQL 开发库：

```bash
sudo apt install libmysqlclient-dev
gcc mysql.c -o bin/mysql $(mysql_config --cflags --libs)
./bin/mysql
```

程序会读同目录下的 `0voice.jpg` 作为测试素材。仓库里没有收录该文件，
自己放一张同名的图片即可（注意看下面第 2 条缺陷，图片不能超过 64 KB）。

## 已知问题

1. **连接凭据硬编码在源码里。** IP、用户名、密码都是字符串宏。
   一旦推送到公开仓库，这些信息就完全暴露。正确做法是放到独立的配置文件
   或环境变量中读取，并把承载凭据的文件写进 `.gitignore`。

2. **`read_image` 存在缓冲区溢出的可能。** 调用侧传入的 `buffer` 是
   `main` 里一个固定 64 KB 的栈上数组，而 `read_image` 内部先取出文件真实大小，
   再按这个大小 `fread`——函数签名里没有接收缓冲区容量，因此无法做边界检查。
   文件一旦超过 64 KB 就会越界写入。修法是给函数增加一个 `capacity` 参数并在
   `fread` 前判断。

3. **64 KB 的栈上数组本身就不合适。** Windows 默认线程栈 1 MB、Linux 8 MB，
   在栈上放这么大的数组容易出问题，应当用 `malloc` 从堆上分配。

4. **`mysql_read` 逐字节读取，效率极低。** 代码把 `result.buffer_length` 固定设为 1，
   于是每调用一次 `mysql_stmt_fetch_column` 只取出一个字节，读满 64 KB 需要
   65536 次调用。应当一次请求整段长度。

5. **`mysql_stmt_init` 的返回值没有检查。** 该函数失败时返回 `NULL`，
   直接传给后续的 `mysql_stmt_prepare` 会导致崩溃。

## 待办

- [ ] 把连接参数抽离到配置文件并加入 `.gitignore`
- [ ] 为 `read_image` 增加缓冲区容量检查
- [ ] 改用堆内存承载图片数据
- [ ] 修正 `mysql_read` 的分块读取粒度
- [ ] 补齐所有 API 的返回值检查
