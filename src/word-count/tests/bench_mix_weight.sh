#!/bin/sh
# bench_mix_weight.sh —— 混合负载：一个大文件 + 若干个刚好不切的小文件
#
# 要回答的问题只有一个：把任务单元从"一个文件"细到"一个字节区间"之后，
# 完工时间到底变了多少。
#
# 构造（-j 8）：
#   huge.txt  64 MiB 正常文本  —— 切成 32 段（每段 2 MiB）
#   s0..s6    每个 1 MiB 正常文本 —— 小于 2 MiB 的门槛，各是一个整文件任务
#   合计 39 个任务，8 个工作线程
#
# 老实现（第八轮之前）在这种输入上的完工时间有一个明确的下界：8 个文件
# 只有 8 个任务，huge 独占一个线程从头做到尾，其余 7 个线程几十毫秒就空了，
# 于是 wall = huge 的单线程耗时。这个量可以直接测：`-j 1 huge.txt`。
# 所以下面第三行不是"估算"，是同一台机器上量出来的地板。
#
# 用法：sh tests/bench_mix_weight.sh <可执行文件> <临时目录>

set -u

CLI=${1:-bin/wordcount}
work=${2:?需要一个临时目录}

LC_ALL=C
export LC_ALL

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI" >&2
	exit 1
fi

mkdir -p "$work" || exit 1

# ---------------------------------------------------------------- 造输入

# 一块 1 MiB 的正常文本（10 份 b.txt），大文件用 cat 反复追加，小文件就是它本身。
# 不直接 head -c 截断 1 MiB：截断点落在词里也无所谓（本模块按字节统计），
# 但用整块可以让"每个小文件正好 1 MiB"这件事一目了然。
i=0
: > "$work/block.txt"
while [ $i -lt 10 ]; do
	cat testdata/b.txt >> "$work/block.txt"
	i=$((i + 1))
done
head -c 1048576 "$work/block.txt" > "$work/s0.txt"

i=1
while [ $i -lt 7 ]; do
	cp "$work/s0.txt" "$work/s$i.txt"
	i=$((i + 1))
done

i=0
: > "$work/huge.txt"
while [ $i -lt 64 ]; do
	cat "$work/s0.txt" >> "$work/huge.txt"
	i=$((i + 1))
done

huge=$(wc -c < "$work/huge.txt" | tr -d ' ')
small=$(wc -c < "$work/s0.txt" | tr -d ' ')
files="$work/huge.txt $work/s0.txt $work/s1.txt $work/s2.txt $work/s3.txt"
files="$files $work/s4.txt $work/s5.txt $work/s6.txt"

# 造出来的字节数必须正好是设计的量：cat 失败时只会得到更短的文件，
# 而下面所有结论都建立在"大文件 64 MiB、小文件各 1 MiB"这个前提上。
if [ "$huge" != "67108864" ] || [ "$small" != "1048576" ]; then
	echo "$work/huge.txt 是 $huge 字节、$work/s0.txt 是 $small 字节，与设计不符" >&2
	exit 1
fi

# run_ms <次数> <参数...>：跑若干轮，返回平均毫秒数（四舍五入到整数）
run_ms() {
	rounds=$1
	shift
	start=$(date +%s%N)
	i=0
	while [ $i -lt "$rounds" ]; do
		"$CLI" "$@" > /dev/null
		i=$((i + 1))
	done
	end=$(date +%s%N)
	echo $(( (end - start) / 1000000 / rounds ))
}

echo "混合负载：1 × $huge 字节 + 7 × $small 字节，共 8 个文件（各跑 3 轮取平均）"

seq_ms=$(run_ms 3 $files)
floor_ms=$(run_ms 3 -j 1 "$work/huge.txt")
par_ms=$(run_ms 3 -j 8 $files)

trace=$(WORDCOUNT_TRACE_SPLIT=1 "$CLI" -j 8 $files 2>&1 >/dev/null | tr -d '\r')
tasks=$(printf '%s\n' "$trace" | sed -n 's/.*共 \([0-9][0-9]*\) 个任务.*/\1/p' | head -1)
segs=$(printf '%s\n' "$trace" | sed -n 's/.*切分 .*：\([0-9][0-9]*\) 段.*/\1/p' | head -1)

printf '  顺序（不带 -j）        %6d ms\n' "$seq_ms"
printf '  老路径的地板           %6d ms   （-j 1 huge.txt：huge 独占一个线程）\n' "$floor_ms"
printf '  -j 8（新）             %6d ms   任务 %s 个（huge %s 段 + 7 个整文件）\n' \
	"$par_ms" "${tasks:-?}" "${segs:-?}"

# 加速比用整数除法算两位小数，避免依赖 awk 的浮点格式
if [ "$par_ms" -gt 0 ]; then
	printf '  相对地板               %d.%02d×\n' \
		$((floor_ms / par_ms)) $(( (floor_ms % par_ms) * 100 / par_ms ))
	printf '  相对顺序               %d.%02d×\n' \
		$((seq_ms / par_ms)) $(( (seq_ms % par_ms) * 100 / par_ms ))
fi

# 输出必须与顺序完全一致：这一组是性能测量，但顺手把正确性再验一次
"$CLI" $files > "$work/seq.out"
"$CLI" -j 8 $files > "$work/par.out"
if cmp -s "$work/seq.out" "$work/par.out"; then
	printf '  输出                  与顺序逐字节相同\n'
else
	echo "  !! 输出与顺序不一致" >&2
	exit 1
fi
