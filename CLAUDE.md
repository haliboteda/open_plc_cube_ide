# 开工入口 —— 从这里开始

**这份文件是给 AI 会话看的。** 一台新机器、一个新会话，读完这份就能干活。

本仓库（`open_plc_cube_ide`，bootloader）是这套产品的**文档主仓**：跨仓库共享的事实全部在 `docs/` 下，其他仓库指过来，不抄。

> ⚠️ **本文件只写"怎么开工"，不写事实本身。** 想知道某条结论、某个引脚、某条需求的状态，一律去 `docs/` —— 那里是唯一出处。这份要是也抄一遍，两份就会漂移，而这套文档 2026-08-16 从 17 份分散笔记合并时，当场就抓到过一处互相矛盾。

---

## 一、这是什么产品

给 Schaeffer AG 的 OpenPLC 板子（STM32H743，64MB SDRAM，以太网 + USB CDC + RS232）做的**带签名校验的在应用编程（IAP）系统**。用户用 Arduino IDE 写 PLC 程序，通过网络或 USB 升级到板子上，bootloader 负责验签、暂存、提交，并保证**校验失败的升级不破坏已经装好的程序**。

**没有共享构建系统**，一个功能常常要同时改两到三个仓库。这是理解一切的前提。

## 二、先读这几份，顺序固定

| 顺序 | 文件 | 为什么在这个位置 |
|---|---|---|
| 1 | **[docs/STATUS.md](docs/STATUS.md)** | **★ 一张表看完全局：要做什么、做到哪、什么在挡路、先做哪个。** 12 行概览 + 优先级 + 54 条明细（需求和测试合并） |
| 2 | [docs/process/WORKING-AGREEMENTS.md](docs/process/WORKING-AGREEMENTS.md) | **协作规矩。改任何东西之前。** 包括"改动前先提方案"、"陈述问题的五项骨架"、"不要想当然"等硬约束 |
| 3 | [docs/design/ARCHITECTURE.md](docs/design/ARCHITECTURE.md) | 三个仓库在哪、哪些代码是跨仓镜像的、RTC 备份寄存器谁占了哪个 |
| 4 | [docs/work/ISSUES.md](docs/work/ISSUES.md) | 已知问题，按优先级排。手头没活了看这里 |

再往下按需取，全部索引在 [docs/INDEX.md](docs/INDEX.md)。`docs/` 按**这份文件多久变一次**分了五个目录：`design/`（很少变）、`test/`（每次跑用例）、`work/`（经常）、`process/`（很少）、`archive/`（只增不改）。

- **看到一个编号不知道是什么** → [docs/ID-MAP.md](docs/ID-MAP.md)。共 10 套编号，两两不撞（2026-08-22 消掉了三套撞车的）
- **要推一个结论之前** → [docs/archive/RETRACTED.md](docs/archive/RETRACTED.md)，13 条"当初以为是 X，实测否定了"
- 引用一个实测数字 → [docs/test/MEASUREMENTS.md](docs/test/MEASUREMENTS.md)，**唯一出处，别处只引用**
- 碰引脚 / 串口 / 启动模式 / SRAM4 → [docs/design/HARDWARE-FACTS.md](docs/design/HARDWARE-FACTS.md)
- 碰 bootloader 的状态扇区 / metadata / 事件日志 → [docs/design/JOURNAL.md](docs/design/JOURNAL.md)
- 碰签名密钥 / 信任根 / 扇区布局 → [docs/design/OWNERSHIP.md](docs/design/OWNERSHIP.md)
- 有人想重开一个已拍板的话题 → [docs/design/DECISIONS.md](docs/design/DECISIONS.md)
- 要编译、要测、或在查一个时有时无的问题 → [docs/test/BUILD-AND-TEST.md](docs/test/BUILD-AND-TEST.md)
- 开工与收尾流程 → [docs/process/SESSION-START.md](docs/process/SESSION-START.md)

🔴 **当前唯一的 P0 是一件硬件故障**：SDRAM 的 D1 线导通极弱，一条挡着四样东西。全部内容在 [docs/work/investigations/sdram-d1.md](docs/work/investigations/sdram-d1.md)，**接手第一件事**是跑一次已经写好但从没跑过的浮空测试（5 分钟，不需要硬件动手）。

