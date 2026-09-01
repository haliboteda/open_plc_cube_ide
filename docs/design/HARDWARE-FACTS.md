# 硬件实测事实

这里每一条都核实过，标注了核实方式和日期。**没核实的不要往这里写。**

> 排查引脚问题时 `Hardware/Production/UpperDeck/netlist.ipc`（IPC-D-356 网表）比原理图 PDF 快得多：
> `grep -E "U5 +-[0-9]+" netlist.ipc` 直接列出每个引脚在哪个网络。

## RS232 路径（PC10 / PC11 / PB10）

核实：KiCad 原理图 `OpenPLC_UpperDeck_R3.pdf` 第 5 页（2026-08-07）+ `netlist.ipc` 逐脚（2026-08-10）。

MAX3221EIPWR（U5），网名 `TXD_RS232_PC10`、`RXD_RS232_PC11`、`ENABLE_RS232_PB10`：

- **PC10 → U5 DIN**（收发器输入，高阻）
- **PC11 ← U5 ROUT**（收发器**输出**，推挽 —— 绝不能从外部往这里灌信号，曾因此出现"发得出收不到"的现象）
- **PB10 = 使能**，高有效，经 MOSFET Q5
- 外部设备接**端子 C05 (TxD) / C06 (RxD) + GND (C02/C11/C12)**，那里是真正的 RS232 电平（±12V）—— **TTL 适配器接上去可能烧掉适配器**

### PB10 拉低 = 整片关断，不只是"不能读"

一个 GPIO 驱动 MAX3221 的**两个**控制脚，其中一条经 MOSFET **反相**：

| PB10 | U5-12 `FORCEOFF` | Q5 (SI2356DS) | U5-1 `EN` | 芯片状态 | 能发能收 |
|---|---|---|---|---|---|
| 低（R68 下拉，**默认**） | 0 | 关断 → R67 上拉 | 1 = 接收器输出高阻 | **Shutdown，电荷泵停转** | 都不行 |
| 高 | 1（`FORCEON` U5-16 永久接 3V3） | 导通 → 漏极接地 | 0 = 接收器输出使能 | Normal | 都行 |

关键：RS-232 的 ±5V 线电平由芯片内部**电荷泵**升压产生，泵一停，发送驱动器**物理上产生不出电平** —— 所以 **PB10 为低时 printf 一个字节都到不了线上**。

⚠️ 函数名 `Enable_RX_RS232` 只描述了 `EN`（接收器三态）那一半，漏掉了 `FORCEOFF`（整片开关）这更重要的一半，容易误判成"只影响接收"。

### 默认是关的

Arduino core 的 `cores/arduino/main.cpp` 只做了 `pinMode(PB_10, OUTPUT)`，从不写电平，所以引脚停在低位、收发器**默认关闭**。sketch 必须自己 `digitalWrite(RS232_EN_Pin, HIGH)`。

bootloader 侧在 `Core/Src/main.c:174` 显式调 `Enable_RX_RS232()`，**在 `MX_UART4_Init()` 和所有 printf 之前**，所以 bootloader 的日志（含 `bootloader_state_init()` 在 `server_decide()` 里打的那些）是通的。

### 电荷泵余电：关了收发器之后仍有几个字节能出来

MAX3221 的 ±5.5V 线电平由**电荷泵**用几只外部电容产生。PB10 拉低时泵停止开关，但电容上的电荷是**指数衰减**的，衰减期间驱动器仍能产生合规的 RS-232 电平。

| 现象 | 机制 |
|---|---|
| bootloader 交权前关了收发器，app 的 `[BOOT] millis=12` 仍然出得来 | 交权后 12 ms，余电还够 |
| `[BOOT]` 前面那个乱码字节 | 电平从 mark（负）滑向 0V 的过程中，接收端把这个跳变判成起始位 |
| `UART echo ready` 前面没有乱码 | 那是 sketch 拉高 PB10 之后打的，泵已经正常在转 |

