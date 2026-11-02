#!/bin/sh
# check_pool.sh —— 池驱动（-p）的端到端检查
#
# 检查的不是"跑得通"，而是三条只有接上线程池才会出问题的性质：
#
#   1. 一个任务都不许丢。四种同步实现的最终值必须精确等于 任务数 × 每任务次数。
#      驱动层算的 expected 与池自己数的 submitted / completed 是两条独立路径，
#      两边对上才说明既没有丢任务、也没有重复执行。
#   2. 有界队列真的在起作用。任务数小于队列容量时背压次数必须为 0；
#      任务数远超容量而只有一个工作线程时，背压次数必须大于 0。
#      只看耗时判断不了这件事——"提交端等了 3 ms"与"恰好每次都有空位、
#      只是任务本身慢"从外部看是一样的。
#   3. 参数校验不许静默忽略。-b 给了却没开 -p、-b 为负、总次数溢出
#      都必须报错退出（退出码 2），而不是"照跑不误，输出自洽"。
#
# 用法：sh tests/check_pool.sh [可执行文件路径]
# 默认使用 bin/counter。

set -u

CLI=${1:-bin/counter}

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

ok() {
	checked=$((checked + 1))
	echo "ok    $1"
}

bad() {
	checked=$((checked + 1))
	failed=$((failed + 1))
	echo "FAIL  $1"
}

# 跑一次，留下 stdout / stderr / 退出码
capture() {
	"$CLI" "$@" > "$work/out" 2> "$work/err"
	echo $? > "$work/rc"
}

expect_rc() {
	want=$1
	got=$(cat "$work/rc")
	if [ "$got" = "$want" ]; then
		ok "退出码 $want（$2）"
	else
		bad "退出码 $want（$2），实际 $got"
	fi
}

expect_grep() {
	pattern=$1
	label=$2
	if grep -q "$pattern" "$work/out" "$work/err"; then
		ok "$label"
	else
		bad "$label（输出里找不到 $pattern）"
	fi
}

# 从表格里取出一行的 expected / final / match 三列
row() {
	awk -v k="$1" '$1 == k { print $2, $3, $4 }' "$work/out"
}

# 从"池统计"那一行取出 key=value 里的值
#
# 用 sed 而不是 awk 按列切：这一行里第一个字段带着"池统计："前缀，
# 最后一个字段后面紧跟中文括号注释，按列切两头都要额外处理。
stat() {
	sed -n "s/.*$1=\([0-9][0-9]*\).*/\1/p" "$work/out" | head -1
}

echo "池驱动检查：$CLI"
echo

# ---------------------------------------------------------------- 1. 正确性
# 8000 次总自增，任务粒度 4，4 个工作线程。
# asm 在非 x86 上会打印 unsupported，跳过而不是判失败。
for kind in mutex spin atomic asm; do
	capture -p -k "$kind" -t 4 -n 2000 -b 4 -r 1
	if grep -q "unsupported" "$work/out"; then
		echo "skip  $kind：当前平台不支持"
		continue
	fi
	expect_rc 0 "$kind 池驱动"
	set -- $(row "$kind")
	if [ "${1:-}" = "8000" ] && [ "${2:-}" = "8000" ] && [ "${3:-}" = "yes" ]; then
		ok "$kind：expected=8000 final=8000 match=yes"
	else
		bad "$kind：期望 8000/8000/yes，实际 ${1:-?}/${2:-?}/${3:-?}"
	fi
done

# ------------------------------------------------------- 2. 池的记账要与驱动对上
capture -p -k atomic -t 4 -n 1000 -b 2 -r 2
expect_rc 0 "两轮 × 1000 任务"
sub=$(stat submitted)
com=$(stat completed)
if [ "${sub:-0}" = "2000" ] && [ "${com:-0}" = "2000" ]; then
	ok "池统计：提交 2000 完成 2000（两轮累加）"
else
	bad "池统计：期望 2000/2000，实际 ${sub:-?}/${com:-?}"
fi
set -- $(row atomic)
if [ "${1:-}" = "2000" ] && [ "${2:-}" = "2000" ]; then
	ok "两轮累加后的 expected 与 final 都为 2000"
else
	bad "两轮累加：期望 2000/2000，实际 ${1:-?}/${2:-?}"
fi

# ------------------------------------------------------------ 3. 背压的边界
# 4000 个任务、队列上限 8192：队列永远满不了，背压必须恰好为 0。
#
# 这里必须显式给 -q：默认容量 4096 大于任务数，结论一样，但读者要自己
# 做一次比较才知道；把容量写在命令行上，"容量 > 任务数所以不会满"是
# 眼前的事实，而不是需要翻实现才能确认的推断。
capture -p -k atomic -t 8 -n 4000 -b 1 -q 8192 -r 1
expect_rc 0 "4000 个任务（小于队列容量）"
if [ "$(stat backpressure)" = "0" ]; then
	ok "任务数小于队列容量时背压为 0"
else
	bad "任务数小于队列容量时背压应为 0，实际 $(stat backpressure)"
fi

