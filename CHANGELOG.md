# Changelog

按工作日记录。每天追加一节，写清**改了什么、为什么改、验证了什么**。

写法约定：

- 一节一个日期，标题为 `## YYYY-MM-DD`。
- 每条以模块名或作用范围开头，用 `-` 列表。
- 只写有信息量的内容。"修了拼写"这类不需要记录。
- 每次改动必须有对应的验证结论（编译警告数、测试项数、实测数据），
  没有验证的改动不写进这里，因为写进来就意味着它是可信的。

---

## 2026-10-25

### mysql-client

- `mc_file_write` 覆盖已有文件时保留它原有的 `0777` 权限位。缺陷是
  `rename(2)` 换的是目录项指向的 inode：新 inode 由 `fopen(tmp, "wb")` 按
  `0666 & ~umask` 创建，与目标文件原来的权限无关。实测（umask 002）
  一个 `0600` 的文件被覆盖一次就变成 `0664`——原本只有所有者能读的导出内容，
  同组用户从此也能读，而函数返回 `MC_OK`、字节数也完全正确。
  这类偏差在功能测试里看不出来，只能靠 `stat` 发现。
- 修法：写入之前先 `stat(path)` 记下目标已有的权限位，`fchmod(fileno(fp), mode)`
  之后再写数据，最后 `rename` 覆盖。三处顺序都有理由，写进 README 3.14：
  `stat` 必须在 `rename` 之前（之后原 inode 已摘掉，没有来源）；
  `fchmod` 放在 `fwrite` 之前（刚创建的临时文件是空的，没有内容可泄露，
  否则会有一段"内容已落盘、权限还是 `0664`"的窗口）；
  只复制 `0777` 三段而不带 `setuid` / `setgid`——这两个位的语义是
  "执行时以文件所有者（或所属组）的身份运行"（`execve(2)`），
  而这里写进去的是一份全新的、未经审查的内容；内核在 `write(2)` 写已有文件时
  会清掉 setuid 位，但换的是新 inode，内核不会替调用方清。
- `fchmod` 失败时按 `MC_ERR_FILE_WRITE` 放弃这次写入，不继续写出一份权限
  对不上预期的文件后再返回 `MC_OK`。
- 顺带记录了一个此前没写下来的行为：`rename` 只看目录写权限、不看目标文件写权限，
  因此目标是 `0444` 时覆盖**仍然成功**（`fopen(path, "wb")` 会 `EACCES`）。
  既然"覆盖成功"，权限位就更必须保持。
- 新增 `tests/test_pure.c` 的"覆盖写的权限位"一节，37 项：`0600` / `0640` /
  `0444` / `0755` 四种起点各覆盖一次并 `stat` 核对，`04755` 一组验 setuid 不被继承，
  外加"目录不可写时返回 `FILE_OPEN` 且目标内容与权限都不变"。
  期望值不硬编码 `0664`，而是现场读一次 umask 算 `0666 & ~umask`，避免测试依赖环境。
- `tests/check_live.sh` 加 2 项：`blob-get` 覆盖一个已存在的 `0600` 文件之后，
  `stat -c %a` 仍是 `600`，验证驱动层真的走的是 `mc_file_write`。
- README：`2.6` 补权限契约与示例，新增 `3.14`（含 0600 → 0664 的实测输出），
  新增 `8.9`（属主 / 属组 / ACL / 扩展属性 / 硬链接都不保留，符号链接替换的是链接本身），
  待办里划掉这一条。头文件里 `mc_file_write` 的契约同步。

### 验证（Ubuntu 22.04 / gcc 11.4.0 / MySQL 8.0.46，服务端与客户端库均从 deb 解包到 /tmp）

- **先确认测试能抓到缺陷**：把 `file.c` 换回上一版提交的实现、其余不变重新编译，
  新单元测试报 5 项失败（`实际 0664，期望 0600` 等）；换回修复版后全绿。
- 三种构建各 **0 条警告**：默认 `-Wall -Wextra -Wconversion`、ASan+UBSan、
  追加 `-Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef`。
