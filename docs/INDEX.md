# 项目笔记

这里放的是**代码本身说不出来的东西**：为什么这么设计、哪些结论是实测过的、哪些坑踩过、哪些事情故意没做。

代码结构、改动历史、修过什么 bug —— 这些 git 和源码自己会说，不要往这里抄。

## 命名约定

**文件名要说出内容或用途。** 2026-08-17 之前这里有 5 个 `README.md`，光看名字分不出哪个是索引、哪个是入口、哪个是待办 —— 已全部改名。**只有仓库根目录的 `README.md` 保留**，那是仓库门面，约定俗成。

四份核心文档回答四个互不重叠的问题：

| 问题 | 文件 |
|---|---|
| **要做到什么**，到哪一步了 | [handover/REQUIREMENTS.md](handover/REQUIREMENTS.md) |
| **怎么证明**做到了 | [handover/TEST-PLAN.md](handover/TEST-PLAN.md) |
| **还欠什么**，怎么做 | [handover/Todo/BACKLOG.md](handover/Todo/BACKLOG.md) |
| **怎么开工、怎么收尾** | [handover/SESSION-START.md](handover/SESSION-START.md) |

## 索引

| 文件 | 什么时候看 |
|---|---|
| **[../CLAUDE.md](../CLAUDE.md)** | **最外层入口，Claude Code 自动加载。** 换台机器要 clone 什么、装什么、配什么、哪些东西不在 git 里。四个仓库各有一份，都指回这里 |
| **[handover/SESSION-START.md](handover/SESSION-START.md)** | 开工顺序与**收尾流程**。从这里进 [handover/](handover/)：需求清单、测试覆盖矩阵、待做模块的设计 |
| [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) | 动手改任何东西之前 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 找不到某个仓库/文件在哪；要改跨仓共享的东西 |
| [HARDWARE-FACTS.md](HARDWARE-FACTS.md) | 碰引脚、串口、启动模式、SRAM4 之前 |
| [BUILD-AND-TEST.md](BUILD-AND-TEST.md) | 要编译、要测、或者在查一个时有时无的问题 |
| [JOURNAL.md](JOURNAL.md) | 碰 bootloader 的状态扇区、metadata、事件日志之前；或者想知道"这块板凭什么认为 app 能跑" |
| [OWNERSHIP.md](OWNERSHIP.md) | 碰签名密钥、信任根、bootloader 扇区布局之前；或者有人问"开箱的板子安不安全" |
| [IAP-STATUS.md](IAP-STATUS.md) | 想知道某条路径验没验过、还欠什么 |
| [DEFERRED-DESIGNS.md](DEFERRED-DESIGNS.md) | 有人问"为什么不做 X" |
| [TODO.md](TODO.md) | 手头没活了，或者想知道某个已知问题排在哪 |

两份不在 `docs/` 但要知道的：

| 文件 | 装什么 |
|---|---|
| [../RELEASE-NOTES.md](../RELEASE-NOTES.md) | 英文，**给外部看**。升级规则、已知问题、发版检查单。**升级风险只靠它兜着，改了行为记得同步** |
| [../OpenPLC_Bootloader.md](../OpenPLC_Bootloader.md) | bootloader **工程结构**：怎么构建、有哪些文件、编出来多大、lwIP 怎么配。⚠️ **只讲工程长什么样，不讲行为** —— 2026-08-17 同步时把行为那部分删了，因为它四个月错了四处 |

## 维护约定

- **发现与现状不符就地改掉**，不要另起一份。这套文档 2026-08-16 从 17 个分散的笔记合并而来，合并时就抓到一处互相矛盾的地方（一边说某脚本还在用，另一边说它已经删了）—— 两份就会漂移，这是规律不是意外。
- **写事实和判断依据，不写流水账。** "PB10 拉低会关断整片 MAX3221（查 netlist.ipc 逐脚核实）"有用；"今天调试了三小时"没用。
- **没验证的结论要标出来。** 写"未验证"比写一个看起来很确定的猜测好得多。
- **日期要写。** 一条 2026-08 的实测结论，两年后读的人得能判断它还算不算数。

## 语言

这套文档用中文。**其他一切落到文件里的东西用英文** —— 代码注释、`#error`/`#warning` 文案、工具的 stdout/stderr、各仓库的 README。详见 [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md)。
