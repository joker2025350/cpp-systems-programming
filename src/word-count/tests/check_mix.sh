#!/bin/sh
# check_mix.sh —— 多文件与切分共用同一条队列时的一致性
#
# 第八轮之前，-j 有两条互斥的路径：多个文件时"一个文件一个任务"（不切），
# 单个文件时"切成若干段"（只有一个文件）。两条路径各自都有测试，但
# "同时给多个文件、其中几个大到该切"这种组合没有走过——而这正是
# 现在唯一的一条并行路径。这个脚本补的就是这个组合。
#
# 除了"输出与顺序一致"，还有一件事必须断言：切分真的发生了、并且任务
# 按权重排过序。理由与 5.9 / 5.10 记的两个假通过同源——如果每个文件都
# 退化成一个整文件任务，逐字节比对照样全绿，等于什么都没测。
#
#   · 切分记录（"切分 文件：N 段"）的条数必须是"不小于 2 MiB 的文件数"，
#     每个的段数 >= 2；
#   · 调度记录（"调度：共 N 个任务"）里的任务数必须等于
#     各文件段数之和，因此大于文件数；
#   · 最大的那个文件排在 argv 末尾，它的段长是 2 MiB，而其余任务的权重
#     都不超过 1.5 MiB。所以"首个任务 >= 2 MiB"这一条只有在按字节数降序
#     提交时才成立——按 argv 顺序提交时首个任务只有 118 字节。
#
# 用法：sh tests/check_mix.sh [可执行文件路径]
# 默认使用 bin/wordcount。

set -u

CLI=${1:-bin/wordcount}

LC_ALL=C
export LC_ALL

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先执行 make）" >&2
	exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

checked=0
failed=0

capture() {
	prefix=$1
	shift
	"$CLI" "$@" > "$work/$prefix.out" 2> "$work/$prefix.err"
	echo $? > "$work/$prefix.rc"
}

fail() {
	echo "  $1"
	shift
	for line in "$@"; do
		printf '    %s\n' "$line"
	done
	failed=$((failed + 1))
	checked=$((checked + 1))
}

pass() {
	printf '  %-52s %s\n' "$1" "$2"
	checked=$((checked + 1))
}

# check_triple <标签> <基线前缀> <待测前缀>：stdout / stderr / 退出码逐字节比对
check_triple() {
	label=$1
	base=$2
	cand=$3
	bad=0

	if ! cmp -s "$work/$base.out" "$work/$cand.out"; then
		echo "  $label：stdout 不一致"
		diff "$work/$base.out" "$work/$cand.out" | head -6
		bad=1
	fi
	if ! cmp -s "$work/$base.err" "$work/$cand.err"; then
		echo "  $label：stderr 不一致"
		diff "$work/$base.err" "$work/$cand.err" | head -6
		bad=1
	fi
	if [ "$(cat "$work/$base.rc")" != "$(cat "$work/$cand.rc")" ]; then
		echo "  $label：退出码不一致（基线 $(cat "$work/$base.rc")，"\
		     "待测 $(cat "$work/$cand.rc")）"
		bad=1
	fi

	if [ $bad -eq 0 ]; then
		printf '  %-46s 一致    %s\n' "$label · stdout/stderr/退出码" "逐字节相同"
		checked=$((checked + 1))
	else
		failed=$((failed + 1))
		checked=$((checked + 1))
	fi
}

# trace_of <参数...>：把驱动打到 stderr 的切分/调度记录取出来
trace_of() {
	WORDCOUNT_TRACE_SPLIT=1 "$CLI" "$@" 2>&1 >/dev/null | tr -d '\r'
}

# field_of <trace> <模式>：用 sed 的模式取一个数
field_of() {
	printf '%s\n' "$1" | sed -n "$2" | head -1
}

size_of() {
	wc -c < "$1" | tr -d ' '
}

# ---------------------------------------------------------------- 造输入
#
# 四个文件，大小跨越"不切 / 该切"的分界线（2 MiB = MIN_CHUNK_BYTES * 2）：
#   small 118 B、sub 约 617 KB  —— 都小于 2 MiB，整份一个任务
#   mid   3 MiB                —— 该切，-j 2 时 3 段
#   big   16 MiB               —— 该切，-j 2 时 8 段，每段 2 MiB
# 顺序是"小到大"，最大的放最后，用来验 LPT（见文件头）。