- 测试项数：单元 99（原 62）+ 接口 20 = 119；`make check` 41；`make check-live` 50（原 48）
  + 流式 24。全部 0 失败。
- 全仓 `make test` 68 + 136 + 119 + 54 + 40 = **417 项**（原 380），0 失败；
  全仓 `make check` 退出码 0、无 FAIL 行；全仓 `make sanitize` 各模块全绿，
  无 ASan / UBSan / LSan 报告。
- ASan 版 CLI 再跑一遍 `check_live.sh`：50 项 0 失败，无泄漏报告。
- `testdata` 字节数未改动。

---

## 2026-10-23

### mysql-client

- 新增流式取数接口 `mc_query_each(conn, sql, fn, ctx, out_rows)`：
  每行调一次回调，峰值内存只与最宽的一行有关，与总行数无关。
  这是 README"已知局限"与"待办"里排在第一位的那一条，今天做掉。
  实现上关键的一步是**不调用** `mysql_stmt_store_result`——它的作用正是
  "把剩下的行全读进客户端内存"，一旦调用，流式就没有意义了。
  同时它在预处理语句上禁用了 `mysql_stmt_data_seek`，所以 `mc_query` 那套
  "两遍遍历 + 一次分配"的优化在这里不成立，改成每列一块缓冲区逐行复用。
- 交付语义按"已处理"而不是"已读入"定义：`out_rows` 写入**已交给回调的行数**，
  包括触发提前停止的那一行与失败之前交付的那些行。回调返回非 0 表示主动停止，
  函数返回 `MC_OK`——`each -n 1` 是正常用法，不该用错误码表示。
- 三处细节是实现能否成立的分界：
  - 每行开始前清零 `is_null` / `error` / `length`。库只在这些字段需要变时写它们，
    不清零时"上一行是 NULL"会让这一行也报成 NULL，而 `fetch` 仍返回 0；
  - `buffer_length` 永远比分配长度少 1，末字节留给 `'\0'`。若两者相等，
    值正好填满缓冲区时 `fetch` 返回 0（不算截断），接着写 `buf[len] = '\0'`
    就越界一字节——ASan 能抓到，未插桩时不会崩；
  - 关闭语句之前必须 `mysql_stmt_free_result`。回调提前停止时结果集还有没读的行，
    连接处于"结果未读完"状态，下一条命令会得到 `Commands out of sync`
    （二进制协议是半双工的）。`test_stream.c` 有一条用例专门验它：
    第 3 行停下，再用同一连接执行 `SELECT 1`。
- 驱动层新增 `each` 子命令与 `-n N`（行数上限）。`-n` 用在别的子命令上直接报
  用法错误，不静默忽略：`sql -n 10` 看起来限制了行数而实际没有，是最难查的一类问题。
  `each` 不打印表头（回调拿不到列名），行的内容与 `sql` 逐字节相同，
  `check_live.sh` 用 `cmp` 比对这一点。
- 新增 `tests/test_stream.c`：24 项正确性用例（与 `mc_query` 逐格比对 NULL 性质 /
  长度 / 内容，7 行覆盖空串、全 NULL、内嵌 `'\0'`、100 KiB 值、UTF-8、负数），
  另有一个测量模式，输出行数 / 字节数 / FNV-1a 校验和 / 峰值 RSS / 耗时。
- 新增 `tests/check_scale.sh` 与 `make check-scale`：1048576 行、20 846 464 字节
  的结果集，两种取数方式各起一个进程测峰值内存
  （`ru_maxrss` 是进程级单调指标，同一进程里先后跑两次只能测到先跑的那个）。
  行数用服务器自己算的 `COUNT(*)` 核对，内容用校验和核对，
  不让两个实现互相比对——共同的错误会互相掩护。

### 验证（Ubuntu 22.04 / gcc 11.4.0 / MySQL 8.0.46，服务端与客户端库均从 deb 解包到 /tmp）

- 三种构建各 **0 条警告**：默认 `-Wall -Wextra -Wconversion`、ASan+UBSan、
  追加 `-Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef`。
