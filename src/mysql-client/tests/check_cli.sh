#!/bin/sh
# ============================================================
# check_cli.sh —— 命令行端到端检查
#
# 单元测试直接调库，覆盖不到"参数解析 → 退出码 → 输出文本"这条链路。
# 这里按终端里的实际用法跑一遍。
#
# 所有用例都不需要运行中的数据库：
#   - 用法错误走退出码 2，在建连之前就返回；
#   - 文件错误走退出码 1，同样在建连之前；
#   - 唯一的网络用例连 127.0.0.1:1，内核直接回 RST，毫秒级失败。
# 因此这个脚本可以在任何机器上跑，包括没有 MySQL 服务的构建机上。
#
# 用法：sh tests/check_cli.sh <mysqlclient 可执行文件>
# 退出码：0 全部通过，1 有失败
# ============================================================

set -u

CLI=${1:-bin/mysqlclient}
CHECKS=0
FAILS=0

if [ ! -x "$CLI" ]; then
	echo "找不到可执行文件：$CLI（先 make）" >&2
	exit 1
fi

TMPD=$(mktemp -d) || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

check_exit() {
	CHECKS=$((CHECKS + 1))
	if [ "$2" -eq "$3" ]; then
		printf 'ok    %s\n' "$1"
	else
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-46s 退出码 %s，期望 %s\n' "$1" "$3" "$2"
	fi
}

check_contains() {
	CHECKS=$((CHECKS + 1))
	case $3 in
	*"$2"*)
		printf 'ok    %s\n' "$1"
		;;
	*)
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-46s 输出里没有 "%s"\n' "$1" "$2"
		;;
	esac
}

check_not_contains() {
	CHECKS=$((CHECKS + 1))
	case $3 in
	*"$2"*)
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-46s 输出里不该出现 "%s"\n' "$1" "$2"
		;;
	*)
		printf 'ok    %s\n' "$1"
		;;
	esac
}

# ------------------------------------------------------------
# 帮助与用法
# ------------------------------------------------------------

OUT=$("$CLI" -h 2>&1)
check_exit "帮助退出码 0" 0 $?
check_contains "帮助列出 ping" "ping" "$OUT"
check_contains "帮助列出 sql" "sql" "$OUT"
check_contains "帮助列出 each" "each" "$OUT"
check_contains "帮助列出 exec" "exec" "$OUT"
check_contains "帮助列出 blob-put" "blob-put" "$OUT"
check_contains "帮助列出 blob-get" "blob-get" "$OUT"
check_contains "帮助说明 stdout 只有数据" "stdout" "$OUT"

OUT=$("$CLI" 2>&1)
check_exit "无参数退出码 2" 2 $?
check_contains "无参数给出提示" "缺少命令" "$OUT"

"$CLI" bogus >/dev/null 2>&1
check_exit "未知命令退出码 2" 2 $?

"$CLI" -Z >/dev/null 2>&1
check_exit "未知全局选项退出码 2" 2 $?

"$CLI" -H >/dev/null 2>&1
check_exit "-H 缺参数退出码 2" 2 $?

"$CLI" -P abc ping >/dev/null 2>&1
check_exit "端口非数字退出码 2" 2 $?

"$CLI" -P 0 ping >/dev/null 2>&1
check_exit "端口 0 退出码 2" 2 $?

"$CLI" -P 70000 ping >/dev/null 2>&1
check_exit "端口越界退出码 2" 2 $?

"$CLI" -s abc blob-get -q 'SELECT 1' -o /dev/null >/dev/null 2>&1
check_exit "-s 非数字退出码 2" 2 $?

# ------------------------------------------------------------
# 子命令缺少必需参数
# ------------------------------------------------------------

"$CLI" sql >/dev/null 2>&1
check_exit "sql 缺 -q 退出码 2" 2 $?

"$CLI" exec -q '' >/dev/null 2>&1
check_exit "exec 空 SQL 退出码 2" 2 $?

"$CLI" blob-put -q 'INSERT T VALUES(?)' >/dev/null 2>&1
check_exit "blob-put 缺 -f 退出码 2" 2 $?

