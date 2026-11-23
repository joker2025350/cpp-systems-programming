#!/bin/sh
# ============================================================
# smoke.sh —— dns-client 入库阶段能离线判定的部分
#
# 为什么只有这几项：真正要测的是"发出去的报文对不对"和"应答解析对不对"，
# 而这两件事现在都躲在 dns_client_commit 里面，没有一个能被单独调用的入口，
# 也没有"从文件读一份应答再解析"的路径。要测就得连真实 DNS 服务器，
# 那既依赖网络，也会把"服务器今天回什么"变成断言的输入。
#
# 所以这里只钉住三件与网络无关的事实：二进制能起、缺参数时不静默成功、
# 缺参数时不输出任何东西。真正的报文级用例要等第二阶段把
# dns_build_request / dns_parse_response 暴露成可单独调用的函数之后再加。
#
# 用法：sh tests/smoke.sh bin/dns-client
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
"$BIN" >/tmp/dnsclient-smoke.out 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
	check "缺参数时退出码非 0（实际 $rc）" "nonzero" "nonzero"
else
	check "缺参数时退出码非 0" "0" "nonzero"
fi

# 2. 缺参数时不该有任何输出：静默失败与"打印了半句再失败"是两种不同的行为，
#    后者在脚本里调用时会污染下游。
out=$(cat /tmp/dnsclient-smoke.out)
check "缺参数时无输出" "${out:-空}" "空"

# 3. 同样的调用跑两次，退出码必须一致（没有随机行为混进参数检查这条路径）
"$BIN" >/dev/null 2>&1
rc2=$?
check "重复调用的退出码一致" "$rc2" "$rc"

rm -f /tmp/dnsclient-smoke.out

printf '检查项 %d，失败 %d\n' "$total" "$bad"
[ "$bad" -eq 0 ]