- 测试项数：单元 62 + 接口 20 = 82；`make check` 41；`make check-live` 48 + 流式 24；
  `make check-scale` 5。全部 0 失败。
- 全仓 `make test` 68 + 136 + 82 + 54 + 40 = **380 项**，0 失败。
- ASan + UBSan 版本跑同一套（82 + 48 + 24）：0 失败，无 ASan / UBSan / LSan 报告；
  流式路径的额外退出路径（回调提前停止）也在其中被走到。
- 实测百万行（同机 loopback，5 轮内取一次）：

  | 方式 | 行数 | 峰值 RSS | 耗时 |
  |---|---|---|---|
  | `mc_query` | 1048576 | 161.9 MiB | 292.5 ms |
  | `mc_query_each` | 1048576 | 10.7 MiB | 211.8 ms |

  内存差 15.1 倍，流式一侧的量级与行数无关；两种方式的 FNV-1a 校验和与
  服务器 `COUNT(*)` 三者一致。流式快 28% 来自省掉本地那一段 24+24+20 MB 的
  分配与 `memcpy`，不是协议更高效——两者传输的字节数相同。

---

## 2026-10-22

### mysql-client

- 单文件 `mysql.c`（355 行，`main` 写死六步流程）改造为模块：
  `include/mysqlclient.h` + `src/{mysqlclient,options,file,status,main}.c` +
  `tests/{test_pure.c,test_db.c,check_cli.sh,check_live.sh,bench_file.c}` +
  `Makefile` + 十节 README。它是仓库里最后一个单文件模块，至此五个模块
  都进入根 `Makefile` 的自动发现范围。
- 接口重设，三处是为了"调用方能被独立使用"：
  - `mc_conn` 不透明。`MYSQL_BIND::is_null` 的类型在 8.0.1 从 `my_bool *`
    改成 `bool *`，`<mysql.h>` 一旦进头文件，调用方就被绑到具体库版本。
    现在版本差异只影响 `src/mysqlclient.c` 一个编译单元。
  - `mc_rows` 按行优先展开成 `cell[row * cols + col]`，另带 `length[]`。
    `length` 不能用 `strlen` 代替：BLOB 字节里可以含 `'\0'`。
    SQL NULL 用空指针表示，与空串区分开。
  - 十一个状态码，文件类与数据库类分开；容量不足是 `MC_ERR_TOOBIG` 而不是
    "返回前 N 字节"，截断后返回成功是 BLOB 代码里最难发现的错误。
- 修掉 14 处缺陷（README 7.2 有完整表）。三条值得单独记：
  - `mysql_stmt_fetch` 的返回值判断把 `MYSQL_NO_DATA`、`MYSQL_DATA_TRUNCATED`
    与真正的错误码混在一个 `break` 里，网络中断时函数会返回 0 字节并"成功"；
  - 多行结果的读回每轮都从头写同一块缓冲区，最后只剩最后一行的内容，
    返回值却完全正常；现在改成单行单列契约，多行返回 `MC_ERR_RESULT`；
  - `main` 的每条失败路径都 `return 0`，脚本无法感知失败。
- 写入改成"临时文件 + `rename`"：`fopen(path, "wb")` 先 truncate，
  此后的失败（`ENOSPC`/`EDQUOT`）会丢掉目标文件的原有内容。与 `contact-list`
  在同一个问题上给了同一个答案。
- 读文件不再用 `fseek`/`ftell` 取长度：`/proc` 伪文件报告长度为 0，
  FIFO 上 `fseek` 直接失败（`ESPIPE`）。改为读到 EOF，填满缓冲区后多读一字节判断越界。
- 取数从"每次 1 字节"改为"一次请求整段剩余长度"：64 KiB 从 65536 次
  `mysql_stmt_fetch_column` 降到 1 次。
- 驱动层从写死的流程改为 5 个子命令（`ping` / `sql` / `exec` / `blob-put` /
  `blob-get`），退出码 0/1/2 分层，`stdout` 只放数据、摘要走 `stderr`。