"$CLI" blob-get -q 'SELECT U_IMG FROM T' >/dev/null 2>&1
check_exit "blob-get 缺 -o 退出码 2" 2 $?

"$CLI" ping -q 'SELECT 1' >/dev/null 2>&1
check_exit "ping 拒绝 -q 退出码 2" 2 $?

"$CLI" each >/dev/null 2>&1
check_exit "each 缺 -q 退出码 2" 2 $?

"$CLI" each -q '' >/dev/null 2>&1
check_exit "each 空 SQL 退出码 2" 2 $?

"$CLI" each -q 'SELECT 1' -f "$TMPD/x.bin" >/dev/null 2>&1
check_exit "each 拒绝 -f 退出码 2" 2 $?

"$CLI" each -q 'SELECT 1' -o "$TMPD/x.out" >/dev/null 2>&1
check_exit "each 拒绝 -o 退出码 2" 2 $?

"$CLI" each -q 'SELECT 1' -n abc >/dev/null 2>&1
check_exit "each -n 非数字退出码 2" 2 $?

"$CLI" each -q 'SELECT 1' -n -3 >/dev/null 2>&1
check_exit "each -n 负数退出码 2" 2 $?

# -n 只对 each 有意义。静默忽略会让 "sql -n 10" 看起来限制了行数，
# 实际一行也没少——那是最难查的一类问题：输出"看起来没问题"。
"$CLI" sql -q 'SELECT 1' -n 1 >/dev/null 2>&1
check_exit "sql 用 -n 退出码 2" 2 $?

"$CLI" sql -q 'SELECT 1' -Z >/dev/null 2>&1
check_exit "子命令未知选项退出码 2" 2 $?

# ------------------------------------------------------------
# 文件错误：必须在建连之前报出来
#
# 一个写错的路径与服务器无关，却要通过一次完整的 TCP 握手才报错，
# 是最让人困惑的失败之一。这里用 -P 1（必连不上）配合：
# 若实现顺序反了，输出里会出现 "connect"，而不会是 "cannot open file"。
# ------------------------------------------------------------

printf 'hello' > "$TMPD/payload.bin"

OUT=$("$CLI" -H 127.0.0.1 -P 1 -u u blob-put \
	-q 'INSERT T VALUES(?)' -f "$TMPD/nonexistent.bin" 2>&1)
check_exit "blob-put 文件不存在退出码 1" 1 $?
check_contains "文件不存在时报 cannot open file" "cannot open file" "$OUT"
check_not_contains "文件错误不被误报成连接错误" "connect" "$OUT"

# ------------------------------------------------------------
# 连接失败：127.0.0.1:1 上不会有服务，内核直接回 RST
# ------------------------------------------------------------

OUT=$("$CLI" -H 127.0.0.1 -P 1 -u nobody ping 2>&1)
check_exit "连不上时退出码 1" 1 $?
check_contains "连不上时报 connect" "connect" "$OUT"
check_contains "连不上时给出状态描述" "cannot connect" "$OUT"

# 空密码必须被接受：报的应该是连接失败，而不是参数错误
OUT=$("$CLI" -H 127.0.0.1 -P 1 -u nobody -p '' ping 2>&1)
check_exit "空密码不触发用法错误" 1 $?

# 文件读成功之后才建连，因此有效文件 + 连不上 = connect 错误
OUT=$("$CLI" -H 127.0.0.1 -P 1 -u nobody blob-put \
	-q 'INSERT T VALUES(?)' -f "$TMPD/payload.bin" 2>&1)
check_exit "有效文件 + 连不上时退出码 1" 1 $?
check_contains "有效文件读到之后才报连接错误" "connect" "$OUT"

# 环境变量参与默认值：MC_PORT 被 -P 覆盖时，仍然是连接错误而不是用法错误
OUT=$(MC_PORT=1 "$CLI" -H 127.0.0.1 -u nobody ping 2>&1)
check_exit "环境变量 MC_PORT 生效" 1 $?
check_contains "环境变量配置的连接同样报 connect" "connect" "$OUT"

# ------------------------------------------------------------

printf '\n检查项 %d，失败 %d\n' "$CHECKS" "$FAILS"
[ "$FAILS" -eq 0 ] || exit 1
exit 0
