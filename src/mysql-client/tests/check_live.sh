#!/bin/sh
# ============================================================
# check_live.sh —— 需要真实数据库的端到端往返验证
#
# 与 check_cli.sh 的分工：
#   check_cli.sh  不需要数据库，验证参数解析、退出码、错误路径；
#   check_live.sh 需要一个连得上的 MySQL/MariaDB，验证 BLOB 往返、
#                 结果集取回、NULL 与空串的区分这些"成功路径"。
#
# 单独成一个脚本而不是并进 make check，是因为它对外部环境有要求：
# 没有数据库的机器上跑必然全红，那种"永远失败的目标"等于没有目标。
#
# 用法：
#   MC_HOST=127.0.0.1 MC_PORT=3306 MC_USER=root MC_PASSWORD= \
#     sh tests/check_live.sh bin/mysqlclient
#
# 连接参数全部通过环境变量传给被测程序（与驱动层的默认值来源一致）。
# 需要 CREATE DATABASE 权限；脚本自建、自删一个测试库。
#
# 退出码：0 全部通过，1 有失败
# ============================================================

set -u

CLI=${1:-bin/mysqlclient}
DB=${MC_TEST_DB:-mc_live_db}
TBL="$DB.t_live"

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
	*"$2"*) printf 'ok    %s\n' "$1" ;;
	*)      FAILS=$((FAILS + 1)); printf 'FAIL  %-46s 没找到 "%s"\n' "$1" "$2" ;;
	esac
}

check_equal() {
	CHECKS=$((CHECKS + 1))
	if [ "$2" = "$3" ]; then
		printf 'ok    %s\n' "$1"
	else
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-46s 实际 "%s"，期望 "%s"\n' "$1" "$2" "$3"
	fi
}

# ------------------------------------------------------------
# 0. 连接
# ------------------------------------------------------------

OUT=$("$CLI" ping 2>&1)
check_exit "ping 成功" 0 $?

if [ "$FAILS" -ne 0 ]; then
	printf '\n连不上数据库，后续用例跳过：%s\n' "$OUT" >&2
	printf '\n检查项 %d，失败 %d\n' "$CHECKS" "$FAILS"
	exit 1
fi

"$CLI" exec -q "DROP TABLE IF EXISTS $TBL" >/dev/null 2>&1
OUT=$("$CLI" exec -q "CREATE DATABASE IF NOT EXISTS $DB" 2>&1)
check_exit "建库" 0 $?

"$CLI" exec -q "CREATE TABLE $TBL (
	id INT AUTO_INCREMENT PRIMARY KEY,
	note VARCHAR(64),
	u_img LONGBLOB
)" >/dev/null 2>&1
check_exit "建表" 0 $?

# ------------------------------------------------------------
# 1. 文本与 NULL 的区分
#
# 三行：中文、空串、NULL。sql 子命令必须把后两者打印成不同的东西，
# 否则 "a\t\tc" 无法判断中间那一列到底是空串还是没有值。
# ------------------------------------------------------------

"$CLI" exec -q "INSERT INTO $TBL(note) VALUES ('睡蓝蕉')" >/dev/null
check_exit "插入 UTF-8 文本" 0 $?
"$CLI" exec -q "INSERT INTO $TBL(note) VALUES ('')" >/dev/null
"$CLI" exec -q "INSERT INTO $TBL(note) VALUES (NULL)" >/dev/null

OUT=$("$CLI" sql -q "SELECT note FROM $TBL ORDER BY id" 2>/dev/null)
check_contains "sql 打印 UTF-8 文本原样" "睡蓝蕉" "$OUT"
check_contains "空串打印为空字段" "$(printf 'note\n睡蓝蕉\n\nNULL')" "$OUT"
check_contains "SQL NULL 打印成 NULL" "NULL" "$OUT"

ERR=$("$CLI" sql -q "SELECT note FROM $TBL ORDER BY id" 2>&1 >/dev/null)
check_contains "行数摘要走 stderr" "3 行" "$ERR"

