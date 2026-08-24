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

### 电荷泵余电 —— 为什么关了收发器还能出来几个字节

⚠️ **上面那条推论（"`[BOOT] millis=` 到不了端子"）被实测否定了**，2026-08-17 在 COM5（就是端子 C05/C06）看到了它。代码事实没错，错的是"所以发不出去"这一步。补上的是**电容**：

MAX3221 的 ±5.5V 线电平不是从 3V3 直接来的，是**电荷泵**（charge pump）用几只外部电容"倒腾"出来的 —— 开关电容反复充放电，把 3V3 抬成 ±5.5V。

**PB10 拉低时，泵停止开关，但那几只电容上的电荷不会瞬间消失。** ±5.5V 轨是**指数衰减**下去的，衰减期间驱动器仍然能产生合规的 RS-232 电平。所以：

| 现象 | 解释 |
|---|---|
| bootloader 交权前关了收发器，app 的 `[BOOT] millis=12` **仍然出得来** | 交权后 12ms，余电还够 |
| `[BOOT]` 前面那个乱码字节 | 电平从 mark（负）**滑向 0V** 的过程中，接收端把这个跳变判成起始位 |
| `UART echo ready` 前面**没有**乱码 | 那是 sketch 拉高 PB10 之后打的，泵已经正常在转 |

⚠️ **这是"能出来"，不是"保证能出来"。** 成立条件是「app 从交权到打印那行的耗时」< 「电容放电到不足以产生有效电平的时间」。现在 12ms 够用，但**没有任何东西保证它** —— 电容容差、温度、app 启动变慢，任何一个都能让这行悄悄消失。

**所以在 core 里加启动打印之前要想清楚这一条**：加得越多，越可能超出余电窗口而**静默丢失**。要可靠输出的唯一办法是打印前主动拉高 PB10，但那会抢走用户 app 对收发器初始状态的控制权 —— 取舍见 [../work/ISSUES.md](../work/ISSUES.md) 的 B1。

## UART4 曾被重复占用 ✅ 2026-08-17 已修（M5），后果比原先以为的严重得多

`Serial_Test` 已从 UART4 挪到 **USART3**（`core:cores/arduino/main.cpp` 的 `HardwareSerial Serial_Test(PC_11_ALT1, PC_10_ALT1)`）。**`ALT1` 是关键**：同样两个引脚，AF8 = UART4，AF7 = USART3。**线一根没变**，端子 C05/C06 和 bootloader 自己的 UART4 日志都不受影响。

### 实测的后果：不是"诊断口变哑"，是**整个 app 挂死**

2026-08-17 实测（修之前）：一个 sketch 只要调 `Serial4.begin(115200)`，

```
** APP Mod ...
[?[B          ← [BOOT] 打到一半断掉，然后什么都没有了
```

**app 卡死，UDP 发现不应答，以太网够不着，IAPTool 报 `No response ... exiting`。** 板子只能靠 ST-Link 擦掉 app 区才救得回来。

⚠️ **原来记的"发送看起来一切正常、只是收不到"是错的** —— 那是推断，不是实测。真实情况严重得多：**发送也死，而且板子失联**。E7 的严重性因此从"排查体验问题"升级为"用户一行普通代码就能让板子变砖（需 ST-Link 才能恢复）"。

⚠️ **`Serial` 不是 `Serial4`。** 当前 FQBN 用 `usb=CDCgen`，`WSerial.h` 里的 `#if !defined(Serial)` 守卫让 `Serial` 保持为 USB CDC，**碰不到 UART4**。所以拿 `Serial` 写的测试在坏 core 上照样通过 —— 这个坑当天踩过一次。**要复现必须显式用 `Serial4`。**

判据和跑法见 `$TOOL/TestCase/TEST-CASES.md` 的 **M5** 一节（`tools/run-m5.ps1`）。

**机制**（修之前）：`HardwareSerial Serial_Test(PC_11, PC_10)` 解析到 **UART4**（AF8），而 `Serial4` / `Serial` 在 PH13/PH14 上**也是 UART4**。`uart_handlers[]` 每个外设只有一个槽位，**最后一次 `begin()` 赢**。

⚠️ 当时记的后果（"静默掐掉接收、发送看起来正常"）**是推断，已被实测否定** —— 见 [../archive/RETRACTED.md](../archive/RETRACTED.md) 第 13 条。

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

### "改成 input 会让 RESET 失效"—— 2026-08-18 受控实验否定了

这条怀疑一直挂着（"只有 PG9 是 output 的时候 RESET 才正常"）。当时**同一次改动里还删掉了 `SystemClock_Config()`**（VOS3 + 64MHz + 0 等待周期超规格读 flash），症状被归到了 PG9 头上。

**只改一个变量重做了一次**：PG9 `GPIO_MODE_OUTPUT_PP` → `GPIO_MODE_INPUT`，`SystemClock_Config()` 一个字没动。实测 **6 次物理 RESET**（全部 `Reset cause: PIN`）：

