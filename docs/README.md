# 项目笔记

这里放的是**代码本身说不出来的东西**：为什么这么设计、哪些结论是实测过的、哪些坑踩过、哪些事情故意没做。

代码结构、改动历史、修过什么 bug —— 这些 git 和源码自己会说，不要往这里抄。

## 索引

| 文件 | 什么时候看 |
|---|---|
| [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) | 动手改任何东西之前 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 找不到某个仓库/文件在哪；要改跨仓共享的东西 |
| [HARDWARE-FACTS.md](HARDWARE-FACTS.md) | 碰引脚、串口、启动模式、SRAM4 之前 |
| [BUILD-AND-TEST.md](BUILD-AND-TEST.md) | 要编译、要测、或者在查一个时有时无的问题 |
| [IAP-STATUS.md](IAP-STATUS.md) | 想知道某条路径验没验过、还欠什么 |
| [DEFERRED-DESIGNS.md](DEFERRED-DESIGNS.md) | 有人问"为什么不做 X" |

## 维护约定

- **发现与现状不符就地改掉**，不要另起一份。这套文档 2026-08-16 从 17 个分散的笔记合并而来，合并时就抓到一处互相矛盾的地方（一边说某脚本还在用，另一边说它已经删了）—— 两份就会漂移，这是规律不是意外。
- **写事实和判断依据，不写流水账。** "PB10 拉低会关断整片 MAX3221（查 netlist.ipc 逐脚核实）"有用；"今天调试了三小时"没用。
- **没验证的结论要标出来。** 写"未验证"比写一个看起来很确定的猜测好得多。
- **日期要写。** 一条 2026-08 的实测结论，两年后读的人得能判断它还算不算数。

## 语言

这套文档用中文。**其他一切落到文件里的东西用英文** —— 代码注释、`#error`/`#warning` 文案、工具的 stdout/stderr、各仓库的 README。详见 [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md)。
