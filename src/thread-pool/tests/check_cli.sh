#!/bin/sh
# ============================================================
# check_cli.sh —— 命令行端到端检查
#
# 单元测试直接调库，覆盖不到"参数解析 → 退出码 → 输出文本"这条链路。
# 这里按用户在终端里的实际用法跑一遍。
#
# 用法：sh tests/check_cli.sh <threadpool 可执行文件>
# 退出码：0 全部通过，1 有失败
# ============================================================

set -u

CLI=${1:-bin/threadpool}
CHECKS=0
FAILS=0

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先 make）" >&2
	exit 1
fi

# check <名称> <期望退出码> <实际退出码>
check_exit() {
	CHECKS=$((CHECKS + 1))
	if [ "$2" -eq "$3" ]; then
		printf 'ok    %s\n' "$1"
	else
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-44s 退出码 %s，期望 %s\n' "$1" "$3" "$2"
	fi
}

# check_contains <名称> <期望子串> <文本>
check_contains() {
	CHECKS=$((CHECKS + 1))
	case $3 in
	*"$2"*)
		printf 'ok    %s\n' "$1"
		;;
	*)
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-44s 输出里没有 "%s"\n' "$1" "$2"
		;;
	esac
}

# ------------------------------------------------------------

OUT=$("$CLI" help 2>&1)
check_exit "help 退出码 0" 0 $?
check_contains "help 列出 run 子命令" "run" "$OUT"
check_contains "help 列出 bounded 子命令" "bounded" "$OUT"
check_contains "help 列出 timeout 子命令" "timeout" "$OUT"
check_contains "help 列出 drain 子命令" "drain" "$OUT"
check_contains "help 列出 spawn 子命令" "spawn" "$OUT"

# ------------------------------------------------------------

OUT=$("$CLI" run -t 2 -n 8 2>&1)
check_exit "run -t 2 -n 8 退出码 0" 0 $?
check_contains "run 校验 8 个任务全部正确" "校验：8 个任务结果正确，0 个不正确" "$OUT"
check_contains "run 报告 completed=8" "completed=8" "$OUT"
check_contains "run 结束后队列为空" "queued=0 running=0" "$OUT"

# 带工作量：4 个线程跑 1000 个 0.05 ms 任务
OUT=$("$CLI" run -t 4 -n 1000 -w 0.05 2>&1)
check_exit "run -t 4 -n 1000 -w 0.05 退出码 0" 0 $?
check_contains "1000 个任务全部正确" "1000 个任务结果正确" "$OUT"
check_contains "completed=1000" "completed=1000" "$OUT"

# 单线程池也必须正确（FIFO 路径）
OUT=$("$CLI" run -t 1 -n 64 2>&1)
check_exit "run -t 1 -n 64 退出码 0" 0 $?
check_contains "单线程池 64 个任务正确" "64 个任务结果正确" "$OUT"

# ------------------------------------------------------------

"$CLI" run -t 0 -n 8 >/dev/null 2>&1
check_exit "run -t 0 被拒绝（退出码 2）" 2 $?

"$CLI" run -t 2 -n 0 >/dev/null 2>&1
check_exit "run -n 0 被拒绝（退出码 2）" 2 $?

"$CLI" run -t -1 >/dev/null 2>&1
check_exit "run -t -1 被拒绝（退出码 2）" 2 $?

"$CLI" bogus >/dev/null 2>&1
check_exit "未知命令退出码 2" 2 $?

"$CLI" run -z >/dev/null 2>&1
check_exit "未知选项退出码 2" 2 $?

# ------------------------------------------------------------
# 有界队列 + 阻塞提交

OUT=$("$CLI" bounded -t 2 -c 4 -n 200 -w 0.02 2>&1)
check_exit "bounded -t 2 -c 4 -n 200 退出码 0" 0 $?
check_contains "bounded 报告队列上限" "队列上限 4" "$OUT"
check_contains "bounded 校验 200 个任务全部正确" "校验：200 个任务结果正确，0 个不正确" "$OUT"
check_contains "bounded 报告 capacity=4" "capacity=4" "$OUT"
check_contains "bounded 报告 completed=200" "completed=200" "$OUT"
check_contains "bounded 结束后队列为空" "queued=0 running=0" "$OUT"