# 列名也来自服务器
OUT=$("$CLI" sql -q "SELECT id, note FROM $TBL LIMIT 1" 2>/dev/null)
check_contains "结果集带列名 id" "id" "$OUT"
check_contains "结果集带列名 note" "note" "$OUT"

# ------------------------------------------------------------
# 1b. 流式取数（each）：行内容必须与 sql 逐字节相同
#
# sql 多打印一行表头（列名），each 的回调只拿到行内容，
# 因此比对时去掉表头。除此之外必须完全一致：NULL 打印成 NULL、
# 空串打印成空字段——这两条在流式路径上走的是另一段代码
# （bind_result + fetch_column），不能拿 sql 的输出代之。
# ------------------------------------------------------------

"$CLI" sql -q "SELECT id, note FROM $TBL ORDER BY id" \
	> "$TMPD/sql.out" 2>/dev/null
check_exit "sql 取三行" 0 $?

"$CLI" each -q "SELECT id, note FROM $TBL ORDER BY id" \
	> "$TMPD/each.out" 2>/dev/null
check_exit "each 退出码 0" 0 $?

tail -n +2 "$TMPD/sql.out" > "$TMPD/sql.body"
CHECKS=$((CHECKS + 1))
if cmp -s "$TMPD/sql.body" "$TMPD/each.out"; then
	printf 'ok    each 与 sql 的行内容逐字节相同\n'
else
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-46s 输出不同\n' "each 与 sql 的行内容"
	diff "$TMPD/sql.body" "$TMPD/each.out" | head -6
fi

ERR=$("$CLI" each -q "SELECT id FROM $TBL ORDER BY id" 2>&1 >/dev/null)
check_contains "each 的行数摘要走 stderr" "3 行" "$ERR"

# 行数上限：出口码仍是 0（主动停止不是失败），stderr 报告的交付行数受限
OUT=$("$CLI" each -q "SELECT id FROM $TBL ORDER BY id" -n 1 2>/dev/null)
check_equal "each -n 1 只输出一行" "$OUT" "1"

ERR=$("$CLI" each -q "SELECT id FROM $TBL ORDER BY id" -n 2 2>&1 >/dev/null)
check_contains "each -n 2 只交付两行" "2 行" "$ERR"

# 不返回结果集的语句
OUT=$("$CLI" each -q "UPDATE $TBL SET note='x' WHERE id=999" 2>&1)
check_exit "each 收到无结果集语句时退出码 1" 1 $?
check_contains "each 提示改用 mc_exec" "mc_exec" "$OUT"

# 语法错误在 prepare 阶段被拒
OUT=$("$CLI" each -q "SELECT FROM" 2>&1)
check_exit "each 语法错误退出码 1" 1 $?
check_contains "each 语法错误报 prepare" "prepare" "$OUT"

# ------------------------------------------------------------
# 2. BLOB 往返：三种长度
# ------------------------------------------------------------

dd if=/dev/urandom of="$TMPD/blob64k.bin" bs=1024 count=64 2>/dev/null
dd if=/dev/urandom of="$TMPD/blob5m.bin" bs=1048576 count=5 2>/dev/null
: > "$TMPD/zero.bin"

for f in blob64k blob5m zero; do
	"$CLI" blob-put -q "UPDATE $TBL SET u_img=? WHERE id=1" \
		-f "$TMPD/$f.bin" >/dev/null 2>&1
	check_exit "blob-put $f" 0 $?

	"$CLI" blob-get -q "SELECT u_img FROM $TBL WHERE id=1" \
		-o "$TMPD/$f.out" >/dev/null 2>&1
	check_exit "blob-get $f" 0 $?

	CHECKS=$((CHECKS + 1))
	if cmp -s "$TMPD/$f.bin" "$TMPD/$f.out"; then
		printf 'ok    %s 往返逐字节相同\n' "$f"
	else
		FAILS=$((FAILS + 1))
		printf 'FAIL  %-46s 字节不一致\n' "$f 往返"
	fi
