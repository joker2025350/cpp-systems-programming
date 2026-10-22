# ============================================================
# 顶层构建入口
#
# 自动发现 src/ 下所有带 Makefile 的模块并转发目标，
# 新增模块只要它自己有 Makefile，这里不用改。
#
#   make            构建全部模块
#   make test       跑全部模块的单元测试
#   make check      跑全部模块的端到端检查
#   make sanitize   全部模块用 ASan + UBSan 重建并跑测试
#   make bench      跑全部模块的基准
#   make clean      清理全部模块
#   make list       列出被识别的模块
#
# 唯一的例外是 src/mysql-client：它依赖系统里的 libmysqlclient，
# 找不到头文件时就无法编译。这类机器上用
#   make SKIP=mysql-client
# 跳过它，其余模块照常构建。SKIP 接受空格分隔的多个模块名。
# ============================================================

ALL_MODULES := $(patsubst src/%/Makefile,%,$(wildcard src/*/Makefile))
MODULES     := $(filter-out $(SKIP),$(ALL_MODULES))

.PHONY: all test check sanitize bench clean list

all:
	@for m in $(MODULES); do \
		printf '\n==> %s\n' "$$m"; \
		$(MAKE) -C src/$$m || exit 1; \
	done
	@printf '\n完成：%s\n' "$(MODULES)"

test:
	@for m in $(MODULES); do \
		printf '\n==> %s\n' "$$m"; \
		$(MAKE) -C src/$$m test || exit 1; \
	done

check:
	@for m in $(MODULES); do \
		if grep -q '^check:' src/$$m/Makefile 2>/dev/null; then \
			printf '\n==> %s\n' "$$m"; \
			$(MAKE) -C src/$$m check || exit 1; \
		fi; \
	done

sanitize:
	@for m in $(MODULES); do \
		printf '\n==> %s\n' "$$m"; \
		$(MAKE) -C src/$$m sanitize || exit 1; \
	done

bench:
	@for m in $(MODULES); do \
		if grep -q '^bench:' src/$$m/Makefile 2>/dev/null; then \
			printf '\n==> %s\n' "$$m"; \
			$(MAKE) -C src/$$m bench || exit 1; \
		fi; \
	done

clean:
	@for m in $(MODULES); do \
		printf '\n==> %s\n' "$$m"; \
		$(MAKE) -C src/$$m clean || exit 1; \
	done

list:
	@printf '识别的模块：\n'
	@for m in $(ALL_MODULES); do \
		case " $(SKIP) " in \
		*" $$m "*) printf '  %-16s (SKIP)\n' "$$m" ;; \
		*)         printf '  %s\n' "$$m" ;; \
		esac; \
	done
