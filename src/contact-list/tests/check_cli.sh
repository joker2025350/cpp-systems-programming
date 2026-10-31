#!/bin/sh
# check_cli.sh —— 用命令行做端到端验证
#
# 单元测试验证的是模块函数；这个脚本验证的是"把模块拼成程序之后，
# 从头到尾走一遍还对不对"，包括：
#   - 工作文件（-f）的载入与自动写回
#   - list 的输出能不能直接当作输入文件（格式自洽）
#   - 保存 → 读回 的内容是否逐字节相同（存取互逆）
#   - 三份历史样例文件能否被正确读入
#   - list --sort 的顺序，以及它与 list 只差顺序、不改文件
#   - 各类错误路径的退出码
#
# 用法：sh tests/check_cli.sh [可执行文件] [testdata 目录]
#
# 布局沿用 05 的写法：数字在前、文件名在后，中文表头手工对齐。

set -u

CLI=${1:-bin/contacts}
TESTDATA=${2:-testdata}
WORK=bin/check_work

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先执行 make）" >&2
	exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK"

checked=0
failed=0

check_eq() {
	# $1 名称  $2 期望  $3 实际
	checked=$((checked + 1))
	if [ "$2" = "$3" ]; then
		printf 'ok    %s\n' "$1"
		return 0
	fi
	failed=$((failed + 1))
	printf 'FAIL  %s\n      期望: [%s]\n      实际: [%s]\n' "$1" "$2" "$3"
	return 1
}

check_rc() {
	# $1 名称  $2 期望退出码  $3 实际退出码
	checked=$((checked + 1))
	if [ "$2" -eq "$3" ]; then
		printf 'ok    %s\n' "$1"
		return 0
	fi
	failed=$((failed + 1))
	printf 'FAIL  %s：退出码 %s，期望 %s\n' "$1" "$3" "$2"
	return 1
}

echo "命令行端到端验证"
echo

# ---------------------------------------------------------------- 1. 工作文件
BOOK=$WORK/book.txt

out=$("$CLI" -f "$BOOK" count 2>/dev/null)
check_eq "工作文件不存在时视为空表" "0" "$out"

"$CLI" -f "$BOOK" add alice 13900001111 2>/dev/null
"$CLI" -f "$BOOK" add bob 13800002222 2>/dev/null
"$CLI" -f "$BOOK" add carol 010-1234567 2>/dev/null

out=$("$CLI" -f "$BOOK" count 2>/dev/null)
check_eq "三次 add 后数量为 3" "3" "$out"

expect='name: alice, phone: 13900001111
name: bob, phone: 13800002222
name: carol, phone: 010-1234567'

check_eq "工作文件内容与插入顺序" "$expect" "$(cat "$BOOK")"

# 换一个进程读同一个文件，数据必须还在
check_eq "另起一个进程仍能读到 3 条" "$expect" "$("$CLI" -f "$BOOK" list 2>/dev/null)"

# ---------------------------------------------------------------- 2. 单条操作
check_eq "find 命中" "name: bob, phone: 13800002222" \
         "$("$CLI" -f "$BOOK" find bob 2>/dev/null)"

"$CLI" -f "$BOOK" find nobody >/dev/null 2>&1
check_rc "find 落空退出码为 1" 1 $?

"$CLI" -f "$BOOK" del bob >/dev/null 2>&1
check_eq "删除后数量为 2" "2" "$("$CLI" -f "$BOOK" count 2>/dev/null)"

"$CLI" -f "$BOOK" del bob >/dev/null 2>&1
check_rc "重复删除退出码为 1" 1 $?

"$CLI" -f "$BOOK" add alice 999 >/dev/null 2>&1
check_rc "重名添加退出码为 1" 1 $?

"$CLI" -f "$BOOK" add a,b 111 >/dev/null 2>&1
check_rc "姓名含逗号被拒绝，退出码为 1" 1 $?

# ---------------------------------------------------------------- 3. 错误路径
"$CLI" >/dev/null 2>&1
check_rc "无参数退出码为 2" 2 $?

"$CLI" nosuchcommand >/dev/null 2>&1
check_rc "未知命令退出码为 2" 2 $?

"$CLI" -f "$BOOK" load "$WORK/definitely_missing.txt" >/dev/null 2>&1
check_rc "载入不存在的文件退出码为 1" 1 $?

"$CLI" -f "$BOOK" save "$WORK/no_such_dir/x.txt" >/dev/null 2>&1
check_rc "保存到不存在的目录退出码为 1" 1 $?

