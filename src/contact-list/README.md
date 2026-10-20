# 04 通讯录（contacts 模块）

一个可复用的通讯录模块，加一个建立在它之上的命令行驱动。

模块用双向链表保存联系人，提供增删查、游标遍历与文件存取。核心是三条约定：
`name` 作为主键保证查找与删除的结果唯一；维护 `head` 与 `tail` 两个指针，
使追加是 O(1) 且遍历顺序等于插入顺序；写出的文件格式一定能被自己的读取器原样读回。

`include/` 与 `src/` 分离，`main.c` 只通过 `libcontacts.a` 链接，不重复编译模块源码——
"这个模块能被独立链接使用"这件事在每次构建时都会被动验证一遍。

```bash
$ ./bin/contacts -f book.txt add alice 13900001111
$ ./bin/contacts -f book.txt add bob 13800002222
$ ./bin/contacts -f book.txt list
name: alice, phone: 13900001111
name: bob, phone: 13800002222
$ ./bin/contacts -f book.txt find bob
name: bob, phone: 13800002222
```

---

## 1 目录结构

```
src/contact-list/
├── include/
│   └── contacts.h           对外接口：数据结构、状态码、增删查、遍历、存取
├── src/
│   ├── contacts.c           模块实现（链表 + 行解析 + 文件读写）
│   └── main.c               命令行驱动（子命令、菜单、输出排版、退出码）
├── tests/
│   ├── test_contacts.c      单元测试，136 项检查
│   ├── check_cli.sh         命令行端到端验证，21 项检查
│   └── bench_contacts.c     不同规模下的构建 / 查找 / 存取耗时
├── testdata/
│   ├── a.txt                158 字节，4 条，逗号后无空格的紧凑写法
│   ├── b.txt                160 字节，4 条，同上
│   └── c.txt                58 字节，2 条，逗号后有空格且标签不同
├── Makefile
└── README.md
```

编译产物统一落在 `bin/`，已被根目录的 `.gitignore` 忽略，不进版本库。

`include/` 与 `src/` 分开不是形式主义。头文件里只放调用方需要知道的东西——类型、
状态码、函数原型、字段容量；实现细节（链表指针怎么维护、行怎么切分、写文件用不用临时文件）
一律不出现在头文件里。调用方读 `contacts.h` 就能用，不需要读 `contacts.c`。

---

## 2 对外接口

### 2.1 数据结构

```c
typedef struct contact {
    char name[CONTACT_NAME_MAX];      /* 64 字节，含 '\0' */
    char phone[CONTACT_PHONE_MAX];    /* 64 字节，含 '\0' */
    struct contact *prev;
    struct contact *next;
} contact;

typedef struct {
    contact *head;      /* 表头 */
    contact *tail;      /* 表尾 */
    size_t   count;     /* 节点数量 */
} contact_book;
```

结构体成员是公开的，为的是让调用方能在栈上直接分配 `contact_book`——
嵌入式环境下不希望在打开一个通讯录时还依赖堆。但约定不直接读写其中字段，
一律通过下面的函数访问。

两个容量宏也是公开的，调用方可以直接用 `CONTACT_NAME_MAX` 声明临时缓冲区，
不必硬编码一个数字——硬编码的后果是模块扩容后调用方的缓冲区悄悄变小，而编译器不会报警。

### 2.2 状态码

```c
typedef enum {
    CONTACTS_OK             = 0,   /* 成功 */
    CONTACTS_ERR_PARAM      = 1,   /* 参数非法，例如传入了 NULL */
    CONTACTS_ERR_NOMEM      = 2,   /* 内存分配失败 */
    CONTACTS_ERR_OPEN       = 3,   /* 无法打开文件 */
    CONTACTS_ERR_READ       = 4,   /* 读取过程中出错 */
    CONTACTS_ERR_WRITE      = 5,   /* 写入或改名失败 */
    CONTACTS_ERR_CLOSE      = 6,   /* 关闭文件时出错 */
    CONTACTS_ERR_FORMAT     = 7,   /* 字段内容不符合格式约定，或行无法解析 */
    CONTACTS_ERR_TOOLONG    = 8,   /* 字段超出容量 */
    CONTACTS_ERR_NOTFOUND   = 9,   /* 没有这个联系人 */
    CONTACTS_ERR_DUPLICATE  = 10   /* 姓名已存在 */
} contacts_status;
```

沿用 C 语言的惯例：0 表示成功，非 0 表示失败。`CONTACTS_OK` 固定为 0，
因此可以直接写 `if (status)` 判断失败。

`CONTACTS_ERR_FORMAT` 与 `CONTACTS_ERR_TOOLONG` 分开是有必要的：前者是"字段内容
里有逗号或者整行缺字段"，后者是"字段太长"。驱动层据此给出不同的提示，
用户才知道该改内容还是改长度。

### 2.3 生命周期