- `config.h` 不再是编译的必需条件：存在时才编进去，否则用内置的
  "本机、无密码"默认值。原实现无条件 `#include "config.h"`，
  新克隆的仓库编译必然失败。

### 构建系统

- 根 `Makefile` 新增 `SKIP`（空格分隔的模块名），`list` 目标给被跳过的模块标 `(SKIP)`。
  `mysql-client` 是唯一有外部依赖的模块（`libmysqlclient`），无头文件的机器上
  用 `make SKIP=mysql-client` 跳过其余照常构建。选显式跳过而不是"探测失败就不编"：
  静默少构建一个模块，最后看到的是"全部成功"，少掉的东西没有任何提示。
- `src/mysql-client/Makefile` 按 `mysql_config` → `pkg-config` →
  命令行变量三处定位客户端库，`sanitize` 递归调用时把 `MYSQL_CFLAGS`/`MYSQL_LIBS`
  一起透传（否则子 make 重新探测一遍，在"用命令行指定解包目录"时探测不到）。
- 库的四个实现文件按依赖分层：`file.c` / `options.c` / `status.c` 不引用任何
  `mysql_*`，因此 `test_pure` 链接后 `ldd` 里没有 libmysqlclient
  （实测 `ldd bin/test_pure | grep mysql` 无输出），可以在没装客户端库的机器上跑。

### 验证

- 环境：Ubuntu 22.04 / Linux 6.8.0-138 / gcc 11.4.0 / x86_64；
  MySQL 8.0.46（客户端库与服务端从 deb 解包到 `/tmp` 运行，无需 root 安装）。
- 三种构建各 **0 条警告**：默认、ASan+UBSan、追加
  `-Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wundef`。
- `make test` 62（纯逻辑，不链接 libmysqlclient）+ 17（接口层，含真实建连失败）= 79 项，
  0 失败；`make check` 33 项，0 失败。
- 新增 `tests/check_live.sh`（`make check-live`），对真实服务器 38 项 0 失败：
  64 KiB / 5 MiB / 0 字节三种长度的 BLOB 往返都 `cmp` 逐字节相同；
  容量不足报出 `5242880 bytes` 且不产生输出文件；多行/NULL/无行分别报
  `more than one row` / `SQL NULL` / `no row`；UTF-8 与含单引号的文本原样取回。
- ASan 版本跑同一套 38 项实测：0 失败，无 ASan/UBSan/LeakSanitizer 报告。
- 全仓 `make test`：68 + 136 + 79 + 54 + 40 = 377 项，0 失败。
- 性能（`make bench`，5 轮取最小值，页缓存内）：1 MiB 写 1702 MB/s、读 3498 MB/s；
  8 MiB 580 / 3547 MB/s；64 MiB 406 / 3376 MB/s。本地搬运不是瓶颈。

## 2026-10-21

### word-count

- 驱动层新增 `-j N`：处理多个文件时，每个文件作为一个任务提交给 `src/thread-pool`
  的线程池，由至多 N 个工作线程并行统计；工作线程数取 `min(JOBS, 文件数)`。
  这是两个模块第一次真正接起来——之前它们只是放在同一个仓库里。
  **依赖方向是刻意的单向**：只有 `src/main.c` 引用 `threadpool.h`，
  `libwordcount.a` 对 `threadpool_*` 零引用，`wordcount.h` 里也没有任何线程类型。
  只想数一个文件的调用方因此既不需要 pthread，也不需要 worker 池。
- 并行输出与顺序输出**逐字节相同**：每个任务只写自己那个 `file_slot`
  （槽位数 = 文件数），主线程在 `threadpool_wait` 之后按 argv 顺序统一打印。
  若让任务自己 `printf`，行序就取决于谁先读完——6 个大小不同的文件跑两遍，
  行序都可能不同，而数字全对，肉眼极难发现。
