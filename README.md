# cpp-systems-programming

Linux 系统编程练习合集。每个主题一个编号目录，包含源码、说明文档和实测结论。

## 目录

| 编号 | 主题 | 关键概念 | 状态 |
|---|---|---|---|
| 01 | 并发计数 | 竞态条件、互斥锁、自旋锁、CPU 原子指令 | 进行中 |

后续主题按编号顺序追加。

## 环境要求

所有代码面向 Linux，依赖以下特性：

- POSIX 线程库（`pthread.h`）
- GCC 内联汇编语法（`__asm__ volatile`）
- Linux 系统调用

在 Windows 上无法直接编译。可选方案：WSL、虚拟机、或任意 Linux 服务器。

编译命令示例：

```bash
gcc 01-concurrent-lock/lock.c -o 01-concurrent-lock/bin/lock -lpthread
```

## 仓库约定

1. 每个主题一个目录，命名格式为 `NN-主题名`，`NN` 是两位编号（01、02 …）。
   两位编号能让目录在文件管理器中始终按顺序排列。
2. 编译产物统一输出到各目录下的 `bin/`，`bin/` 已被 `.gitignore` 忽略。
3. 每个主题目录内必须有一份 `README.md`，写清背景、结论和运行方式。
4. 每次提交只描述一件事，提交说明用中文写清"做了什么"。

## 日常提交流程

```bash
git status                      # 1. 看有哪些改动
git add .                       # 2. 把改动放进暂存区
git commit -m "描述这次改动"     # 3. 打包成一次提交
git push                        # 4. 推送到 GitHub
```

## 进度

- [x] 01 并发计数
- [ ] 02 待补充
