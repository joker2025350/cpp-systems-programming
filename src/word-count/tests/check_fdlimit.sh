#!/bin/sh
# check_fdlimit.sh —— fd 配额：RLIMIT_NOFILE 不够时优雅退化
#
# 第九轮之前，-j 在"文件很多"这个场景下会输在资源上而不是算错：规划阶段
# 给每个可切文件留一个 fd，全部活到任务结束，于是同时打开的文件数 =
# 可切文件数。文件数超过 RLIMIT_NOFILE 时，超出的那些 open 拿到 EMFILE，
# 驱动报 `cannot open file: Too many open files` 并以非零码退出。
#
# 这个行为有两处不对：
#
#   · 同一批文件不加 -j 是能跑完的。也就是说 -j 让一份本来处理成功的输入
#     失败，"两种方式输出逐字节相同（含错误信息与退出码）"这条契约被破坏。
#   · 失败是整文件的，不是整批的：已经被规划好的文件照样统计，于是使用者
#     拿到一份"部分成功"的报告与非零退出码，得自己数哪几个漏了。
#
# 现在的行为是退化而不是报错：配额（软上限 - 预留）用尽之后，剩下的文件
# 退回整文件任务。统计结果完全一样，只是失去段内并行度。
#
# 断言分三层，缺一层就会出现"功能没生效但全绿"：
#
#   1. 输出：受限运行的 stdout 与顺序基线逐字节相同，stderr 为空，退出码 0。
#      这一层在配额根本没生效时也成立（老实现只在超限时才出错，超限不了
#      就一切正常），所以只有它不够。
#   2. 账：从 TRACE 里读"可用 / 已持有 / 放弃切分"三个数，验证
#      已持有 = min(可用, 文件数)、放弃 = 文件数 - 已持有、预留 = jobs + 4。
#      这一层能抓"配额算错"，但抓不到"算对了却没用上"（比如判据写反，
#      或者 held 从不自增）。
#   3. 调度：切分任务数必须多于文件数（配额 > 0 时），且任务总数等于
#      段数之和加整文件任务数。这一层抓"算对了但没真正改变任务划分"。
#
# 另有一组对照：不给上限时匹配的配额必须是 0 个降级，用来证明第 2、3 层
# 里的数字是被上限推出来的，而不是恒定值。
#
# 用法：sh tests/check_fdlimit.sh [可执行文件路径]
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