| 函数 | 作用 |
|---|---|
| `void contacts_init(contact_book *book)` | 初始化为空表，栈上分配之后必须先调用 |
| `void contacts_clear(contact_book *book)` | 释放全部节点，回到空状态，可重复调用 |
| `void contacts_destroy(contact_book *book)` | `contacts_clear` 的别名，表达"这本书到此为止" |
| `size_t contacts_count(const contact_book *book)` | 返回数量，`book` 为 NULL 时返回 0 |

### 2.4 增删查

| 函数 | 说明 |
|---|---|
| `contacts_status contacts_add(book, name, phone)` | 追加到表尾；重名返回 `DUPLICATE` |
| `contacts_status contacts_remove(book, name)` | 按姓名删除；找不到返回 `NOTFOUND` |
| `const contact *contacts_find(book, name)` | 按姓名查找；找不到返回 NULL |

`contacts_add` 对字段的要求是：非空、长度小于容量、不含逗号与换行符。
这三条不是随手定的，理由见 3.3 与 3.4。

返回的 `const contact *` 在下一次 `contacts_remove` / `contacts_clear` 之前保持有效。
追加不会移动已有节点，所以已有的指针在 `contacts_add` 之后仍然可用；
只有删除和清空才会让它失效。

### 2.5 遍历

游标式，而不是"传一个回调函数进去"：

```c
for (const contact *c = contacts_first(book); c != NULL; c = contacts_next(c)) {
    printf("%s %s\n", c->name, c->phone);
}
```

选游标而不选回调，有两个原因：调用方可以在任意位置提前结束，不需要靠回调的返回值
表达"停下来"；也不需要为每种用途定义一个回调类型。遍历顺序是插入顺序。
遍历期间不要增删节点，否则游标会失效。

### 2.6 文件存取

```c
contacts_status contacts_save(const contact_book *book, const char *path);

contacts_status contacts_load(contact_book *book, const char *path,
                              contacts_load_mode mode,          /* APPEND / REPLACE */
                              contacts_load_result *result);    /* 可以为 NULL */
```

`contacts_load_result` 报告这次载入发生了什么：

```c
typedef struct {
    size_t loaded;            /* 成功载入的条数 */
    size_t skipped;           /* 跳过的行数 */
    size_t first_bad_line;    /* 第一个被跳过的行号，0 表示没有 */
} contacts_load_result;
```

`skipped` 只统计"有内容但不合法"的行，空行被静默忽略、不计入。
把这三个数暴露出来，是因为载入是唯一一个"部分成功"是常态的操作——
调用方需要知道是全部读进来了，还是有一部分被丢掉了。

### 2.7 辅助

| 函数 | 作用 |
|---|---|
| `const char *contacts_status_string(status)` | 状态码转可读描述，未知值返回 `"unknown status"`，不会返回 NULL |
| `contacts_status contacts_check_field(value, capacity)` | 单独校验一个字段，供驱动层给出针对性提示 |

`contacts_status_string` 保证不返回 NULL，是为了让调用方能直接写
`printf("%s", contacts_status_string(s))` 而不用先判空。

---

## 3 设计要点

### 3.1 维护 `tail`：顺序与复杂度是同一个决定

只保存 `head` 也能实现插入，但只有两种写法：

- **头插**：O(1)，但遍历顺序与插入顺序相反。这是最开始那版的写法。
- **走到表尾再插**：顺序正确，但每次插入是 O(n)，批量构建变成 O(n²)。

再存一个 `tail` 指针，两个问题一起消失：追加是 O(1)，顺序就是插入顺序。

这一点的影响比看起来大。头插法下 `save` 写出的文件也是倒序，读回来再存一次，
顺序又反一遍——文件内容会随往返次数振荡。而顺序一旦和插入顺序不一致，
"把通讯录导出成文件"这件事就没有稳定结果，比较、diff、增量同步都做不了。

代价是删除时必须同时维护 `tail`。删除一个节点有四种位置：表头、表中、表尾、唯一节点。
下面五行覆盖全部情形：

```c
if (item->prev != NULL) item->prev->next = item->next;
else                    book->head = item->next;

if (item->next != NULL) item->next->prev = item->prev;
else                    book->tail = item->prev;
```

`else` 分支处理的是"被删的是第一个/最后一个"。唯一节点会同时走进两个 `else`，
`head` 与 `tail` 一起变成 NULL，不需要额外的特判。

### 3.2 `name` 作主键

同一个通讯录里不允许重名，`contacts_add` 遇到重名返回 `CONTACTS_ERR_DUPLICATE`。

这条约束换来的是接口的确定性：`contacts_find` 与 `contacts_remove` 都按姓名定位，
如果允许重名，删一个 `alice` 到底删的是哪一个就没法回答了。要么在接口上加"删第几个"，
要么在文档里写"删除第一个匹配的"——后者是隐含契约，调用方很容易忽略。

