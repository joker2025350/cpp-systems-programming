#!/bin/sh
# bench_align_sparse.sh —— 稀疏分隔符输入上的对齐读取预算实测
#
# 用法：sh tests/bench_align_sparse.sh [可执行文件路径] [工作目录]
# 默认 bin/wordcount 与 bin/bench_sparse。
#
# ---------------------------------------------------------------- 要回答什么
#
# 3.8 节把每个切点的对齐读取预算定成 min(段长 / 4, 1 MiB)，其中"1/4"这个除数
# 一直是取舍而不是推导。此前的实测（6.5）只有"全字母文件"一组，而那个文件的
# -j 永远退化（所有切点都被放弃、段数为 1），它只能说明预算挡住了多少浪费，
# 说明不了预算够不够用。
#
# 判断"够不够用"要看一个量：**被接受的切点离下一个分隔符最远有多少字节**。
# 预算只要大于它，这个切点就不会因为预算不够而被放弃；预算比它大得越多，
# 除数就选得越保守（越保守 = 越多纯损耗）。这个量由驱动在
# WORDCOUNT_TRACE_SPLIT 里以"接受切点最长扫描"报出来（第七轮新增）。
#
# ---------------------------------------------------------------- 造哪三类输入
#
# 三类都满足"分隔符稀疏但确实存在"，区别只在行长度：
#
#   行 4 KiB    长行 CSV、每行一条 base64 的访问日志（现实里最常见的形态）
#   行 64 KiB   单列大字段的导出文件、行内没有空白的文本
#   行 1 MiB    极端形态，用来找出预算开始不够的量级
#
# 行内故意不放分隔符：这样"离下一个分隔符的距离"由行长决定，而不是由词长
# 决定。普通英文文本里那个距离只有 5~6 字节（5.10 已断言最长扫描 <= 1024），
# 拿它当样本等于什么都没测。
#
# 这是测量脚本，不做断言：数据要写进 README 6.6，判据是"预算 / 最长扫描"
# 这一列。为了让 make bench 在数据异常时也能给出完整表格，解析失败只打一行
# 提示，不中止整个 bench。

set -u

CLI=${1:-bin/wordcount}
WORK=${2:-bin/bench_sparse}

LC_ALL=C
export LC_ALL

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先执行 make）" >&2
	exit 1
fi

mkdir -p "$WORK" || exit 1

# make_rows <路径> <行长> <总字节>
#
# yes 反复输出同一个模式行，head -c 截到目标字节数。模式行是 (行长 - 1) 个
# 'a' 加一个换行，因此除换行之外没有任何分隔符。最后一行会被截断，
# 不影响结论——文件末尾不产生切点。
make_rows() {
	pattern=$(head -c $(($2 - 1)) /dev/zero | tr '\0' 'a')
	yes "$pattern" | head -c "$3" > "$1"
}

trace_of() {
	WORDCOUNT_TRACE_SPLIT=1 "$CLI" -j "$2" "$1" 2>&1 >/dev/null | tr -d '\r'
}

# field_of <trace> <标签> <单位>：取 "标签 N 单位" 里的 N。
# 用 | 当 sed 的分隔符，因为单位里带 /（"字节/切点"）。
field_of() {
	printf '%s\n' "$1" | sed -n "s|.*$2 \([0-9][0-9]*\) $3.*|\1|p"
}

size_of() {
	wc -c < "$1" | tr -d ' '
}

# run_case <名字> <说明> <行长> <总字节>
run_case() {
	name=$1
	desc=$2
	row=$3
	total=$4
	f="$WORK/$name.bin"

	make_rows "$f" "$row" "$total"
	size=$(size_of "$f")

	echo
	echo "$name：$desc"
	echo "  $size 字节，每行 $row 字节，行内无分隔符"
	printf '  %-6s %8s %5s %5s %11s %10s %9s %6s\n' \
	       "-j" "耗时" "段" "放弃" "预算/切点" "对齐读" "最长扫描" "余量"

	for j in 2 4 8 16; do
		t=$(trace_of "$f" "$j")
		segs=$(printf '%s\n' "$t" | sed -n 's/.*：\([0-9][0-9]*\) 段.*/\1/p')
		skp=$(field_of "$t" '放弃' '个')
		bud=$(field_of "$t" '预算' '字节/切点')
		rd=$(field_of "$t" '对齐扫描读' '字节')
		max=$(field_of "$t" '接受切点最长扫描' '字节')

		if [ -z "$segs" ] || [ -z "$skp" ] || [ -z "$bud" ] \
		   || [ -z "$rd" ] || [ -z "$max" ]; then
			echo "  -j $j 解析切分记录失败：$t" >&2
			continue
		fi

		start=$(date +%s%N)
		for r in 1 2 3; do
			"$CLI" -j "$j" "$f" > /dev/null
		done
		end=$(date +%s%N)

		if [ "$max" -gt 0 ]; then
			margin=$((bud / max))
			margin="${margin}x"
		else
			margin="—"
		fi

		printf '  %-6s %5d ms %5s %5s %11s %10s %9s %6s\n' \
		       "-j $j" $(( (end - start) / 1000000 )) \
		       "$segs" "$skp" "$bud" "$rd" "$max" "$margin"
	done
}

echo "对齐读取预算：稀疏分隔符输入（各跑 3 轮，耗时是 3 轮合计）"
echo "余量 = 预算 / 最长扫描；余量大说明除数保守（多读），余量接近 1 说明快到边界"

run_case csv4k  "行 4 KiB 的长行 CSV / 每行一条 base64" 4096    67108864
run_case log64k "行 64 KiB 的单列大字段"                65536   33554432
run_case blob1m "行 1 MiB 的极端形态"                   1048576 16777216

echo
echo "输入留在 $WORK/ 下（make clean 会一并删除）"
