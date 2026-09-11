# 端口工装第一次上板 —— 清单与待填结果

**这份文件的用途只有一个：把「只有真硬件能回答的问题」列全，并留出填结果的地方。**

工装的主机侧覆盖已经很厚（用例 **H4**：固件侧 114 项 + 上位机侧 24 项，带 `-race`，见 `$TOOL/TestCase/host/porttool_caps/`；产线框架和方案页另有 45 项在 `$TOOL/TestCase/host/porttool_plan/`），但**整套东西一次没上过真板**。下面每一条都是主机侧判不了的。

规程与协议在 [../design/PORTTOOL-FLOW.md](../design/PORTTOOL-FLOW.md)，取舍在 [../design/DECISIONS.md](../design/DECISIONS.md) 第 9–25 条。产线框架的形状在 [../design/PRODUCTION-FRAMEWORK.md](../design/PRODUCTION-FRAMEWORK.md)。本文不重复，只列动作和结果。

> **状态：全部待测。**结果填进下面的「实测」列，日期填「测于」列。已经填上的行不要再动 —— 改结论就改那一行，不要另起一份。

---

## 0 · 前置

| 做什么 | 怎么做 |
|---|---|
| 编出工装镜像 | `cd $TOOL/TestCase && python tools/build_image.py --porttool`。**不用再去工程属性里改宏** —— 符号是构建时传的，不写进 `.cproject`（[../design/DECISIONS.md](../design/DECISIONS.md) 第 14 条 2026-09-08 更新） |
| 确认编对了 | 脚本自己查：编工装时构建日志里**必须**有那条 `PORTTOOL_ENABLE=1 … NOT a bootloader` 的 warning，没有就报错退出 |
| 编回 bootloader | `python tools/build_image.py`（不带参数）。或者 `--both` 两个都编，**bootloader 留在最后** |
| 烧 | ST-Link 直接烧，不涉及 IAP、不涉及授权 |
| 接控制口 | USB-RS232 适配器接端子 **C05 (TxD) / C06 (RxD) / C02 (GND)**，115200 8N1 |
| 起面板 | 双击 `$TOOL/Output/windows/PortTool.exe`，浏览器自动开 |
| 方案页有没有东西 | 看 exe 同目录的 `plans/`，`compile_tool.sh` 每次构建刷新它。空的就是没跑过构建脚本 |

⚠️ **端子是真 ±12 V，必须用 USB-RS232 适配器。接 TTL 适配器可能烧掉**（[../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)）。

✅ **不再需要「记得把符号改回去」** —— 它从来没进过工程文件。但 `Debug/` 里留着的是最后一次构建的产物，所以**测完跑一次不带参数的 `build_image.py`**，让那里恢复成 bootloader。

**2026-09-08 实测的两个尺寸**（`build_image.py --both`）：

| 镜像 | `.bin` | 余量 | 构建 |
|---|---|---|---|
| bootloader | 102,864 | 20,016 | 0 errors **0 warnings** |
| 工装 | 104,096 | — | 0 errors，1 warning = 那条刻意的 `#warning`。**没有限值**，见下 |

✅ **工装镜像不受尺寸约束** —— 用户 2026-09-08 明确：**工装镜像直接 ST-Link 整片烧，不走 IAP，尺寸不用考量。**那条 122,880 的门禁只属于 bootloader（需求 E3：单个 128K 扇区的前 120K，尾部 8K 给 owner 记录）。上表里工装那一行的「余量」只是记录，不是判据。

⚠️ **历史记录**：在两份脚本分开之前，两种镜像共用 `FLASH LENGTH = 120K`，工装第一次真编时溢出 **47,608 字节**（`main.c` 在调完 `PortTool_Run()` 之后的 IAP / lwIP / USB / mbedTLS 在编译器看来仍然可达），靠给 `PortTool_Run()` 加 `noreturn` 省了约 71 KB 才过。`noreturn` 那条**仍然要留着** —— 它不只是省空间，还是「那段代码在这个镜像里到不了」这件事的唯一声明。

✅ **2026-09-08 彻底解开了**：工装有了自己的链接脚本 `STM32H743IIKX_FLASH_PORTTOOL.ld`（`FLASH LENGTH = 2048K`），哪次构建用哪份由 `PLC_LD_SCRIPT` 环境变量决定，**不用加构建配置** —— 见 [DECISIONS.md 第 14 条](../design/DECISIONS.md) 的 2026-09-08 补充。所以上表里工装那一行的字节数只是记录。

---

## 1 · 三个只有你能量的数

这三条是**主机侧永远判不出来**的，也是继续往下做之前最该有的输入。

