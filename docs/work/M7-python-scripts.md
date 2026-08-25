# M7 · 把测试脚本从 PowerShell 改写成 Python 3

**2026-08-19 立项。** 决定人：用户。

## 是什么

`$TOOL/TestCase/` 下的 **24 个 `.ps1`、3168 行**，从 PowerShell 改写成 Python 3。改完删掉 PowerShell 版本。

| 分类 | 个数 | 行数 | 特点 |
|---|---|---|---|
| 纯主机侧（selfcheck 那套） | 12 | ~1596 | 不碰串口、不碰 ST-Link，**可在任何机器上验证** |
| 需要板子 | 16 | ~1572 | 串口、ST-Link、IAPTool 子进程，**只能在接了板子的机器上验证** |

（两类有重叠：`_common.ps1` 两边都用。）

## 做什么用的

**全套自动化测试用一种语言写，而那种语言是 Python。** 用户 2026-08-25 重申了这条要求。

选 Python 的理由：**这套东西已经需要 Python 了** —— selfcheck 里 K1–K6 / X1–X2 / DG1 就是 Python 写的假板子和加密交叉验证，三个文档检查 P7/P8/P9 也是。而 PowerShell 只被这 24 个脚本用。少一种运行时，就少一处只有一半脚本能跑的状态。

## 覆盖哪条需求

**F5**（全套自动化测试脚本用 Python 写，不需要 PowerShell），登记在 `$PROD/docs/STATUS.md`。

## 在哪找

- 脚本：`$TOOL/TestCase/tools/*.ps1`、`$TOOL/TestCase/host/**/*.ps1`
- 平台兼容层：`$TOOL/TestCase/tools/_common.ps1` —— **2026-08-19 刚做完双平台改造，它就是这次改写的规格说明**
- 本机配置：`$TOOL/TestCase/config/machine.{ps1,py}`（gitignored）。**2026-08-20 起由 `tools/init_machine.py` 探测生成，两份同源**，模板已删除
- 判据：`$TOOL/TestCase/TEST-CASES.md`

## 不做会怎样

**两套脚本永远并存。** 每加一条用例要写两遍，每改一处判据要改两处 —— 而两版之间没有任何机制强制一致，漂了不会有人知道。F5 也永远停在 🟡。

⚠️ **2026-08-25：立项时这一节写的是「Debian 机器等于多了一台编辑器」。那个理由作废了**（不再考虑迁移到其他电脑），但**模块本身不变** —— 用户当天重申：自动化测试工具必须用 Python 写。

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

⚠️ **2026-08-25：「Debian 要装」那一列已经不再是选型理由**（不再考虑迁移到其他电脑）。结论不变 —— 改用「全套自动化测试用一种语言、而那种语言是 Python」这条读，三行的排序完全一样。**表本身不动**，它记的是当时怎么判断的。

## 分步计划

**每一步都必须能单独验证，不要合并。**

| 步 | 做什么 | 验什么 |
|---|---|---|
| 1 | ✅ **2026-08-19 已做**：`config/machine.py` + `tools/common.py`（⚠️ `machine.example.py` 2026-08-20 已删，见下） | 见下 |
| 2 | ✅ **2026-08-20 已做**：四个 `check-*.ps1` → `.py`（机器探测属第 1 步） | **两版并排跑，输出逐字节比对 + 七个故障注入用例**。见下 |
| 3 | ✅ **2026-08-22 已做**：`host/` 下**六个脚本**（不是四套 —— fakeboard 有两个，还有 examples_build） | 同上：两版都跑，**通过/失败的用例集合必须完全相同**。见下 |
| 4 | ✅ **2026-08-22 已做**：`selfcheck.py` 串起 1–3 | 和 `selfcheck.ps1` 逐项对照（**当时 12 项**，后来加了 P7/P8/P9）。见下 |
| 5 | 板子侧 16 个（串口、烧写、跑用例） | ⚠️ **每个用例在同一块板子上先跑 ps1 版、再跑 py 版，判据必须一致** |
| 6 | 删掉 `.ps1`，更新 CLAUDE.md / TEST-CASES / CONVENTIONS | **Windows 上 selfcheck 全绿 + 板级用例全过。这就是全部验收条件** |

⚠️ **第 5 步之前不要删任何 `.ps1`。** PowerShell 版是这次改写的**对照基准** —— 删了就没有东西能证明 Python 版测的还是同一件事。

