# 开工与收尾 —— 换台电脑也能接着干的那一套

这个目录装的是**框架和上下文**：产品要做到什么（需求）、怎么证明做到了（测试）、还欠什么（Todo）、以及一个新会话/新机器要怎么快速进入状态。

建于 2026-08-17。

## 这个目录不重复别处的内容

`docs/` 下已有的 8 份笔记是**事实的唯一出处**，这里只**引用**，不抄。抄一遍就是第二份会漂移的文档 —— 这套文档 2026-08-16 从 17 份分散笔记合并而来，合并当场就抓到一处互相矛盾，见 [../INDEX.md](../INDEX.md)。

| 想找 | 去哪 | 不在哪 |
|---|---|---|
| 产品要做到什么、做到没有 | **[REQUIREMENTS.md](REQUIREMENTS.md)** | — |
| 怎么证明、跑什么、谁跑 | **[TEST-PLAN.md](TEST-PLAN.md)** | — |
| 还没做的模块 + 设计思路 | **[Todo/BACKLOG.md](Todo/BACKLOG.md)** | — |
| 某条结论的实测证据 | [../IAP-STATUS.md](../IAP-STATUS.md) | 不要抄进 REQUIREMENTS |
| 用例的判据、怎么跑 | `$TOOL/TestTool/TEST-CASES.md` | 判据贴着代码走，跨仓不搬 |
| 零散 bug 和代码债 | [../TODO.md](../TODO.md) | Todo/ 只放要立项的模块 |
| 改动前的规矩 | [../WORKING-AGREEMENTS.md](../WORKING-AGREEMENTS.md) | — |
| 密钥/信任模型的配图版 | [artifacts/RENDERED-SNAPSHOTS.md](artifacts/RENDERED-SNAPSHOTS.md) | ⚠️ **渲染快照，不是事实来源**，打架以 `docs/` 为准 |

## 文件名的规矩

**名字要说出内容或用途，不要 `README.md`。** 2026-08-17 之前这个目录和 `docs/` 下一共 5 个 `README.md`，光看名字分不清哪个是索引、哪个是入口、哪个是待办。**只有仓库根目录的 `README.md` 例外** —— 那是仓库门面。

这四份的分工是正交的，名字直接对应：

| 问题 | 文件 |
|---|---|
| 要做到什么，到哪一步了 | `REQUIREMENTS.md` |
| 怎么证明做到了 | `TEST-PLAN.md` |
| 还欠什么，怎么做 | `Todo/BACKLOG.md` |
| 怎么开工、怎么收尾 | `SESSION-START.md`（本文件） |

## 新会话的开机顺序

**入口是仓库根目录的 [../../CLAUDE.md](../../CLAUDE.md)**（Claude Code 自动加载），它给的顺序是：

1. **[../WORKING-AGREEMENTS.md](../WORKING-AGREEMENTS.md)** —— 规矩。改任何东西之前
2. **[../ARCHITECTURE.md](../ARCHITECTURE.md)** —— 三个仓库在哪、哪些代码是跨仓镜像的
3. **[REQUIREMENTS.md](REQUIREMENTS.md)** —— 现在做到哪一步
4. **[Todo/BACKLOG.md](Todo/BACKLOG.md)** —— 手头该干什么

碰硬件之前再加一份 [../HARDWARE-FACTS.md](../HARDWARE-FACTS.md)；碰 bootloader 的状态扇区之前加 [../JOURNAL.md](../JOURNAL.md)；碰签名密钥之前加 [../OWNERSHIP.md](../OWNERSHIP.md)。

## 换台电脑

**全部写在仓库根目录的 [../../CLAUDE.md](../../CLAUDE.md) 里** —— clone 哪几个仓库（含地址和 remote 名）、装什么工具、配什么、哪些东西不在 git 里必须单独处理。

那份是**新会话和新机器的入口**：Claude Code 在任何一个仓库里启动都会自动加载它，四个仓库各有一份。这里不抄第二遍。

一句话版本：

```powershell
# clone 三个仓库（地址见 CLAUDE.md）→ 装工具 → 填本机路径 → 自检
cd $TOOL/TestTool
Copy-Item config/machine.example.ps1 config/machine.ps1   # 然后编辑
pwsh ./tools/selfcheck.ps1
```

`selfcheck.ps1` 全绿（或只剩它自己报出来的 SKIP）就说明这台机器可以开工了。**缺什么它会说缺什么**，不会静默跳过 —— 第一步 **A0** 专门打印这台机器上每一项工具解析成了什么。

## 收尾流程 —— 说"今天到此为止"时要做的

用户说**"今天结束" / "到此为止" / "今天就这样"**或类似的话时，把这一天产生的**非项目本身**的东西全部归位，一件都不留在临时目录：

| 今天产生了 | 归到哪 |
|---|---|
| 测试脚本、自动化工具 | `$TOOL/TestTool/tools/` 或 `host/<主题>/`，判据写进 `$TOOL/TestTool/TEST-CASES.md` |
| 一次性探查脚本，但值得留 | 同上，并补一份 README 说清"验证什么 / 前置 / 判据" |
| 新的实测结论 | [../IAP-STATUS.md](../IAP-STATUS.md)，带日期 |
| 新的设计决策与否决理由 | [../DEFERRED-DESIGNS.md](../DEFERRED-DESIGNS.md) 或对应模块的 Todo 文件 |
| 新发现的问题 | [../TODO.md](../TODO.md)（零散）或 [Todo/BACKLOG.md](Todo/BACKLOG.md)（要立项） |
| 需求状态变化 | [REQUIREMENTS.md](REQUIREMENTS.md) 的状态列 |
| 用例跑出来的结果 | [TEST-PLAN.md](TEST-PLAN.md) 的「最近结果」列，带日期 |
| 协作方式上的教训 | [../WORKING-AGREEMENTS.md](../WORKING-AGREEMENTS.md) |

⚠️ **还要人工过一遍 [../../RELEASE-NOTES.md](../../RELEASE-NOTES.md) 的 known-issues 和 not-verified 两节**，逐条问"今天这条还成立吗"。

2026-08-17 收尾时四条 known-issue 全部已经不对了 —— 修好的问题没人回来删条目。这份是对外的，**一条过期的 known-issue 会让读的人去查一个不存在的问题，比没有这一条更糟**。`selfcheck` 只能抓版本号那一种形态，其余靠人。

然后**提交并推送三个仓库**。判断标准只有一条：

> 换一台电脑、从零开一个新会话，照着这个目录能不能接着干？不能就是没归纳完。

⚠️ **`%TEMP%` 和 scratchpad 里的东西换台电脑就全废**，而且不进 git、别人拿不到。这条已经犯过一次（自动烧写脚本写进 scratchpad 还硬编码了绝对路径），所以它是 [../WORKING-AGREEMENTS.md](../WORKING-AGREEMENTS.md) 里的正式约定，不是建议。

## 语言

这个目录用中文，和 `docs/` 其余部分一致。**落到文件里的其他一切用英文** —— 代码注释、脚本 stdout、各仓库 README。理由见 [../WORKING-AGREEMENTS.md](../WORKING-AGREEMENTS.md)。