cp testdata/a.txt "$work/small.txt"
cp testdata/b.txt "$work/sub.txt"

# 用循环拼再截断，不用把 b.txt 写 30 遍：截断点落在哪里由 head -c 决定，
# 与拼了多少份无关，只要源文件够长（每份 617637 字节）。
n=0
while [ $n -lt 6 ]; do
	cat testdata/b.txt
	n=$((n + 1))
done > "$work/mid.src"
head -c 3145728 "$work/mid.src" > "$work/mid.txt"

n=0
while [ $n -lt 30 ]; do
	cat testdata/b.txt
	n=$((n + 1))
done > "$work/big.src"
head -c 16777216 "$work/big.src" > "$work/big.txt"

# 造出来的文件必须正好是要的字节数：head -c 取不到那么多时只会给出更短的
# 文件而不报错，而"16 MiB / 8 段 = 2 MiB"这条断言就靠这个数。
for pair in "mid.txt 3145728" "big.txt 16777216"; do
	set -- $pair
	got=$(size_of "$work/$1")
	if [ "$got" != "$2" ]; then
		echo "  $1 造出 $got 字节，期望 $2（源文件不够长）" >&2
		exit 1
	fi
done

files="$work/small.txt $work/mid.txt $work/sub.txt $work/big.txt"
missing="$work/missing.txt"

echo "多文件与切分共用一条队列"
echo "---------------------------------------------------------------"

# ---------------------------------------------------------------- 一致性

capture seq $files "$missing"
capture seqok $files
echo "  顺序基线：4 个文件（118 B ~ 16 MiB），输出"\
     "$(wc -l < "$work/seqok.out" | tr -d ' ') 行（含 total）"

for j in 2 3 4 8; do
	capture "j$j" -j "$j" $files
	check_triple "-j $j" seqok "j$j"
done

# 含不存在的文件：错误必须落在 argv 顺序上的那个位置，且不能让其它文件的结果消失
for j in 2 4; do
	capture "e$j" -j "$j" $files "$missing"
	check_triple "-j $j 含不存在的文件" seq "e$j"
done

# ---------------------------------------------------------------- 真的切了

# 每个 -j 都单独核对：切分记录的条数、任务数、工作线程数
for j in 2 3 4 8; do
	t=$(trace_of -j "$j" $files)

	n_cut=$(printf '%s\n' "$t" | grep -c '切分 ' || true)
	tasks=$(field_of "$t" 's/.*共 \([0-9][0-9]*\) 个任务.*/\1/p')
	whole=$(field_of "$t" 's/.*整文件任务 \([0-9][0-9]*\) 个.*/\1/p')
	workers=$(field_of "$t" 's/.*工作线程 \([0-9][0-9]*\) 个.*/\1/p')

	segs=$(printf '%s\n' "$t" | sed -n 's/.*切分 .*：\([0-9][0-9]*\) 段.*/\1/p')
	sum_seg=0
	small_seg=0
	for s in $segs; do
		sum_seg=$((sum_seg + s))
		if [ "$s" -lt 2 ]; then
			small_seg=$((small_seg + 1))
		fi
	done

	if [ "$n_cut" -eq 2 ] && [ "$small_seg" -eq 0 ]; then
		pass "-j $j · 只有两个大文件被切分，段数 >= 2" "切分记录 $n_cut 条"
	else
		fail "-j $j · 切分记录不对（$n_cut 条，段数不足的 $small_seg 个）" "$t"
	fi

	# 任务数 = 各文件段数之和 + 整文件任务数（不存在的文件没有任务）
	want=$((sum_seg + whole))
	if [ -n "$tasks" ] && [ "$tasks" = "$want" ] && [ "$want" -gt 4 ]; then
		pass "-j $j · 任务数 = 段数之和 + 整文件任务数"\
		     "$tasks = $sum_seg + $whole，多于 4 个文件"
	else
		fail "-j $j · 任务数 $tasks 与 $sum_seg + $whole 不符" "$t"
	fi

	if [ "$workers" = "$j" ]; then
		pass "-j $j · 工作线程数" "$workers（任务数多于线程数）"
	else
		fail "-j $j · 工作线程数 $workers，期望 $j" "$t"
	fi
done

