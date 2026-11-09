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
# 前两组的行长（4 KiB / 64 KiB）整除段长（1~2 MiB），于是每个建议切点都恰好
# 落在行首，测到的"最长扫描"就是行尾那个换行的距离，也就是这一组的**最坏
# 距离**。这不是巧合而是构造：预算够不够用要看最坏距离，不是平均值。
# 第三组的行长故意取不整除的奇数，让建议切点落在行中，测的是"预算只够覆盖
# 一部分切点"这个中间形态。
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
# 造一个"每行 <行长> 字节、行内除换行外没有分隔符"的文件。
#
# 分两步：先造一个 1 MiB 的块（块里全是同样的行），再把块反复追加到目标文件。
# 不能直接用 `yes 模式行 | head -c 总字节`：模式行一旦超过 128 KiB，
# execve 就返回 E2BIG（Linux 的 MAX_ARG_STRLEN 是 32 页），行 1 MiB 那组
# 会造出一个 0 字节的文件——第一次跑就是这么失败的，而它不会让脚本报错，
# 只会让那一组的三行数据全是空的。
#
# 块里最后一行的换行可能被 head -c 截掉，于是块尾会粘上下一块的行首，
# 造出"两行连成一行"的假行长。为了不让这个偏差进到测量里，块大小取
# 行长（不是某个整数）的整数倍，见下面的 block_target。
make_rows() {
	rowbytes=$2
	total=$3
	block="$1.block"

	# 块目标长度：行长的整数倍，且不超过 1 MiB。行长超过 1 MiB 时就取一行。
	block_target=$((1048576 / rowbytes * rowbytes))
	if [ "$block_target" -le 0 ]; then
		block_target=$rowbytes
	fi

	rm -f "$block"
	if [ "$rowbytes" -le 65536 ]; then
		pattern=$(head -c $((rowbytes - 1)) /dev/zero | tr '\0' 'a')
		# yes 在 head 关掉管道时会收到 SIGPIPE 并打一行 "Broken pipe"，
		# 那是正常终止，不是错误；不屏蔽会让 make bench 的日志里冒出
		# 一堆与测量无关的行。
		yes "$pattern" 2>/dev/null | head -c "$block_target" > "$block"
	else
		head -c $((rowbytes - 1)) /dev/zero | tr '\0' 'a' > "$block"
		printf '\n' >> "$block"
	fi

	blocksize=$(size_of "$block")
	reps=$((total / blocksize))
	rem=$((total - reps * blocksize))

	: > "$1"
	i=0
	while [ "$i" -lt "$reps" ]; do
		cat "$block" >> "$1"
		i=$((i + 1))
	done
	if [ "$rem" -gt 0 ]; then
		head -c "$rem" "$block" >> "$1"
	fi
	rm -f "$block"
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
	printf '  %-6s %8s %5s %5s %11s %10s %5s %9s %6s\n' \
	       "-j" "耗时" "段" "放弃" "预算/切点" "对齐读" "占比" "最长扫描" "余量"

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
			pct=$((rd * 100 / size))%
		else
			# 一个切点都没被接受：最长扫描无样本，余量没有意义。
			# 这一行要看的是"放弃 = 建议"，即预算全部不够用。
			margin="—"
			pct=$((rd * 100 / size))%
		fi

		printf '  %-6s %5d ms %5s %5s %11s %10s %5s %9s %6s\n' \
		       "-j $j" $(( (end - start) / 1000000 )) \
		       "$segs" "$skp" "$bud" "$rd" "$pct" "$max" "$margin"
	done
}

echo "对齐读取预算：稀疏分隔符输入（各跑 3 轮，耗时是 3 轮合计）"
echo "余量 = 预算 / 最长扫描；余量大说明除数保守（多读），余量接近 1 说明快到边界"

run_case csv4k  "行 4 KiB 的长行 CSV / 每行一条 base64" 4096    67108864
run_case log64k "行 64 KiB 的单列大字段"                65536   33554432
run_case blob1m "行 1 MiB 的极端形态（行长取奇数，见上）" 1048571 16777216

echo
echo "输入留在 $WORK/ 下（make clean 会一并删除）"