| 次数 | BOOT0 | 结果 |
|---|---|---|
| 4 次 | 没按 | 正常启动进 app |
| 2 次 | 按住过采样点 | `BOOT0 button is pressed!` → `** UPLOAD Mod ... (BOOT0 held)` |

**RESET 正常，BOOT0 也读得到。真因确实是时钟配置，不是 PG9。**

从电气上也说得通：**复位那一刻 MCU 所有 GPIO 都回到默认态**，固件配的推挽输出在那个瞬间根本不存在 —— 压住这条网的始终只有 R58 那个 10k 下拉，配 input 还是 output 对复位行为不起作用。

⚠️ 改动放在 `Core/Src/gpio.c` 的 **`BOOT0_ConfigureAsInput()`（USER CODE 块）**，由 `main.c` 在 `MX_GPIO_Init()` 之后调用。**没有动 CubeMX 生成的那段** —— 从 `.ioc` 重新生成不会把它冲掉。

> **为什么非改不可**：owner 槽的恢复出厂要按住 BOOT0 约 10 秒（模块 M1）。原来那 1.5 秒窗口短路时间短所以没出事，**按 10 秒就是短路 10 秒**。

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

### ⚠️ 这条总线上没有任何可以下探头的地方 —— 2026-08-21 查原理图确认

**查的是** `Hardware/Production/Bridge/1436_01_SCHAE-BR` 的 Sheet 5（MPU）和 Sheet 6（Memory）。

| | |
|---|---|
| 两端封装 | U1 `STM32H743IIK6` **UFBGA176，0.65 mm 球距**；U6 `AS4C32M16SB-7BIN` **54-ball FBGA，0.8 mm** |
| 中间有没有器件 | **一个都没有**。16 根数据线全部是球到球直连，无串联电阻、无排阻、无测试点 |
| 板上仅有的 4 个测试点 | TP1 / TP2 `/RESET`、TP3 `VREF`、TP5 `ETH_nINT` —— 都不在总线上 |
| 已核实的球号 | `FMC_D0` = `PD14` = U1 **ball M14** ↔ U6 **ball A8**；`FMC_D1` = `PD15` = U1 **ball L14** ↔ U6 **ball B9** |

⚠️ **别再提"量某根数据线的电阻"或"用示波器看 SDRAM 那一端"** —— 两端出线都在封装阴影内（0.65 mm 球距的扇出过孔只能落在那里），万用表和示波器都没有落点。数据总线上的故障**只能靠软件、冷热喷剂、X-ray 或换板对照**去定位。

⚠️ **R32 / R33 那两个 0Ω 不在 `FMC_D2/D3` 上**，它们在 32.768 kHz 晶振（`XTAL2`）那一路。`PD0`（U1 ball B12）和 `PD1`（ball C12）到 SDRAM 同样是直连。曾经照原理图缩略图误认过一次。

## SRAM4 的 no-init 机制

链接脚本 `STM32H743IIKX_FLASH.ld` 第 58 行把 SRAM4 前 32 字节挖出来给 boot handoff 记录（`RAM_D3 ORIGIN = 0x38000020`）。

启动代码不碰这 32 字节，所以放在那里的变量**能活过软复位**（掉电仍然丢）。

**将来需要"跨软复位保留、又不想依赖 VBAT"的状态时，用这个机制扩展即可**，不要另起炉灶去用 RTC 备份寄存器。

> 曾经有三个链接脚本、其中两个没做这个保留，导致 cmake 路径会静默踩掉 handoff 记录。**已解决**：那两个已删除，`CMakeLists.txt` 和 `cmake/gcc-arm-none-eabi.cmake` 现在都指向唯一的那个。

## 文档错误

`Hardware/UpperDeck_overview.txt` 第 77 行（端子表 C05 那行）把 PB10 和 PC10 写反了。同文件第 60 行是对的。**以 KiCad 为准。**

## 蓝牙 COM 口会冻住串口工具（主机侧问题，与固件无关）

**现象**：板子一上电，PC 上另一个串口工具（**不同端口**）卡死约 30 秒；拔掉板子 USB 就正常。

**原因**：Windows 的"蓝牙链接上的标准串行"COM 端口。设备接入 → 广播 `WM_DEVICECHANGE` → 串口工具重扫端口 → 打开蓝牙 SPP 口时会真的去连蓝牙，对端不在就等到超时。工具在 UI 线程同步阻塞，整个程序冻住。

**处理**：设备管理器里禁用所有"蓝牙链接上的标准串行"。2026-08-12 实测有效。不影响蓝牙耳机（走 A2DP/HFP，与 SPP 无关）。**配对新蓝牙设备后可能重新出现。**

**教训**：遇到"插上 A 就影响 B"，先分清卡住的是**整个应用**还是**那个端口的数据**。前者几乎必然是主机侧端口枚举被拖住，与固件无关。这次在固件侧查了 DFU、LPM、PHY 时钟门三个方向，全不是原因。
