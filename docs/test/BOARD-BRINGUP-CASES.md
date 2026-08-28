# 板级测试项对表

**这块板的每个对外接口：接哪几个端子、固件做什么、判据是什么。** 引脚号与信号名出自 `Hardware/STM32H743IIK6_GPIO_ASSIGNMENT_Schaeffer_Bridge_20260822.xlsx` 的 `GPIO_ASSIGNMENT` 表，端子与连接器管脚出自各板的 `netlist.ipc`。

## 写法约定

| 说什么 | 怎么写 | 例 |
|---|---|---|
| **固件里配哪个引脚** | **MCU 引脚号**，只写这一种 | `PB13`、`PC3_C` |
| **人往哪接线** | **外接端口的功能名**在前，端子丝印 / 连接器管脚在括号里 | **Digital Out 1**（端子 A03）、**RS232 TxD**（端子 C05，UpperDeck J10-4） |
| BGA ball 号 | 只出现在文末[速查附录](#附引脚与端子速查) | — |

⚠️ **板上有两个 `J4`，写的时候必须带板名**：**UpperDeck J4** 是模拟量端子的连接器（Analog In 2 / Analog Out 1 / Analog Out 2 / GND），**JunctionLink J4** 是 SPI / I2C 的 20 pin 扩展口。两者的 `J4-3` 完全不是一回事。

⚠️ 端子字母 **C / A 两套并存**，指同一批端子：`Klemmblockzuordnung.pdf` 叫 C01–C12，KiCad 原理图 p1 叫 A01–A12。**接线以功能名和连接器管脚为准**（网表直接给出）。

## 通用前提

| 接什么 | 到哪个外接端口 | 说明 |
|---|---|---|
| **24 V 电源** | + 接 **+12/+24 V 输入**（端子 A15 / A16，LowerDeck X1-2 / X1-1），− 接 **GND Stromversorgung**（端子 A14，LowerDeck X1-3） | **不只是 Digital Out 要它** —— 整块板的 3V3 和 5V 都从 24 V 降下来 |
| **USB-RS232 适配器** | **RS232 TxD**（端子 C05）、**RS232 RxD**（端子 C06），GND 接端子 C02 / C11 / C12 | ⚠️ **必须 RS232 电平**（±12 V）。TTL 适配器接上去可能烧掉适配器 |

---

## 1. 各项的接法与判据

### 1 · Digital In 回环

**接法**：硬件把每组两个 MCU 引脚直接短接 —— `PB5+PC6`、`PB6+PB7`、`PH10+PH11`、`PI5+PI6`。**不需要 24 V。**

**固件**：组里一个脚发、另一个收。发低 → 收端读 0；释放 → 外部 10k 上拉把线拉高，收端读 1。收发互调再测一遍，两个方向都通才算这组过。

- **发送端用开漏，不是推挽。** 这八个脚都挂着 LM339 开集输出 + 10k 上拉 + 50R 串阻：开漏拉低走 3V3/10k ≈ 0.33 mA；推挽在比较器同时灌地时是 62 mA，超过 STM32H7 单脚 25 mA 上限
- **用例自检发送端有没有真的拉低**，用来把「线没接」和「这个脚根本没在驱动」分开 —— 两者现象一样
- ⚠️ **有没有 24 V 决定空载读数**：有 24 V 时比较器「+」节点被 1k1 拉到 0 V，未接线通道**恒读 0**；只靠 ST-Link 3.3 V 供电时 LM339 没电源，10k 上拉独占，**恒读 1**

### 2 · 继电器

**固件**：`PI8 / PI10 / PI11 / PG7 / PG3 / PD3` 同时循环 2 s 高 / 2 s 低（复用 `Core/Inc/relay.h`）。

**量哪里**：

- **MCU 侧**最简单：探头夹 MCU 引脚或 LowerDeck 上 T2–T7 的栅极。**但证不了继电器真的吸合**
- **量触点**：继电器触点是**干接点**，示波器直接夹上去没有波形。要在某一路触点两端串电压源 + 限流电阻，探头量电阻两端

继电器 `HF41F/005-HST`（5 V 线圈，从 `5V0` 供电），续流 `BAS516`（D3–D8），栅极下拉 33k（R3–R8）。六路都在 `.ioc` 里配成 GPIO 输出且 `Locked=true`。

### 3 · 模拟输入（AI）

**接法**：可调电压源接 **Analog In 1**（UpperDeck J3-4）或 **Analog In 2**（UpperDeck J4-1），地接 **Analog GND**（UpperDeck J4-4 / J4-5）。

**固件**：`TestCase/ADC/adc_test.c` 读 ADC，打印原始计数、引脚电压、以及按档位折算的端子电压。16 位分辨率，满量程 65535。

**四条必须知道的事实**（详见 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md)）：