代价是每次添加都要全表查重，批量构建退化为 O(n²)。第 6 节有实测数据；
对通讯录这个场景（几百到几千条）代价可以忽略，到几万条才需要引入索引。

### 3.3 一个能被自己读回来的格式

写出的每一行固定是：

```
name: <姓名>, phone: <电话>
```

这条规则看起来平淡，但它是整个模块最重要的一条契约：**保存与读取必须互逆**。
每一次"保存 → 读取"，得到的内容都必须与保存前逐字节相同。

最初那版没有做到，实测：

```
保存到 rt.txt：
    name: alice, phone: 13900001111

读回后遍历输出：
    name: alice,phone: phone:13900001111
```

电话号码前面多了一个 `phone:`。原因是保存时逗号后带了一个空格，而解析器把
"逗号后的第一个空格"当成了字段名与值的分隔符，于是把 `phone:` 连标签一起吞进了值里。
每往返一次就多一层前缀，反复存几次之后号码变成 `phone: phone: phone:139...`。

这个缺陷不触发任何 sanitizer——从内存安全角度看没有任何越界，所有缓冲区都在范围内，
只是数据被悄悄地改错了。**这类缺陷只能靠"往返测试"发现**：保存、清空、读回、逐字段比较。

### 3.4 按标签定位，不按空格位置

解析器不依赖"第一个空格在哪"，而是先找标签的冒号：

```c
/* 第一个字段：行首到第一个冒号（冒号之前不能先出现逗号） */
p = line;
while (*p != '\0' && *p != ':' && *p != ',') p++;
if (*p != ':') return CONTACTS_ERR_FORMAT;

/* 值 = 冒号之后到第一个逗号，去掉两端空白 */
```

标签文本本身不参与语义，只看冒号。因此下面三种写法都能正确解析：

| 行 | 解析结果 |
|---|---|
| `name: liaoqf,telephone: 23456543789` | `liaoqf` → `23456543789` |
| `name: liaoqf, telephone: 23456543789` | `liaoqf` → `23456543789` |
| `name: liaoqf, phone: 23456543789` | `liaoqf` → `23456543789` |

而原来的"按第一个空格切"只能正确处理第一种。`testdata/c.txt` 就是第二种写法，
旧实现实测输出 `king --> phone:344567754`，新实现输出 `king --> 344567754`。

**写入端只用一种格式，读取端接受多种**——这是所有长期存在的文本格式的通用做法：
新版本写出的东西老版本读不了可以接受，老版本留下的文件新版本必须还能读。

### 3.5 字段里禁止逗号

既然逗号是字段分隔符，字段内容里就不能出现逗号，否则写出去的行再也无法唯一解析。
`contacts_check_field` 在入口处拒掉含 `,` `\n` `\r` 的值。

另一条路是转义：把逗号写成 `\,`，读取时再还原。但通讯录的姓名和电话本来就不需要逗号，
为了一个不存在的需求引入转义规则，会让格式变复杂、让解析器出错的概率变高。
**约束数据比给数据加语法便宜得多。**

电话里的 `+`、`-`、`()` 都是允许的（`+8613800002222`、`010-12345678` 都能存），
只有真正会破坏格式的三个字符被禁止。

### 3.6 原子写入：先写临时文件再改名

```c
n = strlen(path) + sizeof ".tmp";
tmp = malloc(n);
snprintf(tmp, n, "%s.tmp", path);

fp = fopen(tmp, "w");
/* ... 逐行写 ... */
fclose(fp);
rename(tmp, path);      /* POSIX 上原子替换 */
```

直接往目标文件里写，中途失败（磁盘满、进程被 kill、断电）会留下一个被截断的文件，
原来的内容已经没了。写成"临时文件 + rename"，失败时目标文件保持原样，
最坏情况只是多出一个残留的 `.tmp`。

`rename` 在同一个文件系统内是原子操作，所以临时文件必须放在目标文件所在目录，
不能放 `/tmp`——跨文件系统时 `rename` 会返回 `EXDEV`，退化成"拷贝再删除"，原子性就没了。
这也是为什么临时文件名是 `path + ".tmp"` 而不是 `mkstemp` 加上某个全局临时目录。

代价是要求目标目录可写，而不只是目标文件可写。这一点写在头文件的注释里。

### 3.7 坏行只跳过，不中断整个载入

一个格式不对的行不应该让另外 999 行都读不进来。`contacts_load` 的处理是：
跳过该行、计入 `skipped`、记录第一个坏行的行号，然后继续。

```c
if (parsed != CONTACTS_OK
    || contacts_check_field(name, CONTACT_NAME_MAX) != CONTACTS_OK
    || contacts_check_field(phone, CONTACT_PHONE_MAX) != CONTACTS_OK) {
    local.skipped++;
    if (local.first_bad_line == 0) local.first_bad_line = lineno;
    continue;
}
```

