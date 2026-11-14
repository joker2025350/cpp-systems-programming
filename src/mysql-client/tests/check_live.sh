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
# 1b. 流式取数（each）：输出必须与 sql 逐字节相同
#
# 两个子命令都打印表头：列名由列名回调在交付第一行之前给出，
# 因此整份输出可以整体 cmp，不再需要"去掉表头再比"。
# NULL 打印成 NULL、空串打印成空字段这两条在流式路径上走的是另一段代码
# （bind_result + fetch_column），cmp 一并覆盖。
# ------------------------------------------------------------

"$CLI" sql -q "SELECT id, note FROM $TBL ORDER BY id" \
	> "$TMPD/sql.out" 2>/dev/null
check_exit "sql 取三行" 0 $?

"$CLI" each -q "SELECT id, note FROM $TBL ORDER BY id" \
	> "$TMPD/each.out" 2>/dev/null
check_exit "each 退出码 0" 0 $?

CHECKS=$((CHECKS + 1))
if cmp -s "$TMPD/sql.out" "$TMPD/each.out"; then
	printf 'ok    each 与 sql 的输出逐字节相同（含表头）\n'
else
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-46s 输出不同\n' "each 与 sql 的输出"
	diff "$TMPD/sql.out" "$TMPD/each.out" | head -6
fi

# 表头单独确认一次：cmp 通过也可能是两边都不打印表头
check_equal "each 的表头就是列名" \
	"$("$CLI" each -q "SELECT id, note FROM $TBL LIMIT 1" 2>/dev/null | head -1)" \
	"$(printf 'id\tnote')"

# 0 行的结果集：表头照打（元数据与行数无关），摘要报 0 行
check_equal "each 在 0 行时仍打印表头" \
	"$("$CLI" each -q "SELECT id, note FROM $TBL WHERE id < 0" 2>/dev/null)" \
	"$(printf 'id\tnote')"

ERR=$("$CLI" each -q "SELECT id, note FROM $TBL WHERE id < 0" 2>&1 >/dev/null)
check_contains "each 在 0 行时的摘要" "0 行" "$ERR"

ERR=$("$CLI" each -q "SELECT id FROM $TBL ORDER BY id" 2>&1 >/dev/null)
check_contains "each 的行数摘要走 stderr" "3 行" "$ERR"

# 行数上限：出口码仍是 0（主动停止不是失败），表头照打，只出一行数据
OUT=$("$CLI" each -q "SELECT id FROM $TBL ORDER BY id" -n 1 2>/dev/null)
check_equal "each -n 1 输出表头加一行" "$OUT" "$(printf 'id\n1')"

ERR=$("$CLI" each -q "SELECT id FROM $TBL ORDER BY id" -n 2 2>&1 >/dev/null)
check_contains "each -n 2 只交付两行" "2 行" "$ERR"

# 表达式列名：没有别名时服务器给的就是表达式原文
OUT=$("$CLI" each -q "SELECT 1 + 1" 2>/dev/null)
check_equal "each 打印表达式列名" "$OUT" "$(printf '1 + 1\n2')"

# UTF-8 列名按字节原样打印
OUT=$("$CLI" each -q "SELECT 1 AS \`睡蓝蕉\`" 2>/dev/null)
check_equal "each 打印 UTF-8 列名" "$OUT" "$(printf '睡蓝蕉\n1')"

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

# 覆盖已有文件时保留它原有的权限位。
#
# blob-get 的输出走 mc_file_write（临时文件 + rename），而 rename 换掉的是
# inode：新 inode 的权限是 0666 & ~umask，与目标文件原来的权限无关。
# 不显式复制就会出现"0600 的导出文件被写一次变成 0664"这种静默放宽，
# 而退出码仍然是 0。这一条在库层有单元测试，这里验的是驱动层真的走过去了。
: > "$TMPD/perm.bin"
chmod 600 "$TMPD/perm.bin"
"$CLI" blob-get -q "SELECT u_img FROM $TBL WHERE id=1" \
	-o "$TMPD/perm.bin" >/dev/null 2>&1
check_exit "blob-get 覆盖已存在的 0600 文件" 0 $?
check_equal "覆盖后权限位仍是 600" "$(stat -c %a "$TMPD/perm.bin")" "600"

# ------------------------------------------------------------
# 2b. 参数绑定（exec-params）
#
# 与 2. 的 BLOB 用例分工：那里验的是"单个 ? 参数写大字段"（blob-put，
# 走 mc_blob_write），这里验的是多参数、多类型混排以及 -a 的解析。
# 判据尽量用"值原样取回"，而不是只看退出码——参数绑错时退出码照样是 0，
# 只有取回的值能区分。
# ------------------------------------------------------------

"$CLI" exec-params -q "INSERT INTO $TBL(id, note) VALUES(?,?)" \
	-a int:100 -a "text:it's ok" >/dev/null 2>&1
check_exit "exec-params 插入含引号的文本" 0 $?
check_equal "含引号的文本原样取回（不需要写 ''）" \
	"$("$CLI" sql -q "SELECT note FROM $TBL WHERE id=100" 2>/dev/null | tail -1)" \
	"it's ok"