⚠️ **这是"能出来"，不是"保证能出来"。** 成立条件是「app 从交权到打印那行的耗时」<「电容放电到不足以产生有效电平的时间」。电容容差、温度、app 启动变慢，任何一个都能让这行消失。**在 core 里加启动打印之前要想清楚这一条。**

## UART4 与 USART3：同两个引脚，两个复用功能

`PC10 / PC11` 上有两个可选功能：**AF8 = UART4，AF7 = USART3**。

- bootloader 的 printf 控制台走 **UART4**，到 MAX3221 再到 RS232 端子 C05/C06
- core 里的 `Serial_Test` 走 **USART3**（`core:cores/arduino/main.cpp` 的 `HardwareSerial Serial_Test(PC_11_ALT1, PC_10_ALT1)`）。**`ALT1` 就是选 AF7 的写法**，线一根没变

`PH13 / PH14` 那一对也是 UART4（接到扩展口 JunctionLink J4-11 / J4-12）。**同一个 UART 外设只有一个句柄槽位，最后一次 `begin()` 赢** —— 所以不能同时在扩展口上有 TTL 串口又保住 RS232 端子上的控制台。

⚠️ **`Serial` 不是 `Serial4`。** 当前 FQBN 用 `usb=CDCgen`，`WSerial.h` 的 `#if !defined(Serial)` 守卫让 `Serial` 保持为 USB CDC，**碰不到 UART4**。要验 UART4 的行为必须显式用 `Serial4`。

## USB 菜单影响 `Serial` 的含义

当前 FQBN 用的是 `usb=CDCgen`，此时 `Serial` 被 `#define` 成 `SerialUSB`，**它不是硬件串口**。已在设备上证实：全用 `Serial` 的回显 sketch 是通过 USB CDC 回显的。

## PG9 就是 BOOT0 网

核实：Bridge 原理图第 5 页网表 + 实测（2026-08-12）。

网络 `KNX_Prog_KEY` = **PG9（U1 ball C10）+ BOOT0（ball D6）+ SW2 pin1 + R58 (10k) + J2 pin9**。

**PG9 不是普通 GPIO，它决定 MCU 的启动源。**

**R58 = 下拉到 GND**（实测：PG9 配 PULLUP / PULLDOWN / NOPULL 各读一次，三次全 0）。由此：

1. BOOT0 网静态为低 → 复位必然从 flash 启动，不会进 ROM DFU
2. `boot0_is_pressed()` 的 active-high 判断**正确**（SW2 按下拉到 3V3）
3. PG9 **不需要**配成输出。曾经的推挽输出配置有隐患（按下 SW2 = 3V3 经引脚对地短路）—— ✅ **2026-08-18 已改成输入**，隐患消除

PG9 配成 input 不影响复位行为：**复位那一刻 MCU 所有 GPIO 都回到默认态**，固件配的推挽输出在那个瞬间并不存在，压住这条网的始终只有 R58 那个 10k 下拉。

改动在 `Core/Src/gpio.c` 的 **`BOOT0_ConfigureAsInput()`（USER CODE 块）**，由 `main.c` 在 `MX_GPIO_Init()` 之后调用。**没有动 CubeMX 生成的那段**，从 `.ioc` 重新生成不会冲掉。

> **为什么要改成 input**：owner 槽的恢复出厂要按住 BOOT0 约 10 秒。推挽输出时按住 = 3V3 经引脚对地短路 10 秒。

## SDRAM 占掉的 39 个脚（用户 sketch 碰得到）

权威清单在 `Core/Src/fmc.c` 的 `HAL_FMC_MspInit()` 里那段 `FMC GPIO Configuration` 注释（2026-08-17 时在 `:224-264`，**行号会漂，按函数名找**）。

这 39 个脚**只连**板载那颗 AS4C32M16SB（64MB，映射在 `0xC0000000`），别的什么都不接。