**2026-08-22 定的删除节奏：全部推到第 6 步一次性删**，中间这段两套并存。所以文档里「该跑哪一份」现在有一条明确规则：**无板那一层跑 Python，板级仍然只有 PowerShell**（第 5 步还没做）。

### 第 6 步要扫的文档清单（现在就记下来，免得那时漏）

删掉 `.ps1` 的同时，下面这些**具体断言会变成假的**，不是「措辞要更新」而是「内容错了」：

| 在哪 | 现在写的 | 第 6 步之后 |
|---|---|---|
| [../test/CASE-DESIGNS.md](../test/CASE-DESIGNS.md) 的「四层」表 | 层 0 / 层 1 的入口写的是 `tools\selfcheck.ps1` | 改成 `selfcheck.py`；那时应该已经是 `run_all.py` 的一层 |
| `$PROD/docs/STATUS.md` 的 F1 | F1 的证据是 `tools/selfcheck.ps1` | 改成 Python 那一版 |
| ~~那套 A0–A14 编号~~ | — | ✅ **2026-08-22 已删** —— 整个退役了，登记表在 `$PROD/docs/ID-MAP.md` |
| `$PROD/docs/design/ARCHITECTURE.md` 的路径变量一节 | 「对不上时以 ENV 为准」 | ✅ **2026-08-22 已改成不点名脚本** |
| `$TOOL:TestCase/acceptance/checklist.md`:27、:29 | A1–A3 / A7 一条命令 = `tools/selfcheck.ps1` | 同上 |
| `$TOOL:TestCase/TEST-CASES.md`:21、:58 | 目录树里的 `selfcheck.ps1`；以及那条 A0 断言 | 同上 |
| `$TOOL:TestCase/host/*/*.md` 三处 | 「Also run as step A10/A11/A12 of `tools\selfcheck.ps1`」 | ✅ **2026-08-22 已改**：改成用例号 + `selfcheck.py` |
| `$TOOL:CLAUDE.md`:36、:55 | `pwsh ./tools/selfcheck.ps1` | 同上 |
| [../../RELEASE-NOTES.md](../../RELEASE-NOTES.md):169 | 「now checked by `TestCase/tools/selfcheck.ps1`」 | 同上 |

⚠️ **本轮故意没有扫这张表。** 一次改二十几处、而被指向的文件还没删，只会留下一个半新半旧的状态 —— 而这套文档最怕的就是那个。**扫表和删文件必须在同一次提交里。**

⚠️ **验证靠两版逐字节对照，全部在 Windows 上完成。** 只有两版都能跑的机器上才有对照基准。

**2026-08-25：Debian 不再是验收门槛。** 优先保证 Windows 上无错，跨平台什么时候验、验不验，另说。跨平台的代码该怎么写照旧（`sys.platform` 归一化、`python3` vs `python`、`dialout` 组那些已经写好的分支都留着）—— **不写死 Windows，但也不为跑一遍 Debian 而排期。**

### 第 1 步的实测结果（2026-08-19）

`python tools/common.py --probe` 与 selfcheck 的 ENV 一步 **除运行时版本那一行外逐行一致**（12 行全同）。

改写过程中抓到并当场消掉的一处差异：**`shutil.which()` 返回的扩展名带 `PATHEXT` 的大小写**，默认是大写，于是 Python 版打 `go.EXE` 而 PowerShell 版打 `go.exe`。功能上无所谓，但 M7 的验收方式就是逐行比对 —— **一个化妆品差异会训练人忽略 diff**，所以在 `show_cmd()` 里归一化了后缀大小写。

已知的**故意不一致**一处：探测命令时 Linux 上找 `python3` 而不是 `python`（Debian 上没有 `python` 这个名字）。Windows 上两者相同，所以不影响比对。

### 第 2 步的实测结果（2026-08-20）

改写了四个（不是五个 —— 第五个"A0"就是第 1 步的 `common.py --probe`）：

| PowerShell | Python | selfcheck 里是 |
|---|---|---|
| `check-version-sync.ps1` | `check_version_sync.py` | A7 |
| `check-mirror-sync.ps1` | `check_mirror_sync.py` | A8 |
| `check-core-sync.ps1` | `check_core_sync.py` | A9 |
| `check-public-root.ps1` | `check_public_root.py` | P6 |

**验收用两个新脚本做，都在 `$TOOL/TestCase/tools/`：**

