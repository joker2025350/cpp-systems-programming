#!/bin/sh
# ============================================================
# smoke.sh —— epoll-server 入库阶段能离线判定的部分
#
# 这一轮只能钉三类事实：
#   1. 两个二进制能起、缺参数时的退出码与输出、重复调用一致；
#   2. 服务端给了端口之后真的把 MAX_PORT(100) 个端口全部 bind + listen 上去，
#      而不是只 bind 了第一个；
#   3. 端口已被占住时第二个实例的失败路径（退出码 2 与 bind 的 stderr）。
#
# 这三类都不依赖外部网络：只在回环/本机内核表上 bind 与 listen，不 accept、
# 不 connect、不发一个字节。真正要测的东西现在测不了，理由：
#
#   - 服务端"一个事件读多少字节"取决于内核如何切分 TCP 段，必须起真实对端发包，
#     而 ET 的丢数据行为（README 第 1 条）是随机可复现的，不是确定性断言；
#   - 压测客户端的作用是"打满服务端"，它的每个动作都以对端存在为前提，
#     单独跑它只会得到 connect 失败。
#
# 两者都要等第二阶段拆出 include/ + lib*.a、把"注册一个连接""读一次"暴露成
# 可单独调用的函数之后，才能进 check。
#
# 用法：sh tests/smoke.sh <epoll-server> <mul-port-client>
# 输出：ok / FAIL / skip 逐项，末行"检查项 N，失败 M"；退出码 0 表示全过
# ============================================================

SRV=$1
CLI=$2

if [ -z "$SRV" ] || [ ! -x "$SRV" ]; then
	printf 'FAIL 服务端二进制不存在或不可执行：%s\n' "$SRV"
	printf '检查项 1，失败 1\n'
	exit 1
fi
if [ -z "$CLI" ] || [ ! -x "$CLI" ]; then
	printf 'FAIL 客户端二进制不存在或不可执行：%s\n' "$CLI"
	printf '检查项 1，失败 1\n'
	exit 1
fi

total=0
bad=0

check() {
	total=$((total + 1))
	if [ "$2" = "$3" ]; then
		printf 'ok    %s\n' "$1"
	else
		printf 'FAIL  %s（期望 %s，实际 %s）\n' "$1" "$3" "$2"
		bad=$((bad + 1))
	fi
}

TMP=${TMPDIR:-/tmp}/epollsrv-smoke.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# ------------------------------------------------------------
# 服务端：参数路径
# ------------------------------------------------------------

# 1. 缺参数时必须以非 0 退出。原始代码是 `if (argc < 2) { ... return -1; }`，
#    从 main 返回 -1 的退出状态是 255（POSIX 只取低 8 位）。这里不钉死 255，
#    只要求"非 0"：第二阶段很可能改成打印用法并返回 2，那同样是对的。
"$SRV" >"$TMP/s1.out" 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
	check "服务端缺参数时退出码非 0（实际 $rc）" nonzero nonzero
else
	check "服务端缺参数时退出码非 0" "0" nonzero
fi

# 2. 缺参数时的输出恰为 "Param Error"（命令替换会吃掉尾随换行）。
out=$(cat "$TMP/s1.out")
check "服务端缺参数时输出为 Param Error" "${out:-空}" "Param Error"

# 3. 同一调用跑两次，退出码必须一致。
"$SRV" >/dev/null 2>&1
rc2=$?
check "服务端重复调用的退出码一致" "$rc2" "$rc"

# ------------------------------------------------------------
# 服务端：真的把 100 个端口挂上去了
# ------------------------------------------------------------
#
# 基端口的取法：避开 1024 以下的特权端口（非 root 时 bind 会 EACCES，实测
# `./bin/epoll-server 0` 的第二行就是 "bind: Permission denied"——端口 0 拿到
# 的是临时端口，i=1 就撞到 1 号端口了），也避开临时端口段（32768 起），
# 再用 $$ 打散，避免并发跑两份 smoke 时互相踩。
BASE=$((20000 + $$ % 10000))