| 组 | 数量 | 脚 |
|---|---|---|
| 数据 `D0–D15` | 16 | PD14 PD15 PD0 PD1 · PE7–PE15 · PD8 PD9 PD10 |
| 地址 `A0–A12` | 13 | PF0–PF5 · PF12–PF15 · PG0 PG1 PG2 |
| 控制 | 10 | PG4 PG5（BA0/1）· PE0 PE1（NBL0/1）· PG8（SDCLK）· PH2（SDCKE0）· PH3（SDNE0）· PF11（SDNRAS）· PG15（SDNCAS）· PC0（SDNWE） |

⚠️ **变体把 MCU 全部引脚都注册成 Arduino 数字引脚**（`NUM_DIGITAL_PINS 140`），所以用户写 `digitalWrite(PE7, HIGH)`（PE7 = `FMC_D4`）**能编译、能运行、把 SDRAM 数据线拽死**。现象是"SDRAM 偶发读到垃圾"，而且和肇事那行代码之间没有任何提示。

**2026-08-17 起这 39 个脚在变体头里有名字了** —— `core:variants/STM32H7xx/H743/variant_PLC_H743.h` 的 `FMC_RESERVED_*`。**起名不阻止任何事**（`digitalWrite(PE7, ...)` 照样编得过，这是刻意的），只是让人在头文件里就能看见。改 `fmc.c` 的引脚时**两边都要改**，`FMC_RESERVED_PIN_COUNT` 那个 39 是给编译期断言用的锚。

**这不是排布错误** —— 这 39 个脚和本板所有对外 IO（DOUT×8 / DIN×8 / AIN×2 / AOUT×2 / RS232 / RS485 / CAN / KNX）一个都不撞，连 BOOT0 的 PG9 都恰好夹在 PG8 和 PG15 中间空着。**是缺一道防护。**

### 这条总线上没有任何可以下探头的地方

**查的是** `Hardware/Production/Bridge/1436_01_SCHAE-BR` 的 Sheet 5（MPU）和 Sheet 6（Memory）。

| | |
|---|---|
| 两端封装 | U1 `STM32H743IIK6` **UFBGA176，0.65 mm 球距**；U6 `AS4C32M16SB-7BIN` **54-ball FBGA，0.8 mm** |
| 中间有没有器件 | **一个都没有**。16 根数据线全部是球到球直连，无串联电阻、无排阻、无测试点 |
| 板上仅有的 4 个测试点 | TP1 / TP2 `/RESET`、TP3 `VREF`、TP5 `ETH_nINT` —— 都不在总线上 |
| 已核实的球号 | `FMC_D0` = `PD14` = U1 **ball M14** ↔ U6 **ball A8**；`FMC_D1` = `PD15` = U1 **ball L14** ↔ U6 **ball B9** |

⚠️ **别再提"量某根数据线的电阻"或"用示波器看 SDRAM 那一端"** —— 两端出线都在封装阴影内（0.65 mm 球距的扇出过孔只能落在那里），万用表和示波器都没有落点。数据总线上的故障**只能靠软件、冷热喷剂、X-ray 或换板对照**去定位。

⚠️ **R32 / R33 那两个 0Ω 不在 `FMC_D2/D3` 上**，它们在 32.768 kHz 晶振（`XTAL2`）那一路。`PD0`（U1 ball B12）和 `PD1`（ball C12）到 SDRAM 同样是直连。

## SRAM4 的 no-init 机制

链接脚本 `STM32H743IIKX_FLASH.ld` 第 58 行把 SRAM4 前 32 字节挖出来给 boot handoff 记录（`RAM_D3 ORIGIN = 0x38000020`）。

启动代码不碰这 32 字节，所以放在那里的变量**能活过软复位**（掉电仍然丢）。

**将来需要"跨软复位保留、又不想依赖 VBAT"的状态时，用这个机制扩展即可**，不要另起炉灶去用 RTC 备份寄存器。

工程里只有这一个链接脚本，`CMakeLists.txt` 和 `cmake/gcc-arm-none-eabi.cmake` 都指向它。