- `errno` 在任务线程内取值后存进槽位。POSIX.1-2008 规定 `errno` 是 thread-local，
  工作线程里的 `ENOENT` 不会出现在主线程那一份里；等到打印时再读，
  拿到的是主线程自己的残留值，错误信息会指向一个不相干的原因。
- `threadpool_wait` 的返回点构成 happens-before：工作线程在持锁状态下把 `running`
  减一并广播，主线程在同一个互斥量上等它归零，因此 `wait` 之后读槽位不需要额外同步。
- 建池失败直接报错退出，不静默退回串行：`-j 8` 悄悄变成单线程之后输出完全正常、
  只有耗时对不上，这种状态比一条明确的错误信息难查得多。
- 修掉一个只有加了选项才会暴露的缺陷：`total` 行的判定原本是 `argc > 2`，
  引入 `-j` 之后这个条件不再等于"有多个文件"，改为按 `optind` 之后的实际文件数判断。
- 加入 `getopt` 解析（`-j` / `-h`），`CPPFLAGS` 补 `-D_POSIX_C_SOURCE=200809L`：
  `-std=c11` 定义 `__STRICT_ANSI__` 会关掉 glibc 的默认扩展，不定义时 `getopt`
  退化成隐式声明。

### 构建系统

- `src/word-count/Makefile` 新增一条把 thread-pool 建到本模块 `bin/thread-pool/`
  下的规则（子 make 传绝对 `OUT`，`CFLAGS` 原样透传）。不直接链接
  `../thread-pool/bin/libthreadpool.a`：那样 `make -C src/word-count` 从干净工作树
  就会失败，而且 sanitize / tsan 变体会拿到未插桩的那份池——
  "测试跑过了，但池里发生的错误从没被检查过"。代价是根 `make` 编译 thread-pool 两遍。
- `make bench` 里原本有五行直接编译 `src/wordcount.c src/main.c` 生成不同块大小的变体。
  `main.c` 引用线程池之后这五行会链接失败，改为同时传 `-I../thread-pool/include`
  并链接 `$(POOLLIB)`。
- word-count 新增 `make tsan`（此前是纯单线程模块，没有这个目标）。
- 根 `Makefile` 顶部注释仍写着"thread-pool 与 mysql-client 没有 Makefile"，
  与 10-20 的模块化不符，改为只剩 mysql-client。

### 测试

- 新增 `tests/check_parallel.sh`：6 个长度递增的文件（118 B ~ 3.1 MB，短任务先结束，
  完成顺序必然与 argv 不同），用 `-j 1/2/3/4/8/16` 与顺序基线比对 **stdout、stderr、
  退出码**三个产物；另加单文件、中间夹不存在文件、全部不存在、`-j 0` / `-j abc`
  四组边界，共 28 项比对。`make check` 现在跑两段。
- 验证（Ubuntu 22.04 / gcc 11.4.0 / Ryzen 7 5700X）：
  默认、`-funsigned-char`、ASan+UBSan、TSan 四种构建各 **0 条警告**；
  `make test` 40 项、`make check` 6 + 28 项、`make sanitize` 40 项、`make tsan` 40 项
  全部 0 失败，无 sanitizer 报告（TSan 下并行驱动跑了 4 worker 与"含错误文件"两组）。
  全仓 `make test` 68 + 136 + 54 + 40 = 298 项。
- 性能（8 个文件合计 39 528 768 字节，5 轮合计，三次独立测量）：
  顺序 378~379 ms、`-j 1` 380 ms、`-j 2` 194~195 ms、`-j 4` 100 ms、`-j 8` 66~70 ms。
  `-j 8` 加速比 **5.4~5.7×**，吞吐从 ~523 MB/s 提到 ~2820~2990 MB/s。
  不足 8 倍的原因有三处：主线程的提交与汇总本身串行、文件长度不等造成尾部等待、
  8 条流共享内存带宽。`-j 1` 与顺序相差 1% 以内，那是每轮建池与停机的固定开销。