| # | 量什么 | 怎么量 | 期望 | 实测 | 测于 |
|---|---|---|---|---|---|
| **M1** | **VNQ5160K-E 的 PWM 上限** | 面板勾 Digital Out，`mode=hold duty=50`，`freq=` 从 100 扫到 2000 Hz，示波器探端子 **A03** | 未知 —— 它是智能高侧开关，内部有电荷泵和保护逻辑，**手上没有它的数据手册** | ⚠️ 未测：边沿从 ___ Hz 起变糊 | — |
| **M2** | **模拟前端跳线 JP1–JP9 焊没焊** | 面板同时起 Analog In 和 Analog Out，改 aout 的 `mv=`，看 ain 读数跟不跟着动 | 跟着动 = 焊了；不动且落在约 **380 mV** 悬空带（200–650 LSB）= 没焊 | ⚠️ 未测 | — |
| **M3** | **AOUT 的实际电流** | 万用表串在 **Analog Out 1 → 表 → Analog GND** 的回路里，面板设 `mv=1:500`，再设 `1:1500` | **4.883 mA / 14.648 mA**（`Iout = Vin × 10 / 1024`，只在 JP3/JP4 开路时成立） | ⚠️ 未测 | — |

**M1 的结果请告诉我，我补进 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)。**`freq` 参数刻意放开到 2000 Hz 上限就是为了让这个数被量出来，而不是被假设（[../design/DECISIONS.md](../design/DECISIONS.md) 第 10 条）。

⚠️ **M2 要先确认再动手：跳线焊上去不可逆。**

⚠️ **M3 的电流没有任何板上通道能读回。**帧里那个 `预期微安` 是算出来的「应该是多少」，不是测量值 —— **万用表是唯一判据**，这个端口存在的意义就在这里。

---

## 2 · 顺手能验的，性价比最高