# 同一份内容分别走 exec（字面量，需要 '' 转义）与 exec-params，
# 两行必须完全一样：这条把"参数化与字面量写入等价"钉死。
"$CLI" exec -q "INSERT INTO $TBL(id, note) VALUES (101, 'it''s ok')" >/dev/null 2>&1
check_equal "字面量与参数绑定写入同一个值" \
	"$("$CLI" sql -q "SELECT note FROM $TBL WHERE id=101" 2>/dev/null | tail -1)" \
	"$("$CLI" sql -q "SELECT note FROM $TBL WHERE id=100" 2>/dev/null | tail -1)"

OUT=$("$CLI" exec-params -q "UPDATE $TBL SET note=? WHERE id=100" \
	-a "text:a b	c" 2>/dev/null)
check_equal "exec-params 把受影响行数打到 stdout" "$OUT" "1"
check_equal "制表符也按值传递" \
	"$("$CLI" sql -q "SELECT note FROM $TBL WHERE id=100" 2>/dev/null | tail -1)" \
	"a b	c"

"$CLI" exec-params -q "UPDATE $TBL SET note=? WHERE id=100" -a null \
	>/dev/null 2>&1
check_exit "写 NULL 参数" 0 $?
check_equal "NULL 参数写成 SQL NULL 而不是空串" \
	"$("$CLI" sql -q "SELECT note FROM $TBL WHERE id=100" 2>/dev/null | tail -1)" \
	"NULL"

# 无符号 64 位上界：这一条同时钉住"有符号/无符号"两边的方向。
# 若 is_unsigned 的处理反了，-1 会被写成 18446744073709551615，
# 或者 18446744073709551615 报 out of range。
"$CLI" exec-params -q "UPDATE $TBL SET note=? WHERE id=100" \
	-a uint:18446744073709551615 >/dev/null 2>&1
check_exit "写 uint 上界" 0 $?
check_equal "uint 上界取回" \
	"$("$CLI" sql -q "SELECT note FROM $TBL WHERE id=100" 2>/dev/null | tail -1)" \
	"18446744073709551615"

"$CLI" exec-params -q "UPDATE $TBL SET note=? WHERE id=100" -a int:-42 \
	>/dev/null 2>&1
check_exit "写有符号负数" 0 $?
check_equal "有符号负数取回" \
	"$("$CLI" sql -q "SELECT note FROM $TBL WHERE id=100" 2>/dev/null | tail -1)" \
	"-42"

# 大字段：64 KiB 远大于 MC_BLOB_CHUNK（16 KiB），走 send_long_data
dd if=/dev/urandom of="$TMPD/p64k.bin" bs=1024 count=64 2>/dev/null
"$CLI" exec-params -q "INSERT INTO $TBL(id, u_img) VALUES(?,?)" \
	-a int:102 -a "blob:@$TMPD/p64k.bin" >/dev/null 2>&1
check_exit "exec-params 写入 64 KiB BLOB" 0 $?
"$CLI" blob-get -q "SELECT u_img FROM $TBL WHERE id=102" \
	-o "$TMPD/p64k.out" >/dev/null 2>&1
CHECKS=$((CHECKS + 1))
if cmp -s "$TMPD/p64k.bin" "$TMPD/p64k.out"; then
	printf 'ok    exec-params 的长字段往返逐字节相同\n'
else
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-46s 字节不一致\n' "exec-params 的长字段往返"
fi

# 零字节：与"没给参数"是两件事
: > "$TMPD/pzero.bin"
"$CLI" exec-params -q "INSERT INTO $TBL(id, u_img) VALUES(?,?)" \
	-a int:103 -a "blob:@$TMPD/pzero.bin" >/dev/null 2>&1
check_exit "exec-params 写零字节 BLOB" 0 $?
check_equal "零字节 BLOB 的长度是 0" \
	"$("$CLI" sql -q "SELECT LENGTH(u_img) FROM $TBL WHERE id=103" 2>/dev/null | tail -1)" \
	"0"

# ---- 失败路径：都必须在连库之前或执行之前给出可读的原因 ----

OUT=$("$CLI" exec-params -q "INSERT INTO $TBL(id, note) VALUES(?,?)" \
	-a int:200 2>&1)
check_exit "占位符多于参数时退出码 1" 1 $?
check_contains "占位符个数不符的原因" "placeholder" "$OUT"

"$CLI" exec-params -q "SELECT ?" -a int:1 >"$TMPD/params.out" 2>"$TMPD/params.err"
check_exit "把 SELECT 交给 exec-params 时退出码 1" 1 $?
check_contains "提示改用可返回结果集的接口" "mc_query" "$(cat "$TMPD/params.err")"
CHECKS=$((CHECKS + 1))
if [ -s "$TMPD/params.out" ]; then
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-46s 失败时 stdout 仍输出内容\n' "失败时 stdout 为空"
else
	printf 'ok    失败时 stdout 为空\n'
fi