# 容量 1 也要成立：每次提交都要等到工作线程取走上一个任务
OUT=$("$CLI" bounded -t 1 -c 1 -n 50 2>&1)
check_exit "bounded -c 1 退出码 0" 0 $?
check_contains "capacity=1 时 50 个任务全部正确" "50 个任务结果正确" "$OUT"

"$CLI" bounded -t 2 -c 0 -n 8 >/dev/null 2>&1
check_exit "bounded -c 0 被拒绝（退出码 2）" 2 $?

"$CLI" bounded -t 2 -c 4 -n 0 >/dev/null 2>&1
check_exit "bounded -n 0 被拒绝（退出码 2）" 2 $?

# ------------------------------------------------------------
# 有界队列 + 带截止时间的提交
#
# -w 100 让唯一的工作线程忙 100 ms，-m 30 让提交方最多等 30 ms：
# 队列在 100 ms 内不可能腾出空位，因此超时是确定的结果，不是概率事件。

OUT=$("$CLI" timeout -w 100 -m 30 2>&1)
check_exit "timeout -w 100 -m 30 退出码 0" 0 $?
check_contains "timeout 报告发生了超时" "后超时，任务未被接受" "$OUT"
check_contains "timeout 报告队列上限 2" "队列上限 2" "$OUT"
check_contains "timeout 报告 capacity=2" "capacity=2" "$OUT"
check_contains "timeout 报告 timeouts=1" "timeouts=1" "$OUT"
check_contains "timeout 报告背压 1 次" "backpressure=1" "$OUT"
check_contains "超时的任务没有被计入 submitted" "submitted=3" "$OUT"
check_contains "排空后 3 个任务全部执行" "completed=3" "$OUT"
check_contains "超时的任务从未执行" "超时的任务 从未执行" "$OUT"

# 等待时长必须接近 -m。返回码分不清"等满了 30 ms 才超时"与"条件变量的时钟
# 与截止时间错配、每次都立即超时"——两种情况下状态码都是 TIMEOUT，
# 只有这个数字能区分。2026-11-01 的缺陷注入正是靠这一条才被抓住的。
WAITED=$(printf '%s\n' "$OUT" | sed -n 's/.*等待 \([0-9][0-9.]*\) ms 后超时.*/\1/p')
CHECKS=$((CHECKS + 1))
if [ -z "$WAITED" ]; then
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-44s 输出里取不到等待时长\n' "timeout 等满了 -m 才超时"
elif awk -v w="$WAITED" 'BEGIN { exit !(w >= 15 && w <= 500) }'; then
	printf 'ok    %s（%s ms）\n' "timeout 等满了 -m 才超时" "$WAITED"
else
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-44s 实际 %s ms，期望 20 ~ 500\n' "timeout 等满了 -m 才超时" "$WAITED"
fi

# -m 0：截止时间就是"现在"，满队列上必须立即超时
OUT=$("$CLI" timeout -c 1 -w 200 -m 0 2>&1)
check_exit "timeout -m 0 退出码 0" 0 $?
check_contains "timeout -m 0 也走超时分支" "后超时" "$OUT"

"$CLI" timeout -w 30 -m 30 >/dev/null 2>&1
check_exit "timeout 的 -m 不小于 -w 被拒绝（退出码 2）" 2 $?

"$CLI" timeout -t 4 >/dev/null 2>&1
check_exit "timeout 的 -t 4 被拒绝（退出码 2）" 2 $?

"$CLI" timeout -c 0 >/dev/null 2>&1
check_exit "timeout 的 -c 0 被拒绝（退出码 2）" 2 $?

"$CLI" timeout -m -1 >/dev/null 2>&1
check_exit "timeout 的 -m -1 被拒绝（退出码 2）" 2 $?