- 小文件对照：500 个 118 字节文件各跑 20 轮，顺序 67 ms、`-j 4` 41 ms、`-j 8` 36 ms，
  约 1.6~1.9×。此时时间几乎全在 500 次 `open` / `read` / `close` 上，
  并行的是系统调用而不是计算。
- `testdata` 字节数未变：118 / 617637 / 158 / 160 / 58。

### 文档

- `src/word-count/README.md` 补 2.7（跨模块依赖方向）、3.6（并行驱动的任务划分与顺序保证）、
  5.7（并行一致性验证）、5.8（TSan）、6.3（并行度实测）、7.4（第三轮改动的落点）、
  9.5 / 9.6（单文件不能并行、标准输入不并行），待办勾掉 `-h`。
- 根 `README.md` 更新模块表与 `make tsan` 一行，并注明 word-count 是唯一一处模块间接线。

---

## 2026-10-20

### 仓库结构

- 五个主题目录从 `NN-主题名` 改为 `src/英文模块名`：`01-concurrent-lock` →
  `src/concurrent-lock`、`02-thread-pool` → `src/thread-pool`、`03-mysql` →
  `src/mysql-client`、`04-contact-list` → `src/contact-list`、`05-word-count` →
  `src/word-count`。用 `git mv` 完成，历史按重命名保留。
  **原因**：带编号的顶层目录是练习合集的形态，模块平铺在 `src/` 下才是工程形态；
  编号还会让"按功能找模块"变成"按记忆中的序号找模块"。
- 三张笔记图移入 `docs/images/` 并改为英文名。图片不再散落在模块目录里，
  模块目录只留代码、测试与文档。
- 新增根 `README.md`（英文）、`LICENSE`（MIT）、根 `Makefile`（自动发现并转发到各模块）、
  本文件。根 README 换成项目门面式写法：一句话定位、模块表、目录树、快速开始、
  约定。**原因**：原 README 是一张"进度表"，读者看不出这是什么项目、怎么跑起来。
- 删除 `01-concurrent-lock/locks.c`（手写草稿，含拼写错误与语法错误，编译不过）
  与 `01-concurrent-lock/实验记录.md`（全部内容为"待填"）。这类文件留在仓库里
  会让人误以为它们是可用的代码。

### concurrent-lock

- 从 80 行单文件 `lock.c` 改造为模块：`include/counter.h` + `src/counter.c` +
  `src/main.c` + `tests/test_counter.c` + `Makefile`。
- 四种实现原本靠 `#if 0` 在编译期切换，现在变成 `counter_kind` 枚举在运行时选择，
  同一个二进制里五种实现并存。**这是模块化的实质收益**：只有它们同时存在于
  同一个进程，才能保证比较的是同一份 CPU、负载与调度。
- 对外用不透明句柄而不是公开结构体：内部要放 `pthread_mutex_t` 与
  `pthread_spinlock_t`，公开会强迫头文件 `include <pthread.h>` 并固定 ABI。
- 修正原实现的 `pthread_spin_init(..., PTHREAD_PROCESS_SHARED)`：锁在进程私有内存里，
  flag 应为 `PTHREAD_PROCESS_PRIVATE`。
- 修正内联汇编约束：`"m"(*value)` 改为 `"+m"(*value)`（xadd 会改写内存），
  `delta` 用 `"0"()` 显式绑到输出寄存器。
- 发现并记录：原程序在自增循环里的 `usleep(1)` **不是放大竞态而是掩盖它**。
  实测同样 10 线程，不 sleep 时 1000 万次丢 862 万次（86.2%）；
  每次后 sleep 1 ms 时 2000 次一次不丢。原因是自增的读-改-写窗口只有几纳秒，
  把访问间隔拉到毫秒级就等于让碰撞概率趋近于零。
- 验证：零警告（默认 / ASan+UBSan / TSan 三种构建）；
  `make test` 68 项 0 失败；`make tsan` 67 项 0 失败且无 data race 报告；
  TSan 下单独跑 raw 会报 `data race src/counter.c:227`。