| | |
|---|---|
| **端口标号和引脚是反的** | 硬件上 **Analog In 1 = PC3_C**、**Analog In 2 = PA6** |
| **PC3_C 是 ADC3_INP1**，不是 ADC1 通道 13 | 它是专用模拟焊盘，唯一的控制是 `SYSCFG_PMCR.PC3SO` 模拟开关，**模拟采样一律置 1（开路）** |
| **输入有分压** | 电压档比例 0.2494（端子 mV = 引脚 mV × 4.0089）；电流档 124 Ω 取样（端子 µA = 引脚 mV × 1000 / 124） |
| **必须先使能片内 VREFBUF** | 板上没有任何基准芯片。不使能则 VREF+ 悬空在 1.2 V 以下，ADC 读数变成 `0x8000` 这类 2 的幂 |

**档位（2026-08-27 定）**：**AIN1 = 电压档，AIN2 = 电流档**。JP1–JP9 出厂全部开路，要用锡桥 **JP9 脚 2–3 + JP5 脚 1–2**（AIN1 电压）和 **JP8 脚 1–2 + JP6 脚 2–3**（AIN2 电流）。**焊上去不可逆。**

### 4 · 模拟输出（AO）

**接法**：电流表串在回路里 —— **Analog Out 1**（UpperDeck J4-2）→ 表 → **Analog GND**（UpperDeck J4-4 / J4-5）；**Analog Out 2**（UpperDeck J4-3）同理。

**固件**：`TestCase/DAC/dac_test.c` 设 DAC1_OUT1（PA4）= 500 mV、DAC1_OUT2（PA5）= 1500 mV，打印设定电压和期望电流。12 位右对齐，满量程 4095。

**换算**：XTR111 的 `RSET = 1024R`（R9/R10，BOM 就是 `1024R 1%`，芯片 `XTR111AIDGQT`），`Iout = Vin × 10 / 1024`。**500 mV → 4.883 mA，1500 mV → 14.648 mA。**

- **满量程是 VREF+ 不是 3V3**。DAC 从 ADC 模块取实测的 VREF+（`ADC_Test_GetVrefMv()`），测不到才回退到 2500 mV
- **JP3/JP4 开路时上面的期望值才成立**。桥接后 XTR111 的 VIN 变成 DAC 与 `/VREF` 的加权求和节点（4–20 mA 活零点），两个期望值全变
- **没有软件使能**（OD 脚经 10k 硬接地）
- ⚠️ **故障标志 PI4 / PE3 读不出来**：10k 串 + 1k 上拉的方向使断言低电平只到约 3.0 V
- ⚠️ R17/R18 = 1 kΩ 直接挂在 DAC 输出上，20 mA 时要从 DAC 缓冲抽约 2 mA

### 5 · SPI2　6 · SPI6　7 · I2C2

这三项共用同一个对外引出：**JunctionLink J4**，`SAMTEC ERF8-010-01-L-D-EM2-TR`，**0.8 mm 间距 20 pin —— 杜邦线插不进去**，要对插扩展板或转接板。

| 项 | 引脚 | 不接扩展板时能做什么 |
|---|---|---|
| **SPI2** | PI0 / PI1 / PI3 / **PC2_C** | 引脚级 GPIO 推挽翻转 + 回读。⚠️ **MISO 在 `PC2_C` 双焊盘球**，数字单元只能经 SYSCFG 模拟开关到达 |
| **SPI6** | PG13 / PG14 / **PB4** | 同上。⚠️ **MISO = PB4 = NJTRST**，要覆盖复用功能（本板调试只用 SWD，安全）。⚠️ **全板没有片选**：单从机可把从机 CS 接地，多从机需指定一个 GPIO 做软件片选 |
| **I2C2** | PH4 / PH5 / PH6 | 空闲电平检查（两根都为高才算总线没卡死）+ 引脚级翻转回读 |

**I2C2 的硬事实**：

