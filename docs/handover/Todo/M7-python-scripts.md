# M7 · 把测试脚本从 PowerShell 改写成 Python 3

**2026-08-19 立项。** 决定人：用户。

## 是什么

`$TOOL/TestTool/` 下的 **24 个 `.ps1`、3168 行**，从 PowerShell 改写成 Python 3。改完删掉 PowerShell 版本。

| 分类 | 个数 | 行数 | 特点 |
|---|---|---|---|
| 纯主机侧（selfcheck 那套） | 12 | ~1596 | 不碰串口、不碰 ST-Link，**可在任何机器上验证** |
| 需要板子 | 16 | ~1572 | 串口、ST-Link、IAPTool 子进程，**只能在接了板子的机器上验证** |

（两类有重叠：`_common.ps1` 两边都用。）

## 做什么用的

**让另一台 Debian 机器能完整替代 Windows 机器** —— 包括烧板和跑全部用例，不是只看代码。

Debian 上没有 PowerShell。三条路比过（见下），选了 Python，因为 **Windows 和 Debian 都已经需要 Python**（`selfcheck.ps1` 的 A10/A11/A12 就是 Python 写的假板子和加密交叉验证），所以两边都**不用额外装运行时**。

## 覆盖哪条需求

**[F5](../REQUIREMENTS.md)**（换台电脑能接着干）。当前状态是 ✅，但那个 ✅ 的前提是"换的是另一台 Windows"。

## 在哪找

- 脚本：`$TOOL/TestTool/tools/*.ps1`、`$TOOL/TestTool/host/**/*.ps1`
- 平台兼容层：`$TOOL/TestTool/tools/_common.ps1` —— **2026-08-19 刚做完双平台改造，它就是这次改写的规格说明**
- 本机配置：`$TOOL/TestTool/config/machine.ps1`（gitignored），模板 `machine.example.ps1`
- 判据：`$TOOL/TestTool/TEST-CASES.md`

## 不做会怎样

Debian 机器只能读代码、改代码，**跑不了任何一个用例、烧不了板**。等于多了一台编辑器，没多一台工作站。

## 设计思路

### 配置文件用 `machine.py`，不用 JSON / TOML

| 方案 | 问题 |
|---|---|
| **`machine.py`（采用）** | 和现在的 `machine.ps1` 一一对应，**注释能留**（现在那份模板的价值一大半在注释里），还能算派生值。`import` 即可，零解析代码 |
| JSON | **不支持注释**。现有模板里"为什么这个值长这样"的说明全丢 |
| TOML | 支持注释，但 `tomllib` 要 Python 3.11+，**本机是 3.10.4** |

### 平台派生量照搬 `_common.ps1` 的结构

2026-08-19 已经把平台差异收敛成五个量，Python 侧直接对应：

| PowerShell | Python |
|---|---|
| `$PLATFORM` | `sys.platform` 归一成 `windows`/`linux`/`macos` |
| `$EXE` | `".exe" if IS_WIN else ""` |
| `$GOOS_DIR` / `$A15_DIR` / `$CUBE_PLUG` | 同名常量，同样三张映射表 |
| `Get-ScratchFile` | `tempfile.gettempdir()` |
| `Get-GoBin` / `Get-IapTool` / `Get-ProgrammerCli` / `Get-CubeIdeExe` | 同名函数，`glob` 代替 `Get-ChildItem` 通配 |

⚠️ **`$IsWindows` 那个坑在 Python 里不存在**，但对应的坑是 `sys.platform` 在 Windows 上是 `win32` 而不是 `windows` —— 归一化只写一处。

### .NET API 的对应关系

| PowerShell 用的 | 用量 | Python |
|---|---|---|
| `System.IO.File` / `System.IO.Path` | 22 | `pathlib` / `open` |
| `Select-String` | 9 | `re` |
| `Start-Process` + 重定向 | 8 | `subprocess.Popen` |
| `Get-ChildItem -Recurse` | 8 | `pathlib.Path.rglob` |
| `Get-FileHash` | 7 | `hashlib` |
| `System.Net.Sockets.TcpClient` | 4 | `socket`（stdlib） |
| `System.IO.Ports.SerialPort` | 3 | **`pyserial`——唯一的新依赖** |
| `System.Net.Sockets.UdpClient` | 1 | `socket`（stdlib） |

⚠️ **`pyserial` 是唯一要 `pip install` 的东西。** 它要进 `requirements.txt` 和 CLAUDE.md 的安装清单，并且 `selfcheck` 缺它时要**点名报 SKIP**，不能静默。

⚠️ **Linux 上串口还要求用户在 `dialout` 组**，ST-Link 要 udev 规则 —— 这两条和语言无关，已经写在 CLAUDE.md 里。

## 方案对比（已决，不要重开）

| 方案 | Debian 要装 | Windows 要装 | 改多少 | 风险 | 结论 |
|---|---|---|---|---|---|
| 装 pwsh，脚本不动 | PowerShell 7 | 无 | 0 行 | 零 | 否决：用户不想为此装运行时 |
| 改写成 POSIX sh | 无 | 无（Git Bash） | 3168 行 | ⚠️ **最高**：串口要 `stty`+`cat`、UDP/TCP 要 `nc`/`socat`、改二进制字节要 `dd`，每一处都可能悄悄改判据 | 否决 |
| **改写成 Python 3** | 无 | 无 | 3168 行 → 约 2000 | 中 | **采用** |

