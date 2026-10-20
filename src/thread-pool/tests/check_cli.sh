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

OUT=$("$CLI" spawn -n 8 -w 0 2>&1)
check_exit "spawn -n 8 退出码 0" 0 $?
check_contains "spawn 报告墙钟耗时" "墙钟耗时" "$OUT"

# ------------------------------------------------------------

printf '\n检查项 %d，失败 %d\n' "$CHECKS" "$FAILS"
[ "$FAILS" -eq 0 ] || exit 1
exit 0
