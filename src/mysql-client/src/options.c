/*
 * options.c —— 连接参数：初始化与校验
 *
 * mc_options_check 的职责是"在动网络之前把明显不合法的参数挡掉"。
 * 理由很实际：mysql_real_connect 拿到空主机名会去连本机默认 socket，
 * 拿到端口 0 会回退到 3306，这些回退都是库的行为而不是调用方的意图，
 * 排查时看到的却是一条"连接被拒绝"，指向完全错误的方向。
 * 参数错误应当以参数错误的形式报出来（MC_ERR_PARAM），不要混进连接失败。
 */

#include "mysqlclient.h"

#include <stddef.h>

/* 默认端口。3306 是 MySQL 的注册端口，写在这里而不是逐个调用方各写一遍。 */
#define MC_DEFAULT_PORT 3306

/*
 * 有界求长：最多看 cap 个字节，遇到 '\0' 提前结束。
 *
 * 不用 strnlen 是为了不依赖 _POSIX_C_SOURCE：-std=c11 定义 __STRICT_ANSI__，
 * glibc 会把 strnlen 的声明藏起来，函数退化成隐式声明（C11 6.5.2.2 规定
 * 隐式声明已不再是合法形式，gcc 默认仍会容忍并在 -Wimplicit-function-declaration
 * 里报警）。八行循环换掉一个特性测试宏，值得。
 */
static size_t bounded_len(const char *s, size_t cap) {
	size_t n = 0;

	while (n < cap && s[n] != '\0') n++;
	return n;
}

void mc_options_init(mc_options *opt) {
	if (opt == NULL) return;

	opt->host[0]     = '\0';
	opt->user[0]     = '\0';
	opt->password[0] = '\0';
	opt->database[0] = '\0';
	opt->port            = MC_DEFAULT_PORT;
	opt->connect_timeout = 0;

	/*
	 * 不用 memset 全零：port 需要 3306 这个非零默认值，
	 * 而 memset 之后还得再赋一次，两处赋值容易漏掉一处。
	 * 其余字节由结构体定义保证（都是数组或已赋值的标量），
	 * 不存在未初始化的填充字节被读到的路径。
	 */
}

mc_status mc_options_check(const mc_options *opt) {

	if (opt == NULL) return MC_ERR_PARAM;

	/*
	 * 每个字段都检查两件事：非空，且在容量内以 '\0' 结尾。
	 * 第二条不是多余的：调用方可能用 memcpy 填满整个数组而不写结尾符，
	 * 这时 strlen 会一路读出去。bounded_len 返回 cap 就等于"没有结尾符"，
	 * 必须拒绝——把它交给 mysql_real_connect 就是把一个越界读交给了库。
	 */
	if (bounded_len(opt->host, MC_HOST_MAX) == 0) return MC_ERR_PARAM;
	if (bounded_len(opt->host, MC_HOST_MAX) == MC_HOST_MAX) return MC_ERR_PARAM;

	if (bounded_len(opt->user, MC_USER_MAX) == 0) return MC_ERR_PARAM;
	if (bounded_len(opt->user, MC_USER_MAX) == MC_USER_MAX) return MC_ERR_PARAM;

	if (bounded_len(opt->password, MC_PASSWORD_MAX) == MC_PASSWORD_MAX) {
		return MC_ERR_PARAM;
	}
	if (bounded_len(opt->database, MC_DBNAME_MAX) == MC_DBNAME_MAX) {
		return MC_ERR_PARAM;
	}

	/*
	 * 端口范围。TCP 端口是 16 位无符号数（0..65535），0 在 connect(2) 里
	 * 是合法值但表示"由内核挑一个临时端口"，客户端库对它的处理是回退到
	 * 默认端口，两种解释都不是调用方想要的，因此 0 在这里被拒。
	 */
	if (opt->port == 0 || opt->port > 65535u) return MC_ERR_PARAM;

	return MC_OK;
}
