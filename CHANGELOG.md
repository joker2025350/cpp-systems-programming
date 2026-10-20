# Changelog

按工作日记录。每天追加一节，写清**改了什么、为什么改、验证了什么**。

写法约定：

- 一节一个日期，标题为 `## YYYY-MM-DD`。
- 每条以模块名或作用范围开头，用 `-` 列表。
- 只写有信息量的内容。"修了拼写"这类不需要记录。
- 每次改动必须有对应的验证结论（编译警告数、测试项数、实测数据），
  没有验证的改动不写进这里，因为写进来就意味着它是可信的。

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