- 基准（Ryzen 7 5700X，10 线程 × 100 万次 × 3 轮）：
  atomic 143~159 ms（63~70 Mops/s）、asm 152~162 ms、mutex 387~409 ms、
  spin 935~1011 ms、raw 54 ms 但只完成 13.6% 的工作。
  **spin 比 mutex 慢 2.3~2.4 倍**，原因是 10 线程争同一根缓存行造成 ping-pong。
  跨次运行波动可达 10%，因此表里只给区间，不给单次数值。

### 构建系统

- 三个模块的 Makefile 改为**变体构建使用独立输出目录**：产物统一落在 `$(OUT)`，
  默认 `bin/`；`sanitize` / `tsan` 递归调用自身把 `OUT` 覆盖为 `bin/sanitize`、
  `bin/tsan`。**原因是一个实际踩到的坑**：make 判断是否重建只比较时间戳，
  不比较编译选项，共用 `bin/` 时先 `make tsan` 再 `make`，普通构建认为
  `bin/counter` 已是最新而不重新链接，于是留下 TSan 插桩的二进制，
  一执行就报 `FATAL: ThreadSanitizer: unexpected memory mapping`。
  这个缺陷在单独构建时不会出现，只在"跑完检查再跑基准"的顺序里暴露，
  正是根 `Makefile` 把各模块串起来之后才被发现的。
  验证：`make tsan` → `make` → `make bench` 全序列通过（该序列在修复前失败）。

### thread-pool

- 从单文件程序改造为模块：`include/threadpool.h` + `src/threadpool.c` + `src/main.c` +
  `tests/test_threadpool.c`（54 项）+ `tests/check_cli.sh`（19 项）+
  `tests/bench_threadpool.c` + `Makefile` + 重写的 README（九节）。
  根 `Makefile` 的 `wildcard` 自动发现，`make list` 里已出现 `thread-pool`，
  根 Makefile 本身没有改动。**原因**：模块化之前它不在任何构建目标里，
  连"能不能编译"都没人验证。
- 接口重设，四处改变都有具体理由：
  - `nThreadPoolCreate(ThreadPool *pool, int n)` → `threadpool_create(int n, threadpool **out)`。
    旧接口要求调用方在栈上放一个含 `pthread_mutex_t` 的 `ThreadPool`，池内部再
    `memset` 清零；若该对象已被初始化过，`memset` 会抹掉同步原语的内部状态。
    改为池自己分配、不透明句柄对外。
  - 调用方不再 `malloc` 任务节点。旧设计要求任务函数自己 `free(task)`，
    漏一处就是泄漏；现在节点归池，任务返回后由池释放，`arg` 仍归调用方。
  - 新增 `threadpool_wait`。旧实现只能靠 `getchar()` 等任务做完，没有可编程的完成信号。
  - 队列从 LIFO 改 FIFO（尾插头取）。头插头取是栈，先提交的任务会被后来的不断压到队尾，
    在提交快于消费的负载下可能长期得不到执行——这是饥饿，不是"顺序不同"。
    现在"提交顺序 = 执行顺序"成为可以断言的保证。
- `destroy` 由"丢弃队列"改为"优雅停机"：置 closed → 等 `queued`/`running` 归零 →
  置 stop → join。旧实现直接 `free` 掉未执行的任务节点，调用方既不知道丢了哪些任务，
  也没有返回值可查。实测：200 个 1 ms 任务提交后不 wait 直接 destroy，
  200 个全部执行完，destroy 耗时 211 ms。
- 修 `create` 的失败路径：`pthread_create` 中途失败时旧代码直接返回，已创建的工作线程
  既没被置停机标志也没被 join，继续在一个即将被释放的池上等任务（线程泄漏 + 悬空指针）。
  现在按"置 stop → broadcast → join → 析构同步原语"的顺序回滚。
- `threadpool_wait` 检测工作线程自调用（`pthread_equal` 与 `tids[]` 逐个比较）并返回
  `TPOOL_ERR_BLOCKING`。不检测就是永久阻塞：它等 `running == 0`，而调用者自己
  就是一个 running 的任务。用 `pthread_equal` 而非 `==`，因为 POSIX 只规定
  `pthread_t` 是不透明类型，没有规定可比较大小。
