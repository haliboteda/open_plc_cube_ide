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
| 1 | [docs/WORKING-AGREEMENTS.md](docs/WORKING-AGREEMENTS.md) | **协作规矩。改任何东西之前。** 包括"改动前先提方案"、"陈述问题的五项骨架"、"不要想当然"等硬约束 |
| 2 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 三个仓库在哪、哪些代码是跨仓镜像的、RTC 备份寄存器谁占了哪个 |
| 3 | [docs/handover/REQUIREMENTS.md](docs/handover/REQUIREMENTS.md) | 54 条需求，每条做到没有、谁能证明 |
| 4 | [docs/handover/Todo/BACKLOG.md](docs/handover/Todo/BACKLOG.md) | 手头该干什么 |

再往下按需取，全部索引在 [docs/INDEX.md](docs/INDEX.md)：

- 碰引脚 / 串口 / 启动模式 / SRAM4 → [docs/HARDWARE-FACTS.md](docs/HARDWARE-FACTS.md)
- 碰 bootloader 的状态扇区 / metadata / 事件日志 → [docs/JOURNAL.md](docs/JOURNAL.md)
- 碰签名密钥 / 信任根 / 扇区布局 → [docs/OWNERSHIP.md](docs/OWNERSHIP.md)
- 要编译、要测、或在查一个时有时无的问题 → [docs/BUILD-AND-TEST.md](docs/BUILD-AND-TEST.md)
- 开工与收尾流程 → [docs/handover/SESSION-START.md](docs/handover/SESSION-START.md)

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

前三个是必须的；`Hardware` 在核实引脚接法时必须有（**文档打架时以原理图为准**）；`Hello_World_OpenPLC` 是对照，不急；`package_index_json` 只在发板卡包时用。

**六个仓库各带一份 `CLAUDE.md`。** 另外五份只回答"本仓库是什么、在这里最容易踩什么坑"，然后指回本文件 —— 所以在哪个仓库里开会话都不会迷路，也不存在第二份安装说明。

⚠️ **六个地址全部走 SSH，一个 HTTPS 都不要用。** GitHub 已经**不接受密码推送**，HTTPS 的 remote 表现为"能 clone、能 fetch，一 push 就 `Invalid username or token`" —— 2026-08-20 `package_index_json` 就是这么卡住的，当天改成 SSH 才推上去。

放在同一个父目录下，后面的配置最省事：

```
<workspace>/
  open_plc_cube_ide/
  open_plc_arduino/
  IAPTranfer_Tool/
  Hardware/
  ref/Hello_World_OpenPLC/
```

⚠️ **不要照 [README.md](README.md) 里那条 Forgejo 地址 clone `open_plc_cube_ide`。** 2026-08-19 实测：Forgejo 上那份**最新分支还停在 v0.1.2 时代**，既没有 `v0.1.2.1` 也没有 `v0.1.3.1`，反而多出两个 GitHub 上不存在的 `v0.1.2-lwip` / `v0.1.2-old` —— 双向分叉，说明那条 pull mirror 从来没真正跑过。照它 clone 会拿到一份差了一整个版本的代码。`open_plc_arduino` 和 `IAPTranfer_Tool` 两边分支一致，镜像是好的。

## 四、换台机器：装什么

| 工具 | 干什么用的 | 不装会怎样 | 本机已知可用的版本 |
|---|---|---|---|
| **git** | — | — | 2.30.1 |
| **Go** | 构建 `IAPTool` / `TestTool`；selfcheck 的 A1/A3 | 一半用例跑不了 | go 1.23.1 |
| **Python 3** | selfcheck 的 A10/A11/A12（假板子、加密交叉验证）；[M7](docs/handover/Todo/M7-python-scripts.md) 之后是**全部**测试脚本的运行时 | 那三步报 SKIP；M7 完成后一个脚本都跑不了 | 3.10.4 |
| **`pyserial`** | 唯一一个要 `pip install` 的东西。M7 之后所有碰串口的用例都靠它 | 碰串口的用例报 SKIP（**点名说缺它**，不静默） | `pip install -r IAPTranfer_Tool/TestTool/requirements.txt`；Debian 上也可 `apt install python3-serial` |
| **原生 C 编译器** | selfcheck 的 A2：**直接编译 bootloader 的真实 C 源码**做主机侧单元测试 | A2 报 SKIP | Windows: MinGW-W64 gcc 16.1.0；Linux: 系统 gcc 即可 |
| **STM32CubeIDE** | 构建 bootloader；`STM32_Programmer_CLI` 烧写和读回 flash | 上不了板 | 1.10.0 |
| **Arduino IDE 2.x** | 构建 app；它自带的 `arduino-cli` 供 selfcheck 的 A13 用 | A13 报 SKIP，app 编不了 | arduino-cli 1.5.1 |
| **PowerShell 7 (`pwsh`)** | **Linux / macOS 上目前必装** —— 全套测试脚本还是 PowerShell。[M7](docs/handover/Todo/M7-python-scripts.md) 改写成 Python 之后这一行就删掉 | 一个脚本都跑不了 | Windows 上 5.1 即可，脚本兼容两者 |

⚠️ **C 编译器必须是现代版本。** Dev-C++ 带的是 2004 年的 GCC 3.4.2：它直接拒绝 `-std=c11`，用 `-std=c99` 时链接器会崩。拿二十年前的编译器去验证要交给 `arm-none-eabi-gcc 12.x` 的代码，比不测还糟。

