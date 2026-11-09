#!/bin/sh
# check_align_budget.sh —— 验证"切点对齐的读取预算"按段长收缩这件事真的生效
#
# 要回答的问题：单个大文件切段时，切点对齐扫描读了多少字节？
#
# 为什么必须单独测这件事——只看"输出与顺序一致"是不够的，因为：
#
#   1. 对齐扫描（wc_align_chunk）找不到分隔符时会一直往后读，直到它收到的
#      limit。把 limit 传成文件末尾时，一个"整段内容是一个超长单词"的文件
#      （基因序列、单行 minified JSON）会让每个建议切点都读到末尾才放弃，
#      (nchunks-1) 个切点合计读约 (nchunks-1)/2 遍文件，之后还要顺序再读一遍。
#      结果完全正确，只是慢——这类退化不会让任何断言失败。
#   2. 预算生效与不生效，输出都逐字节相同；连段数都可能相同（全部切点被
#      放弃时两者都是 1 段）。所以 ASSERT 输出一致测不出预算。
#
# 因此这里断言五件事，全部来自 WORDCOUNT_TRACE_SPLIT 打出的可观测量：
#
#   · 全字母文件：每个切点都必须被放弃，段数退化成 1（切分被跳过）；
#   · 中段有超长单词的文件：被放弃的只是一部分切点，剩下的仍能切，段数 >= 2，
#     且结果与顺序一致——这是"放弃切点"这条策略唯一可能出错的地方（漏数/重数）；
#   · 预算公式：每个切点的预算 = min(段长 / 4, 1 MiB)，逐组核对数值；
#   · 对齐读取总量 <= 文件大小 / 4，且对 -j 的每个取值都成立；
#   · 被接受切点的最长扫描严格小于预算（第七轮）。前四条对"被放弃的那次扫描
#     也算进最大值"这种写法全绿——那个写法让最长扫描恒等于预算，这个量就
#     退化成预算的副本，说明不了"切点离分隔符有多远"，而 6.6 的除数依据
#     正是这个量。
#
# 最后一条是比例预算存在的全部理由。固定 1 MiB 的预算下这条会失败：
# 32 MiB 文件 -j 8 时 31 个切点各读 1 MiB，合计 31 MiB，等于文件大小的 97%。
#
# 另附一条耗时比值（只作佐证，阈值放得很宽）：不加预算时全字母文件要读
# 数倍于文件大小的字节，-j 4 的耗时会明显高于顺序路径；加了预算后两者接近。
#
# 用法：sh tests/check_align_budget.sh [可执行文件路径]
# 默认使用 bin/wordcount。

set -u

CLI=${1:-bin/wordcount}

LC_ALL=C
export LC_ALL

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先执行 make）" >&2
	exit 1
fi

# 与 src/main.c 的 ALIGN_SCAN_BYTES / ALIGN_BUDGET_DIV 必须一致
CAP=1048576
DIV=4

# 全字母文件的大小。要 >= 2 MiB 才会进切分路径；取 32 MiB 是为了让
# "-j 16 vs 顺序"的读取量差在页缓存内也足够明显（见文件末尾）。
ONEWORD_BYTES=33554432

work=$(mktemp -d) || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

checked=0
failed=0

pass() {
	printf '  %-46s 正确    %s\n' "$1" "$2"
	checked=$((checked + 1))
}

fail() {
	echo "  $1：$2"
	failed=$((failed + 1))
	checked=$((checked + 1))
}

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
		printf '  %-46s 一致    %s\n' "$label · stdout/stderr/退出码" "逐字节相同"
	else
		failed=$((failed + 1))
	fi
	checked=$((checked + 1))
}

# trace_of <文件> <线程数>：把驱动打到 stderr 的切分记录取出来
trace_of() {
	WORDCOUNT_TRACE_SPLIT=1 "$CLI" -j "$2" "$1" 2>&1 >/dev/null | tr -d '\r'
}

# field_of <trace> <标签> <单位>：取 "标签 N 单位" 里的 N
#
# 用 | 当 sed 的分隔符，因为单位里带 /（"字节/切点"）。
field_of() {
	printf '%s\n' "$1" | sed -n "s|.*$2 \([0-9][0-9]*\) $3.*|\1|p"
}

# size_of <文件>：文件字节数
size_of() {
	wc -c < "$1" | tr -d ' '
}

# ---------------------------------------------------------------- 造输入