| # | 验什么 | 接线 | 判据 | 结果 | 测于 |
|---|---|---|---|---|---|
| **B1** | **DO → DI 一次覆盖 16 个通道** | **一根八芯线**把 A03–A10 接到 D02–D09。DO 出 24 V、DI 耐 24 V | 面板同时起 dout（`mode=blink duty=100 period=500`）和 din。**DO 每翻一次，DI 的 `v=` 位域跟着翻，逐位对应** | ⚠️ 未测 | — |
| **B2** | **RS485 链路回环在面板上闭合** | 第二个 USB-RS485 适配器：A 接端子 **A10**（J11-3），B 接 **A11**（J11-2）。在 rs485 卡上把它绑成「对端串口」 | `!rs485` 的 `seq − rx` 恒为 1，`miss` 保持 0，`junk` 保持 0 | ⚠️ 未测 | — |
| **B3** | **RS232 自己那条通道持续通** | 就是控制口那条线 | `!rs232` 的 `miss` 保持 0，`rxlines=` 一直涨 | ⚠️ 未测 | — |
| **B4** | **继电器六路各自独立** | 听咔哒，或量 LowerDeck T2–T7 栅极 | `on=1:1,2:0,3:1` 时只有 1、3 吸合。⚠️ **面板只报「已驱动」不报「已吸合」** —— 板上没有触点回读 | ⚠️ 未测 | — |
| **B5** | **`pt.run sdram.probe` 在真 FMC 上答得对** | 不接线 | 先出几行 `SDRAM_TEST:` 散文，**最后一行**是 `OK sdram.probe base=0xC0000000 size=67108864 ready=1 databus=1 addrbus=1`。三个标志全 1。<br/>⚠️ **`ready=1` 本身就是一条修复的验证**：工装镜像跑在 main.c 的 Phase 1，那里 `MX_FMC_Init()` 还没运行，2026-09-07 之前每个 SDRAM 入口都报 `not_initialised` | ⚠️ 未测 | — |
| **B6** | **`pt.run rtc.read` 能自己把 RTC 拉起来** | 不接线 | `OK rtc.read init=… clk=lsi date=… time=…`。`init=0` 正常（没人设过日历）；**`clk=lsi` 要确认** —— 谈 RTC 精度之前先知道时钟源不是晶振 | ⚠️ 未测 | — |
| **B7** | **`pt.run led.blink` 真的点亮** | 不接线，**眼睛盯着板子** | 敲下去之后**六次闪烁、每次半秒**。回复是 `observed=unknown` —— 固件报不了「看见了」，这一条只有你能判 | ⚠️ 未测 | — |
| **B10** | **CAN 内部回环闭合（不接线）** | 不接线 | 面板起 `can`，`mode=loopback`。`seq − rx` 恒为 1、`miss` 保持 0、`tx` 和 `rx_frames` 一起涨。⚠️ **这只证明控制器、时钟和位时序**，与收发器和总线无关 | ⚠️ 未测 | — |
| **B11** | **CAN 真的上总线** | 端子 **C07/C08** 接一个 CAN 对端，波特率对齐 | `mode=normal`。⚠️ **孤立节点没有应答，`tec` 会涨** —— 那是总线在说实话不是工具的毛病。⚠️ 收发器是隔离的（ISO1044），**隔离侧由 U7 供电：U7 坏了的表现是 `alive=1`、`tx` 在涨、`rx_frames` 恒为 0** | ⚠️ 未测 | — |
| **B12** | **SD 卡两个一次性动作** | 插一张卡 | `pt.run sd.probe` 报 `detected=1 ready=1` 和容量；`pt.run sd.integrity` 报 `identical=1` 且两个 CRC 相同 | ⚠️ 未测 | — |
| **B13** | **`pt.run sdram.sweep` 整片跑得过，而且要多久** | 不接线 | `mismatches=0` `patterns=4`。**`write_ms` + `verify_ms` 这两个数要记下来** —— 产线方案里那个 180 秒的 `timeout_ms` 现在是猜的，实测之后才能定。⚠️ 几十秒别以为卡死了 | ⚠️ 未测 | — |
| **B14** | **`pt.run sdram.retention` 证明自动刷新在跑** | 不接线 | `checked=64 failed=0`，`wait_ms=5000`。⚠️ **这一条和 B13 不是同一件事**：全片扫描过了、这一条照样可能栽 —— 刷新没配好时数据是在等待的那 5 秒里丢的 | ⚠️ 未测 | — |
| **B15** | **`pt.run sd.stress` 64 轮不掉** | 插一张卡 | `passes=64 passed=64 first_bad_pass=0`。**`elapsed_ms` 要记下来**，同样是产线 `timeout_ms` 的依据。⚠️ 如果看到 `passes` 小于 64，那是中途停了（卡不答了就不再耗超时），`first_bad_pass` 是停在第几轮 | ⚠️ 未测 | — |
| **B16** | **`pt.caps` 里有四行 `kind=run`** | 不接线 | `sd` `sdram` `rtc` `led` 各一行，带 `runs=`；`sd` 和 `sdram` 那两行还带 `targets=`（挂上去的交权入口）。**没有任何两行共用同一个 `port=`** | ⚠️ 未测 | — |
| **B17** | **工站 6 方案在真板子上跑完** | 要工装板 / 对端 / 卡 / KNX 电源，见方案里每步的 `_note` | `porttool.exe run <仓>/TestCase/plans/station6-poweron.json --port COMx --csv s6.csv`。⚠️ **缺哪个外部件哪一步就该失败**，那是对的 —— 把缺件跑成通过才是问题。先跑 B8 那份不需要外部件的 | ⚠️ 未测 | — |
| **B9** | **老化跑一夜** | 不接线（接了负载更好） | `porttool.exe run <仓>/TestCase/plans/soak-2h.json --port COMx --json soak.json`。两小时，`faults` 全程 0、`done=1`、`tmax` 别超 70 ℃。⚠️ **继电器每 30 秒才翻一次是刻意的**，别嫌慢 —— 每秒翻一次两小时就吃掉四分之一触点寿命 | ⚠️ 未测 | — |
| **B8** | **整份方案文件一键跑完** | 只要控制口那根线 | `porttool.exe run <仓>/TestCase/plans/bench-smoke.json --port COMx --csv out.csv`。**七步全过、退出码 0**，`out.csv` 里每条判据一行、带限值和实测值 | ⚠️ 未测 | — |

---

## 3 · 顺便看一眼工装本身对不对

这些主机侧都测过了（H4），上板只是确认真硬件上也一样。**如果这几条对不上，先怀疑固件/协议，别怀疑端口。**

| 看什么 | 应该看到 |
|---|---|
| 连上以后端口树 | **8 个会话 + 6 行交权**（14 个 target 按端口归并去重；`rs485` 挂在自己的会话行上）。RS232 有卡但**没有交权按钮**（那个进去就没有命令循环了） |
| 自动回环应答 | 标题栏「已回 N 次」在涨；任一 `loop=ctrl` 端口的 `miss` 保持 0 |
| **把自动应答关掉** | `miss` 必须开始涨，而 `seq` **停住不动**。这是唯一能证明那个计数器有意义的操作 |
| 逐路参数 | dout / relay / aout 的卡上是**每路一个控件**，不是一个装着 `1:20,5:75` 的文本框 |
| 日志暂停 | 暂停时显示「已暂停，攒了 N 行」，继续后补齐 —— **后台一直在读，不丢帧** |
| 端子标签 | DI 的复选框标 `D02`…`D09`；继电器标 `B01+B02` 这样的**触点对** |

---

## 4 · 三条已知的没验证项（不是这次能解决的）

