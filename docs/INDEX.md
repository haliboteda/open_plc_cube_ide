# 项目笔记

这里放的是**代码本身说不出来的东西**：为什么这么设计、哪些结论是实测过的、哪些坑踩过、哪些事情故意没做。

代码结构、改动历史、修过什么 bug —— 这些 git 和源码自己会说，不要往这里抄。

---

## 先看这两份

| 文件 | 回答 |
|---|---|
| **★ [STATUS.md](STATUS.md)** | **这块板要做什么、做到哪、什么在挡路。** 12 行全局概览 + 优先级 + 54 条明细。**需求和测试合并成一张表** —— 它们本来是同一件事的两半 |
| [ID-MAP.md](ID-MAP.md) | 哪个编号住在哪个文件。看到 `T1` / `BG1` / `ISS-B4` / `CHK-A5` / `M3` 不知道是什么就查这里 |

---

## 五个目录，按用途分

**2026-08-22 重组。** 原来 `docs/` 底下十来个文件平铺，加一个 `handover/` —— 而"handover"不回答任何问题。现在按**这份文件多久变一次**分：

| 目录 | 装什么 | 多久变一次 |
|---|---|---|
| [design/](design/) | **为什么这么做。** 架构、信任模型、journal 机制、硬件事实、已决策、故意不做的 | 很少 |
| [test/](test/) | **怎么证明 + 测出来是什么。** 实测数字、用例设计、覆盖缺口、怎么编译 | 每次跑用例 |
| [work/](work/) | **在做什么、还欠什么。** 已知问题、模块设计、正在排查的 | 经常 |
| [process/](process/) | **怎么协作、怎么开工收尾。** | 很少 |
| [archive/](archive/) | **已被否定的结论** + 渲染快照 | 只增不改 |

### design/ —— 为什么这么做

| 文件 | 什么时候看 |
|---|---|
| [design/ARCHITECTURE.md](design/ARCHITECTURE.md) | 找不到某个仓库/文件在哪；要改跨仓共享的东西；查 RTC 备份寄存器谁占了哪个 |
| [design/OWNERSHIP.md](design/OWNERSHIP.md) | 碰签名密钥、信任根、bootloader 扇区布局之前；或者有人问"开箱的板子安不安全" |
| [design/JOURNAL.md](design/JOURNAL.md) | 碰状态扇区、metadata、事件日志之前；或者想知道"这块板凭什么认为 app 能跑" |
| [design/HARDWARE-FACTS.md](design/HARDWARE-FACTS.md) | 碰引脚、串口、启动模式、SRAM4 之前 |
| [design/DECISIONS.md](design/DECISIONS.md) | **有人想重开一个已经拍板的话题。** 六条决定，每条带"什么情况下才该重开" |
| [design/DEFERRED-DESIGNS.md](design/DEFERRED-DESIGNS.md) | 有人问"为什么不做 X" |

### test/ —— 怎么证明

| 文件 | 什么时候看 |
|---|---|
| [test/MEASUREMENTS.md](test/MEASUREMENTS.md) | **所有实测数字的唯一出处。** 引用数字就引这里，不要另抄一份 |
| [test/CASE-DESIGNS.md](test/CASE-DESIGNS.md) | 要写新用例，或想知道某条用例**为什么这么设计** |
| [test/COVERAGE-GAPS.md](test/COVERAGE-GAPS.md) | 想知道**我们知道自己没测到什么** |
| [test/BUILD-AND-TEST.md](test/BUILD-AND-TEST.md) | 要编译，或者在查一个时有时无的问题 |

⚠️ **用例的判据和运行命令不在这里**，在 `$TOOL/TestTool/TEST-CASES.md`（跨仓，贴着代码走 —— 判据要跟着实现走）。

### work/ —— 在做什么

| 文件 | 什么时候看 |
|---|---|
| [work/ISSUES.md](work/ISSUES.md) | 手头没活了；或想知道某个已知问题排在哪、现象是什么。**按优先级排好了** |
| [work/BACKLOG.md](work/BACKLOG.md) | 想知道 M1–M8 八个模块各是什么、什么顺序 |
| `work/M<n>-*.md` | 要动某个模块。一份一个模块，含设计思路、分步计划、验收 |
| [work/investigations/sdram-d1.md](work/investigations/sdram-d1.md) | 🔴 **当前唯一的 P0。** 那根 D1 线的全部测量、已排除的解释、给硬件的请求 |

