# docs

跨模块的设计笔记与配图。模块自己的技术细节在各模块的 `README.md` 里，
这里只放"不专属于某一个模块"或"以图为主"的材料。

## 配图

| 文件 | 内容 | 相关模块 |
|---|---|---|
| `images/concurrent-lock.png` | 四种计数方案的关系图 | [`src/concurrent-lock`](../src/concurrent-lock) |
| `images/thread-pool.png` | 线程池结构笔记 | [`src/thread-pool`](../src/thread-pool) |
| `images/mysql-client.png` | MySQL 调用流程笔记 | [`src/mysql-client`](../src/mysql-client) |

配图放在仓库根而不是模块目录里，是因为它们都是"笔记截图"，
不是模块的组成部分——模块删除或重构时，图片不必跟着搬。

## 约定

- 文件名用小写英文加连字符，与模块名保持一致。
- 引用路径写相对路径（`../../docs/images/x.png`），保证 GitHub 上能直接渲染。
- 图片只做辅助。任何一张图里表达的信息，在模块 README 的文字里都必须能找到，
  否则看图的人和读文字的人会得到不同的结论。
