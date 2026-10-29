# Changelog

按工作日记录。每天追加一节，写清**改了什么、为什么改、验证了什么**。

写法约定：

- 一节一个日期，标题为 `## YYYY-MM-DD`。
- 每条以模块名或作用范围开头，用 `-` 列表。
- 只写有信息量的内容。"修了拼写"这类不需要记录。
- 每次改动必须有对应的验证结论（编译警告数、测试项数、实测数据），
  没有验证的改动不写进这里，因为写进来就意味着它是可信的。

---

## 2026-10-29

### thread-pool

- 有界队列与提交背压，1.1.0（代码在 10-28 写完，本节记录 10-29 的验证结果）。
  1.0.0 只有无界队列：提交速度持续高于消费速度时，队列与任务节点线性增长，
  最后在某一次 `submit` 里以 `malloc` 失败收场，调用方没有回退路径。
  新增 `threadpool_create_bounded` / `threadpool_submit_wait` /
  `threadpool_capacity` / `threadpool_backpressure` 与状态码 `TPOOL_ERR_FULL`
  （枚举末尾，旧值不变；句柄不透明，无 ABI 破坏）。结构上加第三个条件变量
  `space`、`max_queued` / `waiting` / `backpressure` 三个字段。
- 两处不改就会出错的细节：`destroy` 必须等 `waiting` 归零才释放池，
  否则被唤醒的提交方还持着已释放的互斥量（C11 6.2.4p2，对象生存期已结束）；
  `submit_wait` 醒来必须重查 `closed`，漏掉即把任务塞进停机中的池，
  提交方拿到 `OK` 而任务永不执行。工作线程调用 `submit_wait` 时满队列直接
  `FULL`，不等待空位——取任务的只有工作线程，都去等空位就没人消费，必然死锁。
- 驱动层加 `bounded` 子命令（`-c 容量`，阻塞提交，输出 `capacity=` /
  `backpressure=`）；`check_cli.sh` 加 8 项；`bench` 加第 4 组，
  `fork` 两个子进程分别跑无界与容量 64（`ru_maxrss` 是进程级高水位，
  同一进程内连跑两组，第二个数一定包含第一个的峰值）。
- 测试 54 → **109 项**（新增 6 组用例：参数与容量、满即失败、阻塞提交、
  工作线程不等待、4×500 生产者采样队列深度、destroy 唤醒阻塞提交方）。
  用例通过"闸门"构造"队列此刻恰好处在满/非满"这个事实，不靠 `sleep` 猜时序；
  闸门自带超时，实现有缺陷时报 FAIL 而不是把 `make test` 挂死。
- 实测（4 vCPU CI 运行器，`make bench` 第 4 组，1 个工作线程，每任务忙等 3 µs，
  提交 20 万）：无界 8.5 ~ 9.9 ms、队列深度峰值 197008 ~ 197540、RSS 6964 ~ 7092 KiB；
  容量 64 时 852 ~ 887 ms、深度硬性 64、背压 101409 ~ 107864 次、RSS 1204 KiB
  （内存省 5.8 倍，代价是提交端被串行化）。

### 测试与验证（thread-pool 的验证通道换成 CI）

- 10-28 写完之后内网 Ubuntu 连不上（本机换了网段），本机也没有任何 Linux 工具链，
  于是把验证搬到 GitHub Actions：`ubuntu-22.04`、gcc 11.4.0，与各模块 README
  记录的编译器同版本。选这个镜像的理由写进 workflow 文件头：换到 24.04（gcc 13）
  会引入新警告，已经写进文档的"0 条警告"就不再成立于同一前提。