### process/ —— 怎么干活

| 文件 | 什么时候看 |
|---|---|
| [process/WORKING-AGREEMENTS.md](process/WORKING-AGREEMENTS.md) | **动手改任何东西之前** |
| [process/SESSION-START.md](process/SESSION-START.md) | 开工顺序与**收尾流程** |

### archive/ —— 只增不改

| 文件 | 什么时候看 |
|---|---|
| [archive/RETRACTED.md](archive/RETRACTED.md) | **要推一个结论之前，先看这里有没有人推过并被否定。** 13 条"当初以为是 X，实测否定了" |
| [archive/artifacts/](archive/artifacts/) | 六个渲染好的 HTML 单页。⚠️ **不是事实来源**，和 `docs/` 打架时以 `docs/` 为准 |

---

## 两份不在 docs/ 但要知道的

| 文件 | 装什么 |
|---|---|
| [../CLAUDE.md](../CLAUDE.md) | **最外层入口，Claude Code 自动加载。** 换台机器要 clone 什么、装什么、配什么、哪些东西不在 git 里。**七个仓库各有一份**，另外六份只说"本仓库是什么"然后指回来 |
| [../RELEASE-NOTES.md](../RELEASE-NOTES.md) | 英文，**给外部看**。升级规则、已知问题、发版检查单。**升级风险只靠它兜着，改了行为记得同步** |
| [../OpenPLC_Bootloader.md](../OpenPLC_Bootloader.md) | bootloader **工程结构**：怎么构建、有哪些文件、编出来多大、lwIP 怎么配。⚠️ **只讲工程长什么样，不讲行为** —— 行为那部分 2026-08-17 删了，因为它四个月错了四处 |

---

## 命名约定

**文件名要说出内容或用途。** 2026-08-17 之前这里有 5 个 `README.md`，光看名字分不出哪个是索引、哪个是入口、哪个是待办 —— 已全部改名。**只有仓库根目录的 `README.md` 保留**，那是仓库门面，约定俗成。

**目录名要说出"这份文件多久变一次"**，因为那决定了读的人该不该信它。`handover/` 这个名字 2026-08-22 删掉了 —— 它不回答任何问题。

## 维护约定

- **发现与现状不符就地改掉**，不要另起一份。这套文档 2026-08-16 从 17 个分散的笔记合并而来，合并时就抓到一处互相矛盾的地方 —— 两份就会漂移，**这是规律不是意外**。
- **一个事实一个出处。** 数字 → [test/MEASUREMENTS.md](test/MEASUREMENTS.md)；状态 → [STATUS.md](STATUS.md)；判据 → `TEST-CASES.md`。别处只**引用**。⚠️ 2026-08-22 清理时，同一条需求的状态在**五个**文件里各写一遍，三个数字对不上。
- **写事实和判断依据，不写流水账。** "PB10 拉低会关断整片 MAX3221（查 netlist.ipc 逐脚核实）"有用；"今天调试了三小时"没用。
- **没验证的结论要标出来。** 写"未验证"比写一个看起来很确定的猜测好得多。
- **推断和实测分行写。** [archive/RETRACTED.md](archive/RETRACTED.md) 第 6 条就是混在一句里造成的：代码事实完全正确，从它推出的下一步结论完全错误。
- **日期要写全。** 一条 2026-08 的实测结论，两年后读的人得能判断它还算不算数。
- **做完的待办直接删，不要划掉留着。** 唯一例外是被实测否定的推论 —— 那些搬进 [archive/RETRACTED.md](archive/RETRACTED.md)，删了会有人重推一遍。

## 语言

这套文档用中文。**其他一切落到文件里的东西用英文** —— 代码注释、`#error`/`#warning` 文案、工具的 stdout/stderr、各仓库的 README。详见 [process/WORKING-AGREEMENTS.md](process/WORKING-AGREEMENTS.md)。