# 保存到不可写的目录。注意权限位的语义：目录的写权限控制的是"在目录里
# 建立或删除目录项"，不是"改写目录里已有文件的内容"。所以已经存在的
# book.txt 仍然可写；真正失败的是那一步 fopen("book.txt.tmp") —— 临时
# 文件必须新建，而新建目录项需要目录写权限。
RO=$WORK/ro_save
mkdir -p "$RO"
printf 'name: keeper, phone: 13000000000\n' > "$RO/book.txt"

if [ "$(id -u)" -ne 0 ]; then
	"$CLI" -f "$WORK/ro_work.txt" add alice 13900001111 >/dev/null 2>&1
	chmod 0500 "$RO"
	err=$("$CLI" -f "$WORK/ro_work.txt" save "$RO/book.txt" 2>&1 >/dev/null)
	rc=$?
	chmod 0700 "$RO"

	check_rc "保存到只读目录退出码为 1" 1 $rc

	case "$err" in
	*"cannot open file"*) got=OPEN ;;
	*)                    got=$err ;;
	esac
	check_eq "保存失败时指出是打开临时文件失败" "OPEN" "$got"
	check_eq "保存失败时目标文件未被改动" "name: keeper, phone: 13000000000" \
	         "$(cat "$RO/book.txt")"
	check_eq "保存失败时不留 .tmp" "" "$(ls "$RO"/*.tmp 2>/dev/null)"
else
	echo "skip  保存到只读目录（以 root 运行，权限位不生效）"
fi

# ---------------------------------------------------------------- 4. 存取互逆
"$CLI" -f "$BOOK" save "$WORK/rt1.txt" 2>/dev/null

"$CLI" -f "$WORK/rt2.txt" load "$WORK/rt1.txt" -r 2>/dev/null
check_eq "保存→读回 内容完全一致" \
         "$("$CLI" -f "$BOOK" list 2>/dev/null)" \
         "$("$CLI" -f "$WORK/rt2.txt" list 2>/dev/null)"

# 再往返一轮，确认不会累积出多余的 "phone:" 前缀
"$CLI" -f "$WORK/rt2.txt" save "$WORK/rt3.txt" 2>/dev/null
check_eq "第二次往返仍逐字节相同" \
         "$(cat "$WORK/rt1.txt")" "$(cat "$WORK/rt3.txt")"

case "$(cat "$WORK/rt3.txt")" in
*"phone: phone:"*)
	checked=$((checked + 1))
	failed=$((failed + 1))
	printf 'FAIL  %s\n' "往返不累积字段标签"
	;;
*)
	checked=$((checked + 1))
	printf 'ok    %s\n' "往返不累积字段标签"
	;;
esac

# ---------------------------------------------------------------- 5. 格式自洽
# list 的输出应当能直接当成输入文件
"$CLI" -f "$BOOK" list > "$WORK/from_list.txt" 2>/dev/null
"$CLI" -f "$WORK/from_list2.txt" load "$WORK/from_list.txt" -r 2>/dev/null
check_eq "list 输出可直接作为输入文件" \
         "$("$CLI" -f "$BOOK" list 2>/dev/null)" \
         "$("$CLI" -f "$WORK/from_list2.txt" list 2>/dev/null)"

# ---------------------------------------------------------------- 6. 历史样例
for f in a.txt b.txt c.txt; do
	if [ ! -f "$TESTDATA/$f" ]; then
		checked=$((checked + 1))
		failed=$((failed + 1))
		printf 'FAIL  找不到 %s\n' "$TESTDATA/$f"
		continue
	fi

	rm -f "$WORK/sample.txt"
	"$CLI" -f "$WORK/sample.txt" load "$TESTDATA/$f" -r >/dev/null 2>&1
	n=$("$CLI" -f "$WORK/sample.txt" count 2>/dev/null)

	case "$f" in
	a.txt) check_eq "载入 a.txt（4 条）" "4" "$n" ;;
	b.txt) check_eq "载入 b.txt（4 条）" "4" "$n" ;;
	c.txt) check_eq "载入 c.txt（2 条）" "2" "$n" ;;
	esac
done

# c.txt 的电话在旧实现里会被解析成 "phone:344567754"，这里盯住它
rm -f "$WORK/sample_c.txt"
"$CLI" -f "$WORK/sample_c.txt" load "$TESTDATA/c.txt" -r >/dev/null 2>&1
check_eq "c.txt 的电话不带字段标签" "name: king, phone: 344567754" \
         "$("$CLI" -f "$WORK/sample_c.txt" find king 2>/dev/null)"

# ---------------------------------------------------------------- 7. 索引统计
# stats 把内部索引状态暴露到命令行上，让"桶数会不会随删除收缩"这类行为
# 可以被直接看到，而不是只能从耗时反推。
stats_of() {
	# $1 工作文件  $2 字段名
	"$CLI" -f "$1" stats 2>/dev/null | sed -n "s/^$2:[[:space:]]*//p"
}