- 第一次真跑抓到 11 项断言失败，**全部在测试代码里，实现一行没改**：
  闸门用 `clock_gettime(CLOCK_MONOTONIC)` 构造截止时间，而
  `pthread_cond_timedwait` 按条件变量自身的时钟解释它，默认是 `CLOCK_REALTIME`
  （POSIX `pthread_condattr_getclock` 的默认值）。monotonic 的值是"开机以来的
  时长"，当成 1970 年的时间点必然已经过去，`timedwait` 立刻返回 `ETIMEDOUT`——
  闸门从不阻塞，退化成空操作，于是"满队列"被瞬间排空、"背压计数"恒为 0。
  修法：`pthread_condattr_setclock(CLOCK_MONOTONIC)`，并让闸门记录 `timed_out`，
  在"闸门本应按住"的用例里断言它没被兜底超时放开（这条断言即回归检查）。
- TSan 报的一处 data race 也在测试里：`workerprobe.status` 由工作线程写、
  主线程在 `sleep_ms(50)` 之后读。`sleep` 不建立 happens-before，
  两条访问仍构成数据竞争（C11 5.1.2.4p25 的"冲突"定义不看实际是否交叠）。
  改为工作线程 release 存储 `done`、主线程 acquire 读等待它。
- 流水线本身也有一个缺陷：`make test 2>&1 | tee log` 的退出状态取管道最后一段
  （`tee`，恒为 0），不加 `set -o pipefail` 时测试失败而步骤报绿。第一轮的
  11 项失败因此只体现在报告文字里、没有体现在运行结论上。

### CI

- 新增 `.github/workflows/ci.yml`：默认构建、单元测试、`check`、ASan+UBSan、
  TSan、`bench`、加严警告构建，零警告与 sanitizer 干净作为硬条件；
  `SKIP=mysql-client`（需要客户端头文件与活的服务器，`check-live` / `check-scale`
  依赖真实数据库）。同一分支的新 push 取消旧运行。
- 汇总写到 `ci-reports` 分支（每次提交一个文件）。原因：Actions 日志读取要凭据，
  仓库内容不用，报告落在仓库里才是任何人都能查的构建记录；
  代码分支的 diff 也因此不被机器产物污染。失败时把日志尾部一并写进汇总，
  否则"失败"这个事实只能靠人猜。
- `-Wcast-qual` 单独计数、不判定失败：`const` 查询函数要给同步原语加锁，
  而 `pthread_mutex_lock` 的形参非 `const`；按 C11 6.7.3p6，只要对象本身不是
  `const` 限定的，通过转型丢弃 `const` 再修改它是良定义的（`counter` 与
  `threadpool` 都由 `create` 分配、调用方持有非 const 句柄）。
  彻底消掉要把同步原语移出对象（C 没有 `mutable`），已记入两个模块的待办。
- 统计口径踩过一个坑：想数"-Wcast-qual 有几条"不能 `grep -c 'Wcast-qual'`，
  make 回显的编译命令里也含这个旗标，实测数出 25 条而告警总数只有 12 条。
  只数"告警行里的 `-Wcast-qual`"才对。

### 根 Makefile

- 补 `tsan` 转发目标：此前只有单个模块定义了 `tsan`，根入口没有，
  于是 `concurrent-lock` 从没在 TSan 下构建过。加上之后第一次构建就报两处
  `-Wunused-function`——raw 施加例故意制造数据竞争，TSan 构建用
  `-DCOUNTER_TEST_SKIP_RAW` 把它从 `main` 里关掉，但只关了调用点，
  没关 `static` 函数的定义（`test_raw_demonstrates_loss`、`report_value`）。
  把定义纳入同一个条件编译块，两种构建都干净。

### 全仓验证（CI，2026-10-29，ubuntu-22.04 / gcc 11.4.0）

- 默认构建 0 警告；加严警告（追加 `-Wpedantic -Wundef -Wvla -Wformat=2
  -Wshadow -Wstrict-prototypes -Wmissing-prototypes`）0 条，
  另有 12 条 `-Wcast-qual` 记录在案、不参与判定。
