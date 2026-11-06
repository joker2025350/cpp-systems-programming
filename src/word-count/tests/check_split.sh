#!/bin/sh
# check_split.sh —— 验证"单文件切分"与"顺序统计"的输出逐字节相同
#
# 单文件切分最容易出的问题不是算错整行数字，而是只在特定切点位置算错：
# 切点落在词中间时，那个词会被数两次，而行数与字节数完全正常。
# 因此这里不挑固定输入，而是把几类极端文件在多种 -j 下都跑一遍，
# 与顺序输出逐字节比对（stdout / stderr / 退出码）。
#
# 还有一条容易漏的：切分路径可能根本没被走到——文件太小、不是普通文件时
# 都会退回顺序路径，那时"输出一致"是必然的，测了等于没测。
# 所以用 WORDCOUNT_TRACE_SPLIT 让驱动把"切成了几段"打到 stderr，
# 断言段数确实 >= 2，再关掉它做逐字节比对。
#
# 用法：sh tests/check_split.sh [可执行文件路径]
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
		printf '  stdout / stderr / 退出码  一致    %s\n' "$label"
	else
		failed=$((failed + 1))
	fi
	checked=$((checked + 3))
}

# ---------------------------------------------------------------- 造输入

# 1) 混合文本：a.txt 与 6 份 b.txt 相加，约 3.7 MB
cat testdata/a.txt > "$work/mixed.txt"
i=0
while [ $i -lt 6 ]; do
	cat testdata/b.txt >> "$work/mixed.txt"
	i=$((i + 1))
done

# 2) 没有结尾换行的截断文件：长度正好落在词中间的概率极高
head -c 2500000 "$work/mixed.txt" > "$work/noend.txt"

# 3) 3 MiB 全是一个字符：所有切点都必须被推到文件末尾，只剩一段
head -c 3145728 /dev/zero | tr '\0' 'a' > "$work/oneword.txt"

# 4) 3 MiB 全是空格：一个词都没有，全部切点都落在分隔符上
head -c 3145728 /dev/zero | tr '\0' ' ' > "$work/spaces.txt"

# 5) 小于 2 MiB：不该切分，必须退回顺序路径
head -c 1048576 "$work/mixed.txt" > "$work/small.txt"

echo "单文件切分与顺序统计的一致性"
echo "---------------------------------------------------------------"

# ---------------------------------------------------------------- 一致性

for name in mixed noend oneword spaces small; do
	f="$work/$name.txt"
	capture "$name-seq" "$f"

	for j in 2 3 4 8 16; do
		capture "$name-j$j" -j "$j" "$f"
		check_triple "$name · -j $j" "$name-seq" "$name-j$j"
	done
done

# 字节数必须等于文件实际大小：这条不依赖另一条代码路径
for name in mixed noend oneword spaces; do
	f="$work/$name.txt"
	size=$(wc -c < "$f" | tr -d ' ')
	got=$("$CLI" -j 4 "$f" | awk '{print $3}')
	if [ "$size" = "$got" ]; then
		printf '  %-34s 一致    %s 字节\n' "$name · 字节数等于文件大小" "$size"
	else
		echo "  $name · 字节数 $got 与文件大小 $size 不符"
		failed=$((failed + 1))
	fi
	checked=$((checked + 1))
done

# ---------------------------------------------------------------- 真的切了

# 段数必须 >= 2，否则上面那些比对只证明了"退化路径输出一致"。
for name in mixed noend oneword spaces; do
	f="$work/$name.txt"
	# 全字母文件没有可用切点，只剩一段——这是设计上的退化，按 1 段接受
	trace=$(WORDCOUNT_TRACE_SPLIT=1 "$CLI" -j 4 "$f" 2>&1 >/dev/null | tr -d '\r')
	chunks=$(printf '%s' "$trace" | sed -n 's/.*切分 .*：\([0-9][0-9]*\) 段.*/\1/p')

	if [ -z "$chunks" ]; then
		echo "  $name · 没有切分记录：$trace"
		failed=$((failed + 1))
	elif [ "$name" = "oneword" ]; then
		if [ "$chunks" = "1" ]; then
			printf '  %-34s 正确    %s 段（无切点，退化为一段）\n' "$name · 切分段数" "$chunks"
		else
			echo "  $name · 全字母文件不该切出 $chunks 段"
			failed=$((failed + 1))
		fi
	else
		if [ "$chunks" -ge 2 ]; then
			printf '  %-34s 正确    %s 段\n' "$name · 切分段数" "$chunks"
		else
			echo "  $name · 只有 $chunks 段，切分路径没有被走到"
			failed=$((failed + 1))
		fi
	fi
	checked=$((checked + 1))
done

# 小文件必须不走切分：段数记录不该出现
trace=$(WORDCOUNT_TRACE_SPLIT=1 "$CLI" -j 4 "$work/small.txt" 2>&1 >/dev/null | tr -d '\r')
if [ -z "$trace" ]; then
	printf '  %-34s 正确    未切分\n' "small · 小于 2 MiB 不切"
	checked=$((checked + 1))
else
	echo "  small · 不该切分却出现了记录：$trace"
	failed=$((failed + 1))
	checked=$((checked + 1))
fi

# ---------------------------------------------------------------- 非普通文件

# 目录：open 成功、fstat 说不是普通文件，必须退回顺序路径，
# 并给出与顺序模式逐字节相同的错误信息（EISDIR 来自 fread）。
capture dseq testdata
capture dsplit -j 4 testdata
check_triple "目录参数 · -j 4" dseq dsplit

echo
if [ "$failed" -eq 0 ]; then
	echo "全部一致（$checked 项比对）"
	exit 0
fi

echo "$failed 组不一致"
exit 1