## 三、换台机器：clone 什么

**这张表是新机器的第一步，别的地方没有第二份。**

**GitHub（`haliboteda`）是权威**，内网 Forgejo（`git.schaeffer-ag.de`）是镜像。**要 clone 就 clone GitHub 那份。**

| 仓库 | 装什么 | 地址（权威） | remote 名 |
|---|---|---|---|
| `open_plc_cube_ide` | bootloader（CubeIDE 工程）+ **全部共享文档** | `git@github.com:haliboteda/open_plc_cube_ide.git` | `origin` |
| `open_plc_arduino` | 定制 Arduino 板卡包（app 侧） | `git@github.com:haliboteda/open_plc_arduino.git` | `origin`（另有 `internal` 指向 Forgejo） |
| `IAPTranfer_Tool` | Go 写的 PC 工具 `IAPTool` + **全部测试资产 `TestTool/`** | `git@github.com:haliboteda/IAPTranfer_Tool.git` | `origin` |
| `Hello_World_OpenPLC` | 同一块板子的 CubeIDE **参考工程**（发明新写法之前先看它） | `git@github.com:haliboteda/Hello_World_OpenPLC.git` | `origin` |
| `Hardware` | 原理图、netlist、生产文件。**只有 Forgejo 一份** | `ssh://git@git.schaeffer-ag.de/OpenPLC_Alpha/Hardware.git` | `origin` |
| `package_index_json` | Arduino IDE 拉板卡包用的 index json | `git@github.com:haliboteda/package_index_json.git` | `origin` |
| `AI-Skills` | Claude Code 的 skill / plugin（`/openplc:init` 就在里面） | `git@github.com:haliboteda/AI-Skills.git` | `origin` |

前三个是必须的；`Hardware` 在核实引脚接法时必须有（**文档打架时以原理图为准**）；`Hello_World_OpenPLC` 是对照，不急；`package_index_json` 只在发板卡包时用。

**`AI-Skills` 要落到磁盘上**（`bootstrap.py` 会 clone 它）。plugin 部分确实是 Claude Code 自己去取的 —— 五个仓库的 `.claude/settings.json` 里都声明了那个 marketplace —— 但它还带一份 `_shared/rules/`，那些要**同步进 `~/.claude/rules/`** 才能常驻生效（plugin 没有"常驻指令"这个槽位），而同步需要一份 checkout。

**七个仓库各带一份 `CLAUDE.md`。** 另外六份只回答"本仓库是什么、在这里最容易踩什么坑"，然后指回本文件 —— 所以在哪个仓库里开会话都不会迷路，也不存在第二份安装说明。

⚠️ **七个地址全部走 SSH，一个 HTTPS 都不要用。** GitHub 已经**不接受密码推送**，HTTPS 的 remote 表现为"能 clone、能 fetch，一 push 就 `Invalid username or token`" —— 2026-08-20 `package_index_json` 就是这么卡住的，当天改成 SSH 才推上去。

放在同一个父目录下，后面的配置最省事：

```
<workspace>/
  open_plc_cube_ide/
  open_plc_arduino/
  IAPTranfer_Tool/
  Hardware/
  ref/Hello_World_OpenPLC/
  AI-Skills/
```

⚠️ **这个代码块就是"该在磁盘上有什么"的清单** —— `tools/bootstrap.py` 直接读它来决定 clone 什么，读上面那张表来取地址。`package_index_json` 故意不在里面（只在发板卡包时才要）。改这里就等于改 bootstrap 的行为，所以改完跑一下 `python3 tools/test_bootstrap.py`。

⚠️ **clone 完先确认分支，`git clone` 给的是默认分支。** 2026-08-21 实测：五个仓库里**三个**的远端默认分支是 `master`/`main`，而活在版本分支上（当天是 `v0.1.3.1` / `v0.1.3` / `v0.1.3-dev`）。照默认分支开工就是**悄无声息落后一整个版本** —— 和下面那条 Forgejo 警告同样的症状，不同的原因。

