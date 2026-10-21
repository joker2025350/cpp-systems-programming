#!/bin/sh
# check_parallel.sh —— 验证 -j 并行模式的输出与顺序模式逐字节相同
#
# 并行最容易出的问题不是算错，而是"输出顺序跟着完成顺序变"和
# "错误信息落到 stderr 的次序变了"。这类问题肉眼很难发现：
# 数字都对，行序每次却可能不同。只能用逐字节比对。
#
# 比对三个量，缺一不可：
#   stdout  每行的数字与顺序
#   stderr  错误信息的内容与次序
#   退出码  有一个文件失败就必须是 1
#
# 用法：sh tests/check_parallel.sh [可执行文件路径]
# 默认使用 bin/wordcount。

set -u

CLI=${1:-bin/wordcount}

# 固定区域设置：文件名与字节计数都不该受调用环境影响
LC_ALL=C
export LC_ALL

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先执行 make）" >&2
	exit 1
fi

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

# 六个文件，长度依次递增。短任务先完成、长任务后完成，
# 完成顺序与 argv 顺序必然不同，正好用来盯住"输出顺序 = argv 顺序"。
i=0
while [ $i -lt 6 ]; do
	cat testdata/a.txt > "$work/f$i.txt"
	n=0
	while [ $n -lt $i ]; do
		cat testdata/b.txt >> "$work/f$i.txt"
		n=$((n + 1))
	done
	i=$((i + 1))
done

files="$work/f0.txt $work/f1.txt $work/f2.txt $work/f3.txt $work/f4.txt $work/f5.txt"

checked=0
failed=0

# 跑一次并留下三个产物：标准输出、标准错误、退出码
capture() {
	prefix=$1
	shift
	"$CLI" "$@" > "$work/$prefix.out" 2> "$work/$prefix.err"
	echo $? > "$work/$prefix.rc"
}

# 比对三件产物，label 用于输出，base 是基线前缀，cand 是待测前缀
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
		# 固定部分在前、标签在后：中文标签的字节宽度与显示宽度不等，
		# 用 %-28s 去对齐只会让每一行的空格数看起来都不一样。
		printf '  stdout / stderr / 退出码  一致    %s\n' "$label"
	else
		echo "  不一致：$label"
		failed=$((failed + 1))
	fi
	checked=$((checked + 3))
}

echo "并行与顺序的输出一致性"
echo "---------------------------------------------------------------"

capture seq $files
echo "  顺序基线：6 个文件（118 B ~ 3.1 MB），输出 $(wc -l < "$work/seq.out" | tr -d ' ') 行（含 total）"

for j in 1 2 3 4 8 16; do
	capture "p$j" -j "$j" $files
	check_triple "-j $j" seq "p$j"
done

# 单文件：工作线程数取 min(JOBS, 文件数)，应当退回顺序路径，
# 输出必须与不带 -j 时完全一样（含"没有 total 行"这一点）。
capture one "$work/f2.txt"
capture onej -j 8 "$work/f2.txt"
check_triple "-j 8 单文件" one onej

# 错误路径：不存在的文件排在中间。它必须落在 argv 顺序上的那个位置，
# 而且不能让同批其它文件的结果消失。
capture eseq "$work/f0.txt" "$work/missing.txt" "$work/f3.txt"
capture epar -j 4 "$work/f0.txt" "$work/missing.txt" "$work/f3.txt"
check_triple "-j 4 含不存在的文件" eseq epar

# 全都不存在：所有槽位都是失败，total 行不该出现，退出码必须非 0。
capture aseq "$work/nope1.txt" "$work/nope2.txt"
capture apar -j 2 "$work/nope1.txt" "$work/nope2.txt"
check_triple "-j 2 全部不存在" aseq apar

# 参数校验：-j 0 与 -j 非数字都必须以退出码 2 结束。
"$CLI" -j 0 $files > /dev/null 2> "$work/bad.err"
rc_zero=$?
"$CLI" -j abc $files > /dev/null 2> "$work/bad2.err"
rc_text=$?

if [ "$rc_zero" -eq 2 ] && [ "$rc_text" -eq 2 ] && [ -s "$work/bad.err" ] && [ -s "$work/bad2.err" ]; then
	printf '  退出码 2，有错误提示            %s\n' "-j 0 / -j abc 被拒绝"
else
	echo "  -j 0 / -j abc 未被正确拒绝（退出码 $rc_zero / $rc_text）"
	failed=$((failed + 1))
fi
checked=$((checked + 1))

echo
if [ "$failed" -eq 0 ]; then
	echo "全部一致（$checked 项比对）"
	exit 0
fi

echo "$failed 组不一致"
exit 1