- **全板没有任何 I2C 上拉电阻**（四块板的网表和 BOM 都查过）。**没有上拉，I2C2 一个 bit 都不会动** —— 外挂模块必须自带（多数 breakout 有 4.7k/10k）
- **板上没有任何 I2C 从机**（四块板 BOM 全扫），必须外挂。模块用 **3V3** 供电，不要用扩展口 J4-01 的 5 V 供 5V 逻辑模块再直连 SCL / SDA
- 信号上串了 `EMI7204MUTAG`（JunctionLink U1–U5），R/C 值本仓库查不到，影响上升时间 —— 保守起点 **100 kHz**
- ✅ I2C 的 HAL 驱动已在 `Drivers/` 且已启用，不用复制文件

### 8 · I2C3　9 · I2C4 —— 这块板做不到

**I2C3（PH7 / PH8 / PH9）和 I2C4（PB8 / PD13 / PD11）没有引出到板外。** Bridge 原理图 p5 把 `I2C3-SCL/SDA/SMBA` 画在 **J2** 上，J2 对插 UpperDeck **J8**；而 `UpperDeck/netlist.ipc:390-419` 里 J8 的 pin 1、4、11–18、22、23、24、28、29 全是 `NET-(J8-PIN_11-PAD11)` 这种**单管脚未连接网络**。全仓 grep `I2C` 在 UpperDeck / JunctionLink / LowerDeck 三份网表里**只命中 `I2C_2_*`**。

**没端子、没插座、没测试点。** 引脚本身是自由的（`.ioc` 没占），能做的只有引脚级 GPIO 翻转回读，证明 MCU 球到板间连接器那段走线没问题 —— 到不了板外，证不了任何总线行为。

### 10 · UART4（RS232 控制台）

**接法**：USB-**RS232** 适配器接 **RS232 TxD**（端子 C05，UpperDeck J10-4）和 **RS232 RxD**（端子 C06，UpperDeck J10-3），GND 接端子 C02 / C11 / C12。115200 8N1。

**判据**：开机能看到打印 → 发送方向通；敲键有响应 → 接收方向通。另有 `TestCase/RS232/rs232_test.c` 的回显模式。

- ❗**UART4 就是当前的 printf 控制台**（PC10 / PC11 → MAX3221 → RS232 端子）
- `PH13 / PH14` 那对也是 UART4（接到扩展口 JunctionLink J4-11 / J4-12），**改指过去就会把控制台拿走**。这两个脚只有 UART4 和 FDCAN1 两个可选功能，**没有办法同时在扩展口上有 TTL 串口又保住 RS232 端子上的控制台**
- **PB10 必须先拉高**（MAX3221 使能），bootloader 已经做了

### 11 · 板载温度

**不需要接线。** 固件读 PA0 / PA3，打印 ADC 电压和 `T = (mV − 500) / 10`。

- 传感器 `LM50BIM3/NOPB`（LowerDeck U1/U2），**10 mV/°C，−25…+100 °C，`V = 10mV×T + 500mV`**
- **VO 到 MCU 之间没有任何分压、缓冲、串阻或滤波电容** —— 逐板核实过。公式直接可用
- PA0 = ADC1_INP16、PA3 = ADC12_INP15，都是普通球，不需要 SYSCFG
- 想对照就量测试点 `TP3` / `TP4`（丝印 **T-PS**）和 `TP9` / `TP10`（丝印 **T-HS**）
- ⚠️ 输出没有滤波电容，LM50 直接驱动 ADC 采样电容 → **采样时间必须给足**，否则读数偏低
- ⚠️ 命名冲突：LowerDeck 原理图把 U1 那路叫 **Verpolschutz（防反接）**，GPIO 表叫 **Short-Circuit Protection**

### CAN（不在 11 项内）

**接法**：**CAN H** = 端子 C08（UpperDeck J10-1），**CAN L** = 端子 C07（J10-2），**CAN_GND** = 端子 A09（J11-4）。

500 kbit/s，Classic CAN，标准帧。引脚 `PB9`（TX）/ `PI9`（RX），AF9。硬件事实见 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) 的 CAN 一节。

**板上未端接**（R69 120R 串在 JP7 上，出厂开路）—— 短线 500 kbit/s 不需要；需要时优先打开对端设备自带的终端电阻。

### KNX（不在 11 项内）

引脚 `PB14`（TX，TIM12_CH1/AF2）/ `PA10`（RX，TIM1_CH3/AF1），另有 `PD7`（KNX_OK）、`PH12`（KNX_VCC_OK）。TP1 位时序要 MCU 自己产生，104 µs/bit。收、发两个方向都已在板上跑通。