## Digital In 八路：MCU 侧有外部 10k 上拉，固件不要再开内部上拉

2026-08-27 逐条追 `Hardware/Production/UpperDeck/netlist.ipc` + `bom.csv` 得到，为写板级用例 1 而查。

每一路都是同一个形状，从比较器到 MCU 一共三个元件：

```
LM339LV 开集输出（U3/U4）──┬── 10k 上拉到 +3V3
                          └── 50R 串阻 ── UpperDeck J7 ── MCU 引脚
```

| Digital In | MCU 引脚 | 10k 上拉 | 50R 串阻 | J7 管脚 |
|---|---|---|---|---|
| 1 | PC6 | R60 | R76 | J7-9 |
| 2 | PB5 | R61 | R77 | J7-10 |
| 3 | PB6 | R66 | R78 | J7-11 |
| 4 | PB7 | R59 | R75 | J7-12 |
| 5 | PH10 | R43 | R71 | J7-13 |
| 6 | PH11 | R44 | R72 | J7-14 |
| 7 | PI5 | R45 | R73 | J7-15 |
| 8 | PI6 | R46 | R74 | J7-16 |

**对固件的意义**：DIN 引脚配 `GPIO_MODE_INPUT` + `GPIO_NOPULL` 就够了。开集输出遇到"读不到高"的第一反应通常是打开 MCU 内部上拉 —— 这里**不要**：内部约 40k 并上外部 10k 会拖慢上升沿，而且一旦哪块板漏贴了上拉电阻，内部上拉会把这个缺陷盖住，测不出来。

**比较器有供电**时，空载（端子上没有电压）比较器「+」节点被 1k1 拉到 0 V，低于 0.400 V 基准，输出管导通主动灌地 —— 所以未接线的通道恒读 0。

⚠️ **没有 24 V 的时候恰好相反，八路全部恒读 1。** 2026-08-27 实测：只靠 ST-Link 的 3.3 V 供电时 LM339 没有电源，输出管根本导不通，10k 上拉独占这条线。**「恒读 0」和「恒读 1」是两个不同的故障**：恒 0 = 有电但没接线，恒 1 = 整个模拟/24 V 供电没上来。用例 1 现在把两者分开报。

## 模拟量：没有外部基准，必须使能片内 VREFBUF

2026-08-27 实测 + 核实。**这一条决定 ADC 和 DAC 能不能用**。

Bridge 的 BOM 里**没有任何基准电压芯片**，VREF+ 只挂了退耦。不使能片内 VREFBUF，VREF+ 就悬空在 1.2 V 以下 —— 现象是 ADC 读数变成 `0x8000`/`0x4000` 这类精确的 2 的幂（SAR 不收敛，停在中点猜测），DAC 输出接近 0。

**两个必做动作，缺一不可：**

```c
__HAL_RCC_VREF_CLK_ENABLE();        /* VREFBUF 在 APB4 上有独立时钟位 */
HAL_SYSCFG_VREFBUF_VoltageScalingConfig(SYSCFG_VREFBUF_VOLTAGE_SCALE0);
HAL_SYSCFG_VREFBUF_HighImpedanceConfig(SYSCFG_VREFBUF_HIGH_IMPEDANCE_DISABLE);
SET_BIT(VREFBUF->CSR, VREFBUF_CSR_ENVR);
while ((VREFBUF->CSR & VREFBUF_CSR_VRR) == 0U) { }
```

⚠️ **只开 SYSCFG 时钟是不够的。** 漏掉 `__HAL_RCC_VREF_CLK_ENABLE()` 时，对 `VREFBUF->CSR` 的写入会被静默丢弃，寄存器读回来是 `0x00000000`，固件只会报"没 ready"，看不出原因。

**实测 SCALE0 = 2493 mV**（标称 2.5 V，VREFINT 原始值 32235）。选 2.5 V 不是随便挑的 —— 模拟前端两条路径都正好配这个满量程，见下。