done

# ------------------------------------------------------------
# 3. 失败路径（需要服务器才能触发的那几种）
# ------------------------------------------------------------

# 容量不足：必须报 TOOBIG 并给出确切字节数，而不是截断
#
# 先把 5 MB 那份重新写回去。上面那轮往返的最后一次写的是零字节文件，
# 字段此时是空的；不重置的话，1000 字节的容量当然"够用"，
# 这条用例会在什么都没验证的情况下通过。
"$CLI" blob-put -q "UPDATE $TBL SET u_img=? WHERE id=1" \
	-f "$TMPD/blob5m.bin" >/dev/null 2>&1
check_exit "容量测试前重置为 5 MB" 0 $?

OUT=$("$CLI" -s 1000 blob-get -q "SELECT u_img FROM $TBL WHERE id=1" \
	-o "$TMPD/too_small.bin" 2>&1)
check_exit "容量不足退出码 1" 1 $?
check_contains "容量不足报字节数" "5242880 bytes" "$OUT"
CHECKS=$((CHECKS + 1))
if [ -e "$TMPD/too_small.bin" ]; then
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-46s 失败时仍写出了文件\n' "容量不足不产生输出文件"
else
	printf 'ok    容量不足不产生输出文件\n'
fi

# 多行：单值接口必须拒绝，而不是悄悄只用最后一行
OUT=$("$CLI" blob-get -q "SELECT u_img FROM $TBL" -o "$TMPD/multi.bin" 2>&1)
check_exit "多行结果退出码 1" 1 $?
check_contains "多行结果报 more than one row" "more than one row" "$OUT"

# 该列为 NULL：与"查不到行"是两件事
OUT=$("$CLI" blob-get -q "SELECT u_img FROM $TBL WHERE id=3" -o "$TMPD/null.bin" 2>&1)
check_exit "NULL 列退出码 1" 1 $?
check_contains "NULL 列报 SQL NULL" "SQL NULL" "$OUT"

# 查不到行
OUT=$("$CLI" blob-get -q "SELECT u_img FROM $TBL WHERE id=999" -o "$TMPD/none.bin" 2>&1)
check_exit "无行退出码 1" 1 $?
check_contains "无行报 query returned no row" "no row" "$OUT"

# 语句错误：表不存在
OUT=$("$CLI" sql -q "SELECT * FROM $DB.no_such_table" 2>&1)
check_exit "表不存在退出码 1" 1 $?
check_contains "表不存在时报 query" "query" "$OUT"

# 用 sql 执行 INSERT：没有结果集，必须提示改用 exec
OUT=$("$CLI" sql -q "UPDATE $TBL SET note='x' WHERE id=999" 2>&1)
check_exit "sql 收到无结果集语句时退出码 1" 1 $?
check_contains "提示改用 mc_exec" "mc_exec" "$OUT"

# exec 报告受影响行数
OUT=$("$CLI" exec -q "UPDATE $TBL SET note='y' WHERE id=1" 2>&1)
check_exit "exec 更新退出码 0" 0 $?
check_equal "exec 报告受影响 1 行" "$OUT" "1"

# 参数绑定：SQL 里带引号的文本不能被当成语法
OUT=$("$CLI" exec -q "INSERT INTO $TBL(note) VALUES ('a''b')" 2>&1)
check_exit "插入含单引号的文本" 0 $?
OUT=$("$CLI" sql -q "SELECT note FROM $TBL WHERE note='a''b'" 2>/dev/null)
check_contains "含单引号的文本原样取回" "a'b" "$OUT"

# ------------------------------------------------------------
# 4. 清理
# ------------------------------------------------------------

"$CLI" exec -q "DROP TABLE IF EXISTS $TBL" >/dev/null 2>&1
check_exit "删除测试表" 0 $?

printf '\n检查项 %d，失败 %d\n' "$CHECKS" "$FAILS"
[ "$FAILS" -eq 0 ] || exit 1
exit 0