# ------------------------------------------------------------
# 屏障 + 带截止时间的等待
#
# -w 100 让唯一的工作线程忙 100 ms，-m 50 让屏障最多等 50 ms：
# 截止时间落进第一个任务执行期间，超时是构造出来的，不是概率事件。
#
# 这里必须同时看两件事：返回码（屏障到没到）与两组计数（超时那一刻还剩多少、
# 排空之后做完多少）。只看返回码分不清"实现没等到就返回"与"任务真的没做完"。

OUT=$("$CLI" drain -t 1 -n 4 -w 100 -m 50 2>&1)
check_exit "drain -t 1 -n 4 -w 100 -m 50 退出码 0" 0 $?
check_contains "drain 报告屏障未到达" "屏障未到达" "$OUT"
check_contains "drain 后超时" "后超时" "$OUT"
check_contains "超时时没有任务做完" "completed=0 queued=3 running=1" "$OUT"
check_contains "drain 报告 wait_timeouts=1" "wait_timeouts=1" "$OUT"
check_contains "排空后 4 个任务全部正确" "排空后：completed=4，4 个任务结果正确，0 个不正确" "$OUT"

WAITED=$(printf '%s\n' "$OUT" | sed -n 's/.*等待 \([0-9][0-9.]*\) ms 后超时.*/\1/p')
CHECKS=$((CHECKS + 1))
if [ -z "$WAITED" ]; then
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-44s 输出里取不到等待时长\n' "drain 等满了 -m 才超时"
elif awk -v w="$WAITED" 'BEGIN { exit !(w >= 30 && w <= 500) }'; then
	printf 'ok    %s（%s ms）\n' "drain 等满了 -m 才超时" "$WAITED"
else
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-44s 实际 %s ms，期望 30 ~ 500\n' "drain 等满了 -m 才超时" "$WAITED"
fi

# 截止时间远大于串行工作量：屏障必须由"任务做完"触发，而不是等到截止时间。
# 返回码是 OK 只说明条件成立，waited 才能说明没有白等到 5 s。
OUT=$("$CLI" drain -t 1 -n 4 -w 20 -m 5000 2>&1)
check_exit "drain -w 20 -m 5000 退出码 0" 0 $?
check_contains "drain 报告屏障已到达" "屏障已到达" "$OUT"
check_contains "到达屏障时任务全部完成" "completed=4 queued=0 running=0" "$OUT"
check_contains "没有计入等待超时" "wait_timeouts=0" "$OUT"
check_contains "排空后结果仍然正确" "排空后：completed=4，4 个任务结果正确，0 个不正确" "$OUT"

DRAIN_WAITED=$(printf '%s\n' "$OUT" | sed -n 's/.*屏障已到达，等待 \([0-9][0-9.]*\) ms.*/\1/p')
CHECKS=$((CHECKS + 1))
if [ -z "$DRAIN_WAITED" ]; then
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-44s 输出里取不到等待时长\n' "drain 排空即返回"
elif awk -v w="$DRAIN_WAITED" 'BEGIN { exit !(w >= 0 && w <= 2000) }'; then
	printf 'ok    %s（%s ms）\n' "drain 排空即返回" "$DRAIN_WAITED"
else
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-44s 实际 %s ms，期望 0 ~ 2000\n' "drain 排空即返回" "$DRAIN_WAITED"
fi

"$CLI" drain -t 0 >/dev/null 2>&1
check_exit "drain -t 0 被拒绝（退出码 2）" 2 $?

"$CLI" drain -n 0 >/dev/null 2>&1
check_exit "drain -n 0 被拒绝（退出码 2）" 2 $?

"$CLI" drain -m -1 >/dev/null 2>&1
check_exit "drain -m -1 被拒绝（退出码 2）" 2 $?

# ------------------------------------------------------------

OUT=$("$CLI" spawn -n 8 -w 0 2>&1)
check_exit "spawn -n 8 退出码 0" 0 $?
check_contains "spawn 报告墙钟耗时" "墙钟耗时" "$OUT"

# ------------------------------------------------------------

printf '\n检查项 %d，失败 %d\n' "$CHECKS" "$FAILS"
[ "$FAILS" -eq 0 ] || exit 1
exit 0