⚠️ 使能 VREFBUF 的前提是 VREF+ 没有被外部驱动。本板成立：VREFINT 读到满量程 → VREF+ < 1.216 V → 引脚是悬空的，不是被接到 3V3。

### PC3_C 的 SYSCFG 模拟开关：模拟用途要「开」

`SYSCFG_PMCR.PC3SO` 复位默认是**关**（PC3_C 与数字 PC3 单元相连）。2026-08-27 同一个节点两种状态各读一次：**开路 raw ≈ 10200，闭合 raw ≈ 8600，差约 16%** —— 闭合时数字单元会加载这个节点。**模拟采样一律置 1（开路）。**

## 模拟输入前端：九个焊接跳线出厂全开路

2026-08-27 从 `Hardware/Production/UpperDeck/` 的网表 + BOM + 贴片坐标核实。

**JP1–JP9 九个跳线全部是裸铜焊盘，出厂一律开路。** 判据和当初定 JP7（CAN 终端电阻）完全一样：`designators.csv` 里九个都在（PCB 上有焊盘），但 `positions.csv` 和 `bom.csv` **一条都没有**（无贴片坐标、无物料）。对照组：确实贴了的 R69 在三份文件里都有。

**后果：JP5/JP6 开着的时候，MCU 的 PC3_C 和 PA6 两个引脚是彻底悬空的** —— 和端子之间、和地之间都没有通路。此时读到的一切（实测约 380 mV，抖动 200–650 LSB）都只是悬空脚，不是端子信号。

### 每个通道两条路，结构相同

```
端子 ──┬─ JPx 脚2-3 ── 68k ──┬── 22k6 ── GND          电压档
       │                     └── JPy 脚1-2 ── MCU
       └─ JPx 脚1-2 ── 374R ─┬── 124R ── GND          电流档
                             └── JPy 脚2-3 ── MCU
```

| 通道 | 端子 | 端子侧跳线 | MCU 侧跳线 | 68k / 22k6 | 374R / 124R | MCU 引脚 |
|---|---|---|---|---|---|---|
| AIN1 | UpperDeck J3-4 | **JP9** | **JP5** | R19 / R20 | R23 / R24 | PC3_C |
| AIN2 | UpperDeck J4-1 | **JP8** | **JP6** | R21 / R22 | R25 / R26 | PA6 |

电阻都是 0.1%，ESD 保护是 `PESD5Z2.5`（D5/D7/D9/D11）。

**换算（两档都正好配 2.5 V 满量程，这就是选 VREFBUF SCALE0 的理由）：**

| 档 | 换算 | 满量程对应 |
|---|---|---|
| 电压 | 端子 mV = 引脚 mV × **4.0089**（比例 22.6/90.6 = 0.2494） | 10.0 V ↔ 引脚 2.494 V |
| 电流 | 端子 µA = 引脚 mV × 1000 / **124** | 20 mA ↔ 引脚 2.48 V；4 mA ↔ 0.496 V |

⚠️ 电流档那个 **374R 是保护电阻**：总阻 498 Ω，误把 10 V 电压源接到电流档时限流到 20 mA，引脚只到 2.48 V，烧不掉。它不参与换算 —— MCU 只跨在 124R 两端。

### 档位决定（用户 2026-08-27 定）

**AIN1 = 电压档，AIN2 = 电流档。** 要让它们通，得用锡桥这四处：

| 通道 | 档 | 桥哪里 |
|---|---|---|
| AIN1 | 电压 | **JP9 脚 2–3** + **JP5 脚 1–2** |
| AIN2 | 电流 | **JP8 脚 1–2** + **JP6 脚 2–3** |

⚠️ **焊上去不可逆。桥之前先对着上面那张网表图确认焊盘编号。**

## CAN 接口（PB9 / PI9，Upper Deck）

2026-08-26 从原理图 / 网表 / BOM 核实，为写 `TestCase/CAN/` 而查。**bootloader 不用 CAN**，这一节纯粹是硬件事实登记。