- `make test` 全仓 **892 项 0 失败**：concurrent-lock 68、contact-list 209 + 注入 466、
  thread-pool 109、word-count 40。mysql-client 被 `SKIP` 排除，其 119 项不在其中。
- `make check`：contact-list 30 项、thread-pool 30 项、word-count 的两个脚本
  （`wc` 对照 6 项、并行与顺序 28 项比对）全部一致。
- `make sanitize`：0 警告，同一套 892 项全过，无 ASan / UBSan / LSan 报告。
- `make tsan`：0 警告，concurrent-lock 67 项（raw 用例按设计关闭）、
  thread-pool 109 项、word-count 40 项，无 data race 报告。
- `src/*/testdata/*` 未改动：本次 diff 里没有任何 testdata 路径，字节数仍是原值。

## 2026-10-27

### contact-list

- 删除时按装载因子收缩桶表：装载因子低于 1/4 时桶数减半，删空时整块释放桶数组。
  1.1.0 只涨不缩，装过十万条再删到十条仍占 128K 个桶（1 MiB）。1.2.0 实测
  n = 64000 删到 1024 条，桶数 65536 → 4096 → 2048（第一轮抖动再减一档），
  堆占用 10532 KiB → 212 KiB。
- 收缩与扩容共用同一个 `index_rehash`：换一张表、按新桶数重算每个节点的下标，
  只改 `hash_next` 不动 `prev` / `next`。因此收缩不影响遍历顺序，`save` 写出的
  文件与收缩前逐字节相同（单元测试断言行数与首行）。
- 阈值取 1/4 而不是 1/2：让收缩与扩容都把表留在"半满"，两个方向各 2 倍余量。
  理由与过程写进 README 3.11——第一版理由抄的是教科书上的"1/2 会抖动"，
  写完测试一量发现抖动并不发生（扩容判据是"插入后条数 > 桶数"，减半后
  `count <= nbuckets - 1`，要连插两条才再翻倍，2000 轮删加只有 1 次重分配）。
  1/2 真正的问题是收缩后装载因子约 1.0、表是满的，刚还回去的内存立刻处在
  "下一条记录就要拿回来"的状态；取 1/4 后收缩落在 0.5。
- `index_shrink` 用 `while` 而不是 `if`：收缩分配连续失败会积压，
  桶表 1024、count 降到 127 时，一次删除要连减两档（1024 → 512 → 256）。
  分配失败按降级处理（删除已经完成，返回 `NOMEM` 等于谎报"没删掉"），
  与扩容失败同一套理由。
- `contact_book` 与 `contacts_index_stats` 各加一个 `rehashes`（桶表重分配累计次数）。
  起因是验证"阈值附近反复增删会不会反复重建"时，测试里 `--wrap=calloc` 数不准：
  `-O2` 下 GCC 把 `malloc(n)` 紧跟 `memset(p, 0, n)` 合并成 `calloc(1, n)`
  （两者可观察行为相同，变换合法），而 `contacts_add` 正好是"分配节点 + 全零"，
  于是节点分配也进了计数，每轮恒为 1。计数器放进模块里才是确定值。
- 驱动层新增 `stats` 子命令，输出 `count / buckets / entries / used / max_chain /
  rehashes / load`（每行一个 `key: value`，便于脚本取单字段）。它让"桶数随删除收缩"
  在命令行上可直接观察，端到端测试因此能断言桶数而不必靠耗时反推。
- 版本 1.2.0，ABI 与 1.1.0 不兼容：`contact_book` 与 `contacts_index_stats`
  末尾各多一个 `size_t`（`contact` 未变）。`contact_book` 由调用方在栈上分配，
  库与调用方必须一起重编。
