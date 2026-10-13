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

读取时由 `parser_token` 解析，规则是：

1. 第一个逗号之前的部分，取**第一个空格之后**的字符作为姓名
2. 第一个逗号之后的部分，取**第一个空格之后**的字符作为电话

所以字段名本身（`name`、`telephone`、`phone`）无关紧要，只要保证
"字段名 + 冒号 + 空格 + 值"这个形式，并且姓名与电话之间用逗号分隔即可。
仓库里三份数据文件都是合法的输入样例：

| 文件 | 说明 |
|---|---|
| `a.txt` | 4 条记录，字段名用 `telephone`，值之间有空行 |
| `b.txt` | 4 条记录，字段名用 `telephone` |
| `c.txt` | 2 条记录，字段名用 `phone` |

## 编译与运行

```bash
mkdir -p bin
gcc -Wall -Wextra contacts.c -o bin/contacts
./bin/contacts
```

演示 load 功能时，把数据文件放在工作目录下，然后在菜单里选 6 并输入文件名。

## 已知问题

这份实现里有若干缺陷，按严重程度排列。

### 1. `scanf("%s", ...)` 没有任何长度限制

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

### 2. `parser_token` 的第一个循环可能越界读

```c
for (i = 0; buffer[i] != ','; i ++) {
```

循环的终止条件只认逗号。如果这一行里**没有逗号**，`i` 会一路增长，
越过 `buffer` 的 `'\0'`（因为 `'\0'` 不等于 `','`）继续读取，
直到撞上内存里偶然出现的逗号字节或者触发段错误。

### 3. `parser_token` 写入 name / phone 时无边界检查

```c
name[j ++] = buffer[i];
```

`j` 只增不减，且没有与 `NAME_LENGTH` 比较。若输入文件里某一行的姓名字段
超过 15 字节，就会溢出调用方传入的缓冲区。`phone` 同理。

### 4. `load_file` 使用了 `while (!feof(fp))`

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

### 5. `load_file` 中途失败时文件句柄泄漏

```c
struct person *p = (struct person *)malloc(sizeof(struct person));
if (p == NULL) return -2;      /* 直接返回，fp 没有关闭 */
```

循环里分配失败会直接返回，`fopen` 拿到的 `fp` 永不关闭。

### 6. 四个函数声明返回 `int` 却没有 `return` 语句

`save_file`、`print_entry`、`save_entry`、`load_entry` 都是这种情形。
C 语言规定：在要求返回值的函数中执行到末尾而未 return，若调用方使用了
这个返回值，行为是未定义的。

### 7. 退出时链表节点未释放

`main` 只做了 `free(cts)`，而 `cts->people` 指向的整条链表——每个
`struct person` 都是 `malloc` 出来的——一个都没有释放。程序退出时
这部分内存仍然泄漏。

### 8. 调用方不检查返回值

`delete_entry` 在找不到姓名时返回 `-2`，`insert_entry` 在插入失败时返回
`-3`，但 `main` 的 switch 分支对这些返回值一概不理会，用户看不到任何反馈。

### 9. 其他

- `save_file` 在循环体内调用 `fflush(fp)`，每写一行就刷一次盘，
  白白增加 I/O 次数。`fclose` 本身会完成刷新
- `person_insert` 先检查 `ps` 再检查 `ppeople`，两个参数检查顺序
  与实际解引用顺序相反，容易误导读代码的人

## 待办

- [ ] 用带宽度限制的读入替换所有 `scanf("%s")`
- [ ] 为 `parser_token` 增加字段长度与行的合法性检查
- [ ] 修正 `load_file` 的循环条件
- [ ] 补齐所有函数的 `return` 语句
- [ ] 程序退出前释放整条链表
- [ ] 让 `main` 检查各操作函数的返回值并给出提示
