# 04 · 逐项三分：板子自证 / 人工 / 不做

Type: task
Status: ✅ resolved（2026-09-13）

## Question

在「没有陪测板、没有外接仪器」（[DECISIONS.md 39](../../../design/DECISIONS.md)）的前提下，把《生产测试指导文件 V0.1》工站 6 和工站 10 的**每一个测试项**分成三类，并写清自证那一类的判据取自哪个字段。

## 三类的判准

| 类 | 判准 | 产线要做什么 |
|---|---|---|
| **自证** | 激励和读回都在板内，或靠**跳线**就能闭合 | 上位机下发、读帧、自动判 |
| **人工** | 板子没有通路 | 硬件工程师万用表 / 示波器，读数填进面板 |
| **不做** | 要外接可编程仪器 | 从文档里删，或标成后续 |

---

## 逐项

出处：`$TOOL/TestCase/plans/station6-poweron.json` 的 30 个步骤，和 `pt.caps` 报的 16 行端口 / 12 个 `pt.run` 目标。

### A · 自证，已经在跑（14 项）

| 文档测试项 | 怎么测 | 判据字段 |
|---|---|---|
| SDRAM 压力 | `sdram.probe` + `sdram.sweep` | `size` `ready` `databus` `addrbus` / `patterns` `mismatches` |
| SDRAM 保持 | `sdram.retention`（单次）+ **`sdram` 会话**（长跑） | `failed` / `cycles` `failed` |
| SDRAM 读回校验 | `sdram.crc` | `ready` `offset` `bytes` |
| SD 读写压力 | `sd.probe` `sd.integrity` `sd.stress` `sd.speed` | `fs` `identical` `passes/passed` |
| SD 检测脚 | `sd` 会话 | `detected` |
| 以太网 | `eth.link` + `eth` 会话 | `found` `link` `speed` / `conn` `miss` |
| USB | `usb` 会话 | `state` `enum` `miss` |
| RTC 读 | `rtc.read` | `clk`（读 `RCC_BDCR.RTCSEL`，不是写死的字符串） |
| 温度 | `temp` 会话 | `ok` `vdda` `ch1[1]` `ch2[1]` |
| RS232 | `rs232` 会话（`loop=self`，计数器就是判据） | `miss` `rxlines` |
| RS485 引脚级 | **`rs485.pins`**（2026-09-13 新增） | `checked` `busy` `follows` |
| RS485 链路 | `rs485` 会话 | `miss` `junk` `overrun` |
| CAN | `can` 会话 `mode=extloop` —— **过收发器和隔离器，不需要第二个节点** | `miss` `rx_frames` `junk` `lec` |
| KNX | `knx` 会话 `mode=frames` | `bus` `vcc` `chars` `bad` |

### B · 自证，但要一根跳线（2 项）

| 文档测试项 | 怎么测 | ⚠️ |
|---|---|---|
| **DI 通断** | 八芯线 `A03-A10` → `D02-D09`，DO 出 24 V、DI 耐 24 V，**一根线覆盖十六个通道**；`dout mode=blink` + `din`，判据是矩阵对角 | 现在 `digital-in` 的判据是 `v eq 0xFF`，**要外部夹具把八路都拉高**。改成 DO→DI 回环就不需要夹具 |
| **AI** | `AOUT → 已知阻值电阻 → AIN` | ⚠️ AOUT 是**电流**输出，中间必须串电阻；两头共用同一个 VREFBUF，**验线性不验绝对精度**。而且那两个脚现在**彻底悬空**，等焊 JP5/JP6/JP8/JP9（`../../test/AIN-JUMPER-REQUEST.md`） |

### C · 人工（6 项）

**板子没有任何采样通路**，软件怎么写都拿不到数（核实见 [00](00-scope-from-user.md)）：

| 文档测试项 | 为什么 |
|---|---|
| 待机功耗、最高功耗 | 24V 上没有分压采样进 MCU |
| 3.3V / 5V / 5V_EXT 电压 | 同上 |
| 高边输出的**电压电流** | VNQ5160K-E 的电流 sense 没有回到 MCU |
| 继电器触点的**电压电流** | 触点是干接点，走到端子 `B01-B12`，板上无回读 |
| AO 输出**电流** | `D14 → 表 → Analog GND` 串量。⚠️ 期望值取决于 JP3/JP4 焊没焊 |
| 指示灯 | PE2 无回读，只能人看 |

### D · 不做（2 项）

| 文档测试项 | 依据 |
|---|---|
| Hutschienenverbinder 扩展口环回 | [DECISIONS.md 35 + 39](../../../design/DECISIONS.md)。没有能插 J4 的陪测板；`PH13/PH14` 和控制口共用 UART4，测它要放弃控制台 |
| 一切「测试软件读 PC 外接电压电流模块 / 信号发生仪 / 陪测板」 | [DECISIONS.md 39](../../../design/DECISIONS.md) |

---

## ⚠️ 查出来的两个缺口

### 1. 高边输出和继电器现在是「回显判据」，坏板子照样过

`high-side-out` 判 `ch1 eq 100`、`relays` 判 `ch1 eq 1`。**这两个数是下发的命令值，不是回读** —— 帧格式写着 `!relay t=48213 mode=square ch1=1 ch2=0`，`ch1` 就是刚才让它设的那个值。

**后果**：输出级整个坏掉（VNQ5160K-E 不导通、继电器线圈断）**这两步照样通过**。它们现在证明的只是「命令被接受了」。

**能补的**：

| 路 | 做法 | 代价 |
|---|---|---|
| **甲** | 高边输出走 **DO→DI 八芯线**，DI 读回就是真回读 | 一根线，判据改成矩阵对角 |
| **乙** | 继电器触点串 24 V 接到 DI：`24V → 触点A`、`触点B → DI`，DI 读到就是触点真闭合了 | 跳线。⚠️ **接法没核实过，要硬件工程师确认**（触点额定、DI 门限） |
| 丙 | 维持现状，在文档里写明这两项是人工测 | 0，但产线要知道自动判管不着它 |

**推荐甲 + 乙**，两条都只要跳线，不要陪测板 —— 正是用户说的「硬件自己跳线」。

### 2. `digital-in` 依赖一个不存在的夹具

判据 `v eq 0xFF` 要求**外部把八路 DI 全部拉高**，`_note` 里写的是「stimulus comes from the fixture board」。**那块夹具板不存在。** 改成 DO→DI 回环之后，激励由板子自己给，同一根线还顺带解决了缺口 1 的一半。

---

## 结论

- **文档里 24 项软件需求，能自动判的有 16 项**（14 项已在跑 + 2 项要跳线）
- **6 项是人工**，板子无通路 —— 要在文档里明确标出来，否则产线会等一个永远不出现的自动判定
- **2 项不做**
- **另有 2 项现在判得不算数**（高边输出、继电器），补法是跳线，不是陪测板

这些进 [02 给硬件工程师的审核意见](02-doc-review-memo.md)。