只有文件级错误（打不开、读出错、关闭出错）才返回非 0 状态码。

有一类坏行需要单独处理：**超过缓冲区长度的行**。`fgets` 读满缓冲区时不会带换行符，
剩下的字节还在流里。如果直接当下一行处理，一行坏数据会变成两行"看起来正常"的数据——
比直接丢掉的危害大得多。所以检测到这种情况时，把该行剩余字节吃掉，整行作废：

```c
if (len == sizeof line - 1 && line[len - 1] != '\n') {
    int ch;
    while ((ch = fgetc(fp)) != EOF && ch != '\n') { }
    local.skipped++;
    continue;
}
```

### 3.8 `errno` 的保存与恢复

```c
saved = errno;
if (fclose(fp) != 0 && status == CONTACTS_OK) {
    status = CONTACTS_ERR_CLOSE;
    saved  = errno;
}
if (status == CONTACTS_OK && rename(tmp, path) != 0) {
    status = CONTACTS_ERR_WRITE;
    saved  = errno;
}
errno = saved;
```

`fclose` 会刷新缓冲区、可能触发实际的 `write` 系统调用，因此它**可能改写 `errno`**。
先读 `errno` 再 `fclose`，读到的可能是关闭阶段的错误号，把真正的失败点覆盖掉；
`fclose` 成功时还可能把 `errno` 改成一个与本次失败无关的残留值。

驱动层还有一个配套判断：

```c
static int has_errno(contacts_status status) {
    return status == CONTACTS_ERR_OPEN || status == CONTACTS_ERR_READ
        || status == CONTACTS_ERR_WRITE || status == CONTACTS_ERR_CLOSE;
}
```

只有文件级错误才去 `strerror(errno)`。参数错误、重名、格式错误这三类的 `errno`
是上一次系统调用的残留值，照着它打印会输出一句与本次失败毫不相干的话。

### 3.9 `contacts_check_field` 单独暴露

"字段不合法"和"重名"需要给用户不同的指引：前者要改内容，后者要么改名要么先删。
如果模块只返回一个笼统的失败，驱动层要么重复实现一遍校验逻辑，要么给出含糊的提示。
把校验函数单独暴露，驱动层就能在调用 `contacts_add` 之前先判断是哪一类问题——
校验逻辑仍然只有一份。

---

## 4 构建

```bash
make            # 构建静态库 bin/libcontacts.a 与命令行工具 bin/contacts
make lib        # 只构建静态库
make cli        # 只构建命令行工具
make test       # 构建并运行单元测试
make check      # 命令行端到端验证
make sanitize   # 用 ASan + UBSan 重新构建并跑测试，产物在 bin/sanitize/
make bench      # 不同规模下的耗时
make clean      # 删除 bin/（含全部变体）
```

变体构建使用独立输出目录，是因为 make 判断是否重建只比较时间戳、不比较编译选项：
共用 `bin/` 时先跑 `make sanitize` 再跑 `make`，普通构建会认为目标已是最新，
留下一个插桩过的可执行文件，运行时报链接期符号错误。

默认编译参数：

```make
CFLAGS   ?= -std=c11 -Wall -Wextra -Wconversion -O2 -g
CPPFLAGS += -Iinclude
```

| 参数 | 作用 |
|---|---|
| `-std=c11` | 固定语言标准，避免依赖编译器的默认方言 |
| `-Wall -Wextra` | 常规警告集；本主题里有一处 `int` 函数缺 `return` 的缺陷只有它能发现 |
| `-Wconversion` | 检查隐式窄化转换。`fgets` 的长度参数、`snprintf` 的缓冲区大小都在 `size_t` 与 `int` 之间来回，打开它能防住静默截断 |
| `-O2` | 优化；模块不依赖任何未定义行为，优化不会改变结果 |
| `-g` | 保留调试信息，配合 `make sanitize` 时能定位到具体行 |

`CFLAGS ?=` 是条件赋值，可以在命令行上覆盖：

```bash
make CFLAGS="-std=c11 -Wall -Wextra -Wconversion -O0 -g -funsigned-char"
```

**把模块当库用：**

```bash
cc -std=c11 -Wall -Wextra -Wconversion -O2 -Iinclude your_app.c bin/libcontacts.a -o your_app
```

静态库要放在引用它的目标文件之后——`ar` 生成的库按需抽取成员，顺序反了会报未定义符号。

---

## 5 测试与验证

验证环境：**Linux 6.8.0-138 / gcc 11.4.0 / x86_64**。

### 5.1 编译警告

| 编译参数 | 结果 |
|---|---|
| `-std=c11 -Wall -Wextra -Wconversion -O2 -g` | **0 条** |
| `make sanitize`（追加 `-fsanitize=address,undefined`） | 0 条 |

### 5.2 单元测试

`make test`，136 项检查：