# LPT：argv 末尾的 16 MiB 文件在 -j 2 下段长 2 MiB，其余任务的权重都
# 不超过 1.5 MiB。首个任务 >= 2 MiB 只可能来自"按字节数降序提交"。
t=$(trace_of -j 2 $files)
first=$(field_of "$t" 's/.*首个任务 \([0-9][0-9]*\) 字节.*/\1/p')
if [ -n "$first" ] && [ "$first" -ge 2097152 ]; then
	pass "LPT · 首个任务是最大的那段（16 MiB / 8 段）" "$first 字节"
else
	fail "LPT · 首个任务 ${first:-（无记录）} 字节，未按字节数降序时首个任务是最小的那个（118 字节）" "$t"
fi

# 全部小于 2 MiB：一条切分记录都不该有，调度记录仍然存在且任务数 = 文件数
t=$(trace_of -j 4 "$work/small.txt" "$work/sub.txt")
n_cut=$(printf '%s\n' "$t" | grep -c '切分 ' || true)
tasks=$(field_of "$t" 's/.*共 \([0-9][0-9]*\) 个任务.*/\1/p')
whole=$(field_of "$t" 's/.*整文件任务 \([0-9][0-9]*\) 个.*/\1/p')
if [ "$n_cut" -eq 0 ] && [ "$tasks" = "2" ] && [ "$whole" = "2" ]; then
	pass "小于 2 MiB 的文件不切分" "2 个整文件任务，无切分记录"
else
	fail "小于 2 MiB 的文件被切了（切分记录 $n_cut 条，任务 $tasks，整文件 $whole）" "$t"
fi

# 单文件 + -j：只有切分任务，没有整文件任务
t=$(trace_of -j 4 "$work/big.txt")
tasks=$(field_of "$t" 's/.*共 \([0-9][0-9]*\) 个任务.*/\1/p')
whole=$(field_of "$t" 's/.*整文件任务 \([0-9][0-9]*\) 个.*/\1/p')
segs=$(printf '%s\n' "$t" | sed -n 's/.*切分 .*：\([0-9][0-9]*\) 段.*/\1/p')
if [ -n "$segs" ] && [ "$tasks" = "$segs" ] && [ "$whole" = "0" ]; then
	pass "单文件：任务数等于段数" "$tasks 个任务，$segs 段"
else
	fail "单文件的任务数 $tasks 与段数 $segs 不符（整文件任务 $whole）" "$t"
fi

# ---------------------------------------------------------------- 数字本身

# total 行的字节数必须等于四个文件的字节数之和：这条不依赖另一条代码路径
want_bytes=0
for f in $files; do
	want_bytes=$((want_bytes + $(size_of "$f")))
done
got_bytes=$("$CLI" -j 4 $files | tail -1 | awk '{print $3}')
if [ "$got_bytes" = "$want_bytes" ]; then
	pass "total 行的字节数" "$got_bytes 字节"
else
	fail "total 行的字节数是 $got_bytes，四个文件合计 $want_bytes"
fi

# 行数：每个文件一行 + total 一行
want_lines=$((4 + 1))
got_lines=$("$CLI" -j 4 $files | wc -l | tr -d ' ')
if [ "$got_lines" = "$want_lines" ]; then
	pass "stdout 行数（每文件一行 + total）" "$got_lines 行"
else
	fail "stdout 行数是 $got_lines，期望 $want_lines"
fi

# 逐文件核对总数：把 total 行拆开，与"每个文件单独跑一次再相加"比。
# 这里不用 wc 当对照物——wc 的"词"是空白分隔，本模块的词是非字母数字段，
# 同一份文本上两者本来就不同（README 第 8 节），拿它比词数只会得到假失败。
sum_l=0
sum_w=0
sum_b=0
for f in $files; do
	set -- $("$CLI" "$f")
	sum_l=$((sum_l + $1))
	sum_w=$((sum_w + $2))
	sum_b=$((sum_b + $3))
done
total_line=$("$CLI" -j 4 $files | tail -1)
if [ "$total_line" = "$(printf '%8d %8d %8d total' "$sum_l" "$sum_w" "$sum_b")" ]; then
	pass "total 行 = 四个文件各自结果之和"\
	     "行 $sum_l / 词 $sum_w / 字节 $sum_b"
else
	fail "total 行与逐文件相加不符" "total：$total_line"\
	     "相加：$sum_l / $sum_w / $sum_b"
fi

echo
if [ "$failed" -eq 0 ]; then
	echo "全部通过（$checked 项比对）"
	exit 0
fi

echo "$failed 项不一致"
exit 1