| 脚本 | 干什么 | 结果 |
|---|---|---|
| `m7-compare.ps1` | 两版都当子进程跑，输出逐字节比对 | **4/4 完全一致** |
| `m7-compare-faults.ps1` | 逐个注入故障，再比对一次，用完在 `finally` 里还原 | **7/7 一致**，跑完两个仓库 `git status` 干净 |

⚠️ **只比对通过路径等于没比对。** 一个什么都不检查、只打印同样文字的 Python 脚本能轻松通过 `m7-compare.ps1`。所以七个故障用例是必需的，它们逼出的分支是：版本漂移、指纹漂移、单值 DIFF、**多段 DIFF**（FMC 39 个引脚那条，格式化逻辑比其余全部加起来还多）、SKIP（文件缺失）、ONLY-LIVE、ONLY-REPO。

> 这条直接来自 M5 的教训：用例在**未修的**代码上跑出干净的通过。一个"不再看任何东西"的翻译版会以完全相同的方式骗过验收。

**故障注入当场抓到一处真实分歧**（第一轮 7 个里错 1 个）：`check-public-root` 的 DRIFT 提示里写着"`-Print` 能生成这个常量"，而 Python 版的开关是 `--print`。照抄反而会把人指向一个不存在的开关，所以这是**必须的不一致**，登记进 `m7-compare.ps1` 的 `Known` 列表 —— 一条一条显式列出、每次命中都打印出来，而不是把比对放宽。

### 第 3 步的实测结果（2026-08-22）

计划里写的是「四套」，实际是**六个脚本** —— fakeboard 目录下有两个，而 `examples_build` 当初漏记了：

| PowerShell | Python | 用例 | selfcheck 里是 |
|---|---|---|---|
| `host/bootloader_unit/build.ps1` | `build.py` | H2 | A2 |
| `host/variant_check/build.ps1` | `build.py` | P4 | P4 |
| `host/crypto_ref/run-checks.ps1` | `run_checks.py` | X1 / X2 | X1-X2 |
| `host/fakeboard/run-cases.ps1` | `run_cases.py` | K1–K6 | K1-K6 |
| `host/fakeboard/run-downgrade.ps1` | `run_downgrade.py` | DG1 | DG1 |
| `host/examples_build/build.ps1` | `build.py` | P5 | **不在 selfcheck 里**（约 10 分钟，故意排除） |

**验收：`m7-compare.ps1` 从 4 对扩到 10 对 → 10/10 逐字节相同、退出码相同。** `m7-compare-faults.ps1` 仍 **7/7**，且跑完工作区与跑之前**逐字节相同**（那次工作区本来就有未提交改动，它按字节存原文再写回，没有用 `git checkout`）。

`m7-compare.ps1` 为此加了两个机制：

| 机制 | 为什么需要 |
|---|---|
| 每对自带 `Dir` | 第 2 步四对都在 `tools/`，第 3 步六个在 `host/*/` |
| `Volatile`（正则，**两边同时掩掉**） | 两个 fakeboard 脚本会打印 `scratch: <随机临时目录>`。这不是「放宽比对」：`Known` 是把 A 侧改写成 B 侧的固定字符串，而这里两边都不可能相等，因为不是同一次运行。判据是**那段文字不承载任何结论**；命中时每次都打印出掩了什么 |

`examples_build` 这一对**只比对 `--only SDRAM` 的子集**。全量跑一次约十分钟，比对要跑两次 = 二十分钟，而对**脚本逻辑**没有增加任何覆盖 —— 一个库的例子已经走完除「例子更多」以外的每个分支。全量仍然是手工做的事。

#### ★ 抓到两个真实缺陷，都不是格式差异

**一、`DEVNULL` 不等于「没有终端」。** DG1 的 `ask-no-console` 用例要的是 IAPTool 检测到 stdin 不是终端。PowerShell 版写的是 `"" | & $iapRun`，给的是**管道**；我第一版用了 `subprocess.DEVNULL`，而 Windows 上 `DEVNULL` 就是 `NUL`，**`NUL` 是字符设备** —— Go 的 `os.Stdin.Stat()` 对它报 `ModeCharDevice`，于是 IAPTool 认为有终端、打出提示、读到 EOF、走了「操作员拒绝」分支。用例判 FAIL，**但失败原因和它要测的东西毫无关系**。正确写法是真管道（`input=""`）。