```
contacts 1.0.0 单元测试（testdata = testdata）

ok    空通讯录数量为 0
ok    空通讯录 first 为 NULL
ok    空通讯录 find 为 NULL
ok    contacts_count(NULL) 返回 0
...
ok    重名不增加数量
ok    重名时保留原号码
ok    第 0 个节点存在
ok    第 0 个是 c
ok    第 1 个是 a
ok    第 2 个是 b
ok    遍历到末尾
ok    删除表头 a
ok    删表头后第一个是 b
ok    表头 prev 仍为 NULL
ok    删除表中 b
ok    a 的 next 指向 c
ok    删除表尾 c
ok    遍历只有 a 与 b
ok    删除唯一节点
ok    空表再删返回 NOTFOUND
...
ok    往返后数量一致
ok    第 0 条电话一致
ok    保存格式为 name: X, phone: Y
ok    c.txt 第 1 条电话
ok    重复追加 skipped 为 2
ok    好行载入 2 条
ok    坏行跳过 3 条
ok    第一个坏行是第 3 行
ok    超长行整行作废，只载入 2 条
ok    载入不存在的文件

136 项检查，0 项失败
```

其中三组用例值得单独说明：

- **跨位置删除**（表头 / 表中 / 表尾 / 唯一节点）各自独立验证，删完还要检查
  `head->prev` 仍为 NULL、遍历能正常终止。链表删除的 bug 通常就藏在这四种情形的差异里。
- **字段容量边界**：长度恰好等于容量时必须拒绝（没有位置放 `'\0'`），
  等于容量减 1 时必须接受。差一个字节的边界是最容易写错的地方。
- **超长行**：造一行 900 字符、里面含逗号和冒号的数据，验证它被整行作废，
  而不是被截断成两条"看起来正常"的记录。

`main` 返回值是失败项数，可以直接接进其他构建系统。

### 5.3 命令行端到端

`make check`，21 项检查：

```
命令行端到端验证

ok    工作文件不存在时视为空表
ok    三次 add 后数量为 3
ok    工作文件内容与插入顺序
ok    另起一个进程仍能读到 3 条
ok    find 命中
ok    find 落空退出码为 1
ok    删除后数量为 2
ok    重复删除退出码为 1
ok    重名添加退出码为 1
ok    姓名含逗号被拒绝，退出码为 1
ok    无参数退出码为 2
ok    未知命令退出码为 2
ok    载入不存在的文件退出码为 1
ok    保存→读回 内容完全一致
ok    第二次往返仍逐字节相同
ok    往返不累积字段标签
ok    list 输出可直接作为输入文件
ok    载入 a.txt（4 条）
ok    载入 b.txt（4 条）
ok    载入 c.txt（2 条）
ok    c.txt 的电话不带字段标签

全部通过（21 项检查）
```

两个检查是为了固定住格式契约：

- **往返逐字节相同**：把保存出来的文件再载入到一个新文件，`diff` 两次的 `cat` 输出。
  跑两轮，确认不会累积出 `phone: phone:` 这种前缀。
- **`list` 输出可直接作为输入文件**：这等价于说"输出格式就是输入格式"，
  一条命令就能成立，不依赖任何额外约定。

### 5.4 消毒剂

`make sanitize`，用 `-fsanitize=address,undefined -fno-omit-frame-pointer -O1` 重新构建：

```
--- 单元测试（ASan + UBSan）---
136 项检查，0 项失败

--- 命令行（ASan + UBSan）---
name: alice, phone: 13900001111
name: bob, phone: 13800002222
contacts: loaded 2, skipped 0
name: king, phone: 344567754
name: wbj, phone: 1234567865

--- 错误路径 ---
contacts: bin/nonexistent.txt: cannot open file: No such file or directory
```

没有任何 ASan 或 UBSan 报告，也没有 LeakSanitizer 的泄漏报告。
`contacts_destroy` 在 `main` 的每条退出路径上都会执行，包括错误路径。

### 5.5 内存与资源

| 验证项 | 结果 |
|---|---|
| ASan（越界、泄漏） | 无报告 |
| LeakSanitizer（退出时链表未释放） | 无报告，`contacts_destroy` 遍历释放全部节点 |
| 重复 `contacts_destroy` | 不崩溃，数量归 0 |
| 各接口传 NULL | 返回 `CONTACTS_ERR_PARAM` 或安全返回，不崩溃 |
| 保存到不可写的目录 | 返回 `CONTACTS_ERR_OPEN`，不留半成品文件 |
| 载入不存在的文件（REPLACE 模式） | 返回 `CONTACTS_ERR_OPEN`，书内容不被改动 |

最后一条值得说明：`CONTACTS_LOAD_REPLACE` 是**先清空再打开**还是**先打开再清空**，
行为不同。实现里先 `fopen`，成功之后才 `contacts_clear`——否则一个打不开的路径
会把现有内容清掉，用户什么都没得到还丢了数据。

---