| 项 | 事实 | 出处 |
|---|---|---|
| MCU 引脚 | **PB9 = FDCAN1_TX**（ball B4）、**PI9 = FDCAN1_RX**（ball D3），**AF9** | `Hardware/STM32H743IIK6_GPIO_ASSIGNMENT_Schaeffer_Bridge_20260822.xlsx` sheet GPIO_ASSIGNMENT 行 95-97；`Hardware/Production/UpperDeck/netlist.ipc:260-261` |
| 为什么只能是这一对 | FDCAN2 全部引脚被占（PB12=RMII_TXD0、PB5/PB6=DIN2/DIN3、PB13=HSFET_1）；FDCAN1 其余复用脚也被占（PD0/PD1=FMC、PA11/PA12=USB FS、PH13/PH14=UART4） | 同 xlsx 行 8/27/28/53/54/65/68/69/83/84/99 |
| 收发器 | **U8 = TI ISO1044BDR**，SOIC-8，**隔离**型 CAN FD 收发器。**没有 STB/EN/silent 脚** —— 8 个脚全占满，listen-only 只能靠外设的 `FDCAN_MODE_BUS_MONITORING` | `Production/UpperDeck/BOM/1435_00_SCHAE_UD_PCB_Project.csv:51`；`netlist.ipc:259-266` |
| 隔离侧电源 | **U7 = PDS1-S5-S5-M / MPB1205**，5V→5V 隔离 DC/DC 1 W，**装在板子底面**，输入经 L1（6.8 µH）从 +5V 来。⚠️ U7 不出电 = 收发器驱动不了总线 | 同 BOM `:50`；`positions.csv:192` |
| 隔离侧地 | `CAN_GND`，与数字地 `GNDD` 分开。`R83`/`R84` 是 **0R DNP**，贴上就破坏隔离 —— **不要贴** | `netlist.ipc:426-427, 445-446`；BOM `:42` |
| 测试点 | `TP_CAN_VDD1`（隔离 5V）、`TP_CAN_GND1`。⚠️ **CAN_H / CAN_L 上没有测试点**，示波器只能夹 J10 螺丝端子 | `netlist.ipc:599, 593` |
| LED | **没有。** 网表里 CAN 相关网络上没有任何 D 器件 | 网表全扫 |
| 内核时钟 | 本工程从未配过 `FDCANSEL`。**复位默认是 `FDCANSEL=00` = HSE。** 在目标上核实过：`D2CCIP1R` 读 `0x00000000`，而 `RCC_FDCANCLKSOURCE_HSE` 就是 0（`Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_hal_rcc_ex.h:1442`）。`TestCase/CAN/can_test.c` 仍显式选 HSE = 25 MHz（125k/250k/500k/1M 四档 prescaler=1 整除） | 在线读寄存器；HAL 头文件 |

### 端子号

|  | 功能 | 连接器 | 网络 |
|---|---|---|---|
| **C08 / A08** | **CAN H** | J10 pin 1 | `CAN_H` |
| **C07 / A07** | **CAN L** | J10 pin 2 | `CAN_L` |
| **A09** | **CAN_GND** | J11 pin 4 | `CAN_GND` |

端子字母 **C / A 两套并存**，指同一批端子：`Klemmblockzuordnung.pdf` 叫 C01–C12，KiCad 原理图 p1 叫 A01–A12。

⚠️ **端子编号以网表 `netlist.ipc:245-256` 和原理图 `OpenPLC_UpperDeck_R3.pdf` p1 为准**（两者一致）。A09=CAN_GND、A10=RS485 A、A11=RS485 B。

CAN 两根信号跨板走 Upper Deck **J8 pin2（CAN_TXD_PB9）/ pin3（CAN_RXD_PI9）**（`netlist.ipc:391-392`）。

### 终端电阻出厂是断开的