# 队列上限 1、只有一个工作线程、每个任务 5000 次自增：
# 在途任务至多 2 个（1 个在队列、1 个在执行），提交到第 3 个时必然要等。
# 这不是"概率上会满"，而是容量本身决定的——每个任务要跑 5000 次原子加，
# 提交端不可能在它跑完之前腾出空位。
capture -p -k atomic -t 1 -n 50 -b 5000 -q 1 -r 1
expect_rc 0 "队列上限 1 + 慢任务"
bp=$(stat backpressure)
if [ "${bp:-0}" -gt 0 ] 2>/dev/null; then
	ok "队列被打满，背压 $bp 次"
else
	bad "队列必然被打满，背压却为 ${bp:-?}"
fi
set -- $(row atomic)
if [ "${1:-}" = "250000" ] && [ "${2:-}" = "250000" ]; then
	ok "背压之下仍然一个任务都没丢（250000/250000）"
else
	bad "背压路径丢了任务：期望 250000/250000，实际 ${1:-?}/${2:-?}"
fi

# --------------------------------------------------------- 4. batch 的两种极端
# batch=0：任务体不做事，量的是调度开销。期望值为 0 且必须匹配。
capture -p -k atomic -t 4 -n 5000 -b 0 -r 1
expect_rc 0 "batch=0"
set -- $(row atomic)
if [ "${1:-}" = "0" ] && [ "${2:-}" = "0" ] && [ "${3:-}" = "yes" ]; then
	ok "batch=0 时 expected 与 final 都为 0"
else
	bad "batch=0：期望 0/0/yes，实际 ${1:-?}/${2:-?}/${3:-?}"
fi
expect_grep "量的是纯调度开销" "batch=0 时打印了「Mops/s 无意义」的说明"

# 1 个任务 × 1000 次：只有一个任务时，全部自增必然落在同一个工作线程上。
capture -p -k atomic -t 2 -n 1 -b 1000 -r 1
expect_rc 0 "单任务 × 1000 次"
set -- $(row atomic)
if [ "${1:-}" = "1000" ] && [ "${2:-}" = "1000" ]; then
	ok "单任务：expected=1000 final=1000"
else
	bad "单任务：期望 1000/1000，实际 ${1:-?}/${2:-?}"
fi

# ------------------------------------------------------------- 5. 输出契约
capture -p -k atomic -t 3 -n 100 -b 5 -q 32 -r 1
expect_grep "driver=pool" "表头标明了驱动方式"
expect_grep "workers=3" "表头给出了工作线程数"
expect_grep "batch=5" "表头给出了每任务次数"
expect_grep "queue=32" "表头给出了命令行指定的队列上限"

# 线程驱动不该出现池的表头与统计行
capture -k atomic -t 2 -n 1000 -b 1 -r 1
expect_rc 2 "-b 在非池驱动下被拒绝"
if grep -q "driver=pool" "$work/out"; then
	bad "非池驱动打印了池的表头"
else
	ok "非池驱动不打印池的表头"
fi

capture -k atomic -t 2 -n 1000 -r 1
expect_rc 0 "线程驱动（不带任何池选项）"
if grep -q "池统计" "$work/out"; then
	bad "线程驱动打印了池统计行"
else
	ok "线程驱动不打印池统计行"
fi

# ---------------------------------------------------------- 6. 参数校验
capture -p -k atomic -t 2 -n 100 -b -1
expect_rc 2 "-b 为负被拒绝"
expect_grep "非负整数" "-b 为负给出了说明"

capture -p -k atomic -t 2 -n 100 -b abc
expect_rc 2 "-b 非数字被拒绝"

capture -q 16 -k atomic -t 2 -n 100
expect_rc 2 "-q 在非池驱动下被拒绝"

capture -p -k atomic -t 2 -n 100 -q 0
expect_rc 2 "队列上限 0 被拒绝"
expect_grep "队列上限" "-q 为 0 给出了说明"

capture -p -k atomic -t 2 -n 100 -q 65537
expect_rc 2 "队列上限超过 65536 被拒绝"

# 溢出：任务数 × 每任务次数超出 long long。有符号溢出是未定义行为，
# 不能指望"溢出成负数再比对不相等"，必须提前报错。
capture -p -k atomic -t 2 -n 9223372036854775807 -b 2
expect_rc 2 "总次数溢出被拒绝"
expect_grep "超出 long long" "溢出时给出了说明"

capture -p -k atomic -t 0 -n 100
expect_rc 2 "池驱动下 -t 0 被拒绝"

capture -p -k nosuchkind -t 2 -n 100
expect_rc 2 "未知实现被拒绝"

# --------------------------------------------------- 7. raw 只报告不断言
# raw 是数据竞争（C11 5.1.2.4，未定义行为），不能要求它一定丢更新，
# 只检查"跑得完、结果落在 [0, expected] 这个合理区间"。
capture -p -k raw -t 4 -n 200000 -b 1 -r 1
expect_rc 0 "raw 池驱动"
set -- $(row raw)
if [ "${1:-}" = "200000" ] && [ "${2:-0}" -ge 0 ] 2>/dev/null && [ "${2:-0}" -le 200000 ] 2>/dev/null; then
	echo "info  raw 池驱动（4 线程 × 20 万次）：final=${2}，期望 200000"
	ok "raw 的结果落在 [0, expected] 区间内"
else
	bad "raw 的结果越界：${2:-?}"
fi

echo
echo "$checked 项检查，$failed 项失败"
[ "$failed" -eq 0 ] || exit 1
exit 0