| 项 | 为什么这次也验不了 |
|---|---|
| **USB-RS232 适配器驱动在别人机器上装不装得上** | 这台开发机上 PL2303GT 是好的，但那不算「干净机器」。这是零安装唯一管不到的地方（[../design/DECISIONS.md](../design/DECISIONS.md) 第 7 条） |
| **SmartScreen 拦未签名 exe** | 要从网络下载一次才会触发 |
| **防火墙对绑 `127.0.0.1` 的监听** | 同上，要在别人机器上 |

---

## 5 · 测完之后

1. **上面的「实测」栏填掉**，M1 的数额外告诉我一声（要进 HARDWARE-FACTS.md）
2. **`PORTTOOL_ENABLE` 改回 0**
3. 期二还欠一件小的：**面板上按档位换算模拟量**（V / µA / mA）+ 悬空带识别。**它等 M2 的结果** —— 换算公式取决于板子焊成了哪个档位
4. 期三是大头：CAN / KNX / SD 的硬件层抽取、以太网 / USB-CDC 会话、产线序列。清单在 [../design/PORTTOOL-FLOW.md](../design/PORTTOOL-FLOW.md) 的 C.5

---

## 6 · 2026-09-08 逐端口实测结果

**测法**：面板上逐个端口按「开始测试」，判据来自 `$TOOL/TestCase/plans/station6-poweron.json`。整轮由 H5 自动跑（`$TOOL/TestCase/host/porttool_panel/run.py`），**95 项判据全过**。接线状态：RS232（控制口）、网口、CDC、CAN、RS485、KNX 都插着；**SD 卡槽空，DI 无激励，AI 无信号源，RS485 无对端**。

| 端口 | 结论 | 读到什么 |
|---|---|---|
| `sdram` | ✅ | probe / sweep / retention 三个目标各按自己那一步的判据判，都过。整片四花样约 6.5 s |
| `eth` | ✅ | LAN8742A `id=0x0007C131`，link up，100 Mbit/s 全双工，`mdio_errors=0` |
| `rtc` | ✅ | 能读出日历，`clk=lsi` |
| `led` | ✅ | PE2 六次（`observed=unknown` 是固件老实 —— 这个脚没有回读） |
| `can` | ✅ | `mode=extloop`，帧穿过 ISO1044 上总线再回来，每周期都闭合 |
| `rs232` | ✅ | `miss=0`，`rxlines` 一直涨 |
| `dout` | ✅ | 八路满占空，只证明「已驱动」 |
| `relay` | ✅ | 六路各设 `on=1`，能听到咔哒；板上无触点回读 |
| `temp` | ✅ | 两路 LM50，约 30.7 / 31.4 ℃ |
| `aout` | ✅ | 按方案设 1000 / 2000 mV，读回同值（µA 是算的） |
| `soak` | ✅ | 按方案 `minutes=1` 跑完，`faults=0`、`done=1`，页面上一直显示板子报的倒计时 |
| `din` | ❌ | `v=0x00` —— **没有激励，正确的失败** |
| `ain` | ❌ | 通道读数落在悬空带 —— **没有信号源** |
| `rs485` | ❌ | `miss=4` —— **没绑对端** |
| `sd` | ❌ | `detected=0` —— **卡不在槽里**。插着卡的那次是 `f_mount` 报 `FR_NO_FILESYSTEM`，因为卡是 exFAT 而当时 `_FS_EXFAT=0`。**2026-09-11 已把 exFAT 打开**，这一条不再成立 |
| `knx` | ✅ | **2026-09-09 查清了：那一轮的 `bus=odd` 是判据错，不是硬件错。** 判据要求 PA10 空闲为低，而 `/KNX_RX` 是低有效、空闲本来就是高；PD7 在这块板子上恒为 0、连总线断电时也不动。判据改成只看 PH12（拔线实测唯一跟着总线走的那一项）后转 ✅。<br/>**报文级已端到端验证**：`mode=frames` 90 秒发 47 条 GroupValueWrite、47 条原样收回且校验字节通过（`crc_raw=47 crc_inv=0 crc_bad=0`）、收到 47 个真实设备回的 `L_Ack ACK`，用户在 **ETS5 总线监视器里看到了这 47 条**。极性问题在位/字符层被 `knx_slot_level()` 取反抵消，到字节层是正的 —— 见 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) |
| `pwm` `bringup` | 👁 人工判 | 只有交权入口，进去要复位板子，上位机判不了 |

**这一轮抓到并修掉的**（都在上位机，不在固件）：判据按端口匹配而不是按目标匹配（sdram 三个目标共用了第一步的判据）；run 类端口只看最后一个目标的结论；方案里少了 rs232 那一步；`vdda` 限值 2000–3600 mV 太宽 —— 那是 VREFBUF 的 2.5 V 基准，好板子读 2501，宽到 3600 会放过一块基准已经失效的板子。
