# 开工入口 —— 从这里开始

**这份文件是给 AI 会话看的。** 一台新机器、一个新会话，读完这份就能干活。

本仓库（`open_plc_cube_ide`，bootloader）是这套产品的**文档主仓**：跨仓库共享的事实全部在 `docs/` 下，其他仓库指过来，不抄。

> ⚠️ **本文件只写"怎么开工"，不写事实本身。** 想知道某条结论、某个引脚、某条需求的状态，一律去 `docs/` —— 那里是唯一出处。这份要是也抄一遍，两份就会漂移 —— **这是规律不是意外**，理由和先例在 [docs/INDEX.md](docs/INDEX.md) 的「维护约定」。

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

再往下按需取，**完整路由表只有一份，在 [docs/INDEX.md](docs/INDEX.md)** —— 碰到编号、要推结论、引用数字、碰引脚/串口/密钥/扇区、要编译、要收尾……那份文件按"多久变一次"分了五个目录列好了去哪查。这里不重复列一份，重复就会漂移（[docs/INDEX.md](docs/INDEX.md) 的「维护约定」记过一次这类漂移的代价）。

**当前的 P0 在 [docs/STATUS.md](docs/STATUS.md) 第二节**，那里一张表排好了先做哪个。

> 2026-08-21 到 08-23 之间那条「SDRAM D1 线」的 P0 **已经不在了** —— 换了一块新的 upper deck，SDRAM 好用了。旧板子的排查记录留在 [docs/work/investigations/sdram-d1.md](docs/work/investigations/sdram-d1.md)，**根因始终没定位到**，所以万一再出同样的症状，从那份开始看。

## 三、换机器、装机、配路径 —— 不在这个仓库里

装什么、clone 什么、配什么，**全部在 `portable` plugin 里**（`/portable:init`，或直接跑它的 `bootstrap.py`）。这个仓库 2026-08-24 之前是引导入口，现在不是了 —— 一条说明都不留在这里，留就是第二份，必漂移。

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
| 测试脚本、自动化工具 | `IAPTranfer_Tool/TestCase/tools/` 或 `host/<主题>/`，判据写进 `TestCase/TEST-CASES.md` |
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