# 先确认这一段是空的：服务端要是立刻退出（rc != 124），说明 bind 失败，
# 换一段再试。不这样做，"端口被占"会被误报成"启动路径有问题"。
tries=0
while [ "$tries" -lt 4 ]; do
	timeout 0.3 "$SRV" "$BASE" >/dev/null 2>&1
	if [ "$?" -eq 124 ]; then
		break
	fi
	BASE=$((BASE + 200))
	tries=$((tries + 1))
done

# 4 + 5. 用 timeout 观察"一直活着"，并数启动路径上的 100 行输出。
#
# 为什么必须借 pty：stdout 指向管道/文件时是全缓冲，进程被 SIGTERM 杀掉时
# 缓冲区里的内容会整块丢掉，只能读到 0 行（2026-11-25 测 tcp-server 时踩过）。
# `script` 给子进程一个 pty，stdout 变成行缓冲，每一行 printf 立即写出。
if command -v script >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
	script -q -e -c "timeout 1 $SRV $BASE" /dev/null >"$TMP/run.out" 2>&1
	rrc=$?
	check "服务端给端口后进入监听循环（timeout 124）" "$rrc" "124"

	n=$(grep -c '^tcp server listen on port : ' "$TMP/run.out")
	check "100 个端口全部 bind+listen（启动输出 100 行，实际 $n）" "$n" "100"

	# 首尾两行的端口号必须是 BASE 与 BASE+99：中间任何一次 bind 失败都会让
	# 循环提前 return 2，行数不足；反过来，行数对但端口号错了说明 port+i 被
	# 改过（比如加了偏移）。
	# pty 会把行尾换成 CRLF，两个值都要先去掉 \r，否则比较时差一个看不见的字节。
	first=$(grep '^tcp server listen on port : ' "$TMP/run.out" | head -1 | sed 's/.*: //' | tr -d '\r')
	last=$(grep '^tcp server listen on port : ' "$TMP/run.out" | tail -1 | sed 's/.*: //' | tr -d '\r')
	check "首行端口为 $BASE、末行为 $((BASE + 99))" "$first/$last" "$BASE/$((BASE + 99))"
else
	printf 'skip  没有 script / timeout，跳过监听循环这三项\n'
fi

# 6. 端口已被占住时，第二个实例必须 bind 失败、退出码 2、stderr 带
#    "Address already in use"。这一项同时证明第一个实例真的占住了端口，
#    比"进程还活着"更强：活着也可能是卡在别处。
"$SRV" "$BASE" >/dev/null 2>&1 &
holder=$!
sleep 0.3
"$SRV" "$BASE" >"$TMP/dup.out" 2>&1
drc=$?
kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
check "端口被占时第二个实例退出码 2" "$drc" "2"
case "$(cat "$TMP/dup.out")" in
*"Address already in use"*) check "端口被占时 stderr 为 Address already in use" yes yes ;;
*) check "端口被占时 stderr 为 Address already in use" "$(cat "$TMP/dup.out")" "Address already in use" ;;
esac

# ------------------------------------------------------------
# 客户端：参数路径
# ------------------------------------------------------------

# 7. 缺参数时打印用法之后 `exit(0)`——退出码 0 表示成功。这是缺陷，
#    不是被认可的行为（README 客户端第 1 条）：调用方无法把"参数写错了"
#    和"跑完了"区分开。这里钉住的是**当前**行为，第二阶段改成非 0 时这条
#    检查要跟着改，标题里就写着"缺陷"。
"$CLI" >"$TMP/c1.out" 2>&1
crc=$?
check "客户端缺参数时退出码为 0（缺陷，见 README）" "$crc" "0"

# 8. 用法行的字面量：格式串是 "Usage: %s ip port\n"，%s 是 argv[0]。
cout=$(cat "$TMP/c1.out")
check "客户端缺参数时输出为用法行" "${cout:-空}" "Usage: $CLI ip port"

# 9. 重复调用一致。
"$CLI" >/dev/null 2>&1
crc2=$?
check "客户端重复调用的退出码一致" "$crc2" "$crc"

printf '检查项 %d，失败 %d\n' "$total" "$bad"
[ "$bad" -eq 0 ]