# 1) 32 MiB 全是字母：一个词贯穿整个文件，没有任何可用切点
head -c "$ONEWORD_BYTES" /dev/zero | tr '\0' 'a' > "$work/oneword.txt"

# 2) 1 MiB 正常文本 + 4 MiB 单个词 + 1 MiB 正常文本：
#    中间那几个建议切点必须被放弃，两端的可以接受。
#    这是"放弃一部分切点"的唯一实战形态，漏数/重数都会在这里暴露。
cat testdata/b.txt testdata/b.txt > "$work/text2.txt"
head -c 1048576 "$work/text2.txt" > "$work/longword.txt"
head -c 4194304 /dev/zero | tr '\0' 'a' >> "$work/longword.txt"
head -c 1048576 "$work/text2.txt" >> "$work/longword.txt"

# 3) 8 MiB 正常文本：分隔符密集，对齐应该只读几十字节
cat testdata/b.txt testdata/b.txt testdata/b.txt testdata/b.txt \
    testdata/b.txt testdata/b.txt testdata/b.txt testdata/b.txt \
    testdata/b.txt testdata/b.txt testdata/b.txt testdata/b.txt \
    testdata/b.txt testdata/b.txt > "$work/mixed.txt"

echo "切点对齐的读取预算（每切点 = min(段长/4, $CAP)）"
echo "---------------------------------------------------------------"

# ---------------------------------------------------------------- 一致性

for name in oneword longword mixed; do
	f="$work/$name.txt"
	capture "$name-seq" "$f"

	for j in 2 4 8 16; do
		capture "$name-j$j" -j "$j" "$f"
		check_triple "$name · -j $j" "$name-seq" "$name-j$j"
	done
done

# ---------------------------------------------------------------- 预算

# 三个文件都单独断言，判据不同：全字母文件应当一个切点都留不下，
# 中段有超长词的文件应当只放弃一部分，正常文本一个都不该放弃。
for name in oneword longword mixed; do
	f="$work/$name.txt"
	size=$(size_of "$f")
	t=$(trace_of "$f" 4)

	segs=$(printf '%s\n' "$t" | sed -n 's/.*：\([0-9][0-9]*\) 段.*/\1/p')
	pts=$(field_of "$t" '建议切点' '个')
	skp=$(field_of "$t" '放弃' '个')

	if [ -z "$segs" ] || [ -z "$pts" ] || [ -z "$skp" ]; then
		fail "$name · 切分记录不完整" "$t"
		continue
	fi

	case $name in
	oneword)
		if [ "$segs" = "1" ] && [ "$skp" = "$pts" ]; then
			pass "$name · 全部切点被放弃" "$pts 个建议切点全部放弃，退化为 1 段"
		else
			fail "$name · 应放弃全部切点" \
			     "段数 $segs，建议切点 $pts，放弃 $skp"
		fi
		;;
	longword)
		if [ "$segs" -ge 2 ] && [ "$skp" -ge 1 ] && [ "$skp" -lt "$pts" ]; then
			pass "$name · 只放弃超长词内的切点" \
			     "$pts 个建议切点放弃 $skp 个，仍切出 $segs 段"
		else
			fail "$name · 应只放弃一部分切点" \
			     "段数 $segs，建议切点 $pts，放弃 $skp"
		fi
		;;
	mixed)
		if [ "$segs" -ge 2 ] && [ "$skp" = "0" ]; then
			pass "$name · 正常文本不放弃切点" "$pts 个建议切点全部接受，$segs 段"
		else
			fail "$name · 正常文本不该放弃切点" \
			     "段数 $segs，建议切点 $pts，放弃 $skp"
		fi
		;;
	esac
done