check_eq "空表的 buckets 为 0" "0" "$(stats_of "$WORK/empty_stats.txt" buckets)"
check_eq "空表的 load 为 n/a" "n/a" "$(stats_of "$WORK/empty_stats.txt" load)"

check_eq "stats 的 count 与 count 子命令一致" \
         "$("$CLI" -f "$BOOK" count 2>/dev/null)" "$(stats_of "$BOOK" count)"
check_eq "2 条时桶数为 16" "16" "$(stats_of "$BOOK" buckets)"
check_eq "2 条时 entries 等于 count" "2" "$(stats_of "$BOOK" entries)"

# 批量载入 20 条：插入超过 16 条时扩容一次，桶数 32
i=1
: > "$WORK/many.txt"
while [ "$i" -le 20 ]; do
	printf 'name: u%02d, phone: 138%08d\n' "$i" "$i" >> "$WORK/many.txt"
	i=$((i + 1))
done

rm -f "$WORK/bulk.txt"
"$CLI" -f "$WORK/bulk.txt" load "$WORK/many.txt" -r >/dev/null 2>&1
check_eq "20 条时桶数为 32" "32" "$(stats_of "$WORK/bulk.txt" buckets)"

i=1
while [ "$i" -le 17 ]; do
	"$CLI" -f "$WORK/bulk.txt" del "$(printf 'u%02d' "$i")" >/dev/null 2>&1
	i=$((i + 1))
done
check_eq "删到 3 条后 count 为 3" "3" "$(stats_of "$WORK/bulk.txt" count)"
check_eq "删到 3 条后桶数收缩到下限 16" "16" "$(stats_of "$WORK/bulk.txt" buckets)"
check_eq "收缩后 entries 仍等于 count" "3" "$(stats_of "$WORK/bulk.txt" entries)"

# ---------------------------------------------------------------- 8. 排序
# 插入顺序故意与字节序不同：carol 先插入，alice 在中间，Zoe 最后插入。
# 后者同时钉住"大写在小写之前"——按 ASCII 的 0x5A < 0x61，
# 若实现改成大小写不敏感，这一条会先失败。
SBOOK=$WORK/sort_book.txt
"$CLI" -f "$SBOOK" add carol 13800003333 2>/dev/null
"$CLI" -f "$SBOOK" add alice 13900001111 2>/dev/null
"$CLI" -f "$SBOOK" add bob   13800002222 2>/dev/null
"$CLI" -f "$SBOOK" add Zoe   13700000000 2>/dev/null

expect='name: Zoe, phone: 13700000000
name: alice, phone: 13900001111
name: bob, phone: 13800002222
name: carol, phone: 13800003333'

check_eq "list --sort 按姓名升序" "$expect" \
         "$("$CLI" -f "$SBOOK" list --sort 2>/dev/null)"

# 独立对照：行首固定是 "name: "，姓名两两不同，所以"按整行做字节序排序"
# 与"按姓名排序"等价。用 sort(1) 而不是再实现一遍排序，是为了让这条断言
# 不依赖被测代码自己的比较函数。
"$CLI" -f "$SBOOK" list > "$WORK/insertion.txt" 2>/dev/null
check_eq "list --sort 与 LC_ALL=C sort 的结果一致" \
         "$(LC_ALL=C sort "$WORK/insertion.txt")" \
         "$("$CLI" -f "$SBOOK" list --sort 2>/dev/null)"

# 排序只影响这一次输出，磁盘上的行序仍应是插入顺序
check_eq "list --sort 不改工作文件" "$(cat "$WORK/insertion.txt")" "$(cat "$SBOOK")"
check_eq "list --sort 之后数量不变" "4" "$("$CLI" -f "$SBOOK" count 2>/dev/null)"
check_eq "find 不受排序影响" "name: bob, phone: 13800002222" \
         "$("$CLI" -f "$SBOOK" find bob 2>/dev/null)"

# 空表：不为 0 个元素分配，输出为空且退出码为 0
check_eq "空表 list --sort 无输出" "" \
         "$("$CLI" -f "$WORK/sort_empty.txt" list --sort 2>/dev/null)"
"$CLI" -f "$WORK/sort_empty.txt" list --sort >/dev/null 2>&1
check_rc "空表 list --sort 退出码为 0" 0 $?

"$CLI" -f "$SBOOK" list --bogus >/dev/null 2>&1
check_rc "list 未知选项退出码为 2" 2 $?

# ---------------------------------------------------------------- 汇总
echo
if [ "$failed" -eq 0 ]; then
	echo "全部通过（$checked 项检查）"
	rm -rf "$WORK"
	exit 0
fi

echo "$failed / $checked 项失败，中间文件保留在 $WORK/"
exit 1
