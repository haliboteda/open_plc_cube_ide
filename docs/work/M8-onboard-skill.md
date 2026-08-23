# M8 · 一句「初始化」就能在新机器上开工

**2026-08-21 立项。** 决定人：用户。

## 是什么

一个 Claude Code **skill**，`/openplc:init`，装在独立仓库 `AI-Skills` 里，通过 plugin marketplace 分发给本产品的每一个仓库。加上 `init_machine.py` 三处扩展，把「换台电脑」从一串要人记住的步骤变成一句话。

新机器上的全部动作 —— **两条命令**：

```bash
# 地址见 ../../CLAUDE.md 第三节那张表
git clone <open_plc_cube_ide 的 SSH 地址>
python3 open_plc_cube_ide/tools/bootstrap.py
```

已经在会话里时，同一件事是一句 `/openplc:init`。

### 两条路，一份实质

| | 用在什么时候 | 谁做决定 |
|---|---|---|
| `tools/bootstrap.py` | 只有终端，或想无人值守 | 脚本按规则走，拿不准就问 |
| `/openplc:init` | 已经在会话里 | AI 读输出，替你判断问什么、缺的影响哪些用例 |

**substance 全在 `init_machine.py`** —— 探测什么、装什么、写哪些配置。两个外壳都不实现逻辑：`bootstrap.py` 连仓库表和目录布局都是**从 CLAUDE.md 第三节现读**，工具清单和安装命令**从 `PREREQS` 现读**。改行为改 `init_machine.py`，不是改外壳。

⚠️ `bootstrap.py` 破例放在 `open_plc_cube_ide/tools/`，违反 [../../CLAUDE.md](../../CLAUDE.md) 第八节"脚本进 TestTool"的规矩。理由是硬的：**它必须在 `IAPTranfer_Tool` 被 clone 之前就能跑。** 这条例外写进第八节了。

## 做什么用的

**换台电脑（Windows / Debian / macOS）不用重训一个 AI，也不用照着 CLAUDE.md 手工走五步。**

之前缺的不是能力，是**入口**：那五步只存在于 [../../CLAUDE.md](../../CLAUDE.md) 第五节的散文里，没有任何东西保证一个新会话会照着走 —— 它得先读到、还得认出「初始化」这句话对应的是它。

## 覆盖哪条需求

**[F5](../STATUS.md)**（换台电脑能接着干，不用重训一个 AI）。

与 [M7](M7-python-scripts.md) **正交**，两个都覆盖 F5 但方向不同：M7 让 Debian 机器**跑得动用例**，M8 让任何机器**配得起来**。M7 没做完之前，M8 在 Debian / macOS 上的产出是"配好了，但用例要 pwsh"，skill 会照实说而不是假装跳过几项。

## 在哪找

| 东西 | 位置 |
|---|---|
| skill 正文 | `AI-Skills/OpenPLC/Software/skills/init/SKILL.md` |
| plugin / marketplace 清单 | `AI-Skills/OpenPLC/Software/.claude-plugin/plugin.json`、`AI-Skills/.claude-plugin/marketplace.json` |
| 三处扩展 | `$TOOL/TestTool/tools/init_machine.py` —— `PREREQS` 表与 `check_prereqs()`、`write_claude_dirs()` 与 `ignored_by_own_rules()`、`SETTINGS` 里的 `HW_REPO`/`REF_REPO` |
| 单测 | `$TOOL/TestTool/tools/test_init_machine.py`（32 例） |
| marketplace 声明 | 五个仓库各自的 `.claude/settings.json`（`extraKnownMarketplaces` + `enabledPlugins`） |

## 不做会怎样

第二台机器上每次都要有人**记得** CLAUDE.md 第五节存在、并且照它走。2026-08-20 第一次在 Debian 上配置，踩的是模板忘删一半的坑 —— 模板已经删了，但"要人记住流程"这件事没变。

## 设计思路

### skill，不是 agent

skill 是一组**加载进当前会话**的指令：要和用户来回问答、要改本机配置文件、要在主会话里留下上下文。agent 是独立子会话，问不到用户，答案也不会留在主会话里。

