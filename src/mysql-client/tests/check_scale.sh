#!/bin/sh
# ============================================================
# check_scale.sh —— 百万行下的内存对照：物化取数 vs 流式取数
#
# 验证的问题只有一个：mc_query_each 的峰值内存是否与行数无关。
# 为此必须用同一个结果集跑两遍，一遍走 mc_query、一遍走 mc_query_each，
# 而且必须在**两个进程**里跑——峰值 RSS 是进程级、单调不减的指标，
# 同一个进程内先后跑两次，第二次测到的仍是第一次的峰值。
#
# 行数用"自连接倍增"造：一条 INSERT ... SELECT ... FROM 自己 让行数翻倍，
# 20 次就到 1048576 行，比逐行 INSERT 快三个数量级，
# 也不需要存储过程或外部脚本生成 SQL 文件。
#
# 正确性不靠两个实现互相比对（那样共同的错误会互相掩护），
# 而是让服务器自己算 COUNT(*) 作为基准。
#
# 用法：sh tests/check_scale.sh <mysqlclient> <test_stream>
# 退出码：0 全部通过，1 有失败
# ============================================================

set -u

CLI=${1:-bin/mysqlclient}
STREAM=${2:-bin/test_stream}
DB=${MC_TEST_DB:-mc_live_db}
TBL="$DB.t_scale"

CHECKS=0
FAILS=0

# 脚本按 POSIX sh 写，不依赖 bash 数组：Ubuntu 上 /bin/sh 是 dash，
# 用了数组会直接在这里失败，而 CI 里未必有人注意到换成 bash 就能过。
field() {
	# $1 = 模式名（同时是临时文件名），$2 = 键名
	sed -n "s/.*$2=\([0-9a-f.]*\).*/\1/p" "$TMPD/$1.txt"
}

check_true() {
	CHECKS=$((CHECKS + 1))
	if [ "$2" = "1" ]; then
		printf 'ok    %s\n' "$1"
	else
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-44s %s\n' "$1" "$3"
	fi
}

if [ ! -x "$CLI" ] || [ ! -x "$STREAM" ]; then
	echo "用法：sh tests/check_scale.sh <mysqlclient> <test_stream>" >&2
	exit 1
fi

TMPD=$(mktemp -d) || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

if ! "$CLI" ping >/dev/null 2>&1; then
	printf '连不上数据库，跳过\n' >&2
	exit 1
fi

printf '造 100 万行数据（自连接倍增，约 20 条语句）…\n'

"$CLI" exec -q "DROP TABLE IF EXISTS $TBL" >/dev/null 2>&1
"$CLI" exec -q "CREATE DATABASE IF NOT EXISTS $DB" >/dev/null 2>&1
"$CLI" exec -q "CREATE TABLE $TBL (
	id INT PRIMARY KEY,
	s  VARCHAR(32),
	b  VARBINARY(16)
)" >/dev/null 2>&1

# 第 1 行：二进制列放一个含 '\0' 的值，让流式路径的"按长度而非 strlen"也被覆盖
"$CLI" exec -q "INSERT INTO $TBL VALUES (1, 'name-1', CONCAT('a', CHAR(0), 'b'))" >/dev/null

N=1
while [ "$N" -lt 1048576 ]; do
	if ! "$CLI" exec -q "INSERT INTO $TBL
		SELECT id + $N, CONCAT('name-', id + $N), b FROM $TBL" >/dev/null; then
		echo "造数据失败（N=$N）" >&2
		exit 1
	fi
	N=$((N * 2))
done

# 服务器自己算的基准行数
SROWS=$("$CLI" sql -q "SELECT COUNT(*) FROM $TBL" 2>/dev/null | tail -n 1)
printf '服务器基准：%s 行\n\n' "$SROWS"
printf '%-7s %10s %14s %13s %9s\n' '模式' '行数' '内容字节' '峰值RSS(KiB)' '耗时(ms)'
printf '%s\n' '-------------------------------------------------------'

for mode in query each; do
	if "$STREAM" mem "$mode" "SELECT id, s, b FROM $TBL" \
		> "$TMPD/$mode.txt" 2> "$TMPD/$mode.err"; then
		printf '%-7s %10s %14s %13s %9s\n' "$mode" \
			"$(field "$mode" rows)" \
			"$(field "$mode" bytes)" \
			"$(field "$mode" maxrss_kb)" \
			"$(field "$mode" ms)"
	else
		CHECKS=$((CHECKS + 1))
		FAILS=$((FAILS + 1))
		printf 'FAIL  %s 模式运行失败：%s\n' "$mode" "$(cat "$TMPD/$mode.err")"
	fi
done

printf '\n'

QROWS=$(field query rows)
EROWS=$(field each rows)
QKB=$(field query maxrss_kb)
EKB=$(field each maxrss_kb)
QFNV=$(field query fnv)
EFNV=$(field each fnv)

check_true "query 行数等于服务器 COUNT(*)" \
	"$([ "$QROWS" = "$SROWS" ] && echo 1 || echo 0)" "$QROWS vs $SROWS"
check_true "each 行数等于服务器 COUNT(*)" \
	"$([ "$EROWS" = "$SROWS" ] && echo 1 || echo 0)" "$EROWS vs $SROWS"

# 校验和：两条路径对同一份数据必须算出同一个值
check_true "两种模式的校验和相同" \
	"$([ "$QFNV" = "$EFNV" ] && echo 1 || echo 0)" "$QFNV vs $EFNV"

RATIO=$(awk -v a="$QKB" -v b="$EKB" \
	'BEGIN { if (b > 0) printf "%.1f", a / b; else print 0 }')
printf '峰值内存比（query / each）= %s\n\n' "$RATIO"

check_true "流式峰值内存低于物化的三分之一" \
	"$(awk -v r="$RATIO" 'BEGIN { print (r >= 3) ? 1 : 0 }')" "比值只有 $RATIO"
check_true "流式峰值内存低于 32 MiB（与 100 万行无关）" \
	"$(awk -v b="$EKB" 'BEGIN { print (b < 32768) ? 1 : 0 }')" "实际 $EKB KiB"

"$CLI" exec -q "DROP TABLE IF EXISTS $TBL" >/dev/null 2>&1

printf '\n检查项 %d，失败 %d\n' "$CHECKS" "$FAILS"
[ "$FAILS" -eq 0 ] || exit 1
exit 0