OUT=$("$CLI" sql -q "SELECT 1" -a int:1 2>&1)
check_exit "-a 用于 sql 时退出码 2" 2 $?
check_contains "-a 只用于 exec-params" "exec-params" "$OUT"

OUT=$("$CLI" exec-params -q "SELECT 1" -a bogus:1 2>&1)
check_exit "未知参数类型退出码 2" 2 $?
check_contains "未知类型列出可用类型" "int" "$OUT"

OUT=$("$CLI" exec-params -q "SELECT 1" -a uint:-1 2>&1)
check_exit "uint 负数退出码 2" 2 $?
check_contains "uint 负数给出解释" "负数" "$OUT"

OUT=$("$CLI" exec-params -q "SELECT 1" -a blob:nope 2>&1)
check_exit "blob 缺 @ 退出码 2" 2 $?
check_contains "blob 的写法提示" "@" "$OUT"

# ------------------------------------------------------------
# 2c. 语句对象（stmt）：prepare 一次、执行多次
#
# 与 exec-params 的差别只有 prepare 的次数，因此最有力的判据是
# "同一份结果用 stmt 与 sql 打印出来的字节完全一致"——一旦语句对象的
# 取数路径（二进制协议、逐行读）在 NULL / 空串 / 表头上与文本协议
# 有一点不同，cmp 就会红。
# ------------------------------------------------------------

"$CLI" stmt -q "SELECT ? AS a, ? AS b, NULL AS c" \
	-a int:-7 -a text:xyz > "$TMPD/stmt.out" 2>/dev/null
check_exit "stmt 带参数取数" 0 $?

"$CLI" sql -q "SELECT -7 AS a, 'xyz' AS b, NULL AS c" \
	> "$TMPD/sql_ref.out" 2>/dev/null
check_exit "sql 取同一结果" 0 $?

CHECKS=$((CHECKS + 1))
if cmp -s "$TMPD/stmt.out" "$TMPD/sql_ref.out"; then
	printf 'ok    %s\n' "stmt 与 sql 的输出逐字节相同"
else
	FAILS=$((FAILS + 1))
	printf 'FAIL  %-46s 两份输出不同\n' "stmt 与 sql 的输出逐字节相同"
	diff "$TMPD/sql_ref.out" "$TMPD/stmt.out" | head -5
fi

# 无参数语句也走同一条路径
check_equal "stmt 执行无参数语句" \
	"$("$CLI" stmt -q "SELECT 1+1 AS v" 2>/dev/null | tail -1)" "2"

# 空结果集：表头照打，一行数据也没有
check_equal "stmt 空结果集只打印表头" \
	"$("$CLI" stmt -q "SELECT note FROM $TBL WHERE 1=0" 2>/dev/null)" "note"

# 复用：同一条 INSERT 执行 5 次，表里应当多 5 行。
# 这条是"复用"唯一无法伪造的判据——只 prepare 一次却只执行一次，
# 行数不会变；执行了 5 次但每次都重新 prepare，行数也会变，
# 所以行数只管"执行了几次"，prepare 的次数由 6.4 的计时回答。
BEFORE=$("$CLI" sql -q "SELECT COUNT(*) FROM $TBL" 2>/dev/null | tail -1)
OUT=$("$CLI" stmt -q "INSERT INTO $TBL(note) VALUES(?)" -a text:reuse -r 5 \
	2>&1 >/dev/null)
check_exit "stmt -r 5 写入" 0 $?
check_contains "摘要报告执行次数" "5 次执行" "$OUT"
AFTER=$("$CLI" sql -q "SELECT COUNT(*) FROM $TBL" 2>/dev/null | tail -1)
check_equal "表里多了 5 行" "$AFTER" "$((BEFORE + 5))"

# 复用 + 结果集：3 次执行应当打印 3 份表头与 3 份数据。
# 这条同时验证"上一次的结果集没读完就不能再执行"这个协议约束
# 在驱动层是被处理过的（每次执行后都取尽了）。
OUT=$("$CLI" stmt -q "SELECT ? AS v" -a int:9 -r 3 2>/dev/null)
check_equal "SELECT -r 3 打印 3 份表头与 3 行数据" \
	"$(printf '%s\n' "$OUT" | wc -l)" "6"

# 参数个数不符：语句已经 prepare 成功，错在 execute 之前。
# 用 UPDATE 而不是 INSERT：INSERT 的"占位符个数与列数不一致"会被服务端
# 在 prepare 阶段就以 "Column count doesn't match value count" 拒掉，
# 那样测到的是服务端的语法检查，不是本模块的占位符核对。
OUT=$("$CLI" stmt -q "UPDATE $TBL SET note=? WHERE id=?" -a text:x 2>&1)
check_exit "占位符多于参数时退出码 1" 1 $?
check_contains "语句对象也报占位符个数" "placeholder" "$OUT"

# -r 只对 stmt 有意义
OUT=$("$CLI" sql -q "SELECT 1" -r 2 2>&1)
check_exit "-r 用于 sql 时退出码 2" 2 $?
check_contains "-r 只用于 stmt" "stmt" "$OUT"

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