分支名每次发版都变，所以这里不写死任何一个：`python3 tools/init_machine.py` 会打出一张 **branches** 表（每个仓库当前在哪、远端默认是哪），**有的仓库在默认分支、有的不在时它直接警告**。

⚠️ **不要照 [README.md](README.md) 里那条 Forgejo 地址 clone `open_plc_cube_ide`。** 2026-08-19 实测：Forgejo 上那份**最新分支还停在 v0.1.2 时代**，既没有 `v0.1.2.1` 也没有 `v0.1.3.1`，反而多出两个 GitHub 上不存在的 `v0.1.2-lwip` / `v0.1.2-old` —— 双向分叉，说明那条 pull mirror 从来没真正跑过。照它 clone 会拿到一份差了一整个版本的代码。`open_plc_arduino` 和 `IAPTranfer_Tool` 两边分支一致，镜像是好的。

## 四、换台机器：装什么

**装什么、这台机器上装了没有、怎么装 —— 跑一条命令，别照着表手填：**

```bash
cd <workspace>/IAPTranfer_Tool/TestTool
python3 tools/init_machine.py --prereqs      # Windows 上是 python
```

它打出每一项在不在、版本是什么，**并给出这台系统上装它的确切命令**（apt / brew / winget / pip 按平台选）。安装命令只存在于 `tools/init_machine.py` 的 `PREREQS` 表里 —— 这里再抄一遍就是第二份，必漂移。

⚠️ **这一步不能用 selfcheck 的 ENV 一步代替。** ENV 那步的 PowerShell 版要 pwsh 才能跑，而 pwsh 恰好是 Debian / macOS 上最可能缺的那一个。

下面这张表只回答表里没有的那一半：**不装会怎样**。

| 工具 | 干什么用的 | 不装会怎样 |
|---|---|---|
| **git** | — | — |
| **Go** | 构建 `IAPTool` / `TestTool`；selfcheck 的 H1/H3 | 一半用例跑不了 |
| **Python 3** | selfcheck 的 K1-K6 / X1-X2 / DG1（假板子、加密交叉验证）；[M7](docs/work/M7-python-scripts.md) 之后是**全部**测试脚本的运行时 | 那三步报 SKIP；M7 完成后一个脚本都跑不了 |
| **`pyserial`** | 唯一一个要 `pip install` 的东西。M7 之后所有碰串口的用例都靠它 | 碰串口的用例报 SKIP（**点名说缺它**，不静默） |
| **原生 C 编译器** | selfcheck 的 H2：**直接编译 bootloader 的真实 C 源码**做主机侧单元测试 | H2 报 SKIP |
| **STM32CubeIDE** | 构建 bootloader；`STM32_Programmer_CLI` 烧写和读回 flash | 上不了板 |
| **Arduino IDE 2.x** | 构建 app；它自带的 `arduino-cli` 供 selfcheck 的 P4 用 | P4 报 SKIP，app 编不了 |
| **PowerShell 7 (`pwsh`)** | **Linux / macOS 上目前必装** —— 全套测试脚本还是 PowerShell。[M7](docs/work/M7-python-scripts.md) 改写成 Python 之后这一行就删掉 | 一个脚本都跑不了 |

最后两个是**安装目录**而不是 PATH 上的可执行文件，所以它们不在 `--prereqs` 里，在第五节的路径探测里。

⚠️ **C 编译器必须是现代版本。** Dev-C++ 带的是 2004 年的 GCC 3.4.2：它直接拒绝 `-std=c11`，用 `-std=c99` 时链接器会崩。拿二十年前的编译器去验证要交给 `arm-none-eabi-gcc 12.x` 的代码，比不测还糟。

⚠️ **Linux 上还要**：把用户加进 `dialout` 组，否则任何串口都打不开；ST-Link 需要 udev 规则，否则 SWD 表现得像板子没通电。

## 五、换台机器：配什么

### 最省事：一个脚本全干完

新机器上只有这个仓库时，直接跑：

```bash
python3 open_plc_cube_ide/tools/bootstrap.py      # Windows 上是 python
```