## 6 性能

链表方案的代价是可预期的，`make bench` 把它量出来：

```
       n    build(ms)     find(ms)     save(ms)     load(ms)       loaded
--------------------------------------------------------------------------
    1000          1.5          4.0          0.1          1.3         1000
    4000         20.4         65.0          2.2         21.1         4000
   16000        325.6       1091.3          1.4        338.7        16000
   64000       5372.7      18254.9          5.0       5453.5        64000
```

`find` 一列做的是 2n 次查找（n 次命中 + n 次落空）。

### 6.1 三个不同的复杂度

| 操作 | 复杂度 | 原因 |
|---|---|---|
| `save` | **O(n)** | 顺序遍历，每个节点一行 `fprintf` |
| `find` | **O(n)** | 线性扫描，平均比较 n/2 次 |
| `build` / `load` | **O(n²)** | 每加一条都要全表查重（`name` 是主键的代价） |

数据完全对上：n 从 4000 涨到 16000（4 倍），`build` 从 20.4 ms 涨到 325.6 ms（16 倍）；
`save` 从 0.4 ms 涨到 1.4 ms（约 3.5 倍，线性）；`find` 从 65 ms 涨到 1091 ms（约 17 倍，
线性，因为是 2n 次查找）。

单次操作换算一下：

| n | 单次 `find` | 单条 `load` |
|---|---|---|
| 1000 | 约 2.0 µs | 约 1.3 µs |
| 64000 | 约 143 µs | 约 85 µs |

### 6.2 这个代价可以接受吗

**通讯录场景可以。** 一个人的联系人通常在几百到一两千条，此时构建 1.5 ms、
载入 1.3 ms、查找 2 µs——全部在亚毫秒级，用户感知不到。
O(n²) 的曲线要到 1.6 万条以上才进入百毫秒区间，而没有人会把一万六千个联系人
放进一个通讯录里。

**换一个场景就不可以。** 如果这条代码被拿去处理十万行的 CSV 导入，
5 秒的载入时间会让人立刻去查问题。这时候需要的不是优化常数，而是换数据结构。

### 6.3 什么时候该换索引

三个方向，代价递增：

1. **保持链表，把查重换成布隆过滤器**：能挡掉绝大多数"不存在"的情况，
   但布隆过滤器有假阳性，而这里的场景恰好是"真命中也常见"，收益有限。
2. **加一个哈希索引**：`contact_book` 里多一个哈希表，主键映射到节点指针。
   `find`、`add` 的查重、`load` 全部变成 O(1) 均摊，`save` 仍走链表保持顺序。
   代价是多一份内存（每个桶一个指针）和"删节点时要同步删索引"的一致性负担。
3. **换成平衡树 / 跳表**：能同时支持有序遍历和 O(log n) 查找，
   但实现复杂度比哈希表高一个量级，对通讯录是过度设计。

本模块停在方案 0（纯链表），因为主题是双向链表与文件持久化。
真要往方案 2 走，接口不用动——`contacts_find` / `contacts_add` / `contacts_remove`
的语义不变，只是内部多一个索引结构。这也算"接口与实现分离"带来的好处之一。

### 6.4 一个小细节：`save` 为什么比 `load` 快两个数量级

`save` 是 O(n) 的纯顺序写入，64000 条只用了 5 ms（约 500 MB/s，走的是页缓存）。
`load` 每条都要全表查重，同一份数据要 5.5 秒。差的不只是复杂度，还有常数：
`save` 每次只做一次 `fprintf`，`load` 每行要做一次解析、两次字段校验、
一次可能遍历全表的 `strcmp`。

---

## 7 实现记录：从菜单 demo 到模块

这个主题从一份 355 行的教学代码 `contacts.c` 开始，经过两轮改造才成为模块：

| 轮次 | 产物 | 规模 | 做了什么 |
|---|---|---|---|
| 起点 | 原版 `contacts.c` | 355 行 | 能跑，但有多处缺陷，且完全是 demo 形态 |
| 第一轮 | 同一文件的修订版 | 355 行 | 补文档、修格式描述、扩充缺陷清单 |
| 第二轮 | **本模块** | 头文件 + 实现 + 驱动 + 测试 | 接口与实现分离，修掉全部缺陷，可被独立链接 |

原版源码保留在 git 历史里，可以随时取回对照：

```bash
git show 8d9c40f:04-contact-list/contacts.c > /tmp/contacts_orig.c
```

### 7.1 起点

原版是一个启动即进菜单的交互程序：`main` 里 `while (1)` 打印菜单、`scanf` 读选项、
`switch` 分派到 `insert_entry` / `print_entry` / `delete_entry` / `search_entry` /
`save_entry` / `load_entry`。所有函数都在一个文件里，所有输入都走 `scanf`。

这个结构的问题不在于"能不能用"，而在于**它没法被测试**：任何验证都只能往标准输入里
灌一串数字，菜单顺序一改，全部测试作废。

