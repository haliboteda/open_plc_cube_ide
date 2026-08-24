# 开工与收尾 —— 换台电脑也能接着干的那一套

这个目录装的是**框架和上下文**：产品要做到什么（需求）、怎么证明做到了（测试）、还欠什么（Todo）、以及一个新会话/新机器要怎么快速进入状态。

建于 2026-08-17。

## 会话中随时要做的一件事：用户说了什么，当下就落盘

**用户在对话里给出一条事实、一个决定、或者"别做 X" —— 立刻写进拥有它的文件，然后回一句改了哪几个。** 规矩和它的代价写在 [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) 第一节。

不落盘的后果不是"忘了记"，是**下次读文档会把旧状态再讲一遍**，然后用户得把同一句话说第三遍。

## 这个目录不重复别处的内容

`docs/` 下的笔记是**事实的唯一出处**，这里只**引用**，不抄 —— 抄一遍就是第二份会漂移的文档。为什么这条是硬规矩，见 [../INDEX.md](../INDEX.md) 的「维护约定」。

| 想找 | 去哪 | 不在哪 |
|---|---|---|
| 产品要做到什么、做到没有 | **[../STATUS.md](../STATUS.md)** | — |
| 怎么证明、跑什么、谁跑 | **[../STATUS.md](../STATUS.md)** 的覆盖矩阵 | 判据在 `$TOOL/TestCase/TEST-CASES.md` |
| 还没做的模块 + 设计思路 | **[../work/BACKLOG.md](../work/BACKLOG.md)** | — |
| 某条结论的实测证据 | [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md) | **数字的唯一出处**，不要抄进总表 |
| 用例的判据、怎么跑 | `$TOOL/TestCase/TEST-CASES.md` | 判据贴着代码走，跨仓不搬 |
| 零散 bug 和代码债 | [../work/ISSUES.md](../work/ISSUES.md) | Todo/ 只放要立项的模块 |
| 改动前的规矩 | [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) | — |
| 密钥/信任模型的配图版 | [../archive/artifacts/RENDERED-SNAPSHOTS.md](../archive/artifacts/RENDERED-SNAPSHOTS.md) | ⚠️ **渲染快照，不是事实来源**，打架以 `docs/` 为准 |

## 文件名的规矩

**名字要说出内容或用途，不要 `README.md`。** 2026-08-17 之前这个目录和 `docs/` 下一共 5 个 `README.md`，光看名字分不清哪个是索引、哪个是入口、哪个是待办。**只有仓库根目录的 `README.md` 例外** —— 那是仓库门面。

分工是正交的，目录名直接说出这份文件多久变一次：

| 问题 | 文件 |
|---|---|
| **要做到什么、到哪一步了、怎么证明的** | **[../STATUS.md](../STATUS.md)** —— 2026-08-22 起是一张表，原来的 REQUIREMENTS.md 和 TEST-PLAN.md 合并进去了（那两份已不存在） |
| 某个数字的出处 | [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md) |
| 还欠什么，怎么做 | [../work/BACKLOG.md](../work/BACKLOG.md)（立项的模块）／[../work/ISSUES.md](../work/ISSUES.md)（零散问题） |
| 怎么开工、怎么收尾 | `SESSION-START.md`（本文件） |
| 某个编号是什么 | [../ID-MAP.md](../ID-MAP.md) |

## 新会话的开机顺序

**入口是仓库根目录的 [../../CLAUDE.md](../../CLAUDE.md)**（Claude Code 自动加载），它给的顺序是：

1. **[WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md)** —— 规矩。改任何东西之前
2. **[../design/ARCHITECTURE.md](../design/ARCHITECTURE.md)** —— 三个仓库在哪、哪些代码是跨仓镜像的
3. **[../STATUS.md](../STATUS.md)** —— 现在做到哪一步
4. **[../work/BACKLOG.md](../work/BACKLOG.md)** —— 手头该干什么

碰硬件之前再加一份 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)；碰 bootloader 的状态扇区之前加 [../design/JOURNAL.md](../design/JOURNAL.md)；碰签名密钥之前加 [../design/OWNERSHIP.md](../design/OWNERSHIP.md)。

## 收尾流程 —— 说"今天到此为止"时要做的

**流程本身是 skill `/openplc:wrap-up`**（`AI-Skills/OpenPLC/Software/skills/wrap-up/`）。它负责"什么时候该收尾、按什么顺序、别忘了什么"；**下面这张表是它照着做的那张表**，是归档位置的唯一出处，skill 不抄。

用户说**"今天结束" / "到此为止" / "今天就这样"**或类似的话时，把这一天产生的**非项目本身**的东西全部归位，一件都不留在临时目录：

| 今天产生了 | 归到哪 |
|---|---|
| 测试脚本、自动化工具 | `$TOOL/TestCase/tools/` 或 `host/<主题>/`，判据写进 `$TOOL/TestCase/TEST-CASES.md` |
| 一次性探查脚本，但值得留 | 同上，并补一份 README 说清"验证什么 / 前置 / 判据" |
| 新的实测结论 | [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md)，带日期 |
| 新的设计决策与否决理由 | [../design/DEFERRED-DESIGNS.md](../design/DEFERRED-DESIGNS.md) 或对应模块的 Todo 文件 |
| 新发现的问题 | [../work/ISSUES.md](../work/ISSUES.md)（零散）或 [../work/BACKLOG.md](../work/BACKLOG.md)（要立项） |
| 需求状态变化 | [../STATUS.md](../STATUS.md) 的状态列 |
| 用例跑出来的结果 | [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md)，一行一次跑，带日期 |
| 协作方式上的教训 | [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) |

⚠️ **还要人工过一遍 [../../RELEASE-NOTES.md](../../RELEASE-NOTES.md) 的 known-issues 和 not-verified 两节**，逐条问"今天这条还成立吗"。

**这一步必须人工做，没有任何自动化能覆盖** —— `selfcheck` 只抓得住版本号那一种形态。为什么它值得每次收尾花几分钟（2026-08-17 一次核对四条全错），见 [../test/COVERAGE-GAPS.md](../test/COVERAGE-GAPS.md)。

然后**提交并推送三个仓库**。判断标准只有一条：

> 换一台电脑、从零开一个新会话，照着这个目录能不能接着干？不能就是没归纳完。

⚠️ **`%TEMP%` 和 scratchpad 里的东西换台电脑就全废**，而且不进 git、别人拿不到。这条已经犯过一次（自动烧写脚本写进 scratchpad 还硬编码了绝对路径），所以它是 [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) 里的正式约定，不是建议。

## 语言

这个目录用中文，和 `docs/` 其余部分一致。**落到文件里的其他一切用英文** —— 代码注释、脚本 stdout、各仓库 README。理由见 [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md)。