# 预算公式与总量上界：每个 -j 都核对一遍。
#
#   段数 nchunks = 建议切点 + 1（被放弃的切点不占段，但在 TRACE 里
#   "建议切点"报的是 nchunks-1，与段数无关），段长 = size / nchunks，
#   期望预算 = min(段长 / DIV, CAP)。
#
# 上界 size/4 与 -j 无关——这正是比例预算的要点。固定 1 MiB 时这里
# 会在 -j 4 及更大时失败（32 MiB 文件、15 个切点 × 1 MiB = 15 MiB > 8 MiB）。
for name in oneword longword mixed; do
	f="$work/$name.txt"
	size=$(size_of "$f")
	bound=$((size / DIV))

	for j in 2 4 8 16; do
		t=$(trace_of "$f" "$j")
		pts=$(field_of "$t" '建议切点' '个')
		rd=$(field_of "$t" '对齐扫描读' '字节')
		bud=$(field_of "$t" '预算' '字节/切点')
		max=$(field_of "$t" '接受切点最长扫描' '字节')

		if [ -z "$pts" ] || [ -z "$rd" ] || [ -z "$bud" ] || [ -z "$max" ]; then
			fail "$name · -j $j 切分记录不完整" "$t"
			continue
		fi

		want=$(( (size / (pts + 1)) / DIV ))
		if [ "$want" -gt "$CAP" ]; then
			want=$CAP
		fi

		if [ "$bud" = "$want" ]; then
			pass "$name · -j $j 预算公式" \
			     "$pts 个切点，段长 $((size / (pts + 1)))，预算 $bud"
		else
			fail "$name · -j $j 预算应为 $want" \
			     "TRACE 报 $bud，段长 $((size / (pts + 1)))，size $size"
		fi

		if [ "$rd" -le "$bound" ]; then
			pass "$name · -j $j 对齐读取不超 size/4" \
			     "$rd 字节 <= $bound（文件的 $((rd * 100 / size))%）"
		else
			fail "$name · -j $j 对齐读取超出 size/4" \
			     "$rd 字节 > $bound（文件的 $((rd * 100 / size))%）"
		fi

		# 最长一次扫描必须落在预算之内：被接受的切点只可能"在预算内找到了
		# 分隔符"，所以这个值严格小于预算；等于预算说明它把被放弃的那次
		# （必然读满预算）也算进了最大值——那样这个量就退化成预算的副本，
		# 再也说明不了"切点离分隔符有多远"。
		if [ "$max" -lt "$bud" ]; then
			pass "$name · -j $j 最长扫描在预算内" \
			     "$max 字节 < 预算 $bud"
		else
			fail "$name · -j $j 最长扫描 $max 字节" \
			     "应当小于预算 $bud（说明把被放弃的扫描也算进去了）"
		fi
	done
done

# 正常文本的读取量应当远小于上界：分隔符平均每几字节一个，
# 每个切点读不到 1 KiB。这条判据能发现"对齐改成逐块读整个区间"这类退化。
t=$(trace_of "$work/mixed.txt" 4)
rd=$(field_of "$t" '对齐扫描读' '字节')

if [ -n "$rd" ] && [ "$rd" -le 65536 ]; then
	pass "mixed · 正常文本对齐开销可忽略" "$rd 字节"
else
	fail "mixed · 正常文本对齐读了 $rd 字节" "期望 <= 65536"
fi

# 单个切点的扫描距离另有一条更紧的界：正常文本里一个词最多几十字节，
# 所以"最长的一次扫描"应当只有个位到两位数。总量那条按切点数累加，
# 切点多时会被摊薄，看不出一两个异常大的扫描；这条不容易被摊薄。
max=$(field_of "$t" '接受切点最长扫描' '字节')

if [ -n "$max" ] && [ "$max" -le 1024 ]; then
	pass "mixed · 单次扫描只有几十字节" "最长 $max 字节"
else
	fail "mixed · 单次最长扫描 $max 字节" "期望 <= 1024"
fi

# ---------------------------------------------------------------- 耗时佐证

# 不加预算时 32 MiB 全字母文件的 -j 4 要读 15 个切点 x 16 MiB 的对齐字节，
# 耗时会是顺序路径的数倍；加了预算后两者只差一次 32 MiB 的顺序读，
# 比值应接近 1。阈值放到 3.0：这条只用来兜住"预算被去掉"的量级回归，
# 不当作性能指标（CI 上单次耗时抖动大）。
start=$(date +%s%N)
"$CLI" "$work/oneword.txt" > /dev/null
mid=$(date +%s%N)
"$CLI" -j 4 "$work/oneword.txt" > /dev/null
end=$(date +%s%N)

seq_ms=$(( (mid - start) / 1000000 ))
par_ms=$(( (end - mid) / 1000000 ))

if [ "$seq_ms" -gt 0 ] && [ $((par_ms * 10 / seq_ms)) -le 30 ]; then
	pass "oneword · -j 4 不比顺序慢一个量级" \
	     "顺序 ${seq_ms} ms，-j 4 ${par_ms} ms"
else
	fail "oneword · -j 4 耗时 $par_ms ms 对顺序 $seq_ms ms" "比值超过 3.0"
fi

echo
if [ "$failed" -eq 0 ]; then
	echo "全部通过（$checked 项比对）"
	exit 0
fi

echo "$failed 项失败"
exit 1