### 7.2 修掉的缺陷

| # | 缺陷 | 依据与后果 | 处理 |
|---|---|---|---|
| 1 | `char name[16]` / `char phone[32]` 偏小 | 姓名只有 15 字节可用，UTF-8 下一个汉字 3 字节，五个字就满 | 容量提到 64 / 64，并且公开成宏 |
| 2 | `scanf("%s", ...)` 无宽度限制 | `%s` 不检查目标缓冲区容量，输入超长直接越界写栈 | 驱动层改用 `fgets` + 长度校验，模块层统一走 `contacts_check_field` |
| 3 | `parser_token` 第一个循环只认逗号 | 行内没有逗号时越过 `'\0'` 继续读（`'\0' != ','`），ASan 报 `stack-buffer-overflow`，访问偏移 256 越过了 128 字节的 `buffer` | 解析器先判分隔符是否存在，长度全程有界 |
| 4 | `parser_token` 写 `name[j++]` 无边界检查 | 字段超长时溢出调用方缓冲区，ASan 实测报在 `contacts.c:123` | 解析结果先校验长度再拷贝 |
| 5 | **存取不互逆** | `save` 写 `, phone: `，解析器按"第一个空格"切，读回变成 `phone:139...`，每往返一次多一层前缀 | 统一格式 + 标签感知解析 + 入口禁止逗号，往返逐字节相同 |
| 6 | `while (!feof(fp))` | C11 7.21.10.2：`feof` 指示器由**失败的**读取设置，是事后标志，循环必然多跑一轮 | 以 `fgets` 返回值为条件 |
| 7 | `load_file` 中途 `malloc` 失败直接返回 | `fp` 永不关闭，文件描述符泄漏 | 单点关闭，所有路径都走 `fclose`，`errno` 先存后恢复 |
| 8 | 四个函数声明 `int` 却没有 `return` | C11 6.9.1p12：执行到终结的 `}` 且调用方使用返回值是未定义行为；GCC `-Wall` 直接报 `-Wreturn-type` | 统一返回 `contacts_status` |
| 9 | 退出时只 `free(cts)`，不释放链表 | LeakSanitizer 实测每条泄漏 `sizeof(struct person)` = 64 字节 | `contacts_destroy` 遍历释放，`next` 在 `free` 之前取出 |
| 10 | 头插法导致顺序反转 | `Print` 与 `save` 都是倒序，往返一次顺序又反一遍 | 维护 `tail`，O(1) 追加，顺序 = 插入顺序 |
| 11 | `count` 在载入时只增不减，追加与替换语义不明 | 同一个文件读两遍，条数翻倍且分不清哪些是新的 | `contacts_load` 显式传 `APPEND` / `REPLACE` |
| 12 | 调用方不检查返回值 | `delete_entry` 返回 -2、`insert_entry` 返回 -3，`main` 一概不理，用户看不到任何反馈 | 状态码分类 + 驱动层退出码 + 针对性提示 |
| 13 | `save_file` 每写一行就 `fflush` | 白白增加系统调用；`fclose` 本身会刷新 | 只在关闭时刷新，并改成"临时文件 + rename" |

第 5 条是这一组里最值得留意的：**它不触发任何 sanitizer**。所有缓冲区都在范围内，
没有任何越界、没有任何泄漏，但数据被悄悄改错了。这类缺陷只能靠往返测试发现，
也是"能跑"与"正确"之间最容易被忽略的那道缝。

第 3 条和第 4 条有先后关系：写越界（`contacts.c:123`）在多数输入下先触发，
把读越界（`contacts.c:119`）挡住。修完一个必须重跑，才能看到后面那个。

### 7.3 结构上的改变

| 原版 | 本模块 |
|---|---|
| 一个 `contacts.c` 包含全部逻辑 | `include/`（接口）+ `src/`（实现）+ `src/main.c`（驱动） |
| 只能通过菜单交互 | 子命令（可脚本化）+ `menu` 子命令（保留原交互方式） |
| 内部函数全部非 `static`，符号污染全局命名空间 | 公开接口 + `libcontacts.a`，`main.c` 只链接不重编 |
| 错误只能用 `-1` / `-2` / `-3` 表达 | `contacts_status` 十一种分类，附 `contacts_status_string` |
| 无限定宽度的 `scanf("%s")` | `fgets` + `contacts_check_field` |
| 手写测试，无自动化 | `make test`（136 项）+ `make check`（21 项）+ `make sanitize` |
| 手动 `gcc` 一行命令 | `Makefile`，目标拆分，零警告构建 |
| 只能统计不能测量 | `make bench` 给出四个规模的实测耗时 |

改动里最关键的一步是**把"用户输入"从模块里挪出去**。原版的 `insert_entry` 里
既有 `scanf` 又有链表插入，两件事粘在一起，导致这个函数既没法在测试里调用，
也没法在别的程序里复用。拆开之后，模块只接受"已经到手的两段字符串"，
输入怎么来、要不要提示、用什么读，全部由驱动层决定。