- 测试：`test_contacts` 新增收缩一节（174 → 209 项）——1/4 处不收缩、255 条时减半、
  一路减到下限 16、删空后 `buckets` 归 0、删空后再插入重建索引、收缩后遍历顺序仍是
  插入顺序、收缩后保存的文件行数与首行不变、累计重分配次数 7（建表 1 + 扩容 6）
  与 13（再收缩 6 次）。`test_alloc` 新增三组（410 → 466 项）：收缩分配失败只降级
  且不计入 `rehashes`、下一次删除重试成功、连续失败后的积压连减两档、
  边界处 2000 轮增删只重分配 1 次且装载因子回到 0.5。`check_cli.sh` 新增 9 项
  （21 → 30）：`stats` 与 `count` 一致、空表 `load` 为 `n/a`、批量载入 20 条时
  32 个桶、删到 3 条时回到下限 16。
- 验证（Ubuntu 22.04 / gcc 11.4.0 / Linux 6.8）：默认构建 0 警告；
  追加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow` 0 警告；
  `make sanitize`（ASan + UBSan，含注入测试）无报告；单元 209 + 注入 466 + CLI 30 全过；
  全仓 `make test` 68 + 209 + 466 + 99 + 20 + 54 + 40 = **956 项** 0 失败；
  全仓 `make check` 退出码 0（contact-list 30、mysql-client 41、thread-pool 19）。
- 反向验证：收缩阈值改 1/2 → 单元 3 项、注入 9 项 FAIL，其中
  `抖动后装载因子回到 0.5` 实际 0、`抖动起点桶数为 1024` 实际 512；
  `while` 改 `if` → `一次删除连减两档到 256` 实际 512、`连减两档计入两次重分配` 实际 1；
  判据 `<` 改 `<=` → `装载因子 1/4 时不收缩` 实际 512（期望 1024）。三条全部被抓住。
- 实测（`make bench` 末尾新增收缩一段，n = 64000）：删到 1024 条耗时 5.1 ms
  （62976 次删除，单次 0.08 µs）；随后 10 万轮"删一条加一条"12.2 ms，
  `rehashes` 增量 1。testdata 字节数仍是 158 / 160 / 58，未动。

---

## 2026-10-26

### contact-list

- 加入哈希索引：`contact_book` 多一个桶数组，`contact` 多一个桶链指针，
  `contacts_find` / 查重 / `contacts_remove` 从 O(n) 变成 O(1) 均摊，
  因此 `build` 与 `load` 从 O(n²) 回到 O(n)。接口语义不变（只新增一个诊断函数），
  1.1.0 的 ABI 与 1.0.0 不兼容：`contact` 从 136 字节变成 152 字节，库与调用方必须一起重编。
  动机是 9.1 与 9.2 两条已知局限已经与本模块的用途冲突——六万四千条时单次查找
  132 µs、载入 5.4 s，任何批量导入都不可用，而这已经不是常数问题。
- 选分离链接而不是开放寻址：删除时只改两个指针，与链表删除同构，不需要墓碑或搬移。
  桶数取 2 的幂，用 `nbuckets - 1` 掩码代替取模（运行期除数无法被编译器优化成乘法）。
  哈希用 FNV-1a：逐字节、乘数为奇数（`0x100000001b3`，低字节 `0xb3`；
  乘数为偶数会逐步清零低位熵，用低位取下标就会退化）、不带随机种子以便结果可复现。
- 装载因子超过 1.0 时桶数翻倍。**扩容分配失败只降级不失败**（旧表继续用，结果仍然正确，
  只是链更长）；**首次建索引失败必须返回 `NOMEM`**（否则等于把 O(n²) 悄悄还给调用方）。
  重挂桶链只改 `hash_next`，不动 `prev` / `next`——否则遍历顺序不再等于插入顺序，
  `save` 写出的文件会跟着变，破坏 3.3 的文件契约。
- 删除用二级指针拆链：`link` 指向"存放该节点指针的位置"，链首与链中间共用一段代码。
  分配顺序改为"先索引、后节点"，使节点分配失败时桶表只是一张空表，仍然自洽。
- 修掉一处被索引放大的缺陷：`contacts_load` 原来把 `contacts_add` 的 `NOMEM`
  与"坏行"归进同一个分支，内存耗尽时剩余所有行都被计成 `skipped` 并返回 `CONTACTS_OK`。
  改为立即中止并返回 `NOMEM`，`loaded` 是中止前的真实条数、`skipped` 保持 0。
- 新增诊断接口 `contacts_index_get`（桶数 / entries / 非空桶 / 最长链）。
  索引是内部实现，但这个接口让"两份结构是否一致""哈希分布是否合理"变成可断言的事实，
  而不是靠"跑得快所以大概对"。
- 新增 `tests/test_alloc.c`（410 项）与 `make test-alloc`：用 `-Wl,--wrap=malloc`
  与 `--wrap=calloc` 做分配失败注入，逐分配点核对六项不变量（`count`、遍历长度、
  索引 `entries`、已插入姓名全部可查、电话正确、未插入姓名不可查）。
  边界写清楚：`--wrap` 只重定向未定义引用，libc 内部（如 `fopen` 的 stdio 缓冲区）
  不受影响，注入的正是模块自己的分配。
- `tests/test_contacts.c` 增加索引用例（38 项，总数 136 → 174）：桶数增长点
  （16 条仍为 16 桶、第 17 条扩到 32）、两千条的 entries/count/遍历长度三者一致、
  装载因子 ≤ 1.0、最长链 ≤ 20、非空桶 > 50%、倒序删除一半后保留项全在、删除项全消、
  重名被拒后 entries 与 count 不变、全删后无残留命中。删除用例按倒序进行，
  因为桶链是头插：倒序删先删链首，正序删先删链尾，拆链写错通常只错一种。
- 文档：README 新增 2.8（诊断）、3.10（索引的全部取舍）、5.5（注入）、5.6（反向验证）、
  6.2（索引形态）、6.4（为什么仍要保留链表）；6.1 / 6.3 / 9.1 / 9.2 / 9.9 重写；
  新增 9.7（不随机化导致的哈希洪泛，给 28C3 / SipHash 的依据）、9.8（删除不收缩）。
  头文件同步接口契约与版本注释。
- 验证（Ubuntu 22.04 / gcc 11.4.0 / Linux 6.8）：
  默认构建 0 警告；追加 `-Wpedantic -Wcast-qual -Wundef -Wvla -Wformat=2 -Wshadow`
  0 警告（`-Wcast-qual` 抓到过一处把 `const contact *` 强转回非 const 的写法，已改成非 const 迭代）；
  `make sanitize`（ASan + UBSan，含注入测试）0 报告；单元 174 + 注入 410 + CLI 21 全过；
  全仓 `make test` 68 + 174 + 410 + 119 + 54 + 40 = **865 项** 0 失败。
- 反向验证（证明测试抓得住错）：只挂链表不挂桶链 → 大量 FAIL；
  删除不拆桶链 → 普通构建 `double free or corruption`、ASan 报 `heap-use-after-free`
  （栈顶 `__interceptor_strcmp`）；哈希恒返回 0 → `最长链不超过 20` 与 `非空桶超过一半` 失败；
  扩容不重挂 → `entries 等于 count` 实际 976（期望 2000）。四条全部被抓住。
- 实测（同一台机器同一次会话，`make bench`）：n = 64000 时
  `build` 5019.7 → 10.7 ms（469×）、`find`（2n 次）16868.8 → 8.4 ms（2008×）、
  `load` 5358.9 → 11.2 ms（478×）；`save` 8.1 → 4.7 ms 属于页缓存噪声，作为对照组。
  单次 `find` 132 µs → 65 ns。索引形态：65536 桶、装载因子 0.98、最长链 7、
  非空桶 56%、堆占用 10016 KiB（160 B/条，与 152 字节节点 + 8.2 字节桶表的算术一致）。
  testdata 字节数仍是 158 / 160 / 58，未动。

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