发送由 `TestCase/KNX/knx_test.c` 的 `KNX_TX_ENABLE` 控制，**当前默认 0（只收）** —— 只收时总线上只有外部流量，打印出来的东西不可能是自己的回声。硬件事实见 [../design/HARDWARE-FACTS.md](../design/HARDWARE-FACTS.md) 的 KNX 一节。

---

## 2. 固件侧的实现

**一个端口一个子目录**，都在 `TestCase/` 下。`common/` 装公用件：`testcase_hal_guard.h`、`bringup_test.c`、vendored FatFs，以及工程里唯一自带 HAL 副本的地方（ADC / DAC / SD / SDMMC / FDCAN）。

⚠️ `TestCase/common` 必须在 include path 上（`.cproject` 两个 build config 各一条 `../TestCase/common`）—— `stm32h7xx_hal_conf.h` 找那些副本头文件走的是 include path，不是相对路径。

### 入口由 `Core/Src/main.c` 顶部的一组宏选，同时只能开一个

| 宏 | 入口 | 装什么 |
|---|---|---|
| `BRINGUP_TEST_ENABLE` | `common/bringup_test.c` 的 `BringUp_Test_Run()` | **项 1、2、3、4、11 一次烧录同时跑** |
| `KNX_TEST_ENABLE` | `KNX/knx_test.c` | KNX 分阶段用例 |
| `CAN_TEST_ENABLE` | `CAN/can_test.c` 的 `CAN_Test_Run()` | CAN 分段：P0 报告、P1/P2 回环、P3 监听、P4 正常 |
| `CAN_SOAK_TEST_ENABLE` | `CAN_Test_Soak_Run()` | 周期发帧 + 打印收到的帧，可关发送 |
| `CAN_SCOPE_TEST_ENABLE` | `CAN_Test_Scope_Run()` | 三段循环，给示波器看 PB9 用：GPIO 方波 → 回环连发 → 正常模式连发 |
| `CAN_ECHO_TEST_ENABLE` | `CAN_Test_Echo_Run()` | 只收不发，收到帧就把 payload 当大端整数加 1，原 ID 返回 |

`main.c` 里有一条 `#error` 守着：多开一个就编不过（每个 `*_Test_Run` 都不返回）。

### 项 1、2、3、4、11 为什么能同时跑

五项没有一个引脚重叠，也没有一个碰到 SDRAM 那 39 根 FMC 引脚（PG3 = FMC_A13、PD3 = FMC_CLK 的复用本工程没用，SDRAM 时钟走 PG8、地址只到 A12）。唯一的共享资源是 **ADC1**，被项 3 和项 11 同时需要 —— 两项写在同一个文件里、共用一个句柄；两次 `HAL_ADC_Init()` 打同一个实例会把前一次的配置静默覆盖。

每一项都是**非阻塞 tick**。项 2 要给示波器看 2 s 方波，所以任何一项都不许长时间阻塞 —— 项 1 的八通道扫描因此写成状态机而不是 `HAL_Delay` 循环。

**串口按键**（RS232 端子 C05/C06，115200 8N1）：`1` `2` `3` `4` `b` 单独开关某一项（`b` = 项 11），`a` 全开，`?` 看帮助。

⚠️ **一处物理耦合不是故障**：项 1 在切 Digital Out 的负载，而那份热量正是项 11 的 T-HS 传感器测的东西。跑起来 T-HS 缓慢上升是板子在正常工作。

| 项 | 代码 | 用什么外设 |
|---|---|---|
| 1 Digital IN 回环 | `TestCase/DIN/din_test.c` | 纯 GPIO |
| 2 继电器 | `bringup_test.c` 复用 `Core/Src/relay.c` | 纯 GPIO |
| 3 AI | `TestCase/ADC/adc_test.c` | ADC3（PC3_C）+ ADC1（PA6） |
| 4 AO | `TestCase/DAC/dac_test.c` | DAC1 两个通道 |
| 11 温度 | `TestCase/ADC/adc_test.c` | ADC1（PA0、PA3） |
| CAN | `TestCase/CAN/can_test.c` | FDCAN1 |
| KNX | `TestCase/KNX/knx_test.c` | TIM12 / TIM1 |

### 还缺的

