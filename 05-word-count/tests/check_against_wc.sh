#!/bin/sh
# check_against_wc.sh —— 用与模块完全无关的路径独立计算一遍，逐项比对
#
# 三个计数的验证方式：
#   行数、字节数：直接与 GNU wc 比对，应当完全一致（定义没有歧义）
#   单词数      ：wc -w 的口径不同（它按空白切分），因此改用
#                 "把所有非字母数字字节换成换行，再数非空行"的等价变换
#
# 布局刻意采用 "数字在前、文件名在后"，与 wc 一致。
# 文件名长度可变，放在最后一列就不会破坏对齐。
#
# 用法：sh tests/check_against_wc.sh [可执行文件路径]
# 默认使用 bin/wordcount。

set -u

CLI=${1:-bin/wordcount}

# 固定区域设置，保证 tr 与 grep 的字符类按字节处理，不受调用环境影响
LC_ALL=C
export LC_ALL

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先执行 make）" >&2
	exit 1
fi

failed=0
checked=0

echo "与 wc / 独立方法交叉验证"
echo
# 注意：中文标签的显示宽度与字节宽度不等，表头的空格是手工对齐的，
# 不要按 ASCII 的习惯去改 printf 的宽度。
echo "          行          词         字节  文件"
echo "---------------------------------------------------------------"

for f in testdata/*.txt; do
	[ -f "$f" ] || continue

	# 本模块的输出顺序：行 词 字节 文件名
	set -- $("$CLI" "$f")
	g_lines=$1
	g_words=$2
	g_bytes=$3

	# 独立计算
	w_lines=$(wc -l < "$f" | tr -d ' ')
	w_bytes=$(wc -c < "$f" | tr -d ' ')
	r_words=$(tr -c '[:alnum:]' '\n' < "$f" | grep -c '[[:alnum:]]')

	checked=$((checked + 1))
	printf '%12s %12s %14s  %s\n' "$g_lines" "$g_words" "$g_bytes" "$f"

	if [ "$g_lines" != "$w_lines" ]; then
		printf '%*s  ^ 行数不一致：wc = %s\n' 40 "" "$w_lines"
		failed=$((failed + 1))
	fi
	if [ "$g_words" != "$r_words" ]; then
		printf '%*s  ^ 词数不一致：独立方法 = %s\n' 40 "" "$r_words"
		failed=$((failed + 1))
	fi
	if [ "$g_bytes" != "$w_bytes" ]; then
		printf '%*s  ^ 字节数不一致：wc = %s\n' 40 "" "$w_bytes"
		failed=$((failed + 1))
	fi
done

echo
if [ "$failed" -eq 0 ]; then
	echo "全部一致（$checked 个文件，$(($checked * 3)) 项比对）"
	exit 0
fi

echo "$failed 项不一致"
exit 1