### 装在独立仓库，套一层 plugin 壳

Claude Code 默认只加载**当前项目**的 `.claude/skills/`。skill 要放在 `AI-Skills` 里，就必须走 plugin + marketplace：

- `AI-Skills/.claude-plugin/marketplace.json` 用**相对路径**（`./OpenPLC/Software`）引用 plugin —— git 来源下有效，**不能用 `..`**。
- 各仓库 `.claude/settings.json` 里的 `extraKnownMarketplaces` 让 marketplace 在信任目录后自动加入。
- `enabledPlugins` 是**对象映射**（`{"openplc@ai-skills": true}`），不是数组。
- **不给 plugin 写 `version`**：写了就是钉版本，装过的副本要等版本号变才更新。新机器上拿到一份过期的 skill，正是这个 skill 要防的事。

### 常驻规则不能是 skill，也不能是 plugin

2026-08-21 把散在各处的 AI 侧内容归拢时撞到的机制事实：

| 想要 | 唯一可行的机制 | 在 git 里吗 |
|---|---|---|
| 按需加载的流程 | plugin 的 `skills/` | ✅ |
| **每个会话、每个项目常驻** | **`~/.claude/rules/*.md`** | ❌ machine-local |
| 每个会话、单个项目常驻 | 仓库的 `CLAUDE.md` / `.claude/rules/` | ✅ |
| Claude 自己记的东西 | `~/.claude/projects/<p>/memory/` | ❌ **官方文档明确：不跨机器共享** |

**"提编号就给路径""要动手的步骤用 🍍 标出来"这两条是常驻规则，不是任务** —— skill 只在被调用或被判定相关时加载，plugin 根本没有常驻槽位。而它们当时只存在于 machine-local 的 memory 里，**换台电脑必丢**，而且 `WORKING-AGREEMENTS.md` 里也没有。

解法：文本进 git（`AI-Skills/_shared/rules/`），`bootstrap.py` 第 6b 步**拷进 `~/.claude/rules/`**，每份拷贝带头注明出处和刷新命令。**"生成的拷贝"不是这个项目要防的那种重复** —— 要防的是两份手工维护的。同一道理下 `machine.py` 也是生成的。

顺带清掉的三份 machine-local 记忆，其中一份已经漂了：它一直指向 2026-08-17 就改名的 `docs/ai/README.md`，还写着"提交三个仓库"（实际六个）。**没人会去更新一份不在 git 里的文件。**

### 用户 2026-08-21 改了安装的边界

原来定的是「只报告，用户自己装」。当天下午改成**脚本执行安装**，于是 `PREREQS` 的每个平台值从一句话变成 `{"cmd", "auto"}`：`auto=True` 的脚本可以跑，`auto=False` 的必须人来（多步、要登录、或压根没有包）。

`--prereqs` 显示这份数据，`bootstrap.py` 执行它 —— **一份数据两种用法，不是两份清单**。表完整性有单测盯着（每个条目、每个平台都必须明确说 auto 是真还是假，不许默认）。

同时把 **Arduino IDE 和 STM32CubeIDE 也纳入 `PREREQS`** —— 它们是安装目录不是 PATH 上的可执行文件，但"我必须装什么"是一个问题，应该有一个答案。检查函数直接复用已有的 `detect_ide()` / `detect_cubeide()`。CubeIDE 三个平台都是 `auto=False`：下载要 ST 账号，没有任何包管理器有它，假装能自动装只会在更远的地方失败。

### 「装完了」和「这个进程看得见」是两回事

`winget` / `apt` 装完之后，**新的 PATH 不会进入一个已经在运行的进程**。所以 `bootstrap.py` 装完会**重新跑一遍检查**，装上但看不见的点名说出来，让你开个新终端再跑一次 —— 而不是报成功，然后让你去 debug 一个错误的方向。

### fresh clone 落在默认分支上，那不是工作分支

2026-08-21 查出来的：五个仓库里**三个**的远端默认分支是 `master`/`main`，工作分支却是 `v0.1.3.1` / `v0.1.3` / `v0.1.3-dev`。「clone 完就开工」等于**悄无声息落后一整个版本**，症状和 CLAUDE.md 记的 Forgejo 陈旧镜像一模一样，原因不同。

