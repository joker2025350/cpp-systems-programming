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
读取侧对应地用 `mysql_stmt_fetch_column` 按区间取出。

## 文件说明

| 文件 | 内容 |
|---|---|
| `mysql.c` | 完整源码 |
| `config.h.example` | 连接配置模板，使用前需复制为 `config.h` |
| `mysql.png` | 流程笔记图 |

## 配置连接参数

连接参数不写在源码里，而是从 `config.h` 读取：

```bash
cp config.h.example config.h
```

然后编辑 `config.h`，填入自己的服务器地址、账号、密码和库名。

`config.h` 已被 `.gitignore` 忽略，**不会进入版本库**。这样凭据不会随源码
泄露到公开仓库——这一点很重要，因为 Git 的历史是不可撤销的，
一旦密码被提交，即使后续删除，它依然永久留在旧提交里。

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

## 编译与运行

需要先安装 MySQL 开发库：

```bash
sudo apt install libmysqlclient-dev
mkdir -p bin
gcc mysql.c -o bin/mysql $(mysql_config --cflags --libs)
./bin/mysql
```

程序会读同目录下的 `0voice.jpg` 作为测试素材。仓库里没有收录该文件，
自己放一张同名的图片即可（图片不能超过 64 KB，见下）。

## 缺陷修复记录

首次实现里有 5 处缺陷，现已全部修正。

### 1. 连接凭据硬编码在源码里

原代码把服务器地址、用户名、密码直接写成字符串宏：

```c
#define KING_DB_SERVER_IP   "192.168.199.134"
#define KING_DB_USERNAME    "admin"
#define KING_DB_PASSWORD    "123456"
```

这对本地实验无害，但一旦推到公开仓库，凭据就完全暴露。**Git 的历史不可撤销**，
事后删除也无效——旧提交里依然能翻出来。

修正：抽出为独立的 `config.h`，仓库里只保留 `config.h.example` 模板，
并把 `config.h` 写入 `.gitignore`。

### 2. `read_image` 缺少缓冲区容量参数

原函数的签名是：

```c
int read_image(char *filename, char *buffer);
```

它内部先取出文件的真实长度，再按这个长度 `fread`。但函数**不知道缓冲区有多大**，
无法做任何边界检查——调用方传进来的缓冲区一旦小于文件长度，就会越界写入。

修正：增加 `capacity` 参数，并在 `fread` 之前比较文件长度与容量，超限直接返回 `-4`。

### 3. 64 KB 数组放在栈上

原代码在 `main` 里声明 `char buffer[FILE_IMAGE_LENGTH]`，其中
`FILE_IMAGE_LENGTH` 是 `64*1024`。栈空间是有限的（Windows 默认 1 MB、
Linux 8 MB），在上面放 64 KB 的数组容易出问题，而且一旦函数调用层次加深，
栈溢出风险会持续放大。

修正：改用 `malloc` 从堆上分配，并在 `Exit` 标签处统一 `free`。

### 4. `mysql_read` 逐字节读取，效率极低

原代码把 `result.buffer_length` 固定设为 1：

```c
result.buffer_length = 1;
mysql_stmt_fetch_column(stmt, &result, 0, start);
start += result.buffer_length;
```

这意味着每调用一次 `mysql_stmt_fetch_column` 只取回**一个字节**。
读满 64 KB 需要 65536 次函数调用，每一次都涉及一次到客户端的列数据拷贝。

修正：一次请求整段剩余长度。

```c
unsigned long remaining = wanted - offset;
result.buffer   = buffer + offset;
result.buffer_length = remaining;
```

同时补上了容量检查——如果数据库里的字段长度超过调用方缓冲区，
返回 `-7` 而不是直接溢出。为此 `mysql_read` 的最后一个参数语义
从"缓冲区长度（未使用）"明确成了 `capacity`。

### 5. `mysql_stmt_init` 的返回值没有检查

该函数在内存不足等情况下返回 `NULL`。原代码直接把它传给
`mysql_stmt_prepare`，会导致崩溃。

修正：在 `mysql_write` 和 `mysql_read` 中都加入 `NULL` 检查。

### 附带修正

- 所有错误返回路径补上 `mysql_stmt_close(stmt)`，避免语句句柄泄漏
- 语句级错误改用 `mysql_stmt_error(stmt)` 获取。原来用的是 `mysql_error(handle)`，
  那是连接级错误信息，对语句错误往往返回空字符串，掩盖了真实原因
- 新增 `#include <stdlib.h>`，`malloc` / `free` 需要它

## 验证结果

在 Ubuntu 22.04 / gcc 11.4.0 / MySQL 8.0.46 头文件环境下验证：

| 检查项 | 方法 | 结果 |
|---|---|---|
| 编译 | `gcc -Wall -Wextra -c`，使用真实 MySQL 头文件 | 零警告 |
| `read_image` 边界检查 | 7 项单元测试 | 全部通过 |

单元测试覆盖的情形：

| 用例 | 预期 | 结果 |
|---|---|---|
| 文件小于容量 | 返回真实长度 100 | PASS |
| 内容完整性 | 缓冲区内容与源文件一致 | PASS |
| 文件大于容量（100 字节 → 容量 50） | 返回 `-4`，缓冲区未被写入 | PASS |
| 文件不存在 | 返回 `-2` | PASS |
| 文件名为 `NULL` | 返回 `-1` | PASS |
| 容量为 0 | 返回 `-1` | PASS |
| `write_image` 写入 | 返回实际写入字节数 | PASS |

编译验证使用 `-c` 只编译不链接，因此不需要运行中的 MySQL 服务实例，
也不需要 `libmysqlclient.so`——这对持续集成环境同样适用。

## 待办

- [x] 把连接参数抽离到配置文件并加入 `.gitignore`
- [x] 为 `read_image` 增加缓冲区容量检查
- [x] 改用堆内存承载图片数据
- [x] 修正 `mysql_read` 的分块读取粒度
- [x] 补齐所有 API 的返回值检查
- [ ] 补充 `write_image` 与 `mysql_read` 的联合测试（需真实数据库实例）