## 分步计划

**每一步都必须能单独验证，不要合并。**

| 步 | 做什么 | 验什么 |
|---|---|---|
| ~~1~~ | ✅ **2026-08-19 已做**：`config/machine.{py,example.py}` + `tools/common.py` | 见下 |
| ~~2~~ | ✅ **2026-08-20 已做**：四个 `check-*.ps1` → `.py`（A0 属第 1 步） | **两版并排跑，输出逐字节比对 + 七个故障注入用例**。见下 |
| 3 | `host/` 下四套（bootloader_unit、crypto_ref、fakeboard、variant_check） | 同上：两版都跑，**通过/失败的用例集合必须完全相同** |
| 4 | `selfcheck.py` 串起 1–3 | 和 `selfcheck.ps1` 的 12 项结果逐项对照 |
| 5 | 板子侧 16 个（串口、烧写、跑用例） | ⚠️ **每个用例在同一块板子上先跑 ps1 版、再跑 py 版，判据必须一致** |
| 6 | 删掉 `.ps1`，更新 CLAUDE.md / WORKING-AGREEMENTS / TEST-CASES / SESSION-START | selfcheck 全绿；在 Debian 上完整跑一遍 |

⚠️ **第 5 步之前不要删任何 `.ps1`。** PowerShell 版是这次改写的**对照基准** —— 删了就没有东西能证明 Python 版测的还是同一件事。

⚠️ **验证在 Windows 上做。** Debian 上跑不了 `.ps1`，两版没法对照；Windows 上两版都能跑，所以对照必须在 Windows 完成，最后才拿去 Debian 验证可移植性。

### 第 1 步的实测结果（2026-08-19）

`python tools/common.py --probe` 与 `selfcheck.ps1` 的 A0 **除运行时版本那一行外逐行一致**（12 行全同）。

改写过程中抓到并当场消掉的一处差异：**`shutil.which()` 返回的扩展名带 `PATHEXT` 的大小写**，默认是大写，于是 Python 版打 `go.EXE` 而 PowerShell 版打 `go.exe`。功能上无所谓，但 M7 的验收方式就是逐行比对 —— **一个化妆品差异会训练人忽略 diff**，所以在 `show_cmd()` 里归一化了后缀大小写。

已知的**故意不一致**一处：探测命令时 Linux 上找 `python3` 而不是 `python`（Debian 上没有 `python` 这个名字）。Windows 上两者相同，所以不影响比对。

### 第 2 步的实测结果（2026-08-20）

改写了四个（不是五个 —— 第五个"A0"就是第 1 步的 `common.py --probe`）：

| PowerShell | Python | selfcheck 里是 |
|---|---|---|
| `check-version-sync.ps1` | `check_version_sync.py` | A7 |
| `check-mirror-sync.ps1` | `check_mirror_sync.py` | A8 |
| `check-core-sync.ps1` | `check_core_sync.py` | A9 |
| `check-public-root.ps1` | `check_public_root.py` | A14 |

**验收用两个新脚本做，都在 `$TOOL/TestTool/tools/`：**

| 脚本 | 干什么 | 结果 |
|---|---|---|
| `m7-compare.ps1` | 两版都当子进程跑，输出逐字节比对 | **4/4 完全一致** |
| `m7-compare-faults.ps1` | 逐个注入故障，再比对一次，用完在 `finally` 里还原 | **7/7 一致**，跑完两个仓库 `git status` 干净 |

⚠️ **只比对通过路径等于没比对。** 一个什么都不检查、只打印同样文字的 Python 脚本能轻松通过 `m7-compare.ps1`。所以七个故障用例是必需的，它们逼出的分支是：版本漂移、指纹漂移、单值 DIFF、**多段 DIFF**（FMC 39 个引脚那条，格式化逻辑比其余全部加起来还多）、SKIP（文件缺失）、ONLY-LIVE、ONLY-REPO。

> 这条直接来自 M5 的教训：用例在**未修的**代码上跑出干净的通过。一个"不再看任何东西"的翻译版会以完全相同的方式骗过验收。

**故障注入当场抓到一处真实分歧**（第一轮 7 个里错 1 个）：`check-public-root` 的 DRIFT 提示里写着"`-Print` 能生成这个常量"，而 Python 版的开关是 `--print`。照抄反而会把人指向一个不存在的开关，所以这是**必须的不一致**，登记进 `m7-compare.ps1` 的 `Known` 列表 —— 一条一条显式列出、每次命中都打印出来，而不是把比对放宽。

### 三个 PowerShell 语义陷阱（翻译时逐条对着改的）

**这三条不是格式差异，是会改判据的差异。**