- 旧目录里三个讲解版文件（`threadpool_annotated.c`、`threadpool_bug_demo.c`、
  `threadpool_win32.c`）移入 `notes/`，旧的 `threadpool.c` 删除（被 `src/threadpool.c` 取代）。
  三者都是模块化之前的形态（任务节点自释放、每个 worker 一份 terminate 标志），
  留在模块根目录会让人误以为它们是当前接口的实现。
- 验证：三种构建（默认 / ASan+UBSan / TSan）均 0 条警告；
  `make test` 54 项、`make check` 19 项、`make sanitize` 54 项、`make tsan` 54 项，
  全部 0 失败，无 sanitizer 报告。全仓 `make test` 合计 68 + 136 + 54 + 40 = 298 项。
- TSan 首次运行报告了一处 data race，位置在测试代码：压力测试第二轮让 100 个任务
  共用同一个 `slot`，多个工作线程并发写同一块内存。池无问题，是测试把
  "每个任务独占参数"这条约定用错了。改为每任务一个 `slot` 后 TSan 干净。
- 性能（Ryzen 7 5700X，8 核 16 线程，5 轮取最小值，两轮独立测量）：
  20 万个空任务的调度成本 1 线程 121~131 ns/任务、2 线程 146~154、
  4 线程 290~305、8 线程 637~654；对照"每任务新建线程再 join"约 16700~17130 ns/任务，
  单线程池约为它的 1/140。
  **空任务吞吐随线程数单调下降**：所有工作线程争同一把锁和同一条队列的
  `head`/`tail`（同一缓存行），空任务时缓存行 ping-pong 就是全部成本。
  任务粒度 100 µs 时 8 线程加速比 7.98（效率 100%），说明竞争在 100 µs 尺度上可忽略。

### 其他

- `.gitignore` 里 `01-concurrent-lock/lock`、`03-mysql/config.h` 两条路径随目录重命名更新为
  `src/...`，否则 `config.h` 会重新变成被追踪状态——它含真实数据库凭据。
- 发现并修复：`.gitattributes` 的两条 `-text` 规则在目录重命名后**静默失效**。
  规则原本写的是 `04-contact-list/testdata/*` 与 `05-word-count/testdata/*` 两条具体路径，
  搬到 `src/<模块>/testdata/` 之后不再匹配任何文件，而 git 对不匹配的规则
  不会有任何提示。后果不是仓库里的 blob 被改写（blob 仍是 158/160/58/118/617637），
  而是**检出与导出时按 `core.autocrlf=true` 加上 CR**：`git archive HEAD` 导出后
  同一份数据变成 165/165/60/120。只在"从远程重新取一份"时暴露，本机工作区看不出来。
  规则改为 `**/testdata/** -text`（`**` 跨目录，结构调整时不用再改），
  并注明 `*/testdata/*` 也不行——gitattributes 的 `*` 与 `.gitignore` 一样不跨 `/`。
  验证：修复后 `git archive HEAD` 导出的归档，五个数据文件字节数与 blob 完全一致。
- 同时发现换行符策略缺失：仓库里没有任何 `text` 属性，所有文本文件的检出形态
  由各人本地的 `core.autocrlf` 决定。在 `core.autocrlf=true` 的机器上导出，
  `tests/*.sh` 会变成 CRLF，在 Linux 上 `sh tests/check_cli.sh` 直接报
  `set: Illegal option -`（`-e` 后面跟了一个 CR）。这看起来像脚本写错，
  实际与脚本无关。**一个只能在 Linux 上构建的仓库，不该让构建结果取决于
  谁在哪台机器上 clone。**
  加 `* text=auto eol=lf`：文本文件在仓库里与检出后都是 LF，二进制仍走二进制判定；
  它与 `-text` 的顺序有意义，`-text` 写在后面才能覆盖全局规则。
  `git add --renormalize .` 确认仓库里原本就是 LF，blob 无一变更。
