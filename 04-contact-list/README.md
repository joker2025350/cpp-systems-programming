# 04 通讯录

一个命令行通讯录程序，用双向链表存储联系人，支持增删查改和文件读写。

## 功能

启动后进入菜单循环，输入数字选择操作：

```
********************************************************
***** 1. Add Person          2. Print People ********
***** 3. Del Person          4. Search Person *******
***** 5. Save People         6. Load People *********
***** Other Key for Exiting Program ********************
********************************************************
```

| 操作 | 说明 |
|---|---|
| 1 Add | 输入姓名和电话，插入链表 |
| 2 Print | 遍历链表，打印全部联系人 |
| 3 Del | 按姓名查找并删除 |
| 4 Search | 按姓名查找并打印 |
| 5 Save | 把当前链表写入指定文件 |
| 6 Load | 从指定文件读入联系人，追加到链表 |

输入其他任意值退出程序。

## 数据结构

```c
struct person {
    char name[NAME_LENGTH];      /* 16 字节 */
    char phone[PHONE_LENGTH];    /* 32 字节 */
    struct person *next;
    struct person *prev;
};

struct contacts {
    struct person *people;       /* 链表头 */
    int count;                   /* 联系人数量 */
};
```

链表操作由两个宏完成，`LIST_INSERT` 采用头插法，`LIST_REMOVE` 覆盖被删
元素位于表头、表中、表尾以及链表只剩一个元素的情形。用宏而不是函数实现，
是因为宏不区分类型，同一套逻辑以后可以复用到其他节点类型上。

**注意头插法的副作用**：`Print` 输出的顺序与插入顺序相反。先插入的会被
后插入的挤到链表深处。

## 文件读写

保存格式（`save_file` 写出）：

```
name: liaoqf, phone: 23456543789
name: zhaomeiping, phone: 14583943453
```

读取时由 `parser_token` 解析，实际规则是：

1. 第一个逗号之前的部分：找到**第一个空格**，取它之后的所有字符作为姓名
2. 第一个逗号之后的部分：同样找到**第一个空格**，取它之后的所有字符作为电话

关键在"第一个空格"落在哪里。它取决于逗号后面**有没有空格**，两种格式的
解析结果完全不同（实测）：

| 文件 | 行格式 | 实测解析结果 |
|---|---|---|
| `a.txt` | `name: liaoqf,telephone: 23456543789` | `liaoqf --> 23456543789` 正确 |
| `b.txt` | `name: qiuxiang,telephone: 98765678123` | `qiuxiang --> 98765678123` 正确 |
| `c.txt` | `name: king, phone: 344567754` | `king --> phone:344567754` **错误** |

`a.txt` / `b.txt` 里逗号紧跟字段名，第一个空格出现在字段名冒号之后，取到的是
纯数字；`c.txt` 里逗号后面先有一个空格，这个空格被当成字段名与值的分隔符，
于是字段名 `phone:` 连同冒号一起被留在了值里。

三份文件都不是"随便怎么写都行"的样例，只有 `a.txt` / `b.txt` 那种紧凑写法
才是这个解析器能正确处理的格式。

## 编译与运行

```bash
mkdir -p bin
gcc -Wall -Wextra contacts.c -o bin/contacts
./bin/contacts
```

演示 load 功能时，把数据文件放在工作目录下，然后在菜单里选 6 并输入文件名。

用消毒剂验证（ASan 检测越界与泄漏）：

```bash
gcc -Wall -Wextra -g -fsanitize=address contacts.c -o bin/contacts_asan
./bin/contacts_asan
```

注意：LeakSanitizer 检测到泄漏后会直接终止进程、不刷新 stdio 缓冲区，
管道里的输出会全部丢失。需要看正常输出时加 `ASAN_OPTIONS=detect_leaks=0`。

## 已知问题

这份实现里有若干缺陷，按严重程度排列。

### 1. `save_file` 写出的格式，`load_file` 读不回来（存取不互逆）

`save_file` 写出的每一行是：

```c
fprintf(fp, "name: %s, phone: %s\n", item->name, item->phone);
```

逗号后面**带一个空格**——正是上面表格里解析会出错的那一种。实测一次完整的
存取往返：

```
保存进 rt.txt 的内容：
    name: alice, phone: 13900001111

读回时解析结果：
    file token : alice --> phone:13900001111

遍历输出：
    name: alice,phone: phone:13900001111
```

每做一次"保存再读取"，电话号码前面就多一个 `phone:` 前缀。反复存取会不断累积。
这个缺陷不触发任何 sanitizer，因为从内存安全角度看没有任何越界。