**二、`kill()` 不 `wait()` 会让两个假板子同时监听。** `fake_board.py:130` 的 TCP socket 设了 `SO_REUSEADDR`，所以上一个用例的进程还没死透时，下一个用例照样能 bind 同一个端口，**两个都在监听，谁 accept 是未定义的**。症状是某条用例报「board was never sent a flash command」，而它的日志里只有启动那一行 —— 因为连接被上一个进程接走、记到了上一个文件里。**这是偶发的**，第一轮失败第二轮就过，而偶发的用例比没有用例更糟。

⚠️ **PowerShell 版有同一个竞态**（`Stop-Process` 之后不等进程真的退出），只是它慢几毫秒，通常赢下来。Python 版修成了确定的（`kill()` + `wait()`，并在读日志前等日志停止增长）。**PS 版没改** —— 它在第 6 步之前仍是对照基准，改了就等于改基准。要不要一并修，见 [../../TODO.md](ISSUES.md)。

顺带修掉的一处重复：两个 fakeboard 脚本各抄了一份端口解析、IAPTool 暂存、假板子启动等五个辅助函数（PS 版注释里就写着「same as run-cases.ps1」）。Python 侧合成一份 `host/fakeboard/_common.py`。

### 第 4 步的实测结果（2026-08-22）

`tools/selfcheck.py`，**12/12 PASS，退出码 0，summary 表与 `selfcheck.ps1` 逐字节相同**。

⚠️ **这一对的判据不是「整段输出相同」，而是「每一项结论相同」**（当时 12 项） —— 也就是本表第 4 行原本写的那句。**整段相同做不到**，原因值得记下来：

> **PowerShell 有两条输出通道。** `Write-Host` 实时直达控制台；而原生命令的 stdout 进管道，被 `Step` 收进 `$out`，**等整步跑完才缩进两格打出来**。所以 `selfcheck.ps1` 的 X1-X2 那步里，`sha256_ref.py` 和 `ecdsa_verify.py` 的输出出现在那一步自己的 `===== result` **之后** —— 读起来像是先出结论再做验证。
>
> 要复现这个错序，就得让 `run_checks.py` 把子进程输出攒起来延后打 —— 而 `run_checks.py` 已经和 `run-checks.ps1` 证明过逐字节相同。**为了模仿一个错序去破坏一对已经通过的，是划不来的交换。** Python 版按事情发生的真实顺序打印。

`m7-compare.ps1` 为此加了第三个机制 **`Compare = "summary"`**：把比对收窄到 `===== summary` 之后那段，并且**每次都打印出「收窄了」**。这一对还刻意用 `-Quick`/`--quick` 跑 —— 那五个慢步骤上面各自都是一对、已经单独比过，在这里再跑一遍要多花十二分钟且不增加任何覆盖；这一对要验的是**编排和那张表**。

另外两处刻意不同（都让机器更好用，且在装齐工具的机器上不可能改变任何结论）：平台那一行报 Python 而不是 PowerShell（第 1 步就登记过的偏差）；**K1–K6 / X1–X2 / DG1 不再以「PATH 上有没有 `python`」为前置** —— 跑它们的就是当前这个解释器，没什么可查的，而 PS 版写死的 `python` 正是让这三步在只有 `python3` 的机器上 SKIP 的原因。

#### ★ 又抓到一个 PowerShell 陷阱，这次在 harness 自己身上

加第 11 对时 `m7-compare.ps1` 当场坏了，症状是：

```
selfcheck.py: error: unrecognized arguments: - - q u i c k
```

`@("--quick")` 被**逐字符** splat 了。根因是这个写法：

```powershell
$pyArgs = if ($p.PyArgs) { $p.PyArgs } else { @() }   # ❌
$pyArgs = @(if ($p.PyArgs) { $p.PyArgs } else { @() }) # ✅
```

**单元素数组从 `if` 块里出来会被解包成那个元素本身**（这里是字符串 `"--quick"`），而 `@` splat 一个字符串 = splat 它的字符数组。前面十对里唯一带参数的 `examples` 是 `@("-Only","SDRAM")` **两个**元素，数组保住了 —— 所以这个缺陷一直藏着，直到出现第一个只带一个参数的对。

⚠️ 顺带把 harness 的落盘时机也改了：**原始输出在任何收窄之前就写盘**，所以失败时留在磁盘上的永远是完整捕获，而不是被裁过的那段。就是靠这一条才两分钟内定位了上面这个 splat 问题 —— 第一版报的只有「两边都找不到 `===== summary`」。