分支名每次发版都变，所以不写进任何文档：`report_branches()` 现场打表，并在**有的仓库在默认分支、有的不在**时警告 —— 那正是忘了 checkout 的形状。默认分支从 clone 写下的本地 ref `refs/remotes/origin/HEAD` 读，不联网。

**但"版本号最高"这个启发式不能用来自动决定。** `bootstrap.py` 第一次 `--dry-run` 当场把 `open_plc_arduino` 的 `v0.1.3.1-old-knx` 排成了最新，差一点就 checkout 过去。两处修：

1. 名字里带 `old` / `backup` / `bak` / `tmp` / `wip` / `deprecated` 的直接排除；同版本号下**短名字优先**（后缀是限定词，光名字才是主线）。
2. **默认永远是保持当前分支。** 排出来的最高版本只作为提示 —— 因为即使修好排序，`open_plc_arduino` 的最高版本是 `v0.1.3.1` 而在干活的是 `v0.1.3-dev`。**没人回答时绝不切**：只在"当前在非版本分支且存在版本分支"（也就是刚 clone 完）时才把最高版本当默认，并把这一步记成未完成。

### 信任目录是前置条件，而且非交互调用不会问

`extraKnownMarketplaces` 和 committed 的 allow 规则**都要等目录被信任之后才生效**。2026-08-21 用 `claude -p ...` 验证时当场撞到：它打印 `Ignoring 33 permissions.allow entries ... this workspace has not been trusted`，然后照旧跑完 —— 非交互调用不弹信任框，只是静默跳过那些设置。所以新机器上第一次必须**交互式**开一次。

### 那条 `claude plugin install` 消不掉

Claude Code v2.1.195 起，**只由项目 settings 启用、且来自外部来源的 plugin 不会自动安装**，它报"未安装"并把命令打出来。所以是"一次性两条命令"，不是一条。

### 三处扩展，各有各的理由

| 扩展 | 为什么 `init_machine.py` 非做不可 |
|---|---|
| `--prereqs` | 前置运行时检查必须在 **Python** 里。selfcheck 的 ENV 一步（PowerShell 版）要 pwsh 才能跑，而 pwsh 恰好是 Debian / macOS 上最可能缺的那一个 —— 一个在缺前置条件的机器上跑不起来的前置检查器不算检查器 |
| `--write-claude-dirs` | 这个产品没有共享构建系统，一个功能常常要同时改两三个仓库。不授权兄弟仓库，会话每读一次就弹一次权限 —— 那是持续一整天的摩擦，不是一次性的 |
| `HW_REPO` / `REF_REPO` | 不在探测表里，skill 就无从知道它们缺不缺 |

### 安装命令放代码，「不装会怎样」放文档

`PREREQS` 表里放**各平台的安装命令**（数据）；CLAUDE.md 第四节留**不装会怎样**（判断）。两边各放一半，谁也不重复谁。第四节原来那一列"本机已知可用的版本"删掉了 —— 那是一台机器的现状写进了一份声称机器无关的文档，`--prereqs` 现场打出来的才对。

## 分步计划

| 步 | 做什么 | 中间状态怎么验 |
|---|---|---|
| 1 ✅ | `AI-Skills` 骨架 + `skills/init/SKILL.md` | `claude plugin validate ./OpenPLC/Software`；`claude --plugin-dir` 免安装试跑 |
| 2 ✅ | `--prereqs` | 本机输出逐条核对；与 `machine.py` 的 `HOST_CC` 不能互相矛盾 |
| 3 ✅ | `--write-claude-dirs` + gitignore 护栏 | 408 条手工批准的 allow 一条不少；重跑是 no-op；解析不了的文件拒绝改写 |
| 4 ✅ | `HW_REPO` / `REF_REPO` + macOS 串口修复 | 单测 32 例全过 |
| 5 ✅ | 五个仓库的 `.claude/settings.json` + 缺的 `.gitignore` 行 | 每个仓库 `settings.local.json*` 都被**自己**的 `.gitignore` 挡住 |
| 6 ✅ | 文档归位 | 本文件 + BACKLOG + F5 + CLAUDE.md 四五八节 |
| 6b ✅ | **`tools/bootstrap.py`** —— 八步全自动，含安装 | `--dry-run` 逐步核对；`test_bootstrap.py` 20 例；真跑装上了 pyserial 并复查可见 |
| 7 ⬜ | **`AI-Skills` 推上 GitHub** | marketplace 拉得下来；`claude plugin install openplc@ai-skills` 成功 |
| 8 ⬜ | **第二台机器（Debian）从零走一遍** | 见验收 |
| 9 ⬜ | **macOS 走一遍** | 同上，外加下面那张待核实清单 |