修法：把 `save_file` 的格式串改成 `"name: %s,phone: %s\n"`（去掉逗号后的空格），
并在 `parser_token` 里改成按固定分隔符切分，不要依赖"第一个空格"这种偶然位置。

### 2. `load_file` 是追加，不是替换

`load_file` 把读到的节点逐个**头插**进链表，调用方已有的节点一个都不动，
`cts->count` 也跟着累加。所以对同一个文件调用两次加载，记录数会翻倍。

### 3. `scanf("%s", ...)` 没有任何长度限制

`insert_entry` 里直接读入用户输入：

```c
char name[NAME_LENGTH] = {0};   /* 16 字节 */
scanf("%s", name);
```

`%s` 转换说明符**不检查目标缓冲区的容量**，会一直写入直到遇到空白字符。
输入超过 15 个字符就会越过 `name` 数组的边界，覆盖相邻的栈变量。
`p->name`、`p->phone` 以及 `delete_entry`、`search_entry` 中的局部数组
都有同样的问题。

正确做法是限定宽度：`scanf("%15s", name)`，或用 `fgets` 读入后自行截断。

### 4. `parser_token` 的第一个循环可能越界读

```c
for (i = 0; buffer[i] != ','; i ++) {
```

循环的终止条件只认逗号。如果这一行里**没有逗号**，`i` 会一路增长，
越过 `buffer` 的 `'\0'`（因为 `'\0'` 不等于 `','`）继续读取，
直到撞上内存里偶然出现的逗号字节或者触发段错误。

ASan 实测（输入一行既无逗号也无空格的数据，绕开写入路径单独暴露读的问题）：

```
==ERROR: AddressSanitizer: stack-buffer-overflow
READ of size 1
    #0 ... in parser_token contacts.c:119
    #1 ... in load_file contacts.c:160
    [128, 256) 'buffer' (line 151) <== Memory access at offset 256 overflows this variable
```

`buffer` 是 `load_file` 里的 `char buffer[128]`，访问发生在偏移 256 处，
也就是越过了整个数组再往前走 128 字节。把 `length` 纳入条件即可：

```c
for (i = 0; i < length && buffer[i] != ','; i ++)
```

### 5. `parser_token` 写入 name / phone 时无边界检查

```c
name[j ++] = buffer[i];
```

`j` 只增不减，且没有与 `NAME_LENGTH` 比较。若输入文件里某一行的姓名字段
超过 15 字节，就会溢出调用方传入的缓冲区。`phone` 同理。

ASan 实测报在 `contacts.c:123`：

```
==ERROR: AddressSanitizer: stack-buffer-overflow
    #0 ... in parser_token contacts.c:123
    #1 ... in load_file contacts.c:160
    [32, 48) 'name' (line 157) <== Memory access at offset 48 overflows this variable
```

这两条缺陷有先后关系：写越界（第 123 行）在多数输入下先触发，把读越界
（第 119 行）挡住。修完一个必须重跑，才能看到后面那个。

### 6. `load_file` 使用了 `while (!feof(fp))`

```c
while (!feof(fp)) {
    char buffer[BUFFER_LENGTH] = {0};
    fgets(buffer, BUFFER_LENGTH, fp);
    ...
}
```

`feof()` 只有在一次读取**失败之后**才会返回真，它是"已经读过文件末尾"的
事后标志，不能用来预测下一次读取是否有效。这个循环因此会多执行一轮：
最后一轮 `fgets` 返回 `NULL`、`buffer` 保持初始的全零状态，然后被送进
`parser_token` 处理。

正确写法是把读取本身作为条件：

```c
while (fgets(buffer, BUFFER_LENGTH, fp) != NULL) {
```

**这个问题在本程序里被另一处检查掩盖了**：空轮时 `buffer` 全零，`strlen`
得到 0，`parser_token` 开头的 `if (length < MIN_TOKEN_LENGTH) return -2;`
（`MIN_TOKEN_LENGTH` 为 5）把它挡掉，调用方 `continue`，所以看不出异常。
数据文件里的空行同样是靠这句被跳过的。一旦调整或删除这句长度检查，
多跑一轮的后果立刻现形。

### 7. `load_file` 中途失败时文件句柄泄漏

```c
struct person *p = (struct person *)malloc(sizeof(struct person));
if (p == NULL) return -2;      /* 直接返回，fp 没有关闭 */
```

循环里分配失败会直接返回，`fopen` 拿到的 `fp` 永不关闭。

### 8. 四个函数声明返回 `int` 却没有 `return` 语句