pass() {
	printf '  %-56s %s\n' "$1" "$2"
	checked=$((checked + 1))
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

field_of() {
	printf '%s\n' "$1" | sed -n "$2" | head -1
}

size_of() {
	wc -c < "$1" | tr -d ' '
}

# ---------------------------------------------------------------- 造输入
#
# 24 个都该切分的文件（每个约 2.95 MiB > 2 MiB 的门槛），用来把配额顶满。
# 24 是配合下面 ulimit 取的：limit 16 时配额 8，于是 8 个切、16 个退化，
# 两个数都不为 0，任一边写反都会露出来。

NFILES=24
n=0
while [ $n -lt "$NFILES" ]; do
	n=$((n + 1))
	f="$work/f$n.txt"
	k=0
	while [ $k -lt 5 ]; do
		cat testdata/b.txt >> "$f"
		k=$((k + 1))
	done
done

# 每个文件都必须不小于 2 MiB，否则它本来就不切分，"退化"这个动作无从发生，
# 下面的账会算成"少退了几个"却看不出原因。
too_small=0
for f in "$work"/f*.txt; do
	if [ "$(size_of "$f")" -lt 2097152 ]; then
		too_small=$((too_small + 1))
	fi
done
if [ "$too_small" -ne 0 ]; then
	echo "  有 $too_small 个造出来的文件小于 2 MiB（testdata/b.txt 不够长）" >&2
	exit 1
fi

set -- "$work"/f*.txt
files=$*

echo "fd 配额（RLIMIT_NOFILE）"
echo "---------------------------------------------------------------"

# 顺序基线：不加 -j，逐文件处理，不占配额
"$CLI" $files > "$work/seq.out" 2> "$work/seq.err"
seq_rc=$?
if [ "$seq_rc" -ne 0 ] || [ -s "$work/seq.err" ]; then
	echo "  顺序基线没有干净跑完（退出码 $seq_rc）" >&2
	cat "$work/seq.err" >&2
	exit 1
fi
echo "  顺序基线：$NFILES 个文件，$(size_of "$work/seq.out") 字节输出"
echo

# 受限运行：在子 shell 里改软上限（降低软上限不需要特权），
# 用 exec 换掉那个 shell，保证被测进程的 fd 表里只有它自己的三个标准流。
# trace_of：同样的受限运行，只要 stderr（TRACE 记录），不管 stdout
run_limited() {
	limit=$1
	out=$2
	err=$3
	shift 3
	(
		ulimit -n "$limit" || exit 99
		exec "$CLI" "$@"
	) > "$out" 2> "$err"
}

trace_limited() {
	limit=$1
	shift
	(
		ulimit -n "$limit" || exit 99
		export WORDCOUNT_TRACE_SPLIT=1
		exec "$CLI" "$@"
	) 2>&1 >/dev/null | tr -d '\r'
}

# 先确认这个 shell 能把软上限压到要测的值。压不下去就直接报出来，
# 不要让它表现成"程序出错"。
if ! ( ulimit -n 16 ) 2>/dev/null; then
	echo "  跳过：本 shell 无法把 ulimit -n 设为 16" >&2
	exit 1
fi

JOBS=4
RESERVE_EXTRA=4

for limit in 16 10 8; do
	run_limited "$limit" "$work/l$limit.out" "$work/l$limit.err" -j "$JOBS" $files
	rc=$?
	t=$(trace_limited "$limit" -j "$JOBS" $files)

	budget=$(field_of "$t" 's/.*可用 \([0-9][0-9]*\) 个.*/\1/p')
	held=$(field_of "$t" 's/.*已持有 \([0-9][0-9]*\) 个.*/\1/p')
	degr=$(field_of "$t" 's/.*放弃切分的文件 \([0-9][0-9]*\) 个.*/\1/p')
	resv=$(field_of "$t" 's/.*预留 \([0-9][0-9]*\)、.*/\1/p')
	want_budget=$((limit - JOBS - RESERVE_EXTRA))
	if [ "$want_budget" -lt 0 ]; then
		want_budget=0
	fi
	want_held=$want_budget
	if [ "$want_held" -gt "$NFILES" ]; then
		want_held=$NFILES
	fi
	want_degr=$((NFILES - want_held))

	# 1. 输出
	if [ "$rc" -ne 0 ] || [ -s "$work/l$limit.err" ] || \
	   ! cmp -s "$work/seq.out" "$work/l$limit.out"; then
		fail "ulimit -n $limit · 输出与顺序基线不一致（退出码 $rc）" \
		     "$(head -3 "$work/l$limit.err")"
	else
		pass "ulimit -n $limit · stdout 与顺序基线逐字节相同，stderr 空" \
		     "退出码 0"
	fi

	# 2. 账
	if [ "$resv" = "$((JOBS + RESERVE_EXTRA))" ] && \
	   [ "$budget" = "$want_budget" ] && \
	   [ "$held" = "$want_held" ] && \
	   [ "$degr" = "$want_degr" ]; then
		pass "ulimit -n $limit · 配额 $budget = $limit - 预留 $resv" \
		     "持有 $held，降级 $degr / $NFILES"
	else
		fail "ulimit -n $limit · 账不对（预留 $resv 期望 $((JOBS + RESERVE_EXTRA))，" \
		     "可用 $budget 期望 $want_budget，持有 $held 期望 $want_held，" \
		     "降级 $degr 期望 $want_degr）" "$t"
	fi

	# 3. 调度：配额 > 0 时必须真的还有切分任务
	tasks=$(field_of "$t" 's/.*共 \([0-9][0-9]*\) 个任务.*/\1/p')
	whole=$(field_of "$t" 's/.*整文件任务 \([0-9][0-9]*\) 个.*/\1/p')
	segs=$(printf '%s\n' "$t" | sed -n 's/.*切分 .*：\([0-9][0-9]*\) 段.*/\1/p')
	sum_seg=0
	for s in $segs; do
		sum_seg=$((sum_seg + s))
	done

	if [ "$budget" -gt 0 ]; then
		if [ -n "$tasks" ] && [ "$tasks" -gt "$NFILES" ] && \
		   [ "$tasks" = "$((sum_seg + whole))" ] && \
		   [ "$(printf '%s\n' "$t" | grep -c '切分 ' || true)" = "$held" ]; then
			pass "ulimit -n $limit · 仍有切分任务，任务数 = 段数之和 + 整文件" \
			     "$tasks = $sum_seg + $whole"
		else
			fail "ulimit -n $limit · 任务划分不对（任务 $tasks，" \
			     "段数之和 $sum_seg，整文件 $whole，切分记录 $(printf '%s\n' "$t" | grep -c '切分 ' || true) 条）" "$t"
		fi
	else
		# 配额为 0：一个切分都不该有，任务数必须正好等于文件数
		if [ -n "$tasks" ] && [ "$tasks" = "$NFILES" ] && \
		   [ "$whole" = "$NFILES" ]; then
			pass "ulimit -n $limit · 配额为 0 时全部退化成整文件任务" \
			     "$tasks 个任务"
		else
			fail "ulimit -n $limit · 配额为 0 但仍有切分（任务 $tasks，整文件 $whole）" "$t"
		fi
	fi
done

# ---------------------------------------------------------------- 对照
#
# 不给上限（用本 shell 自己的软上限）：配额远大于文件数，降级必须是 0、
# 持有必须等于文件数。没有这一组，上面那些"降级 N 个"没有理由相信是
# 上限推出来的——它们也可能来自某个恒定值。

t=$(WORDCOUNT_TRACE_SPLIT=1 "$CLI" -j "$JOBS" $files 2>&1 >/dev/null | tr -d '\r')
held=$(field_of "$t" 's/.*已持有 \([0-9][0-9]*\) 个.*/\1/p')
degr=$(field_of "$t" 's/.*放弃切分的文件 \([0-9][0-9]*\) 个.*/\1/p')
if [ "$held" = "$NFILES" ] && [ "$degr" = "0" ]; then
	pass "对照 · 上限足够时不降级" "持有 $held 个，降级 0 个"
else
	fail "对照 · 上限足够时持有 $held 个、降级 $degr 个，期望 $NFILES / 0" "$t"
fi

# 上限高于 INT_MAX 时按"无上限"处理，不应出现负数或溢出值
t=$(WORDCOUNT_TRACE_SPLIT=1 "$CLI" -j "$JOBS" $files 2>&1 >/dev/null | tr -d '\r')
limit_field=$(field_of "$t" 's/.*软上限 \([0-9][0-9]*\)、.*/\1/p')
if [ -z "$limit_field" ]; then
	# 本 shell 的上限要是真的大到 INT_MAX 以上，驱动会打"无上限"
	if printf '%s\n' "$t" | grep -q '软上限 无上限'; then
		pass "软上限超过 INT_MAX 时按无上限处理" "无上限"
	else
		fail "软上限既不是数字也不是\"无上限\"" "$t"
	fi
else
	pass "软上限读数" "$limit_field"
fi

# ---------------------------------------------------------------- 数字本身

# 独立于配额的一层：total 行的字节数必须等于各文件字节数之和。
# 降级会改变任务划分，但不该改变任何统计数字。
want_bytes=0
for f in $files; do
	want_bytes=$((want_bytes + $(size_of "$f")))
done
got_bytes=$("$CLI" -j "$JOBS" $files | tail -1 | awk '{print $3}')
if [ "$got_bytes" = "$want_bytes" ]; then
	pass "total 行字节数（降级不改变统计）" "$got_bytes 字节"
else
	fail "total 行字节数是 $got_bytes，$NFILES 个文件合计 $want_bytes"
fi

echo
if [ "$failed" -eq 0 ]; then
	echo "全部通过（$checked 项比对）"
	exit 0
fi

echo "$failed 项不一致"
exit 1
