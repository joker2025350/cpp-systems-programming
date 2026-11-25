#!/bin/sh
# ============================================================
# smoke.sh —— tcp-server 入库阶段能离线判定的部分
#
# 为什么只有这几项：这个程序真正要做的事是"把接入的连接挂到 epoll 上，
# 有数据就读出来打屏"。accept 与 epoll_ctl(ADD) 写在同一个 for 循环里，
# 读与关写在 else 分支里，中间没有任何能被单独调用的入口，也没有
# "给一段连接、断言我发出了什么事件"的路径。要测就得起真实对端、
# 真的发包——那既依赖网络，也会把"对端今天怎么连"变成断言的输入。
#
# 所以这里只钉住与网络无关的事实：二进制能起、缺参数时的退出码与输出、
# 重复调用一致、给了端口之后真的进入监听循环而不自己退出。最后一项
# 用端口 0（内核分配，永不冲突）+ timeout 观察，不连任何对端，
# 也不发包，因此仍然与网络无关。
#
# 用法：sh tests/smoke.sh bin/tcp-server
# 输出：ok / FAIL 逐项，末行"检查项 N，失败 M"；退出码 0 表示全过
# ============================================================

BIN=$1

if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
	printf 'FAIL 二进制不存在或不可执行：%s\n' "$BIN"
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

# 1. 参数缺失时必须以非 0 退出。原始代码是 `if (argc < 2) return -1;`，
#    从 main 返回 -1 的退出状态是 255。这里不钉死 255，只要求"非 0"，
#    因为第二阶段很可能把它改成打印用法并返回 2，那同样是正确的。
"$BIN" >/tmp/tcpsrv-smoke.out 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
	check "缺参数时退出码非 0（实际 $rc）" "nonzero" "nonzero"
else
	check "缺参数时退出码非 0" "0" "nonzero"
fi

# 2. 缺参数时的输出是 "Param Error"（printf 里带换行，命令替换会吃掉尾随换行）。
#    这一条是行为契约：它比"退出了"更强，能挡住"退出码对但什么都没说"
#    与"输出被顺手改成了别的字样"。
out=$(cat /tmp/tcpsrv-smoke.out)
check "缺参数时输出为 Param Error" "${out:-空}" "Param Error"

# 3. 同样的调用跑两次，退出码必须一致（没有随机行为混进参数检查这条路径）
"$BIN" >/dev/null 2>&1
rc2=$?
check "重复调用的退出码一致" "$rc2" "$rc"

# 4. 给了端口就要真的进入监听循环，不能自己退出。端口 0 让内核挑一个，
#    所以不会有"端口被占用导致 bind 失败"的偶发失败。timeout 用默认的
#    SIGTERM，退出码 124 表示"到点被杀"，也就是进程一直活着。
if command -v timeout >/dev/null 2>&1; then
	timeout 1 "$BIN" 0 >/tmp/tcpsrv-run.out 2>&1
	rrc=$?
	check "给端口后进入监听循环（timeout 124）" "$rrc" "124"
	# 5. 启动路径上不该有任何输出：accept 循环只在收到数据时打屏。
	#    这一项同时挡住 "Param Error" 被误打到成功路径上。
	rout=$(cat /tmp/tcpsrv-run.out)
	check "启动后无多余输出" "${rout:-空}" "空"
else
	printf 'skip  没有 timeout，跳过监听循环这两项\n'
fi

rm -f /tmp/tcpsrv-smoke.out /tmp/tcpsrv-run.out

printf '检查项 %d，失败 %d\n' "$total" "$bad"
[ "$bad" -eq 0 ]