它按顺序做八件事：探 SSH → clone 缺的仓库 → **确认分支**（不是默认分支）→ 装缺的工具（**每条命令先打出来，问一次**）→ 探测本机路径 → 授权兄弟仓库给会话 → 装 `openplc` plugin → 自检并汇报。

| 想 | 加什么 |
|---|---|
| 先看它要干什么，一个字节都不改 | `--dry-run` |
| 无人值守，全取默认 | `--yes` |
| 只报告缺什么，绝不安装 | `--no-install` |
| 仓库放到别处 | `--workspace <dir>` |
| 只跑其中几步 | `--skip 4 --skip 7` |

**可以重复跑** —— 已经对的它保留，只补缺的。

⚠️ **`bootstrap.py` 破例放在本仓库的 `tools/`，不在 `IAPTranfer_Tool/TestTool/tools/`**（第八节的规矩）。理由是它必须在 `IAPTranfer_Tool` 存在**之前**就能跑，而这个仓库是你第一个 clone 的。它自己不实现任何逻辑：仓库表和目录布局**从第三节现读**，工具清单和安装命令**从 `init_machine.py` 的 `PREREQS` 现读**，一份都不抄。

### 手工：只配路径

```bash
cd <workspace>/IAPTranfer_Tool/TestTool
python3 tools/init_machine.py       # Windows 上是 python
```

它写出 `config/machine.ps1` 和 `config/machine.py`（都 gitignored），**两份同源生成，不会互相漂移**。

再跑一次带 `--write-claude-dirs`，把探测出来的兄弟仓库授权给 Claude Code（写进各仓库 `.claude/settings.local.json` 的 `additionalDirectories`）。不做这一步，会话每读一次兄弟仓库都要弹一次权限 —— 这个产品**没有共享构建系统**，一个功能常常要同时改两三个仓库，所以那是持续一整天的摩擦。

### 「初始化」——用户说这句话时

**跑 `/openplc:init`。** 完整流程在那个 skill 里，本文件不重述 —— 重述就是第二份，必漂移。

**它和 `bootstrap.py` 是同一件事的两条路，不是两套实现：**

| | 用在什么时候 | 谁在做决定 |
|---|---|---|
| `bootstrap.py` | 手上只有终端，想一条命令跑完 | 脚本按规则走，拿不准就问，`--yes` 时取默认 |
| `/openplc:init` | 已经在会话里 | AI 读脚本和 `init_machine` 的输出，替你判断该问什么、缺的东西影响哪些用例 |

**实质都在 `init_machine.py` 里**（探测什么、装什么、写哪些配置），两条路都只是外壳。所以改行为要改 `init_machine.py`，不是改这两个外壳中的某一个。

skill 装在 `AI-Skills` 仓库（第三节最后一行），由各仓库 `.claude/settings.json` 里声明的 marketplace 自动取来。**新机器上第一次要手工装一次 plugin**：

```bash
claude plugin install openplc@ai-skills
```

这一条消不掉 —— 来自外部来源的 plugin，Claude Code 不会因为项目 settings 声明了就自动安装，但它会把这条命令打给你。装完之后就只有 `/openplc:init` 一句。

它比下面这个循环多做三件 `init_machine.py` 做不到的事：clone 缺的仓库、检查前置运行时（第四节）、把兄弟仓库授权给会话。核心那段还是这个循环：

1. 跑 `python3 tools/init_machine.py`（我这类环境里它**不会提问**，见下）。
2. 读它输出的 **`not found`** 一节。每一项都带齐了：**这东西是什么、已经找过哪些地方、这个平台上的例子、以及固化它的确切命令**。
3. **把这几项问用户。** 连"已经找过哪些地方"一起给他 —— 否则对方合理的反应是"可是我装了啊"。
4. 用户把路径发过来，跑 `--set NAME=<path>`（可以一次给多个）固化。
5. 重复到只剩用户自己也没装的东西，然后跑 `python3 tools/common.py --probe` 确认。

**不要让用户手工编辑配置文件**，也不要替他猜路径。`--set` 是唯一的记录方式，它写进两份配置并且下次重跑会保留。