## 验收

第 7 步之前只能验到"不回归"。**真正的验收在第二台机器上**：

1. 从零 clone `open_plc_cube_ide`，`claude plugin install openplc@ai-skills`，`/openplc:init`。
2. 走完六步，不需要人去翻 CLAUDE.md。
3. `python3 tools/common.py --probe` 全绿。
4. 用例：`pwsh ./tools/selfcheck.ps1` 全绿或只剩它自己报出的 SKIP（M7 未完成时 Debian 上仍需 pwsh）。
5. **把第一次真机运行的输出归档进本文件**，包括所有被打脸的推断 —— 这是 2026-08-20 Debian 那次当场产出两个修复的方式。

## macOS 待核实清单

mac 分支从来没在真机上跑过。以下**是推的，不是实测的**：

| 项 | 推的是什么 | 怎么核实 |
|---|---|---|
| `detect_ide()` | `/Applications/Arduino IDE.app/Contents` 底下拼 `resources/app/...`，而 app 包里实际是 `Contents/Resources/app`（大写 R） | APFS 默认大小写不敏感所以可能碰巧能过；**格式化成大小写敏感的盘上会挂**。两种都试 |
| `CUBE_PLUG` | CubeIDE 插件后缀 `macos64` | 对着真实安装目录看一眼 |
| `detect_a15()` | Arduino 数据目录 `~/Library/Arduino15` | 同上 |
| Homebrew 前缀 | Apple Silicon 是 `/opt/homebrew`，Intel 是 `/usr/local` | `HOST_CC` 走 PATH 上的 clang，应该无关；`--prereqs` 的 brew 命令与前缀无关 |

**已经修掉的一条不在上表**：`detect_log_ports()` 的 POSIX 分支原来只找 `/dev/ttyUSB*` 和 `/dev/ttyACM*`，**macOS 上这两个都不存在**（是 `/dev/cu.usbserial*` / `/dev/cu.usbmodem*`）。那不是疑点，是确定的缺陷，2026-08-21 直接修了并配了单测（用假文件系统，因为没有真机）。同时 `cu.*` 优先于 `tty.*`：打开 `tty.*` 设备会阻塞到 DCD 拉起，而三线 RS232 适配器永远不拉。

## 已否决

| 方案 | 为什么砍 |
|---|---|
| 六个仓库各放一份 skill 副本 | 同一份内容六个副本 —— 正是这套文档 2026-08-16 从 17 份笔记合并时要消掉的结构 |
| 放用户级 `~/.claude/skills/` | 不进 git，换台电脑就没了，与整条需求直接矛盾 |
| 让 Claude 跑 `sudo apt install` | 用户 2026-08-21 定的边界：**只报告，用户自己装**。装错了不容易回滚，而且 winget 的行为和 apt 差很远 |
| 把仓库地址、工具清单抄进 skill | skill 只写流程。唯一的例外是引导用的那一条 `open_plc_cube_ide` clone 地址 —— 要读到仓库表得先有那个仓库，消不掉，所以在文件里明写"这是例外、为什么" |
| 给 plugin 钉 `version` | 见上：钉了就不更新 |
| 往 `Hardware` / `Hello_World_OpenPLC` 也写 `settings.local.json` | 那两个是只读参考仓库，写进去没收益，还要多担一份私有文件泄漏的风险。它们仍然被**授权**给会话读，只是不作为写入目标 |