| 缺什么 | 挡住 | 怎么补 |
|---|---|---|
| **SPI 的 HAL 驱动** | 项 5、6 | 从 `STM32Cube_FW_H7_V1.12.1` 复制到 `TestCase/common/` —— 该包 HAL 版本正是 1.11.5，和本工程一致（ADC / DAC / FDCAN 三次都走通了这条路） |
| I2C 的 HAL 驱动 | 项 7 | **已在 `Drivers/` 且已启用**，不用动 |

**`.ioc` 一律不动**（Digital In / Digital Out / ADC / DAC / SPI / I2C 全都不在里面），`Core/Inc/stm32h7xx_hal_conf.h` 一律不动 —— 用 `TestCase/common/testcase_hal_guard.h` 那套本地模块开关的做法。

---

## 附：引脚与端子速查

**BGA ball 号只在这一节出现**，供硬件量球时用；正文一律用 MCU 引脚号。

**项 1 · Digital Out → Digital In 回环对照**

| 通道 | 外接端口（发） | 端子 | MCU 引脚 | BGA | 外接端口（收） | 端子 | MCU 引脚 | BGA |
|---|---|---|---|---|---|---|---|---|
| 1 | Digital Out 1 | A03 | PB13 | P13 | Digital In 1 | D02 | PC6 | H15 |
| 2 | Digital Out 2 | A04 | PB0 | R5 | Digital In 2 | D03 | PB5 | A6 |
| 3 | Digital Out 3 | A05 | PH15 | D13 | Digital In 3 | D04 | PB6 | B6 |
| 4 | Digital Out 4 | A06 | PE4 | B1 | Digital In 4 | D05 | PB7 | B5 |
| 5 | Digital Out 5 | A07 | PA8 | F15 | Digital In 5 | D06 | PH10 | L13 |
| 6 | Digital Out 6 | A08 | PA9 | E15 | Digital In 6 | D07 | PH11 | L12 |
| 7 | Digital Out 7 | A09 | PI7 | C2 | Digital In 7 | D08 | PI5 | C4 |
| 8 | Digital Out 8 | A10 | PE5 | B2 | Digital In 8 | D09 | PI6 | C3 |

GPIO 表里 Digital Out 的信号名是 `HSFET_1`–`HSFET_8`（高端驱动 FET 控制输出），Digital In 是 `DIN1`–`DIN8`（兼作编码器 Enc1a–Enc4b）。

**项 2 · 继电器对照**

| 外接端口 | MCU 引脚 | BGA | 驱动 MOSFET | 触点连接器 | 文档标的端子 |
|---|---|---|---|---|---|
| Relay 1 触点 | PI8 | D2 | T7 (SI2356DS) | LowerDeck X8-1 / X8-2 | B01 / B02 |
| Relay 2 触点 | PI10 | E3 | T6 | LowerDeck X8-3 / X7-1 | B03 / B04 |
| Relay 3 触点 | PI11 | E4 | T5 | LowerDeck X7-2 / X7-3 | B05 / B06 |
| Relay 4 触点 | PG7 | J14 | T4 | LowerDeck X6-1 / X6-2 | B07 / B08 |
| Relay 5 触点 | PG3 | K15 | T3 | LowerDeck X6-3 / X5-1 | B09 / B10 |
| Relay 6 触点 | PD3 | D11 | T2 | LowerDeck X5-2 / X5-3 | B11 / B12 |

继电器 `HF41F/005-HST`（**5 V 线圈**，从 `5V0` 供电），续流 `BAS516`（D3–D8），栅极下拉 33k（R3–R8）。六路都已在 `.ioc` 里配成 GPIO 输出且 `Locked=true`。

**项 3、4、10、11 · 模拟量、温度、串行端子对照**