`save_file`、`print_entry`、`save_entry`、`load_entry` 都是这种情形。
C11 6.9.1p12 规定：若执行到终结函数的 `}`，且调用方使用了该次调用的返回值，
行为是未定义的。当前 `main` 把这些返回值都丢掉了，严格说还没构成未定义行为，
但只要有人把调用改成 `if (save_entry(cts) != 0)`，问题立刻成立。

GCC 在 `-Wall` 下会直接指出（`-Wreturn-type` 属于 `-Wall`）：

```
contacts.c: In function 'save_file':
contacts.c:111:1: warning: control reaches end of non-void function [-Wreturn-type]
contacts.c: In function 'print_entry':
contacts.c:218:1: warning: control reaches end of non-void function [-Wreturn-type]
contacts.c: In function 'save_entry':
contacts.c:274:1: warning: control reaches end of non-void function [-Wreturn-type]
contacts.c: In function 'load_entry':
contacts.c:284:1: warning: control reaches end of non-void function [-Wreturn-type]
```

### 9. 退出时链表节点未释放

`main` 只做了 `free(cts)`，而 `cts->people` 指向的整条链表——每个
`struct person` 都是 `malloc` 出来的——一个都没有释放。程序退出时
这部分内存仍然泄漏。

LeakSanitizer 实测（插入一条记录后退出）：

```
==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 64 byte(s) in 1 object(s) allocated from:
    #0 ... in __interceptor_malloc
    #1 ... in insert_entry contacts.c:186
    #2 ... in main contacts.c:317

SUMMARY: AddressSanitizer: 64 byte(s) leaked in 1 allocation(s).
```

泄漏的 64 字节正是 `sizeof(struct person)`。插入 N 条就泄漏 64 × N 字节。

修法是加一个 `contacts_destroy`，遍历链表逐个 `free`：

```c
void contacts_destroy(struct contacts *cts) {
    struct person *item = cts->people;
    while (item != NULL) {
        struct person *next = item->next;   /* 必须先存后继，再释放当前节点 */
        free(item);
        item = next;
    }
    cts->people = NULL;
    cts->count = 0;
}
```

`next` 必须在 `free` 之前取出。写成 `free(item); item = item->next;` 是典型的
释放后使用（use-after-free），ASan 会以 `heap-use-after-free` 报出。

### 10. 调用方不检查返回值

`delete_entry` 在找不到姓名时返回 `-2`，`insert_entry` 在插入失败时返回
`-3`，但 `main` 的 switch 分支对这些返回值一概不理会，用户看不到任何反馈。

### 11. 其他

- `save_file` 在循环体内调用 `fflush(fp)`，每写一行就刷一次盘，
  白白增加 I/O 次数。`fclose` 本身会完成刷新
- `person_insert` 先检查 `ps` 再检查 `ppeople`，两个参数检查顺序
  与实际解引用顺序相反，容易误导读代码的人

## 待办

- [ ] 统一存取格式：`save_file` 去掉逗号后的空格，或重写 `parser_token` 按固定分隔符切分
- [ ] 为存取写"往返测试"：保存 → 清空 → 读回 → 逐字段比较
- [ ] 用带宽度限制的读入替换所有 `scanf("%s")`
- [ ] 为 `parser_token` 增加字段长度与行的合法性检查
- [ ] 修正 `load_file` 的循环条件
- [ ] 明确 `load_file` 是追加还是替换的语义，并让 `count` 一致
- [ ] 补齐所有函数的 `return` 语句
- [ ] 程序退出前释放整条链表
- [ ] 让 `main` 检查各操作函数的返回值并给出提示

## 消毒剂验证的局限

用 ASan 验证这份代码时要知道它的检测边界：**红区加在变量之间，不加在结构体
字段之间**。实测同一处越界写入，放在不同位置结果相反：

| 写入长度 | 写在 `struct person` 内部（对象共 64 字节） | 写在独立的 `char name[16]` 之外 |
|---|---|---|
| 17 | 未报告，`phone` 被污染 | 报告 stack-buffer-overflow |
| 32 | 未报告 | 报告 |
| 64 | 未报告（刚好写满结构体） | 报告 |
| 80 | 报告（越过整个对象） | 报告 |

也就是说 `scanf("%s", p->name)` 在 `p` 是 64 字节对象时，写入 17～64 字节
之间**不会**被 ASan 报告，但已经把 `phone`、`next`、`prev` 覆写成任意值，
后续遍历会解引用野指针。这一类越界只能靠代码审查和边界检查发现。

Clang 有 `-fsanitize-address-field-padding` 可以为字段间插入红区，但 GCC
不支持该选项（本机 gcc 11.4.0 直接报 `unrecognized command-line option`）。