| | |
|---|---|
| 直接给答案 | `--set CUBEIDE=/opt/st/stm32cubeide_1.10.0` |
| 一次给多个 | `--set IDE=... --set CUBEIDE=...` |
| 清掉某一项 | `--set NAME=`（空值） |
| 装了新东西，重新找 | `--redetect CUBEIDE` |
| 只看不写、也不问 | `--check` |
| 只看缺哪些运行时 | `--prereqs`（第四节） |
| 授权兄弟仓库给会话 | `--write-claude-dirs` |

### 直接在终端里跑它的话

**它也会自己问。** 提问时给三行：这东西是什么、**它已经找过哪些地方**、这个平台上正确答案长什么样。粘进去就行 —— 从资源管理器复制带的引号会自动去掉，`~` 会展开，粘错一层目录（比如给 `/opt/st` 而不是 `/opt/st/stm32cubeide_1.10.0`）会当场指出来。直接回车 = 跳过。

⚠️ **它只在"确实有人能回答"时才提问。** 判据是两条同时成立：stdin 是终端，**且**环境里没有自动化标记（`CLAUDECODE` / `AI_AGENT` / `CI` / `GITHUB_ACTIONS` / `GIT_TERMINAL_PROMPT=0`）。光看 `isatty()` 不够 —— **AI 会话和 CI 任务都持有真终端，`isatty()` 在里面返回 True**，而没人看的提问不会报错，它会一直挂着。不提问时它会说出是哪条原因。`--no-input` 强制关，`--ask` 强制开。

**所以在 AI 会话里跑它就是上面那个循环**：它不问，它报告；问用户的是 AI。

已有且仍然成立的值会保留，所以手工调过的地方重跑不会被冲掉；第一次覆盖前会留一份 `.bak`。`--set` 给的路径即使不存在也会照写（可能是还没装），但会点名说它不存在 —— 打错字不该静默通过。

⚠️ **没有模板可抄了。** `machine.example.ps1` / `machine.example.py` 已删除 —— 它们和 `init_machine.py` 里的 `SETTINGS` 表是同一份清单的两个出处，必然漂移。而且模板那套"两个平台的值都给、删掉不用的那套"，**忘记删就是最常见的用法错误**：2026-08-20 第一次在 Debian 上就踩了。

**只往配置里放"这台机器"的值。** 凡是能从平台推出来的东西 —— 可执行文件后缀、板卡包里的 `win`/`linux`/`macosx` 子目录、Go 的输出目录、CubeIDE 插件的 `win32`/`linux64` 后缀 —— 全部由 `tools/_common.ps1`（Python 侧是 `tools/common.py`）自动推导，不要写进配置。判断标准：**另一台同样系统的机器会不会有不同的值？** 不会就不属于这里。

⚠️ **`CORE_LIVE` 结尾的板卡包版本号不要写死** —— 发版就变。`init_machine.py` 是用通配找出来的，升级板卡包后重跑一次即可。

selfcheck 的 **ENV** 一步会把这台机器上每一项解析成什么全部打出来，**缺什么点名说缺什么**，不静默跳过。全绿（或只剩它自己报出的 SKIP）就可以开工。

## 六、不在 git 里、必须单独处理的东西

| 东西 | 为什么不在 git 里 | 换机器怎么办 |
|---|---|---|
| `TestTool/config/machine.{ps1,py}` | 每台机器都不一样 | **不用搬也不用抄**：`python3 tools/init_machine.py` 自己探测生成 |
| `$CORE_LIVE`（Arduino IDE 实际加载的板卡包目录） | 是 IDE 的安装目录，不是仓库 | 装 Arduino IDE → 装 OpenPLC_Alpha 板卡包 → 用 `open_plc_arduino` 的内容覆盖它。⚠️ 方向是**单向的**，细节见 [docs/design/ARCHITECTURE.md](docs/design/ARCHITECTURE.md) |
| `IAPServer/keys/fw_signing_key.pem`（真签名私钥） | 私钥 | **2026-08-19 确认：目前没有真私钥，开发用仓库里的 `fw_signing_key.TEST_ONLY.pem`。** 将来产生真私钥后，它就是换机器时唯一必须手工搬运的文件 —— 到时候回来改这一行 |
| `.claude/settings.local.json` | 里面全是本机绝对路径的一次性命令，换台机器一条都匹配不上 | **不用搬。** 跨平台通用的那部分已经提交在 `.claude/settings.json` 里（只读命令 + `AI-Skills` marketplace 声明），新机器开箱就有；本机的 `additionalDirectories` 由 `--write-claude-dirs` 生成 |
| 安装好的 `openplc` plugin | Claude Code 的用户级状态，不是仓库内容 | 新机器上 `claude plugin install openplc@ai-skills` 一次。marketplace 已在 `.claude/settings.json` 里声明，所以不用记地址 |