| 外接端口 | 端子 / 连接器管脚 | MCU 引脚 | BGA | ADC / DAC 通道 |
|---|---|---|---|---|
| Analog In 1 | UpperDeck J3-4 | PC3_C | M5 | ADC3_INP1（需 SYSCFG 模拟开关） |
| Analog In 2 | UpperDeck J4-1 | PA6 | P3 | ADC12_INP3 |
| Analog Out 1 | UpperDeck J4-2 | PA4 | N4 | DAC1_OUT1 |
| Analog Out 2 | UpperDeck J4-3 | PA5 | P4 | DAC1_OUT2 |
| （AOUT1 故障标志，读不出来） | 板内 | PI4 | D4 / D14 冲突 | — |
| （AOUT2 故障标志，读不出来） | 板内 | PE3 | A1 | — |
| （板载温度，防反接侧 T-PS） | 测试点 TP3 / TP4 | PA0 | N3 | ADC1_INP16 |
| （板载温度，高端开关侧 T-HS） | 测试点 TP9 / TP10 | PA3 | R2 | ADC12_INP15 |
| RS232 TxD | 端子 C05，UpperDeck J10-4 | PC10 | B14 | — |
| RS232 RxD | 端子 C06，UpperDeck J10-3 | PC11 | B13 | — |
| （RS232 收发器使能，必须先拉高） | 板内 | PB10 | R12 | — |
| CAN H / CAN L | 端子 C08 / C07，UpperDeck J10-1 / J10-2 | PB9（TX）/ PI9（RX） | B4 / D3 | — |
| RS485 A / B | 端子 A10 / A11，UpperDeck J11-3 / J11-2 | PD5（TX）/ PD6（RX） | C11 / B11 | — |
| （RS485 使能 / 方向） | 板内 | PI2 / PD4 | C14 / D10 | — |

**项 5–7 · 扩展口 JunctionLink J4**（`SAMTEC ERF8-010-01-L-D-EM2-TR`，0.8 mm 20 pin）

| 管脚 | 信号 | MCU 引脚 | BGA |
|---|---|---|---|
| J4-01 | +5 V ⚠️ 信号都是 3V3 | — | — |
| J4-02 / J4-07 | GND | — | — |
| J4-03 | SPI2_MISO | **PC2_C** | M4 |
| J4-04 | SPI2_MOSI | PI3 | C13 |
| J4-05 | SPI2_SCK | PI1 | D14 |
| J4-06 | SPI2_NSS | PI0 | E14 |
| J4-08 | I2C2_SDA | PH5 | J4 |
| J4-09 | I2C2_SCL | PH4 | H4 |
| J4-10 | I2C2_SMBA | PH6 | M11 |
| J4-11 | UART4_TX | PH13 | E12 |
| J4-12 | UART4_RX | PH14 | E13 |
| J4-13 | SPI6_SCK | PG13 | A8 |
| J4-14 | SPI6_MOSI | PG14 | A7 |
| J4-15 | SPI6_MISO | **PB4** = NJTRST | A9 |
| J4-16 – J4-19 | SWO / JTDI / SWCLK / SWDIO | PB3 / PA15 / PA14 / PA13 | A10 / A13 / A14 / A15 |
| J4-20 | DEBUG_TRIGGER | PC7 | G15 |

**未引出的**：I2C3 = PH7 / PH8 / PH9（N12 / M12 / M13），I2C4 = PB8 / PD13 / PD11（A5 / M15 / N14）。

**端子块 —— 外接端口一览**

| 块 | 外接端口 | 端子 | 连接器管脚 |
|---|---|---|---|
| **A**（LowerDeck） | GND Digital Out | A02 / A12 | X4-3 / X2-1 |
| | **Digital Out 1 – 8** | **A03 – A10** | X4-2, X4-1, X3-4, X3-3, X3-2, X3-1, X2-4, X2-3 |
| | GND Stromversorgung | A14 | X1-3 |
| | **+12/+24 V 输入** | **A15 / A16** | X1-2 / X1-1 |
| **D**（UpperDeck） | GND | D01 | J1-1 |
| | **Digital In 1 – 8** | **D02 – D09** | J1-2, J1-3, J1-4, J2-1, J2-2, J2-3, J2-4, J3-1 |
| | GND | D10 / D11 | J3-2 / J3-3 |
| **C**（UpperDeck） | **RS232 TxD / RxD**（控制台） | **C05 / C06** | J10-4 / J10-3 |
| | CAN L / CAN H | C07 / C08 | J10-2 / J10-1 |
| | CAN_GND | A09 | J11-4 |
| | RS485 A / B | A10 / A11 | J11-3 / J11-2 |
| **模拟**（UpperDeck） | Analog In 1 / Analog In 2 | J3-4 / J4-1 见上 | J3-4 / J4-1 |
| | Analog Out 1 / Analog Out 2 | J3-4 / J4-1 见上 | J4-2 / J4-3 |
| | Analog GND | — | J4-4 / J4-5 |

（A01、A11、A13 的丝印未核实。C 和 A 两套字母并存指同一批端子 —— `Klemmblockzuordnung.pdf` 叫 C，KiCad 原理图第 1 页叫 A。）