**R69 = 120R 1% 已贴**，但串在 **JP7** 上 —— 一个 `SolderJumper_2_Open`，出厂**开路**（`designators.csv:86` 有它，但 BOM 和 `positions.csv` 都没有条目，就是一块裸铜）。所以**整板未端接**。要端接只能用锡把 JP7 桥上，不可逆。短线（<1 m）500 kbit/s 用不着；需要的时候优先打开对端设备自带的终端电阻。

出处：BOM `:40`；`Schematics/OpenPLC_UpperDeck_R3.pdf` p5；`netlist.ipc:420-421`。

## KNX 接口（STKNX U9，Upper Deck）

出处：`Production/Bridge/1436_01_SCHAE-BR.pdf` p5、`Production/UpperDeck/netlist.ipc`、`Schematics/OpenPLC_UpperDeck_R3.pdf` p4。实现在 `TestCase/KNX/`。

**U9 = STKNX，是裸模拟 TP1 收发器，不是 TP-UART** —— 没有字节级主机协议，**TP1 的位时序要 MCU 自己产生**。STKNX 全部工作电源取自 KNX 总线（pin 11 `KNX_A`），总线不供电时整个隔离侧是死的。

| 信号 | MCU 引脚 | 复用 | 说明 |
|---|---|---|---|
| `KNX_TX` | **PB14** | TIM12_CH1 / AF2 | 高 = 有源脉冲，低 = 空闲 |
| `KNX_RX` | **PA10** | TIM1_CH3 / AF1 | 只在总线上有有源脉冲时为高 |
| `KNX_OK` | **PD7** | GPIO | STKNX pin 21，总线电源正常 |
| `KNX_VCC_OK` | **PH12** | GPIO | STKNX pin 19，VCCCORE 正常 |
| `KNX_Prog_LED` | **PG11** | GPIO | 心跳指示 |
| `KNX_Prog_KEY` | **PG9** | — | ⚠️ **和 BOOT0 同一条网**，KNX 代码从不驱动它 |

**TP1 位时序**（STKNX datasheet DocID031327 Rev 1 §5.1 p17）：位周期 **104 µs**（9600 bit/s）；逻辑 1 / 空闲 = `KNX_TX` 全程低；逻辑 0 = `KNX_TX` 高 35 µs 再低 69 µs。

**收发链极性，两端都不反相：**

- **TX**：`+3V3 → U12 LED 阳极；阴极 → R88 332R → PB14`。PB14 低点亮 LED；U12（TLP2362）反相开集，LED 亮则输出低；R94 10k 上拉到 VCCCORE。**净结果 PB14 高 → STKNX pin 24 高 → 有源脉冲。**
- **RX**：`VCCCORE → R89 332R → U13 LED 阳极；阴极 → STKNX pin 23`。pin 23 低点亮 LED，U13 开集输出把 PA10 拉低。

⚠️ **PB14 绝不能悬空。** 没有 LED 电流时 U12 输出释放，R94 把 STKNX pin 24 拉高，收发器会持续从总线抽流。STKNX 自己在 pin 24 上只有 6 µA 下拉，拉不过 10k。**空闲时 PB14 必须驱动为低。**

⚠️ **`/KNX_RX` 板上没有上拉**，不同于另外三个光耦（R92/R93/R94）。高电平只能来自 **MCU 内部上拉**，所以 PA10 一律配 `GPIO_PULLUP`。

**总线供电特征**（用来一眼分辨总线有没有电）：

| 状态 | PH12 | PD7 | PA10 |
|---|---|---|---|
| 总线供电、空闲 | HIGH | HIGH | LOW |
| 总线无电 | LOW | LOW | HIGH |

**与 datasheet 的四处偏差**（外部网络其余部分逐点符合 Figure 3 buck-disabled，CGATE 10 µF + CVDDHV 220 µF 正好是 Table 8 的 30 mA fan-in 那一行）：