⚠️ **Linux 上还要**：把用户加进 `dialout` 组，否则任何串口都打不开；ST-Link 需要 udev 规则，否则 SWD 表现得像板子没通电。

## 五、换台机器：配什么

**全套脚本只认一个文件**：`IAPTranfer_Tool/TestTool/config/machine.ps1`（gitignored）。

```powershell
cd <workspace>/IAPTranfer_Tool/TestTool
Copy-Item config/machine.example.ps1 config/machine.ps1   # 然后编辑
pwsh ./tools/selfcheck.ps1          # Windows 上也可以直接 .\tools\selfcheck.ps1
```

模板里 **Windows 和 Linux 两套示例值都给了**，删掉不用的那套即可。

**只往 `machine.ps1` 里放"这台机器"的值。** 凡是能从平台推出来的东西 —— 可执行文件后缀、板卡包里的 `win`/`linux`/`macosx` 子目录、Go 的输出目录、CubeIDE 插件的 `win32`/`linux64` 后缀 —— 全部由 `tools/_common.ps1` 自动推导，不要写进配置。判断标准：**另一台同样系统的机器会不会有不同的值？** 不会就不属于这里。

`selfcheck.ps1` 的 **A0** 会把这台机器上每一项解析成什么全部打出来，**缺什么点名说缺什么**，不静默跳过。全绿（或只剩它自己报出的 SKIP）就可以开工。

## 六、不在 git 里、必须单独处理的东西

| 东西 | 为什么不在 git 里 | 换机器怎么办 |
|---|---|---|
| `TestTool/config/machine.ps1` | 每台机器都不一样 | 从 `machine.example.ps1` 抄一份改 |
| `$CORE_LIVE`（Arduino IDE 实际加载的板卡包目录） | 是 IDE 的安装目录，不是仓库 | 装 Arduino IDE → 装 OpenPLC_Alpha 板卡包 → 用 `open_plc_arduino` 的内容覆盖它。⚠️ 方向是**单向的**，细节见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| `IAPServer/keys/fw_signing_key.pem`（真签名私钥） | 私钥 | **2026-08-19 确认：目前没有真私钥，开发用仓库里的 `fw_signing_key.TEST_ONLY.pem`。** 将来产生真私钥后，它就是换机器时唯一必须手工搬运的文件 —— 到时候回来改这一行 |
| `.claude/settings.local.json` | 里面全是本机绝对路径的一次性命令，换台机器一条都匹配不上 | **不用搬。** 跨平台通用的那部分已经提交在 `.claude/settings.json` 里（只读命令：`git status/log/diff`、`go vet`、`command -v` 等），新机器开箱就有 |

⚠️ **`.claude/settings.json` 提交，`.claude/settings.local.json` 不提交。** 两个仓库的 `.gitignore` 里各写了一行挡住后者 —— 之前它只被**本机的全局 gitignore** 挡着，换台电脑那条不存在，一提交就把本机私有配置推上去了。

## 七、平台差异的处理规矩

1. **脚本里不写平台名。** 需要区分时用 `_common.ps1` 里的 `$PLATFORM` / `$EXE` / `$GOOS_DIR` / `$A15_DIR` / `$CUBE_PLUG`。
2. **相对路径一律用 `/`。** Windows 的 .NET 路径 API 全都接受正斜杠，Linux 不接受反斜杠 —— 所以 `/` 是唯一两边都对的写法。
3. **临时文件走 `Get-ScratchFile`**，不要用 `$env:TEMP`：那个变量在 Linux 上是空的，`"$env:TEMP/x.out"` 会变成往文件系统根目录写。
4. **可执行文件走 `Get-GoBin` / `Get-IapTool` / `Get-ProgrammerCli` / `Get-CubeIdeExe`**，不要拼 `.exe`。
5. **遇到一个新的、只有本机知道的路径 —— 加进 `machine.example.ps1` 并告诉用户**，不要硬编码，也不要猜。

> ⚠️ **2026-08-19 的改造只在 Windows 上验证过**（selfcheck 全绿）。Linux 那一半是按平台差异逐条消除的，**没有真机验证** —— 第一次在 Linux 上跑时，A0 打出来的那张表就是排查起点。

## 八、脚本和产物放哪

**任何要用第二次的东西，直接写进仓库里的固定位置**，不要放 `%TEMP%` / scratchpad —— 那些换台电脑就全废，而且不进 git、别人拿不到。

| 东西 | 去哪 |
|---|---|
| 测试脚本、自动化工具 | `IAPTranfer_Tool/TestTool/tools/` 或 `host/<主题>/`，判据写进 `TestTool/TEST-CASES.md` |
| 一次性探查命令（看个尺寸、grep 一下） | 不落盘，直接跑 |

## 九、语言

| 内容 | 语言 |
|---|---|
| `docs/` 下的项目笔记、本文件 | 中文 |
| 代码注释、`#error` 文案、工具的 stdout/stderr、各仓库 `README.md` | **英文** |
| 对话 | 中文 |

## 十、收尾

用户说"今天结束 / 到此为止"时，按 [docs/handover/SESSION-START.md](docs/handover/SESSION-START.md) 的收尾流程把当天产生的东西全部归位，并提交推送三个仓库。判断标准只有一条：

> 换一台电脑、从零开一个新会话，照着这些文件能不能接着干？不能就是没归纳完。
