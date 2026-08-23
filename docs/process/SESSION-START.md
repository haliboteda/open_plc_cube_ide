# 开工与收尾 —— 换台电脑也能接着干的那一套

这个目录装的是**框架和上下文**：产品要做到什么（需求）、怎么证明做到了（测试）、还欠什么（Todo）、以及一个新会话/新机器要怎么快速进入状态。

建于 2026-08-17。

## 这个目录不重复别处的内容

`docs/` 下的笔记是**事实的唯一出处**，这里只**引用**，不抄 —— 抄一遍就是第二份会漂移的文档。为什么这条是硬规矩，见 [../INDEX.md](../INDEX.md) 的「维护约定」。

| 想找 | 去哪 | 不在哪 |
|---|---|---|
| 产品要做到什么、做到没有 | **[../STATUS.md](../STATUS.md)** | — |
| 怎么证明、跑什么、谁跑 | **[../STATUS.md](../STATUS.md)** 的覆盖矩阵 | 判据在 `$TOOL/TestTool/TEST-CASES.md` |
| 还没做的模块 + 设计思路 | **[../work/BACKLOG.md](../work/BACKLOG.md)** | — |
| 某条结论的实测证据 | [../test/MEASUREMENTS.md](../test/MEASUREMENTS.md) | **数字的唯一出处**，不要抄进总表 |
| 用例的判据、怎么跑 | `$TOOL/TestTool/TEST-CASES.md` | 判据贴着代码走，跨仓不搬 |
| 零散 bug 和代码债 | [../work/ISSUES.md](../work/ISSUES.md) | Todo/ 只放要立项的模块 |
| 改动前的规矩 | [WORKING-AGREEMENTS.md](WORKING-AGREEMENTS.md) | — |
| 密钥/信任模型的配图版 | [../archive/artifacts/RENDERED-SNAPSHOTS.md](../archive/artifacts/RENDERED-SNAPSHOTS.md) | ⚠️ **渲染快照，不是事实来源**，打架以 `docs/` 为准 |

## 文件名的规矩

**名字要说出内容或用途，不要 `README.md`。** 2026-08-17 之前这个目录和 `docs/` 下一共 5 个 `README.md`，光看名字分不清哪个是索引、哪个是入口、哪个是待办。**只有仓库根目录的 `README.md` 例外** —— 那是仓库门面。

分工是正交的，目录名直接说出这份文件多久变一次：

| 问题 | 文件 |
|---|---|
| **要做到什么、到哪一步了、怎么证明的** | **[../STATUS.md](../STATUS.md)** —— 2026-08-22 起是一张表，原来的 `REQUIREMENTS.md` 和 `TEST-PLAN.md` 合并进去了 |
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

## 换台电脑

**全部写在仓库根目录的 [../../CLAUDE.md](../../CLAUDE.md) 里** —— clone 哪几个仓库（含地址和 remote 名）、装什么工具、配什么、哪些东西不在 git 里必须单独处理。

那份是**新会话和新机器的入口**：Claude Code 在任何一个仓库里启动都会自动加载它，**[../../CLAUDE.md](../../CLAUDE.md) 第三节表里的七个仓库各有一份**。这里不抄第二遍。

**两条命令。** clone 文档主仓，然后跑脚本：

```bash
git clone <地址见 CLAUDE.md 第三节那张表 —— 七个仓库只有那一份>
python3 open_plc_cube_ide/tools/bootstrap.py       # 全部开关见 CLAUDE.md 第五节
```

第二条消不掉：脚本得先到那台机器上，而把它弄过去就是那次 clone。

`bootstrap.py` 干八件事 —— 探 SSH、clone 其余仓库、**确认分支**、装缺的工具（每条命令先打出来问一次）、探测本机路径、授权兄弟仓库给会话、装 `openplc` plugin、自检并汇报。**先看它要干什么就加 `--dry-run`；可以重复跑。**

已经在会话里的话，同一件事说一句 `/openplc:init` 就行。两者是同一件事的两条路，实质都在 `init_machine.py` 里 —— 分工见 [../../CLAUDE.md](../../CLAUDE.md) 第五节。

⚠️ **脚本跑完还剩两件只有人能做的事，它会在最后点名说：**

1. **在仓库目录里交互式开一次 Claude Code，接受信任对话框。** 没信任之前，committed 的 `.claude/settings.json` **全部静默失效** —— 包括那条让 marketplace 自动加入的声明。
2. **装 STM32CubeIDE。** 下载要 ST 账号，任何包管理器都没有它。

Linux 上还有第三件：`sudo usermod -aG dialout $USER`，然后**登出再登入**。

⚠️ **默认分支不是工作分支**（2026-08-21 五个仓库里三个如此）。脚本会逐个仓库确认，而且**在没人回答时绝不替你切** —— 版本号最高的分支不一定是在干活的那个，`open_plc_arduino` 就是这样。

那个 skill 做的事（立项文件 [../work/M8-onboard-skill.md](../work/M8-onboard-skill.md)）：clone 缺的仓库 → 报告缺哪些运行时 → 探测本机路径并把问题问回给你 → 把兄弟仓库授权给会话 → 自检 → 汇报这台机器能干什么、不能干什么。

手工版本（skill 不可用时）：

```bash
cd $TOOL/TestTool
python3 tools/init_machine.py --prereqs            # 缺什么、怎么装（平台差异见 CLAUDE.md 第四节）
python3 tools/init_machine.py                      # 探测路径，不用手填
python3 tools/init_machine.py --write-claude-dirs   # 让会话读得到兄弟仓库
pwsh ./tools/selfcheck.ps1
```

selfcheck 全绿（或只剩它自己报出来的 SKIP）就说明这台机器可以开工了。**缺什么它会说缺什么**，不会静默跳过 —— 第一步 **ENV** 专门打印这台机器上每一项工具解析成了什么。`--list` 可以先看它会跑哪 12 步、各证明哪条需求。⚠️ PowerShell 版的 ENV 要 pwsh 才跑得起来，所以 pwsh 本身缺不缺要靠 `--prereqs` 看。

## 收尾流程 —— 说"今天到此为止"时要做的

**流程本身是 skill `/openplc:wrap-up`**（`AI-Skills/OpenPLC/Software/skills/wrap-up/`）。它负责"什么时候该收尾、按什么顺序、别忘了什么"；**下面这张表是它照着做的那张表**，是归档位置的唯一出处，skill 不抄。

用户说**"今天结束" / "到此为止" / "今天就这样"**或类似的话时，把这一天产生的**非项目本身**的东西全部归位，一件都不留在临时目录：

| 今天产生了 | 归到哪 |
|---|---|
| 测试脚本、自动化工具 | `$TOOL/TestTool/tools/` 或 `host/<主题>/`，判据写进 `$TOOL/TestTool/TEST-CASES.md` |
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