### 第 5 步开工前先修掉的一个隐藏分歧：串口解码

`common.py` 的**串口那一半从 2026-08-19 写好起，从来没有任何东西 import 过**。第 5 步的 13 个板级脚本全都要靠它，所以先拿真板子跑了一次 —— 当场撞出两件事，**都不是崩不崩的问题，是「两版看到的不是同一份文本」**：

| | |
|---|---|
| **PS 侧** | .NET `SerialPort.ReadExisting()`，其 `Encoding` 默认是 **ASCIIEncoding** → 高于 0x7F 的字节一律变 `?` |
| **Python 侧（改前）** | `decode("utf-8", "replace")` → 同样的字节变 **U+FFFD** |

后果两条：

1. **同一份字节，两版渲染成不同字符。** 而第 5 步的判据正是「同一块板上两版判定相同」—— 抓到的日志一有差异，就得花时间确认那是不是用例的问题。
2. **U+FFFD 在 GBK 控制台上编不出来**，打印抓到的日志直接 `UnicodeEncodeError` 整个脚本挂掉。⚠️ **这不是边角情况**：app 启动时在 `[BOOT]` banner 前会吐一个乱码字节（[../../TODO.md](ISSUES.md) A2），所以**几乎每次上板都会碰到**。

修法是新增 `decode_serial()`：按 ASCII 解码，再把 U+FFFD 换成 `?` —— 同时对齐 .NET 的行为并消掉崩溃。**两个问题一处修好。**

> 这条值得单独记：它是「Python 版跑得起来、但看到的东西和 PS 版不一样」的教科书例子，而且如果等到第 5 步逐个脚本对照时才发现，会被误判成十三个脚本各自的 bug。

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
| `machine.example.*` 复制过去没编辑时，ENV 一步只报 8 条 MISSING，不说"你这份配置是 Windows 的" | 模板设计成"两套值都给、删掉不用的那套"，那么**忘记删**就是最常见的用法错误，而它的表现是 8 条互不相关的缺失 |
| `COM5 does not exist -- check the adapter and the dialout group` | **建议是错的。** Linux 上 `COM5` 根本不是设备名，让人去查串口线和 dialout 组是把人往硬件上带 |

现在 ENV 会在配置与平台不符时先打一段话点名是哪几个变量，并且把 `COMn` 单独识别成"配置里还是 Windows 那套"。**配置正确时一个字都不多打**，所以 Windows 上的 ENV 输出没变（selfcheck 仍 12/12）。

⚠️ 已知的**故意不一致**又多一处：这段话里 Python 版写 `config/machine.py`、PowerShell 版写 `config/machine.ps1`。和既有的 MISSING 提示同理，Windows 上正常时都不打印，不影响比对。

⚠️ `COMn` 那条分支**只在 Windows 上按代码走查过**，没有实跑 —— 它只在非 Windows 上执行。下次 Debian 跑 ENV 那步时确认。

### 第 2 步**没有**证明的事

- **`check-core-sync` 一次差多个文件时的行列顺序没验过。** 故障用例每次只加一个文件，所以 `Get-ChildItem -Recurse` 的遍历顺序和 `os.walk` 的排序遍历有没有分歧，目前不知道。判据（谁进 ONLY-LIVE / DIFF / ONLY-REPO、退出码）一定相同，**打印顺序可能不同**。真要多文件漂移时按行比对，先按行排序再比。
- **对照只在两版都能跑的地方做过。** 没有 PowerShell 的地方没有基准 —— 这是 M7 从一开始就定下的，验证在 Windows 做。

---

## 验收

1. `selfcheck.py` 在 Windows 上 12/12 全过，且**每一项的结论和 `selfcheck.ps1` 相同**
2. 板子侧用例在同一块板上两版判据一致（至少覆盖 T1/T3/S1/S3/DG1/AU1/OW1）
3. 删掉 `.ps1` 之后完整跑一遍 selfcheck + 至少一次真实烧写
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

> 同一条教训 M5 那次已经付过一次学费，记在 `$PROD/docs/CONVENTIONS.md`。

一套"跑得通但测的东西变了"的脚本，比跑不了更危险。**所以每一步的验收都是"和旧版比对"，不是"新版能跑"。**
