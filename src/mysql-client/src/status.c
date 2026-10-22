/*
 * status.c —— 状态码到可读描述的映射
 *
 * 单独成一个编译单元，是为了让"纯逻辑"这一层完全不引用 MySQL 客户端库：
 * 只链接 libmc.a 里 status.o / options.o / file.o 的测试程序，
 * 不需要 libmysqlclient.so，也不需要一台运行中的服务器。
 * 链接器按需抽取静态库成员，没有引用就不会拉进 mysqlclient.o，
 * 那些 mysql_* 符号也就不会成为未定义符号。
 */

#include "mysqlclient.h"

const char *mc_status_string(mc_status status) {

	switch (status) {
	case MC_OK:             return "ok";
	case MC_ERR_PARAM:      return "invalid parameter";
	case MC_ERR_NOMEM:      return "out of memory";
	case MC_ERR_FILE_OPEN:  return "cannot open file";
	case MC_ERR_FILE_READ:  return "read error";
	case MC_ERR_FILE_WRITE: return "write error";
	case MC_ERR_CONNECT:    return "cannot connect";
	case MC_ERR_QUERY:      return "query failed";
	case MC_ERR_RESULT:     return "unexpected result set";
	case MC_ERR_STMT:       return "statement failed";
	case MC_ERR_TOOBIG:     return "data larger than buffer";
	case MC_ERR_NODATA:     return "no data";
	}

	/*
	 * 不给 default 分支，让 gcc 的 -Wswitch 在新增枚举值却漏写描述时报警。
	 * 能走到这里只可能是调用方把整数强转成了 mc_status。
	 */
	return "unknown status";
}