| PowerShell | 行为 | Python 要写成 |
|---|---|---|
| `-replace` / `-match` / `-notmatch` | **大小写不敏感** | `re.sub(..., flags=re.I)` / `re.search(..., re.I)` |
| `[regex]::Match(...)` | **大小写敏感** | 默认即可 —— 和上一行是反的 |
| `-ne` / `-eq`（字符串） | **大小写不敏感** | 比十六进制指纹时要 `.lower()`，否则 `owner_slot.c` 换成大写字节就一边过一边不过 |
| `Select-Object -Unique` | **大小写不敏感** | 去重前先 `.lower()` |
| `Get-ChildItem -Recurse`（不带 `-Force`） | **不列隐藏项**；Windows 上 `.git` 带 Hidden 属性 | `os.walk` 里显式剪掉 `.git`，否则文件集合就不同了 |
| `Get-Content -Raw` | 保留 CRLF，吃掉 BOM | `open(..., newline="")` 再手工去 BOM（`common.py` 的 `read_text`）|

顺手修掉 `common.py` 里第 1 步遗留的一处同类问题：`assert_target_reachable` 用 `in` 做子串匹配，而原版是大小写不敏感的 `-match`。

### 第一次 Debian 实跑（2026-08-20）

跑的是 `python3 tools/common.py --probe`，机器上只有 Python 3.13.5 和 Go，别的都没装。**平台派生层在真机上没崩**：`platform linux`、`linux64` 插件后缀、`linux` 板卡包目录、找 `python3` 而不是 `python`、8 项缺失全部点名。这是这一层第一次离开 Windows。

抓到两个问题，都已修（`common.py` 和 `_common.ps1` 同步改，两版输出仍逐字一致）：

| 问题 | 为什么要紧 |
|---|---|
| `machine.example.*` 复制过去没编辑时，A0 只报 8 条 MISSING，不说"你这份配置是 Windows 的" | 模板设计成"两套值都给、删掉不用的那套"，那么**忘记删**就是最常见的用法错误，而它的表现是 8 条互不相关的缺失 |
| `COM5 does not exist -- check the adapter and the dialout group` | **建议是错的。** Linux 上 `COM5` 根本不是设备名，让人去查串口线和 dialout 组是把人往硬件上带 |

现在 A0 会在配置与平台不符时先打一段话点名是哪几个变量，并且把 `COMn` 单独识别成"配置里还是 Windows 那套"。**配置正确时一个字都不多打**，所以 Windows 上的 A0 输出没变（selfcheck 仍 12/12）。

⚠️ 已知的**故意不一致**又多一处：这段话里 Python 版写 `config/machine.py`、PowerShell 版写 `config/machine.ps1`。和既有的 MISSING 提示同理，Windows 上正常时都不打印，不影响比对。

⚠️ `COMn` 那条分支**只在 Windows 上按代码走查过**，没有实跑 —— 它只在非 Windows 上执行。下次 Debian 跑 A0 时确认。

### 第 2 步**没有**证明的事

- **`check-core-sync` 一次差多个文件时的行列顺序没验过。** 故障用例每次只加一个文件，所以 `Get-ChildItem -Recurse` 的遍历顺序和 `os.walk` 的排序遍历有没有分歧，目前不知道。判据（谁进 ONLY-LIVE / DIFF / ONLY-REPO、退出码）一定相同，**打印顺序可能不同**。真要多文件漂移时按行比对，先按行排序再比。
- **只在 Windows 上比过。** Debian 上跑不了 `.ps1`，没有对照基准 —— 这是 M7 从一开始就定下的，验证在 Windows 做。

---

## 验收

1. `selfcheck.py` 在 Windows 上 12/12 全过，且**每一项的结论和 `selfcheck.ps1` 相同**
2. 板子侧用例在同一块板上两版判据一致（至少覆盖 T1/T3/S1/S3/DG1/AU1/OW1）
3. 在 Debian 上完整跑一遍 selfcheck + 至少一次真实烧写
4. `TEST-CASES.md` 里每条用例的"怎么跑"改成 Python 命令

## 已否决

| 想法 | 为什么否决 |
|---|---|
| 一次性全部改完再验证 | 3168 行一起改，出问题无法二分定位。**判据变了都不会有人发现** |
| 改写时顺手优化脚本逻辑 | ⚠️ **绝对不行。** 这次只做语言翻译，行为必须一模一样 —— 否则"输出不一致"到底是翻译错了还是优化对了，说不清 |
| 先删 `.ps1` 再写 Python | 删掉对照基准 |
| 用 JSON 存配置 | 不支持注释，现有模板的说明会全部丢失 |
| 用 TOML | `tomllib` 要 Python 3.11+，本机 3.10.4 |

## 教训：这次最该防的是什么

**不是"写不出来"，是"写出来了、跑通了、但测的东西变了"。**

> M5 那次第一版用例用了 `Serial` 而不是 `Serial4`，在**未修的 core** 上跑出了干净的通过 —— 差点改完 core、看到绿灯、宣布修好，而用例从头到尾没碰过那个缺陷。（[../../WORKING-AGREEMENTS.md](../../WORKING-AGREEMENTS.md)）

一套"跑得通但测的东西变了"的脚本，比跑不了更危险。**所以每一步的验收都是"和旧版比对"，不是"新版能跑"。**