---

## 8 文件格式规范

这一节把格式写成可以独立实现的形式——换一门语言重写读取器时，只看这一节就够。

**编码**：字节流，不做字符集转换。UTF-8 文本按原样存。

**记录**：一行一条。`\n` 或 `\r\n` 结尾都接受，读取时把行尾的 `\r` `\n` `\t` 空格一并去掉。

**字段**：一条记录由两个字段组成，用**逗号**分隔。每个字段写成 `标签冒号 值`，
字段值取冒号之后、到下一个分隔符（逗号或行尾）之间的内容，再去掉两端空白。

```
name: alice, phone: 13900001111
└─┬─┘ └─┬─┘  └─┬──┘ └────┬────┘
  │     │      │         └─ 第二字段的值
  │     │      └─ 第二字段的标签（内容不参与语义）
  │     └─ 第一字段的值
  └─ 第一字段的标签（内容不参与语义）
```

**标签**：读取时忽略内容，只认冒号。因此 `name:` / `telephone:` / `phone:` / `tel:`
都可以。写入时统一用 `name:` 与 `phone:`。

**约束**：

- 字段值不能为空
- 字段值不能含 `,` `\n` `\r`
- 第一字段长度 < 64 字节，第二字段 < 64 字节（均可通过宏调整，但读写双方必须一致）
- 单行长度不超过 510 字节（超过整行作废，不会被截断成两条）

**空行**：忽略。**无法解析的行**：跳过并计数，不中断载入。

**写出**：`name: <v1>, phone: <v2>\n`，字段值按原样写出，不做任何转义。

---

## 9 已知局限

### 9.1 查找是线性扫描

`contacts_find` 从表头逐个比较，最坏要看完整个链表。数据规模见第 6 节：
一千条时单次约 2 µs，六万四千条时约 143 µs。需要更快的查找就得加索引（6.3 有讨论）。

### 9.2 批量构建与载入是 O(n²)

`name` 作为主键的代价。通讯录规模下感觉不到，但与"导入十万行 CSV"这类需求不兼容。

### 9.3 没有事务语义

`CONTACTS_LOAD_REPLACE` 会先清空再载入。如果载入中途出错（不是坏行，而是读错误），
已经清掉的原内容回不来。需要"失败保持原样"的调用方应当先载入到另一个 `contact_book`，
成功之后再交换。

保存倒是安全的：临时文件 + `rename`，失败时目标文件保持原样。

### 9.4 没有并发控制

模块内部不加锁。不同的 `contact_book` 之间完全独立，可以并行使用；
同一个 `contact_book` 上的并发调用需要调用方自己加锁。

### 9.5 排序、分组、导入导出都未实现

当前只按插入顺序保存与输出。按姓名排序、按首字母分组、导出 CSV / vCard
都不在模块范围内，需要调用方自己做——遍历接口是游标式的，做这些事不需要模块配合。

### 9.6 平台相关的一处

`contacts_save` 用 `rename` 覆盖目标文件。POSIX 上是原子替换；
Windows 上同名 `rename` 会失败，需要 `ReplaceFile` 之类的接口。模块不处理这种差异。

---

## 10 待办

- [ ] 加哈希索引：`find` / 查重 / `load` 变成 O(1) 均摊，接口不变
- [ ] 支持按姓名排序输出（`list --sort`），需要先把链表转成指针数组
- [ ] 导出 CSV / vCard
- [ ] 多字段匹配（按电话反查、按姓名前缀匹配）
- [ ] 为 `contacts_save` 补一个"磁盘满 / 只读目录"的故障注入测试
- [ ] 结构化文本里出现 `\r` 之外的罕见控制字符时的行为，明确写进格式规范
- [ ] 把 `contacts_load` 的逐行临时缓冲区改成可配置，处理超长备注字段

---

## 测试数据

| 文件 | 大小 | 条数 | 写法 | 用途 |
|---|---|---|---|---|
| `testdata/a.txt` | 158 字节 | 4 | `name: X,telephone: Y`，逗号后无空格 | 紧凑写法，中间含空行 |
| `testdata/b.txt` | 160 字节 | 4 | 同上 | 同上，另一组数据 |
| `testdata/c.txt` | 58 字节 | 2 | `name: X, phone: Y`，逗号后有空格 | **关键样例**：旧实现会解析成 `phone:344567754` |

三份文件都是原始素材，内容一字未改。`c.txt` 是暴露"按空格切分"这个设计缺陷的
最小样例，留着它比留十份正确样例有用。

这些文件在根目录 `.gitattributes` 里标了 `-text`，不做换行符转换——
`c.txt` 是 58 字节这个事实本身是文档的一部分，换行符一转换数字就对不上了。