| # | 偏差 | 软件能不能看见 |
|---|---|---|
| a | R88/R89/R90/R91 = 332 R，TLP2362 的 LED 电流典型 5.4 mA、最坏 4.3 mA；TLP2362 建议 IF(ON) ≥ 7.5 mA，只保证 IFHL ≤ 5.0 mA 时开关。⚠️ R89/R90/R91 由 VCCCORE 供电，其 IREG 上限 20 mA，空闲时三个 LED 同时亮 —— **不能简单靠减小电阻解决** | 能，表现为丢脉冲计数 |
| b | `/KNX_RX` 没有板上上拉 | 能 |
| c | C27（CVDDHV）是 220 µF / **10 V**，Table 2 要求 ≥ 35 V | ❌ 只能手工量 |
| d | C20（CREF）是 470 nF / **10 V**，同样要求 ≥ 35 V。VREF 自己的 KNX_OK 门限在 9.7 V…13.5 V | ❌ 只能手工量 |

## 引脚分配表：硬件工程师的 xlsx

`Hardware/STM32H743IIK6_GPIO_ASSIGNMENT_Schaeffer_Bridge_20260822.xlsx`，单张 sheet `GPIO_ASSIGNMENT`，138 行覆盖 UFBGA176 上用到的每一个脚。列是：序号、引脚名、**BGA 球号**、备用功能全表、附加功能（ADC/DAC/WKUP）、**信号名**、电源域、中英文说明。取自原理图 Sheet05-MPU。

**这份表管什么、不管什么**（2026-09-01 逐条核对过）：

| 事实 | 谁说了算 | 为什么 |
|---|---|---|
| **信号名、BGA 球号、备用功能** | **这份 xlsx** | 全仓库文档里的球号已和它逐条比对，**没有一处不一致** |
| 外设怎么配（时钟、AF、模式） | `open_plc_cube_ide.ioc` | CubeMX 工程定义，见 [CUBEMX-RULES.md](CUBEMX-RULES.md) |
| 端子号、跨板连接器、板上是否真的布了线 | `Hardware/Production/*/netlist.ipc` + 原理图 | **xlsx 只说 MCU 这一侧**，它列出一个信号不等于板子上接了 |

### 已知的三处出入

1. **xlsx 漏了 `PE15 = FMC_D12`。** 第 42 行是空的，`FMC_D12` 整条不在表里。`.ioc` 有（`PE15.Signal=FMC_D12_DA12`），本文件上面那张 FMC 表也有。**是表缺行，不是工程错** —— 该反馈给硬件。
2. **`PI2 = RS485_EN` 在这块板上不存在。** 表里有，Bridge 板布到了 J2，但 UpperDeck 一根没接。细节写在 [../test/BOARD-BRINGUP-CASES.md](../test/BOARD-BRINGUP-CASES.md) 的引脚速查附录里。**这是「xlsx 只说 MCU 侧」的典型例子。**
3. **RS485 两根信号有两套标签。** xlsx 叫 `RS485_TX`（PD5）/ `RS485_RX`（PD6）；网表叫 `RS485_DI_PD5` / `RS485_RO_PD6`。同一条网，看哪份文件用哪个名。

### xlsx 有、本仓库以前没记过的脚

多数是以太网 / FMC / 晶体 / NC，不必单独记。有实际功能的这几个记一下：

| 引脚 | 球 | 信号名 | 说明 |
|---|---|---|---|
| **PE2** | A2 | `LED_DRIVER` | 系统指示灯驱动，**高亮低灭**。xlsx 把它归在「未使用引脚」一节下，但它有功能 |
| **PE6** | B3 | `SDIO1_CD` | SD 卡检测，**低 = 已插入** |
| PC9 | F14 | `DAC1_EXTI9` | 预留 |
| PB1 / PF9 | R4 / L2 | `C-ADC2_HSPEED` / `C-ADC4_HSPEED` | 预留，ADC 高速通道 |
| PG10 / PB2 | B10 / M6 | `SPI1_NSS` / `SPI3_MOSI` | 预留 |

`DIN1`–`DIN8` 在表里的信号名带编码器后缀（`DIN1_PC6-Enc1a` … `DIN8_PI6-Enc4b`），八路兼作四组编码器 A/B 相。