⚠️ **`.claude/settings.json` 提交，`.claude/settings.local.json` 不提交。** **五个仓库**的 `.gitignore` 里各写了一行挡住后者（模式是 `.claude/settings.local.json*`，星号连 `.bak` 一起挡）。

之前只有两个仓库写了这一行，`Hardware` 和 `Hello_World_OpenPLC` 靠的是**本机的全局 gitignore** —— 换台电脑那条不存在，一提交就把本机私有配置推上去了。这个事故已经发生过一次，所以 `--write-claude-dirs` 现在**会先检查这一行在不在**：只被本机全局规则挡着的仓库，它拒绝写入并说要往哪儿加哪一行。

## 七、平台差异的处理规矩

1. **脚本里不写平台名。** 需要区分时用 `_common.ps1` 里的 `$PLATFORM` / `$EXE` / `$GOOS_DIR` / `$A15_DIR` / `$CUBE_PLUG`。
2. **相对路径一律用 `/`。** Windows 的 .NET 路径 API 全都接受正斜杠，Linux 不接受反斜杠 —— 所以 `/` 是唯一两边都对的写法。
3. **临时文件走 `Get-ScratchFile`**，不要用 `$env:TEMP`：那个变量在 Linux 上是空的，`"$env:TEMP/x.out"` 会变成往文件系统根目录写。
4. **可执行文件走 `Get-GoBin` / `Get-IapTool` / `Get-ProgrammerCli` / `Get-CubeIdeExe`**，不要拼 `.exe`。
5. **遇到一个新的、只有本机知道的路径 —— 加进 `tools/init_machine.py` 的 `SETTINGS` 表**（连同探测方式和一段说明），不要硬编码，也不要猜。加进去它就会在每台机器上被自动找出来，而不是变成又一条要人工填的说明。

> ⚠️ **2026-08-19 的改造只在 Windows 上验证过**（selfcheck 全绿）。Linux 那一半是按平台差异逐条消除的，**没有真机验证** —— 第一次在 Linux 上跑时，A0 打出来的那张表就是排查起点。

## 八、脚本和产物放哪

**任何要用第二次的东西，直接写进仓库里的固定位置**，不要放 `%TEMP%` / scratchpad —— 那些换台电脑就全废，而且不进 git、别人拿不到。

| 东西 | 去哪 |
|---|---|
| 测试脚本、自动化工具 | `IAPTranfer_Tool/TestTool/tools/` 或 `host/<主题>/`，判据写进 `TestTool/TEST-CASES.md` |
| 一次性探查命令（看个尺寸、grep 一下） | 不落盘，直接跑 |

**唯一的例外是 `open_plc_cube_ide/tools/bootstrap.py`**（连同它的 `test_bootstrap.py`）。它必须在 `IAPTranfer_Tool` 被 clone **之前**就能跑，所以只能放在你第一个 clone 的仓库里。除它之外不要往这个 `tools/` 目录加东西 —— 加了就是第二个脚本目录。

## 九、语言

| 内容 | 语言 |
|---|---|
| `docs/` 下的项目笔记、本文件 | 中文 |
| 代码注释、`#error` 文案、工具的 stdout/stderr、各仓库 `README.md` | **英文** |
| 对话 | 中文 |

## 十、收尾

用户说"今天结束 / 到此为止"时，按 [docs/process/SESSION-START.md](docs/process/SESSION-START.md) 的收尾流程把当天产生的东西全部归位，并提交推送三个仓库。判断标准只有一条：

> 换一台电脑、从零开一个新会话，照着这些文件能不能接着干？不能就是没归纳完。
